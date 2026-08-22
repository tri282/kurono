/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_AUDIO_SINK_H
#define KURONO_AUDIO_SINK_H

#include <stdatomic.h>

#include "format.h"
#include "ring.h"

/*
 * AudioSink: anything that consumes PCM in the session format
 * (DESIGN.md VI). audio/sink.c is the only implementation and the only
 * file permitted to use miniaudio (CLAUDE.md; DESIGN.md III.2) -- no
 * miniaudio type appears in this header. AudioSink.open takes no data
 * callback: sink.c owns the entire IV.2 callback body itself (plan.md
 * VII gap 3, closed 2026-08-07 -- SinkDataCallback does not exist).
 * AudioSink.stop is contractually synchronous: the callback is provably
 * not running once it returns (DESIGN.md VI, IV.9.b step 2).
 */
typedef struct {
  int  (* open)(void * self, SessionFormat * inout_fmt,
                SinkNotifyCallback on_notify, void * user);
  int  (* start)(void * self);
  int  (* stop)(void * self);          /* synchronous: callback not running on return */
  void (* close)(void * self);
  void * self;
} AudioSink;

/*
 * sink_get: fill *out with the vtable bound to the process's one
 * miniaudio-backed AudioSink. There is exactly one device per process
 * (DESIGN.md VI), so every field this implementation needs is a
 * process-wide static in sink.c and self is unused (NULL) -- kept in
 * the struct only for symmetry with AudioSource, whose interface this
 * mirrors. Takes the AudioSink to fill, returns nothing.
 */
void sink_get(AudioSink * out);

/*
 * sink_is_started: true iff the device is in ma_device_state_started.
 * A pure atomic load of ma_device.state and nothing else -- no
 * logging, no lazy reopen, no touching the device-event slot, no
 * caching. const-qualified so the no-mutation contract is in the type.
 * Safe to call from any thread; the intended callers are T1 (the
 * device-loss watchdog, phase 8) and T2 (the seek fence's fast path,
 * phase 7) -- never T3 (DESIGN.md IV.7.5, IV.8.a; plan.md VII gap 2,
 * closed 2026-08-08). Takes the AudioSink, returns 1 if started, 0
 * otherwise (including "never opened").
 */
int sink_is_started(const AudioSink * s);

/*
 * sink_underrun_count: how many times the data callback has found
 * pcm_ring short and written silence, since the device was opened.
 *
 * DESIGN.md XI.6 makes this a user-visible number rather than a
 * debugging aid: "a visible underrun counter in the status bar is
 * worth more than any amount of reasoning about whether underruns are
 * happening". T3 increments it memory_order_relaxed inside the
 * callback; this is the matching relaxed load, and relaxed is right on
 * both sides -- the value is for display, nothing orders against it,
 * and a reader one repaint behind is not wrong in any way a user could
 * notice.
 *
 * Note the floor amendment 05 gave it: PulseAudio's ma_device_start
 * pre-fills by invoking the callback before uncorking, and pcm_ring
 * may still be empty then, so expect at most one per play transition.
 * Read it as "underruns beyond one per play". Takes nothing.
 */
unsigned sink_underrun_count(void);

/*
 * sink_cb_alive: 1 iff the data callback is currently executing, from
 * the g_cb_alive flag phase 5 added as the residual-risk-2 mitigation
 * (phase-5-sink.md I.3). Phase 8's teardown reads it immediately after
 * AudioSink.stop returns: DESIGN.md IV.9.b step 2 asserts stop is
 * synchronous and the callback is provably finished on return, and this
 * is the one cheap way to keep observing that rather than trusting it.
 * Reported, never assert(3)ed -- assert compiles out under -DNDEBUG,
 * which is precisely the release build where a broken assumption here
 * would matter most. Takes nothing, returns 1 or 0.
 */
int sink_cb_alive(void);

