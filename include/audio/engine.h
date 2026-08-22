/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_AUDIO_ENGINE_H
#define KURONO_AUDIO_ENGINE_H

#include <limits.h>   /* PATH_MAX */
#include <stdint.h>

#include "decoder.h"
#include "format.h"
#include "ring.h"
#include "sink.h"

/*
 * audio/engine.c is T2, the decoder thread, and the only module
 * permitted to hold both a ring pointer and a sink pointer
 * (DESIGN.md III.2). Confinement: engine.c reaches the decoder only
 * through AudioSource (src.open/read/seek/close) and the sink only
 * through AudioSink plus the narrow T2-binding accessors phase 7 added
 * to sink.h (sink_pcm_ring, sink_flush_flag, sink_cb_seq,
 * sink_frames_consumed, sink_set_gain, sink_set_producer_active) --
 * see phase-7.md for why those exist and why they are a flagged scope
 * deviation. No ffmpeg or miniaudio type or symbol appears here.
 */

/*
 * EngineState: T2's own view of whether it is producing. ENG_STOPPED
 * (parked, no track or a track was explicitly stopped), ENG_PLAYING
 * (actively filling pcm_ring), ENG_PAUSED (parked by CMD_PAUSE with a
 * track still open). ENG_DEVICE_LOST is reserved for the reopen-ladder
 * policy, which lives on T1 (DESIGN.md IV.8) -- engine.c never sets it
 * itself; it is part of this enum only because plan.md's
 * phase-7-engine.md I.1 gives all four values together.
 *
 * Phase 8 built the ladder and deliberately did NOT start storing this
 * value into the Engine: T1 writing T2's state atomic would park T2
 * behind its back, bypassing park() and leaving both the
 * producer-active flag and the IV.8.b step 1 handshake stale. T1 keeps
 * its own device-policy state and parks T2 the one supported way,
 * through engine_park_sync. The value stays for a later phase that
 * wants to surface it.
 */
typedef enum { ENG_STOPPED, ENG_PLAYING, ENG_PAUSED, ENG_DEVICE_LOST } EngineState;

/*
 * CommandKind / Command: the T1 -> T2 channel (DESIGN.md IV.6). path
 * is an inline array inside the command slot, not a pointer -- no
 * allocation, no ownership transfer, no lifetime question across the
 * thread boundary (DESIGN.md V.1 step 2).
 */
typedef enum {
  CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_SEEK, CMD_STOP, CMD_QUIT, CMD_SET_GAIN,
  /* Amendment 09: what plays after the current track, told to T2 in
     advance so a natural EOF-to-next transition needs no round trip to
     T1 (DESIGN.md V.3 step 3). T2 stores it and acts on nothing until
     EOF; an empty path clears the slot and means end of queue. Unlike
     CMD_PLAY this does NOT flush -- that asymmetry is the whole design
     (DESIGN.md V.1 step 3 versus V.3). */
  CMD_SET_NEXT
} CommandKind;

typedef struct {
  CommandKind kind;
  char        path[PATH_MAX];
  int64_t     track_id;
  int64_t     arg_ms;
  float       gain;
} Command;

/*
 * EventKind / Event: the T2 -> T1 channel (DESIGN.md IV.6). This queue
 * has exactly one writer -- T2 -- and one reader -- T1; T3 never posts
 * to it (DESIGN.md IV.6: "T3 never posts a queued event"). EV_EOF's
 * track_id is the track that just finished; there is deliberately no
 * EV_TRACK_ENDED -- that is derived on T1 from g_frames_consumed
 * versus boundary_ring (DESIGN.md IV.6).
 */
typedef enum {
  EV_STREAM_INFO, EV_OPEN_FAILED, EV_DECODE_ERROR, EV_EOF
} EventKind;

