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
 * track still open). ENG_DEVICE_LOST is reserved for phase 8's
 * reopen-ladder policy, which lives on T1 (DESIGN.md IV.8) -- engine.c
 * never sets it itself; it is part of this enum only because
 * plan.md's phase-7-engine.md I.1 gives all four values together and
 * phase 8 needs the same type.
 */
typedef enum { ENG_STOPPED, ENG_PLAYING, ENG_PAUSED, ENG_DEVICE_LOST } EngineState;

/*
 * CommandKind / Command: the T1 -> T2 channel (DESIGN.md IV.6). path
 * is an inline array inside the command slot, not a pointer -- no
 * allocation, no ownership transfer, no lifetime question across the
 * thread boundary (DESIGN.md V.1 step 2).
 */
typedef enum {
  CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_SEEK, CMD_STOP, CMD_QUIT, CMD_SET_GAIN
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
  StreamInfo info;     /* valid only when kind == EV_STREAM_INFO */
  int        averror;  /* valid only for EV_OPEN_FAILED / EV_DECODE_ERROR */
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
 * engine_create: allocate an Engine bound to fmt (passed straight to
 * decoder_create, DESIGN.md II.2) and sink (used only for
 * sink_is_started -- engine.c never calls sink.open/start/stop/close;
 * those stay on T1, DESIGN.md IV.9). Also allocates boundary_ring and
 * binds pcm_ring via sink_pcm_ring(), so sink.open must already have
 * succeeded before this is called. Takes the negotiated session format
 * and the sink handle, returns a heap-allocated Engine or NULL on
 * allocation failure (nothing is left partially allocated on NULL).
 */
Engine * engine_create(const SessionFormat * fmt, AudioSink * sink);

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
void engine_pause(Engine * e);
void engine_resume(Engine * e);
void engine_seek(Engine * e, int64_t ms);
void engine_stop(Engine * e);
void engine_quit(Engine * e);
void engine_set_gain(Engine * e, float gain);

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