/*
 * sink_dispatch_notifications: deliver the most recent device
 * notification, if any, to the SinkNotifyCallback registered with
 * AudioSink.open. T1 ONLY, called from the EVSRC_PIPE branch of the
 * event loop once the doorbell byte the notification callback wrote has
 * been drained (DESIGN.md IV.3, IV.8.a; format.h's SinkNotifyCallback).
 * All device policy runs from here, on T1, and never on T3.
 *
 * The pending slot holds one notification, not a queue: two arriving
 * back to back between drains collapse into the later one. That is
 * deliberate and safe, because the notification is explicitly not the
 * detector (DESIGN.md IV.8.a) -- the authoritative signal is T1's
 * sink_is_started poll on the redraw tick, and this path only supplies
 * an early wakeup and the status-bar text. Takes nothing, returns 1 if a
 * notification was dispatched, 0 if none was pending.
 */
int sink_dispatch_notifications(void);

/*
 * sink_alloc_default_rings: allocate pcm_ring and viz_ring at the
 * default session format of f32 / 48000 / 2 with no device open, and
 * report that format in *out_fmt. This is DESIGN.md XI.2's browse-only
 * requirement in one call: "T2 is still created but parked, and the
 * rings are still allocated using a default session format", so every
 * ring pointer stays non-NULL and every code path stays uniform whether
 * or not a device was ever obtained. Reached from two places, both on
 * T1: a device that failed to open at startup (DESIGN.md IV.9.a step 5),
 * and a reopen ladder that gave up after 60 s (IV.8.b step 3).
 *
 * Does not touch the device and does not make sink_is_started true --
 * browse-only means there is no device, not a stopped one. Takes the
 * SessionFormat to fill, returns 0 on success or -1 if either ring could
 * not be allocated (neither is left allocated on -1).
 */
int sink_alloc_default_rings(SessionFormat * out_fmt);

/*
 * sink_free_rings: free pcm_ring and viz_ring -- DESIGN.md IV.9.b
 * step 6, deliberately separate from AudioSink.close (step 3). The
 * split exists because pcm_ring has two touchers: T3, which is gone
 * once close's ma_device_uninit returns, and T2, which is not. Freeing
 * inside close would be a use-after-free in T2's ring_write for as
 * long as T2 outlives the device -- the whole reason IV.9.b puts step
 * 6 after step 5's join rather than bundling it with the uninit.
 * Callers must therefore run stop -> close -> CMD_QUIT -> join -> this,
 * in that order. Idempotent (ring_free tolerates an already-freed
 * Ring), so a failure path that has not yet started T2 may call it
 * straight after close. Takes nothing, returns nothing.
 */
void sink_free_rings(void);

/*
 * ---------------------------------------------------------------------
 * T2 bindings -- added in phase 7 (audio/engine.c), flagged in
 * plans/kurono/05-impl/phase-7.md as a scope deviation from that
 * phase's stated file list. DESIGN.md III.2 says "audio/engine.c is
 * the only module permitted to hold both a ring pointer and a sink
 * pointer" and IV.7 has T2 read/write g_flushing, g_cb_seq, and
 * g_frames_consumed directly -- none of which phase 5 exposed, because
 * nothing needed them yet. These five accessors are pure address-of /
 * store wrappers around statics this file already owns; none of them
 * add a line to kurono_data_callback or kurono_notification_callback,
 * and none change what those two functions do. Every pointer returned
 * is process-wide static storage, valid for the address-of operation
 * at any time; the *values* behind pcm_ring's pointer are only
 * meaningful between a successful AudioSink.open and the matching
 * close, which is why sink_pcm_ring alone is gated on g_device_open --
 * the others are plain atomics with a well-defined value (0/false) even
 * before open.
 * ---------------------------------------------------------------------
 */

/*
 * sink_pcm_ring: the pcm_ring Ring* sink_open_impl allocated, for
 * audio/engine.c (T2) to write into -- the producer side of the same
 * ring T3's data callback reads via ring_read (DESIGN.md IV.1: T2 owns
 * pcm_ring's write cursor). Takes nothing, returns the pointer, or
 * NULL if the rings are not currently allocated.
 *
 * Gated on the rings existing, not on the device being open: in
 * browse-only state (DESIGN.md XI.2) there is no device and the rings
 * are allocated anyway, by sink_alloc_default_rings, precisely so this
 * never returns NULL to a caller that would then need a second code
 * path. Callers that cache the returned pointer across a device reopen
 * (IV.8.b) must re-fetch it afterwards -- engine_device_reflush does.
 * The only legal caller of
 * ring_reset on the pointer this returns is pcm_flush_and_resync
 * (audio/engine.c) -- DESIGN.md IV.4 rule 5, enforced by
 * `make check-flush`.
 */