typedef struct {
  EventKind  kind;
  int64_t    track_id;
  StreamInfo info;      /* valid only when kind == EV_STREAM_INFO */
  int        averror;   /* valid only for EV_OPEN_FAILED / EV_DECODE_ERROR */
  uint64_t   end_frame; /* valid only for EV_EOF: T2's monotonic write-frame
                           counter at EOF, in the same frame domain as
                           g_frames_consumed. T1 holds PLAY_PLAYING until the
                           consumed counter reaches it (amendment 06) -- the
                           decoder runs up to PCM_LEAD_SECONDS ahead, so EOF
                           on T2 is not the end of the audio. */
} Event;

/*
 * BoundaryEntry: one boundary_ring slot (DESIGN.md V.3), 16 bytes.
 * frame_index is T2's monotonic write-frame counter value at the
 * moment track_id started; T1 pops entries whose frame_index has been
 * reached by g_frames_consumed. engine.c owns the write cursor, T1 the
 * read cursor (DESIGN.md IV.1) -- both may call the generic ring.h
 * functions on the Ring* engine_boundary_ring returns.
 */
typedef struct {
  uint64_t frame_index;
  int64_t  track_id;
} BoundaryEntry;

typedef struct Engine Engine;

/*
 * engine_create: allocate an Engine bound to fmt, to `src` -- the
 * AudioSource it will decode through -- and to sink (used only for
 * sink_is_started; engine.c never calls sink.open/start/stop/close,
 * those stay on T1, DESIGN.md IV.9). Also allocates boundary_ring and
 * binds pcm_ring via sink_pcm_ring(), so sink.open must already have
 * succeeded before this is called.
 *
 * **The Engine takes ownership of src** and destroys it through
 * src.destroy in engine_destroy. The caller must not destroy it, and
 * must not use it afterwards. On failure src is destroyed here too, so
 * an unchecked NULL return leaks nothing.
 *
 * The source is a parameter rather than something this function builds
 * (it called decoder_create itself until phase 18) because DESIGN.md
 * VI's NullSource/SineSource are only substitutable if the engine never
 * names a concrete implementation. That was the gap: the engine
 * constructed its own decoder and then called decoder_retarget_rate and
 * decoder_destroy on it, so no double could be injected and §VI's claim
 * that the ring and the callback can be exercised without an audio file
 * was aspirational. audio/testsrc.c holds the doubles; the production
 * caller passes decoder_create(fmt).
 *
 * Takes the negotiated session format, the source to own, and the sink
 * handle. Returns a heap-allocated Engine, or NULL on allocation
 * failure or a src with a NULL self (nothing is left partially
 * allocated on NULL).
 */
Engine * engine_create(const SessionFormat * fmt, AudioSource src, AudioSink * sink);

/*
 * engine_destroy: free everything engine_create allocated, including
 * closing whatever file the AudioSource still has open. Only safe
 * after engine_join has returned (or engine_start was never called) --
 * this does not stop or join the thread itself. Takes the Engine
 * (NULL is a no-op), returns nothing.
 */
void engine_destroy(Engine * e);

/*
 * engine_set_event_pipe: cache the self-pipe write fd T2 writes a
 * wakeup byte to after posting an event (DESIGN.md IV.6, mirroring
 * sink_set_event_pipe's contract exactly). Call before engine_start.
 * -1 (the default) means "no wakeup pipe": the doorbell write is
 * skipped, which is what lets --play run with no event loop. event.c
 * owns the fd; engine.c only caches it and never closes it. Takes the
 * Engine and the fd, returns nothing.
 */
void engine_set_event_pipe(Engine * e, int fd);

/*
 * engine_start: pthread_create T2. T2 starts parked on the command
 * condvar with no track loaded (DESIGN.md IV.9.a step 7) -- this must
 * be called after sink_pcm_ring() would return non-NULL, i.e. after a
 * successful AudioSink.open. Takes the Engine, returns 0 on success or
 * -1 if pthread_create failed or e->pcm_ring is NULL.
 */
int engine_start(Engine * e);

