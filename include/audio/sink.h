#ifndef KURONO_AUDIO_SINK_H
#define KURONO_AUDIO_SINK_H

#include "format.h"

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

/*
 * sink_run_tone: implements --tone (plan.md VI.8) -- the SineSource of
 * DESIGN.md VI. Opens the sink, fills pcm_ring with a 441 Hz tone from
 * this thread (mirroring what audio/decoder.c's T2 will do once it
 * exists), starts the device, plays for a few seconds, stops, and
 * closes. Exercises the ring, the callback, gain, and the device with
 * no ffmpeg and no audio file. Prints the negotiated session format and
 * the underrun count. Takes nothing, returns 0 on success, -1 on
 * device-open or device-start failure.
 */
int sink_run_tone(void);

#endif // KURONO_AUDIO_SINK_H