Ring * sink_pcm_ring(void);

/*
 * sink_flush_flag: pointer to g_flushing (DESIGN.md IV.7.1-.3). T3's
 * data callback acquire-loads it every invocation, unmodified by this
 * accessor's addition. T2 (audio/engine.c) is the only external writer
 * -- release-store true in engine_flush_begin, release-store false in
 * engine_flush_end -- and no other file may touch it. Takes nothing,
 * returns the pointer; always valid (static storage).
 */
atomic_bool * sink_flush_flag(void);

/*
 * sink_cb_seq: pointer to g_cb_seq, the callback-invocation counter
 * the seek fence polls (DESIGN.md IV.7.3-.4). T3 release-increments it
 * at the end of every invocation, including the flushing path,
 * unmodified by this accessor. Every external caller (T2 only) reads
 * it acquire and must never write it. Takes nothing, returns the
 * pointer; always valid (static storage).
 */
const atomic_uint * sink_cb_seq(void);

/*
 * sink_frames_consumed: pointer to g_frames_consumed. T3 release-adds
 * to it every invocation outside the flush window (DESIGN.md IV.2 step
 * 6, unchanged by this accessor). T2 is the only external writer, and
 * only from inside pcm_flush_and_resync, itself only reachable inside
 * a flush window (DESIGN.md IV.7.6) -- T1 may read it (polled ~30 Hz,
 * DESIGN.md IV.6) but must never write it. Takes nothing, returns the
 * pointer; always valid (static storage).
 */
atomic_uint_least64_t * sink_frames_consumed(void);

/*
 * sink_set_gain: encode gain as the IEEE-754 bit pattern
 * apply_gain already expects and relaxed-store it into g_gain_bits
 * (DESIGN.md IV.2's "one representation, no lock-freedom check, no
 * fallback branch"). The only intended caller is audio/engine.c's
 * CMD_SET_GAIN handler (DESIGN.md IV.6: set_gain travels over the
 * Command channel like every other transport action, not directly
 * from T1 to this atomic). Safe to call before AudioSink.open --
 * sink_open_impl overwrites it with unity gain at open time regardless.
 * Takes the new gain, returns nothing.
 */
void sink_set_gain(float gain);

/*
 * sink_set_producer_active: record whether T2 is currently filling
 * pcm_ring, so the data callback can gate TRACE_RING_LOW on real
 * playback headroom instead of the terminal drain-to-empty every run
 * ends with once T2 parks (plans/kurono/04-plan/phase-7-engine.md
 * carry-in item; see phase-7.md for the "gate vs. end-of-stream
 * marker" decision). audio/engine.c calls this exactly on every
 * producing/parked transition: true just before the fill loop starts
 * (CMD_PLAY success, CMD_RESUME), false on every park (CMD_PAUSE,
 * CMD_STOP, EOF, a decode error, CMD_QUIT). Takes 1 (producing) or 0
 * (parked), returns nothing.
 */
void sink_set_producer_active(int active);

/*
 * sink_set_event_pipe: cache the self-pipe's write fd for the
 * notification callback to write a wakeup byte to (DESIGN.md IV.3,
 * IV.9.a step 5). event.c owns both ends of the pipe and is the only
 * thing that ever closes it; sink.c only caches this fd and must never
 * close it (DESIGN.md IV.9.b step 9's warning). Call before
 * AudioSink.open -- ma_device_init itself can raise a notification.
 * The default, -1, means "no wakeup pipe": the notification callback's
 * write is skipped rather than attempted on an invalid fd, which is
 * what lets --tone run the device standalone with no event loop.
 * Takes the fd, returns nothing.
 */
void sink_set_event_pipe(int fd);

/* sink_run_tone is gone (phase 18, plan.md VII gap 1). It was --tone's
   implementation and DESIGN.md VI's SineSource in the only form the
   code then permitted: a tone generator living inside sink.c, writing
   pcm_ring from T1 with the engine bypassed entirely. The real
   SineSource is now audio/testsrc.c's TESTSRC_SINE -- an AudioSource,
   injectable through engine_create, driving the same T2 path a real
   file does. That is what §VI always described; this was the
   stand-in. */

#endif // KURONO_AUDIO_SINK_H