/*
 * engine_join: block until T2 has returned from its thread function.
 * The only legal way to know T2 is done (DESIGN.md IV.9.b step 5) --
 * call only after a CMD_QUIT has been pushed (engine_quit) and only
 * once. Takes the Engine, returns nothing.
 */
void engine_join(Engine * e);

/*
 * engine_play / engine_pause / engine_resume / engine_seek /
 * engine_stop / engine_quit / engine_set_gain: T1-side convenience
 * wrappers over the command channel (DESIGN.md IV.6's payload list).
 * Each pushes one Command and signals the command condvar; each may
 * block briefly (bounded by T2's processing latency) if the command
 * queue is momentarily full -- see phase-7.md for why push is
 * blocking rather than drop-oldest. path is copied into the command's
 * inline buffer; the caller's string does not need to outlive the
 * call. All take the Engine plus the command's payload and return
 * nothing -- T1 learns the outcome asynchronously via
 * engine_poll_event, exactly as DESIGN.md V.1 step 5 describes for
 * CMD_PLAY.
 */
void engine_play(Engine * e, const char * path, int64_t track_id);

/*
 * engine_set_next: tell T2 what follows the current track, so a natural
 * EOF-to-next transition needs no round trip to T1 (amendment 09,
 * DESIGN.md V.3 step 3). Takes the Engine, the path (NULL or "" clears
 * the slot and means end of queue) and its library id; returns nothing.
 *
 * Push it whenever the answer changes: on EV_STREAM_INFO for the current
 * track, and after ANY queue mutation while playing. queue.h guarantees
 * queue_peek_advance matches the following queue_advance only when
 * nothing mutates in between, so a missed re-push is how T2 ends up
 * holding a path the queue no longer agrees with.
 *
 * Unlike engine_play this does NOT flush. That asymmetry is the design:
 * an explicit CMD_PLAY flushes (V.1 step 3), a natural transition must
 * not (V.3), and T3 never learns either happened.
 */
void engine_set_next(Engine * e, const char * path, int64_t track_id);
void engine_pause(Engine * e);
void engine_resume(Engine * e);
void engine_seek(Engine * e, int64_t ms);
void engine_stop(Engine * e);
void engine_quit(Engine * e);
void engine_set_gain(Engine * e, float gain);

/*
 * engine_park_sync: push CMD_PAUSE and block until T2 has actually
 * stopped writing to pcm_ring. T1 only. This is DESIGN.md IV.8.b step 1
 * (phase-8-device-loss.md I.5 step 1) and it exists because CMD_PAUSE on
 * its own is not enough.
 *
 * engine_pause only *queues*: T2 observes the command when it next
 * reaches a drain point, and until then it may be blocked inside
 * AudioSource.read decoding, or partway through the ring_write loop of a
 * chunk it has already decoded. Steps 2-4 of the ladder do not wait for
 * it, so step 4's flush -- a ring_reset on T1 -- can land while T2 is
 * still writing: two threads mutating one SPSC ring's write index, which
 * ring.h says breaks silently. ma_device_uninit plus ma_device_init are
 * slow enough that T2 will usually have parked first, which makes the
 * unsynchronised version correct almost every time and therefore worse,
 * not better.
 *
 * Polling engine_state for ENG_PAUSED does not fix it either: park
 * stores the state memory_order_relaxed and engine_state loads relaxed,
 * so observing the new state carries no happens-before edge against T2's
 * ring writes. The handshake is positive instead of a poll -- T2 sets a
 * parked flag under cmd_mtx and broadcasts, T1 waits on cmd_cond -- and
 * it is the mutex that supplies the release/acquire edge. On return, T1
 * is guaranteed to see every ring write T2 completed, including the case
 * of a T2 blocked in a slow read that a state spin would have missed.
 *
 * Idempotent and safe when T2 is already parked or was never started;
 * the wait is unbounded by design, because a bounded one would reopen
 * exactly the race this closes. Takes the Engine, returns nothing.
 */
void engine_park_sync(Engine * e);

/*
 * engine_device_reflush: the device-reopen ladder's flush (DESIGN.md
 * IV.8.b step 4), and the third and last legal path to
 * pcm_flush_and_resync. T1 only, and only between engine_park_sync and
 * the CMD_RESUME of step 6.
 *
 * Rebinds pcm_ring (the reopen freed and reallocated it, so a pointer
 * cached at engine_create is not to be trusted), retargets the decoder's
 * resampler to the newly negotiated rate (decoder_retarget_rate), then
 * unconditionally flushes and resyncs both frame counters to 0 and
 * publishes the flush snapshot. Unconditional rather than gated on the
 * rate having changed: one extra ring reset on an event that is already
 * gapped buys a path that runs every time instead of only on the rarest
 * and least-tested day.
 *
 * It calls pcm_flush_and_resync rather than ring_reset -- that function
 * is file-static and is the only legal caller of ring_reset(pcm_ring) in
 * the tree, which `make check-flush` enforces mechanically. The counter
 * resync is inside it and must never be repeated at the call site:
 * check-flush guards ring_reset calls, not counter stores, so a second
 * store here would not be caught.
 *
 * Emits nothing to trace_ring: trace_emit is T3-only and this runs on
 * T1. The counters go to the flush snapshot (amendment 03), which
 * pcm_flush_and_resync already publishes and engine_last_flush reads.
 *
 * Takes the Engine and the newly negotiated session format; returns 0 on
 * success, or -1 if T2 is not parked (the caller skipped step 1) or the
 * rings are not currently allocated -- in both cases nothing is touched.
 */
int engine_device_reflush(Engine * e, const SessionFormat * fmt);

/*
 * engine_poll_event: pop one Event if the queue is non-empty. T1 only.
 * Call in a loop after a pipe wakeup (or, headless, on a plain poll
 * interval) until it returns 0 -- draining the whole queue, not just
 * one entry, is the caller's responsibility. Takes the Engine and the
 * Event to fill, returns 1 if an event was popped, 0 if the queue was
 * empty.
 */
int engine_poll_event(Engine * e, Event * out);

/*
 * engine_last_flush: the flush snapshot (DESIGN.md IV.10, amendment
 * 03). Reads the counters recorded by the most recent
 * pcm_flush_and_resync -- the record that used to be TRACE_FLUSH,
 * moved out of trace_ring because that ring is single-producer T3-only
 * while the flush runs on T2 (IV.7) and on T1 (IV.8.b step 4).
 *
 * Process-wide, not per-Engine: there is one flush path and one
 * renderer, and phase 8's ladder writes it from T1 while no Engine
 * pointer is necessarily in scope. Either out-parameter may be NULL.
 *
 * T1 only, on the tick that drains trace_ring. Returns the generation
 * counter -- 0 if no flush has happened yet, otherwise the number of
 * flushes since process start, which is what makes "did the ladder
 * actually flush?" answerable.
 */
uint64_t engine_last_flush(uint64_t * write_frames, uint64_t * frames_consumed);

/*
 * engine_state: T2's current EngineState, relaxed-loaded. T1 may poll
 * this (e.g. phase 8's device-loss watchdog checks "engine state is
 * PLAYING"). Takes the Engine, returns its state.
 */
EngineState engine_state(const Engine * e);

/*
 * engine_boundary_ring: the boundary_ring Ring* T2 writes and T1 reads
 * (DESIGN.md IV.1, V.3). Returned so a later phase (12/17) can call
 * ring_read on it directly, the same pattern used for pcm_ring and
 * viz_ring elsewhere -- engine.c does not wrap it in a peek/pop
 * function of its own. Takes the Engine, returns the pointer; always
 * valid once engine_create has succeeded.
 */
Ring * engine_boundary_ring(Engine * e);

#endif // KURONO_AUDIO_ENGINE_H
