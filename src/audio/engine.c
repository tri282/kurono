/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * audio/engine.c -- T2, the decoder thread: the command/event channels
 * (DESIGN.md IV.6), the producer side of pcm_ring, and the seek/play
 * flush window (DESIGN.md IV.7).
 *
 * Confinement (CLAUDE.md; DESIGN.md III.2): this file reaches the
 * decoder only through AudioSource (src.open/read/seek/close) and the
 * sink only through AudioSink plus the five narrow T2-binding
 * accessors phase 7 added to include/audio/sink.h -- sink_pcm_ring,
 * sink_flush_flag, sink_cb_seq, sink_frames_consumed, sink_set_gain,
 * sink_set_producer_active. No decoder library or audio-device-library
 * symbol appears here; the plan's confinement grep over src/ must
 * still name only the decoder implementation file as a hit.
 *
 * engine.c is the only module permitted to hold both a ring pointer
 * (pcm_ring, borrowed from sink.c; boundary_ring, owned here) and a
 * sink pointer (used only for sink_is_started's fence check --
 * sink.open/start/stop/close stay on T1, DESIGN.md IV.9).
 *
 * The TRACE_FLUSH contradiction phase 7 flagged here is settled by
 * amendment 03: the flush record is not a trace_ring record at all but
 * a single-slot snapshot (g_flush_stat below), because trace_ring is
 * single-producer T3-only while pcm_flush_and_resync runs on T2 and,
 * from phase 8, on T1. trace_emit is still never called from this file.
 *
 * See plans/kurono/05-impl/phase-7.md for the one thing this file
 * still deliberately does NOT do: EOF never takes the "no flush"
 * transition path of DESIGN.md V.3, because no CommandKind exists yet
 * for T1 to hand T2 a next track ahead of end-of-stream.
 */

#include "../../include/audio/engine.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ENGINE_CHANNELS 2u /* SessionFormat.channels is always 2, DESIGN.md II.2 */

#define CMD_QUEUE_CAP 16
#define EVT_QUEUE_CAP 16

/* DESIGN.md IV.5: 10 ms poll on the command condvar while pcm_ring is
   full, then re-check free space. */
#define BACKPRESSURE_TIMEOUT_MS 10

/* DESIGN.md IV.7.3/.4: poll g_cb_seq every 1 ms, give up and reset
   anyway after 200 ms so a wedged device cannot hang a seek. */
#define FLUSH_FENCE_POLL_MS 1
#define FLUSH_FENCE_CAP_MS  200

/* Frames per AudioSource.read() call in the fill loop. Arbitrary --
   mirrors TONE_CHUNK_FRAMES (audio/sink.c), large enough not to be
   dominated by call overhead, small enough to stay responsive to
   commands between chunks. */
#define READ_CHUNK_FRAMES 1024

/* boundary_ring: 8 slots of 16-byte BoundaryEntry records (DESIGN.md
   V.3, phase-7-engine.md I.7). ring_init rounds up to a power of two,
   so this is a floor, not an exact size. */
#define BOUNDARY_RING_SLOTS 8
#define BOUNDARY_RING_BYTES ((size_t) BOUNDARY_RING_SLOTS * sizeof(BoundaryEntry))

_Static_assert(sizeof(BoundaryEntry) == 16, "BoundaryEntry must stay 16 bytes");

/*
 * FlushStat: the flush record (DESIGN.md IV.10, amendment 03). One
 * slot, not a ring -- a flush is an event per minute, so nothing ever
 * wants the second-most-recent one, and trace_ring's wait-free lossy
 * design exists for the audio callback's constraints, which neither of
 * this snapshot's writers has.
 *
 * Written by whichever thread is inside a flush window: T2 for the seek
 * path (DESIGN.md IV.7), T1 for phase 8's reopen ladder (IV.8.b step
 * 4). Two writers is safe here in a way multi-producer trace_emit would
 * not have been -- every field is an independent atomic, so there is no
 * slot claiming, no CAS loop, and no unbounded retry anywhere. Read by
 * T1 on the tick that drains trace_ring.
 *
 * gen is stored last with release and read first and last with acquire:
 * a seqlock over 24 bytes. A reader that sees gen change between its
 * two loads retries.
 */
typedef struct {
  atomic_uint_least64_t gen;             /* 0 until the first flush */
  atomic_uint_least64_t write_frames;
  atomic_uint_least64_t frames_consumed;
} FlushStat;

static FlushStat g_flush_stat;

/*
 * Engine: T2's exclusive state (DESIGN.md IV.1) -- the AudioSource,
 * boundary_ring's write cursor, and the monotonic write-frame counter
 * -- plus the two cross-thread channels and the borrowed pcm_ring/
 * AudioSink pointers. Opaque outside this file.
 */
struct Engine {
  AudioSource src;
  AudioSink * sink;      /* borrowed; only sink_is_started is ever called on it */
  Ring      * pcm_ring;  /* borrowed from sink_pcm_ring(); NOT owned, NEVER freed here */
  Ring        boundary_ring;

  uint32_t sample_rate;  /* SessionFormat.sample_rate, cached at creation for CMD_SEEK's ms->frame math */
  uint64_t write_frames; /* T2's own monotonic write-frame counter, DESIGN.md IV.1 */
  uint64_t flush_gen;    /* bumped by every pcm_flush_and_resync; see the fill loop */
  int64_t  cur_track_id;
  int      has_track;    /* 1 iff src currently has a file open */

  /* Amendment 09's prefetch slot: what T1 last said follows the current
     track. T2 state, touched only on T2 (in handle_command and at EOF),
     so it needs no synchronisation of its own -- it arrives through the
     mutex-protected command queue like every other command payload. An
     empty next_path means "nothing follows", which is the end-of-queue
     path and behaves exactly as it did before this existed. */
  char     next_path[PATH_MAX];
  int64_t  next_track_id;

  atomic_int state;      /* EngineState, T1-pollable via engine_state */

  pthread_mutex_t cmd_mtx;
  pthread_cond_t  cmd_cond;
  Command cmd_q[CMD_QUEUE_CAP];
  int cmd_head, cmd_tail, cmd_count;

  /* T2's parked state, guarded by cmd_mtx. Set by T2 in park() once it
     has stopped writing to pcm_ring, cleared by T2 in unpark() before it
     resumes; read by engine_device_reflush as a precondition. Not the
     handshake: T2 keeps it set while servicing a play or seek popped from
     its idle wait, which is why engine_park_sync waits on pause_ack
     instead (amendment 11). Under the command mutex rather than beside
     `state` for the release/acquire edge; `state` is relaxed on both
     sides. Starts at 1: T2 begins parked with no track (DESIGN.md IV.9.a
     step 7). */
  int parked;

  /* Amendment 11: engine_park_sync's handshake. park_ticket is T1's
     counter (T1 only); pause_ack is the last ticket T2 has handled,
     written by T2 and waited on by T1, both under cmd_mtx. */
  uint64_t park_ticket;
  uint64_t pause_ack;

  /* Steady-state headroom (#8). T2-only and unsynchronised: read by
     engine_lead_stats after engine_join, the engine_frames_account rule.
     Armed when a write finds pcm_ring full; disarmed by park and by
     engine_flush_begin, so start, seek and play refills are never
     counted. lead_low is UINT64_MAX until the first armed sample. */
  int      lead_armed;
  unsigned lead_u0;        /* sink_underrun_count() at arm */
  uint64_t lead_low;       /* min ring_fill after a read while armed, frames */
  uint64_t lead_frames;    /* frames written while armed */
  unsigned lead_underruns; /* underruns inside armed segments */

  pthread_mutex_t evt_mtx;
  Event evt_q[EVT_QUEUE_CAP];
  int evt_head, evt_tail, evt_count;

  int event_pipe_w; /* cached, never owned -- see engine_set_event_pipe */

  pthread_t thread;
  int       thread_started;
};

/* ---------------------------------------------------------------------
 * Command channel: T1 -> T2. Fixed-size array under a mutex + condvar
 * (DESIGN.md IV.6). cmd_push blocks if the queue is momentarily full
 * rather than dropping -- at CMD_QUEUE_CAP 16 against one command per
 * user action, this should never actually happen; blocking keeps a
 * CMD_QUIT from ever being the one command that gets silently dropped.
 * ------------------------------------------------------------------- */

/* cmd_push: append cmd to the queue, blocking (on cmd_cond, not
   spinning) if it is momentarily full. T1 only. Takes the Engine and
   the command to copy in, returns nothing. */
static void
cmd_push(Engine * e, const Command * cmd)
{
  pthread_mutex_lock(&e->cmd_mtx);
  while (e->cmd_count == CMD_QUEUE_CAP) {
    pthread_cond_wait(&e->cmd_cond, &e->cmd_mtx);
  }
  e->cmd_q[e->cmd_tail] = *cmd;
  e->cmd_tail = (e->cmd_tail + 1) % CMD_QUEUE_CAP;
  e->cmd_count++;
  pthread_cond_broadcast(&e->cmd_cond);
  pthread_mutex_unlock(&e->cmd_mtx);
} /* cmd_push */

/* cmd_try_pop: pop one command without blocking. T2 only, used to
   drain pending commands between fill-loop chunks. Takes the Engine
   and the command to fill, returns 1 if one was popped, 0 if the
   queue was empty. */
static int
cmd_try_pop(Engine * e, Command * out)
{
  int got = 0;
  pthread_mutex_lock(&e->cmd_mtx);
  if (e->cmd_count > 0) {
    *out = e->cmd_q[e->cmd_head];
    e->cmd_head = (e->cmd_head + 1) % CMD_QUEUE_CAP;
    e->cmd_count--;
    pthread_cond_broadcast(&e->cmd_cond); /* wake any pusher blocked on a full queue */
    got = 1;
  }
  pthread_mutex_unlock(&e->cmd_mtx);
  return got;
} /* cmd_try_pop */

/* cmd_pop_wait: pop one command, waiting up to timeout_ms if the queue
   is empty (or indefinitely if timeout_ms < 0). T2 only. Used both for
   idle parking (timeout_ms == -1) and for the DESIGN.md IV.5
   backpressure poll (timeout_ms == BACKPRESSURE_TIMEOUT_MS) -- the
   caller is responsible for re-checking whatever condition it was
   waiting on (free ring space, in the backpressure case) after a
   timeout with no command, exactly as DESIGN.md IV.5 specifies. A
   spurious wakeup falls through to "still empty" and correctly reports
   no command either way. Takes the Engine, the command to fill, and
   the timeout; returns 1 if a command was popped, 0 otherwise. */
static int
cmd_pop_wait(Engine * e, Command * out, int timeout_ms)
{
  pthread_mutex_lock(&e->cmd_mtx);

  if (e->cmd_count == 0) {
    if (timeout_ms < 0) {
      while (e->cmd_count == 0) {
        pthread_cond_wait(&e->cmd_cond, &e->cmd_mtx);
      }
    } else {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec  += timeout_ms / 1000;
      ts.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec  += 1;
      }
      pthread_cond_timedwait(&e->cmd_cond, &e->cmd_mtx, &ts);
    }
  }

  int got = 0;
  if (e->cmd_count > 0) {
    *out = e->cmd_q[e->cmd_head];
    e->cmd_head = (e->cmd_head + 1) % CMD_QUEUE_CAP;
    e->cmd_count--;
    pthread_cond_broadcast(&e->cmd_cond);
    got = 1;
  }

  pthread_mutex_unlock(&e->cmd_mtx);
  return got;
} /* cmd_pop_wait */

/* ---------------------------------------------------------------------
 * Event channel: T2 -> T1. This queue has exactly one writer (T2) and
 * one reader (T1) -- T3 never posts to it (DESIGN.md IV.6). Full is
 * dropped, not blocked, the same rule trace_emit uses for trace_ring
 * (amendment-02 decision 3): a status channel that can stall the
 * producer is worse than a dropped status update, and at one event per
 * track change or error this should never actually fill.
 * ------------------------------------------------------------------- */

/* evt_push: append ev to the queue and, if a wakeup pipe is
   registered, write one doorbell byte. T2 only. Takes the Engine and
   the event to copy in, returns nothing. */
static void
evt_push(Engine * e, const Event * ev)
{
  pthread_mutex_lock(&e->evt_mtx);
  if (e->evt_count < EVT_QUEUE_CAP) {
    e->evt_q[e->evt_tail] = *ev;
    e->evt_tail = (e->evt_tail + 1) % EVT_QUEUE_CAP;
    e->evt_count++;
  }
  pthread_mutex_unlock(&e->evt_mtx);

  if (e->event_pipe_w >= 0) {
    (void) write(e->event_pipe_w, "\0", 1); /* O_NONBLOCK; EAGAIN ignored, mirrors DESIGN.md IV.3 */
  }
} /* evt_push */

/* engine_poll_event: see include/audio/engine.h. */
int
engine_poll_event(Engine * e, Event * out)
{
  int got = 0;
  pthread_mutex_lock(&e->evt_mtx);
  if (e->evt_count > 0) {
    *out = e->evt_q[e->evt_head];
    e->evt_head = (e->evt_head + 1) % EVT_QUEUE_CAP;
    e->evt_count--;
    got = 1;
  }
  pthread_mutex_unlock(&e->evt_mtx);
  return got;
} /* engine_poll_event */

/* ---------------------------------------------------------------------
 * The flush window (DESIGN.md IV.7). One routine pair, used by both
 * CMD_SEEK and CMD_PLAY -- an explicit play is a seek to 0 of a
 * different file (DESIGN.md V.1 step 3).
 * ------------------------------------------------------------------- */

/* lead_disarm: close the current steady-state segment, if one is open,
   folding its underruns into lead_underruns. T2 only. Takes the Engine,
   returns nothing. */
static void
lead_disarm(Engine * e)
{
  if (e->lead_armed) {
    e->lead_underruns += sink_underrun_count() - e->lead_u0;
    e->lead_armed = 0;
  }
} /* lead_disarm */

/* engine_flush_begin: take ownership of pcm_ring's contents for T2 by
   raising g_flushing and fencing against any in-flight callback
   invocation. Called only from T2, only from do_play and do_seek. See
   DESIGN.md IV.7. Takes the Engine, returns nothing. */
static void
engine_flush_begin(Engine * e)
{
  lead_disarm(e);

  atomic_bool * flushing = sink_flush_flag();
  const atomic_uint * cb_seq = sink_cb_seq();

  unsigned seq0 = atomic_load_explicit(cb_seq, memory_order_acquire);
  atomic_store_explicit(flushing, true, memory_order_release);

  /* IV.7.5 fence skip: if the device is not started, the callback
     provably is not running and cannot start without ma_device_start
     (T1 only) -- reset immediately rather than burning the 200 ms cap. */
  if (sink_is_started(e->sink)) {
    for (int i = 0; i < FLUSH_FENCE_CAP_MS; i++) {
      unsigned s = atomic_load_explicit(cb_seq, memory_order_acquire);
      if (s - seq0 >= 2u) { /* IV.7.3: TWO, not one */
        break;
      }
      struct timespec ts;
      ts.tv_sec  = 0;
      ts.tv_nsec = (long) FLUSH_FENCE_POLL_MS * 1000000L;
      nanosleep(&ts, NULL);
    }
  }
} /* engine_flush_begin */

/*
 * pcm_flush_and_resync: the ONLY legal caller of ring_reset on
 * pcm_ring in the tree (DESIGN.md IV.8.b's generalising rule; enforced
 * by `make check-flush`). Resets pcm_ring and boundary_ring and sets both
 * T2's monotonic write counter and g_frames_consumed to at_frame.
 * Reached from exactly three places, which DESIGN.md IV.8.b calls the
 * complete and final list: do_play and do_seek below (IV.7, V.1 step 3),
 * both on T2, and engine_device_reflush (IV.8.b step 4) on T1. Takes the
 * Engine and the frame index to resync to, returns nothing.
 *
 * Called from T2 inside a flush window, or from T1 inside the reopen
 * ladder with T2 parked by engine_park_sync and T3 joined by
 * ma_device_uninit -- in both cases exactly one thread can touch
 * pcm_ring's write index at the moment of the reset, which is the
 * property the SPSC contract actually requires.
 *
 * Publishes the flush snapshot rather than emitting TRACE_FLUSH
 * (amendment 03). DESIGN.md IV.7/IV.8.b used to say this function
 * "emits TRACE_FLUSH carrying both counters," which contradicted
 * trace_emit's own contract: trace_ring is single-producer T3-only,
 * and this function runs on T2 here and on T1 in phase 8's reopen
 * ladder. Calling trace_emit from either would migrate that ring's
 * producer across three threads -- the exact silent-breakage case
 * ring.h's SPSC contract warns about -- for one diagnostic line. The
 * counters still get recorded; they go to g_flush_stat instead.
 */
static void
pcm_flush_and_resync(Engine * e, uint64_t at_frame)
{
  ring_reset(e->pcm_ring);
  ring_reset(&e->boundary_ring);
  e->write_frames = at_frame;
  atomic_store_explicit(sink_frames_consumed(), at_frame, memory_order_release);

  /* Every reset invalidates any chunk the fill loop is midway through
     writing: those frames were decoded at the old position. The loop
     samples this counter before it starts and abandons the chunk if it
     changed. T2-only, so a plain increment is enough. */
  e->flush_gen++;

  /* Publish the snapshot (DESIGN.md IV.10, amendment 03). Fields
     relaxed, gen released last -- a reader that sees the new gen is
     guaranteed to see both fields that go with it. */
  atomic_store_explicit(&g_flush_stat.write_frames, e->write_frames,
                        memory_order_relaxed);
  atomic_store_explicit(&g_flush_stat.frames_consumed, at_frame,
                        memory_order_relaxed);
  atomic_fetch_add_explicit(&g_flush_stat.gen, 1, memory_order_release);
} /* pcm_flush_and_resync */

/* engine_last_flush: see include/audio/engine.h. */
uint64_t
engine_last_flush(uint64_t * write_frames, uint64_t * frames_consumed)
{
  /* Seqlock read: gen, payload, gen again. Bounded by construction --
     a retry only happens if a flush landed between the two loads, and
     a flush is a per-seek event, not a per-callback one. */
  for (;;) {
    uint64_t g0 = atomic_load_explicit(&g_flush_stat.gen, memory_order_acquire);
    uint64_t w  = atomic_load_explicit(&g_flush_stat.write_frames,
                                       memory_order_relaxed);
    uint64_t c  = atomic_load_explicit(&g_flush_stat.frames_consumed,
                                       memory_order_relaxed);
    uint64_t g1 = atomic_load_explicit(&g_flush_stat.gen, memory_order_acquire);

    if (g0 == g1) {
      if (write_frames != NULL) {
        *write_frames = w;
      }
      if (frames_consumed != NULL) {
        *frames_consumed = c;
      }
      return g0;
    }
  }
} /* engine_last_flush */

/* engine_flush_end: release the flush window (DESIGN.md IV.7.6, .8).
   Takes the Engine, returns nothing. */
static void
engine_flush_end(Engine * e)
{
  (void) e;
  atomic_store_explicit(sink_flush_flag(), false, memory_order_release);
} /* engine_flush_end */

/* engine_device_reflush: see include/audio/engine.h. */
int
engine_device_reflush(Engine * e, const SessionFormat * fmt)
{
  if (e == NULL || fmt == NULL) {
    return -1;
  }

  /* Step 1's handshake is a precondition, not a suggestion: without it
     the ring_reset below races T2's write index. Checked rather than
     assumed, because the failure it guards against is silent -- a caller
     that skipped engine_park_sync gets -1 and a reported error, not a
     ring that is quietly corrupt for the rest of the process. */
  pthread_mutex_lock(&e->cmd_mtx);
  int parked = e->parked;
  pthread_mutex_unlock(&e->cmd_mtx);
  if (!parked) {
    return -1;
  }

  /* The reopen freed and reallocated the rings at the newly negotiated
     rate, so the pointer cached at engine_create is refetched rather
     than trusted. */
  Ring * pcm = sink_pcm_ring();
  if (pcm == NULL) {
    return -1;
  }
  e->pcm_ring    = pcm;
  e->sample_rate = fmt->sample_rate;

  /* DESIGN.md II.2's warning permits the resampler's target to change on
     this path and on no other, because T3 provably does not exist here
     (step 2's ma_device_uninit joined it) and T2 is parked. Reported but
     not fatal: a rebuild failure leaves a source that cannot decode this
     file, which the next read surfaces as EV_DECODE_ERROR through the
     normal per-file path (DESIGN.md XI.1), and the flush below must
     still run either way or the counters stay apart. */
  /* Through the vtable, and NULL-tolerant: a source that does not
     resample has no target to move, and that is success rather than
     failure (decoder.h's note on the slot). The production decoder
     always provides it. */
  int ret = (e->src.retarget != NULL)
                ? e->src.retarget(e->src.self, fmt->sample_rate)
                : 0;

  /* The same two routines the seek window uses, not a second divergent
     implementation of the sequence (DESIGN.md IV.7.8). The fence inside
     engine_flush_begin costs nothing here: sink_is_started is false with
     the device freshly initialised and not yet started, so IV.7.5's skip
     branch takes it. */
  engine_flush_begin(e);
  pcm_flush_and_resync(e, 0);
  engine_flush_end(e);

  return (ret < 0) ? -1 : 0;
} /* engine_device_reflush */

/* ---------------------------------------------------------------------
 * Command handlers.
 * ------------------------------------------------------------------- */

/* park: stop producing, record new_state, and complete the IV.8.b step 1
   handshake by raising `parked` under cmd_mtx and broadcasting. T2 only.

   Every call site is a point past which T2 performs no further
   ring_write on this chunk: the command handlers all return into a loop
   that re-reads `state` and falls through to the idle wait, and the
   backpressure wait's caller breaks out of the write loop the moment
   state leaves ENG_PLAYING. That is what makes the flag mean "T2 is not
   writing" and not merely "T2 intends to stop". The mutex is not held on
   entry -- cmd_pop_wait and cmd_try_pop both release it before handing a
   command to handle_command -- so taking it here cannot deadlock.

   Takes the Engine and the EngineState to record, returns nothing. */
static void
park(Engine * e, EngineState new_state)
{
  lead_disarm(e);
  sink_set_producer_active(0);
  atomic_store_explicit(&e->state, new_state, memory_order_relaxed);

  pthread_mutex_lock(&e->cmd_mtx);
  e->parked = 1;
  pthread_cond_broadcast(&e->cmd_cond);
  pthread_mutex_unlock(&e->cmd_mtx);
} /* park */

/* unpark: the exact inverse of park -- clear the handshake flag, mark
   the producer active, and enter ENG_PLAYING. T2 only, and always
   before the first ring_write of the resumed run rather than after, so
   there is no window in which T2 is writing while `parked` still reads
   1. Called from the two places T2 resumes producing: a successful
   CMD_PLAY and CMD_RESUME (phase-8-device-loss.md I.5 step 1, "clear
   parked wherever T2 resumes producing"). Takes the Engine, returns
   nothing. */
static void
unpark(Engine * e)
{
  pthread_mutex_lock(&e->cmd_mtx);
  e->parked = 0;
  pthread_mutex_unlock(&e->cmd_mtx);

  sink_set_producer_active(1);
  atomic_store_explicit(&e->state, ENG_PLAYING, memory_order_relaxed);
} /* unpark */

/* do_play: CMD_PLAY, inside a flush window (DESIGN.md V.1 step 3).
   Closes any previously open file, opens cmd->path via AudioSource,
   and resyncs to frame 0 regardless of outcome -- the flush window was
   already entered, so its reset must run either way. Posts
   EV_STREAM_INFO on success or EV_OPEN_FAILED on failure; queue
   advance policy and the five-consecutive-failures cutoff
   (DESIGN.md XI.1) belong to T1, not here (DESIGN.md V.1 step 5).
   Takes the Engine and the command, returns nothing. */
static void
do_play(Engine * e, const Command * cmd)
{
  engine_flush_begin(e);

  if (e->has_track) {
    e->src.close(e->src.self);
    e->has_track = 0;
  }

  /* An explicit play invalidates whatever was prefetched: it names what
     follows the track being replaced, not what follows this one. T1
     re-pushes on the EV_STREAM_INFO below, so the slot is only empty for
     the width of that round trip -- and empty is the safe value, since
     it degrades to the EV_EOF path rather than to a wrong track. */
  e->next_path[0]  = '\0';
  e->next_track_id = 0;

  StreamInfo info;
  memset(&info, 0, sizeof(info));
  int ret = e->src.open(e->src.self, cmd->path, &info);

  pcm_flush_and_resync(e, 0);
  engine_flush_end(e);

  Event ev;
  memset(&ev, 0, sizeof(ev));
  ev.track_id = cmd->track_id;

  if (ret == 0) {
    e->cur_track_id = cmd->track_id;
    e->has_track     = 1;
    unpark(e);

    ev.kind = EV_STREAM_INFO;
    ev.info = info;
  } else {
    park(e, ENG_STOPPED);
    ev.kind    = EV_OPEN_FAILED;
    ev.averror = ret;
  }
  evt_push(e, &ev);
} /* do_play */

/* do_seek: CMD_SEEK, inside a flush window (DESIGN.md IV.7). A no-op
   if no track is open. On a successful AudioSource.seek, resyncs to
   ms * R / 1000 (DESIGN.md IV.7.6); on failure, leaves pcm_ring and
   both frame counters untouched rather than resyncing to a position
   the decoder never reached -- not exercised by this phase's verify
   suite and flagged in phase-7.md as an underspecified failure path.
   No event is posted either way (DESIGN.md V.2: position is always
   read from g_frames_consumed). Takes the Engine and the command,
   returns nothing. */
static void
do_seek(Engine * e, const Command * cmd)
{
  if (!e->has_track) {
    return;
  }

  engine_flush_begin(e);

  int ret = e->src.seek(e->src.self, cmd->arg_ms);
  if (ret == 0) {
    uint64_t at_frame = (uint64_t) cmd->arg_ms * (uint64_t) e->sample_rate / 1000u;
    pcm_flush_and_resync(e, at_frame);
  }

  engine_flush_end(e);
} /* do_seek */

/* handle_command: dispatch one popped Command. T2 only. Takes the
   Engine, the command, and an out-parameter set to 1 on CMD_QUIT (left
   untouched otherwise); returns nothing. */
static void
handle_command(Engine * e, const Command * cmd, int * quit)
{
  switch (cmd->kind) {
    case CMD_PLAY:
      do_play(e, cmd);
      break;

    case CMD_SEEK:
      do_seek(e, cmd);
      break;

    case CMD_PAUSE:
      if (e->has_track) {
        park(e, ENG_PAUSED);
      }
      if (cmd->ticket != 0) {
        /* Acknowledged whether or not T2 was already parked: the ack says
           every earlier command has finished (amendment 11). */
        pthread_mutex_lock(&e->cmd_mtx);
        e->pause_ack = cmd->ticket;
        pthread_cond_broadcast(&e->cmd_cond);
        pthread_mutex_unlock(&e->cmd_mtx);
      }
      break;

    case CMD_RESUME:
      if (e->has_track
          && atomic_load_explicit(&e->state, memory_order_relaxed) == ENG_PAUSED) {
        unpark(e);
      }
      break;

    case CMD_STOP:
      /* DESIGN.md gives no explicit "a stop" data-flow section and
         CMD_STOP is not exercised by this phase's verify suite --
         implemented conservatively: close the file and park, without
         touching pcm_ring (only pcm_flush_and_resync's two call sites,
         both already accounted for, may do that). Flagged in
         phase-7.md. Buffered audio already in pcm_ring plays out to
         silence exactly as it does at end-of-queue (DESIGN.md V.4). */
      if (e->has_track) {
        e->src.close(e->src.self);
        e->has_track = 0;
      }
      park(e, ENG_STOPPED);
      break;

    case CMD_QUIT:
      if (e->has_track) {
        e->src.close(e->src.self);
        e->has_track = 0;
      }
      park(e, ENG_STOPPED);
      *quit = 1;
      break;

    case CMD_SET_GAIN:
      sink_set_gain(cmd->gain);
      break;

    case CMD_SET_NEXT:
      /* Stored and nothing else. Deliberately does not open the file,
         does not touch pcm_ring, and does not change EngineState: it is
         a note for later, consumed at EOF by advance_to_next below.
         Overwritable until then -- a second one replaces the first, and
         there is no queue of them because T1 only ever has one answer
         to "what is next". */
      snprintf(e->next_path, sizeof e->next_path, "%s", cmd->path);
      e->next_track_id = cmd->track_id;
      break;
  }
} /* handle_command */

/*
 * advance_to_next: the T2 half of a continuous track transition
 * (DESIGN.md V.3 steps 1-3, amendment 09). Takes the Engine; returns 1
 * when the next track is open and writing continues, 0 when there was
 * nothing to advance to or it could not be opened -- in which case the
 * caller runs the unchanged end-of-queue path.
 *
 * The three things this must NOT do are the reason it is its own
 * function. It does not stop, does not reset a ring, and does not touch
 * the sink: an explicit CMD_PLAY flushes and a natural transition must
 * not (DESIGN.md V.1 step 3 versus V.3, and the note between them). T3
 * is entirely unaware anything happened, which is the property that
 * makes the whole design safe.
 *
 * e->write_frames is deliberately not reset. It is the counter both the
 * boundary entry and EV_EOF.end_frame are keyed on, and the only thing
 * that may rewind it is pcm_flush_and_resync.
 */
static int
advance_to_next(Engine * e)
{
  if (e->next_path[0] == '\0') {
    return 0; /* end of queue: DESIGN.md V.4, unchanged */
  }

  /* Taken out of the slot before anything can fail, so a failed open
     cannot leave the same path to be retried forever. */
  char    path[PATH_MAX];
  int64_t id = e->next_track_id;
  snprintf(path, sizeof path, "%s", e->next_path);
  e->next_path[0]  = '\0';
  e->next_track_id = 0;

  /* Step 2 before step 3: the boundary names the frame at which the new
     track starts, which is the current write count -- sampled before a
     single frame of the new file has been written. Pushed before the
     open so the entry is not lost if the open is slow and T1 ticks
     meanwhile; a boundary for a track that then fails to open is
     harmless, because T1 pops it against a consumed count that the
     failed track will never advance past. */
  BoundaryEntry b;
  b.frame_index = e->write_frames;
  b.track_id    = id;
  /* A full ring drops the entry rather than blocking T2 (DESIGN.md V.3's
     warning): T1 resynchronises on the next tick. Eight slots at 30 Hz
     means overflow needs eight tracks to end inside 33 ms. */
  (void) ring_write(&e->boundary_ring, (const uint8_t *) &b, sizeof b);

  if (e->has_track) {
    e->src.close(e->src.self);
    e->has_track = 0;
  }

  StreamInfo info;
  memset(&info, 0, sizeof(info));
  if (e->src.open(e->src.self, path, &info) != 0) {
    /* Degrades to exactly the pre-amendment behaviour: the caller posts
       EV_EOF for the track that just finished and parks, and T1's drain
       plus its five-failures rule take it from there. Per-file failures
       are never fatal. */
    return 0;
  }

  e->cur_track_id = id;
  e->has_track    = 1;

  Event ev;
  memset(&ev, 0, sizeof(ev));
  ev.kind     = EV_STREAM_INFO;
  ev.track_id = id;
  ev.info     = info;
  evt_push(e, &ev);
  return 1;
} /* advance_to_next */

/* ---------------------------------------------------------------------
 * The thread body.
 * ------------------------------------------------------------------- */

/* engine_thread_main: T2's entry point. Parked (idle wait, no ring
   writes) while state != ENG_PLAYING; otherwise pulls one
   READ_CHUNK_FRAMES chunk at a time from src and writes it into
   pcm_ring, applying the DESIGN.md IV.5 backpressure wait when the
   ring is full and draining any pending commands between chunks
   without blocking. AudioSource.read's three-valued contract
   (DESIGN.md VI) is not folded together: n == frames is normal,
   0 <= n < frames is EOF (posts EV_EOF, parks -- DESIGN.md V.4), and
   n < 0 is a mid-stream decode failure (posts EV_DECODE_ERROR, parks)
   -- never the same branch. Takes the Engine (via void *), returns
   NULL. */
static void *
engine_thread_main(void * arg)
{
  Engine * e = (Engine *) arg;
  float chunk[READ_CHUNK_FRAMES * ENGINE_CHANNELS];

  int quit = 0;
  while (!quit) {
    EngineState st = (EngineState) atomic_load_explicit(&e->state, memory_order_relaxed);

    if (st != ENG_PLAYING) {
      /* Idle: nothing to produce, so block indefinitely for the next
         command. The IV.5 10 ms poll applies only to the ring-full
         backpressure wait below, which cannot fire here. */
      Command cmd;
      if (cmd_pop_wait(e, &cmd, -1)) {
        handle_command(e, &cmd, &quit);
      }
      continue;
    }

    /* Producing: drain any commands that arrived since the last chunk
       without blocking, then pull and write one chunk. */
    Command cmd;
    while (cmd_try_pop(e, &cmd)) {
      handle_command(e, &cmd, &quit);
      st = (EngineState) atomic_load_explicit(&e->state, memory_order_relaxed);
      if (quit || st != ENG_PLAYING) {
        break;
      }
    }
    if (quit || st != ENG_PLAYING) {
      continue;
    }

    long n = e->src.read(e->src.self, chunk, READ_CHUNK_FRAMES);

    /* Steady-state sample: during a refill gap only the consumer acts, so
       this is the gap's minimum to within one period. */
    if (e->lead_armed) {
      uint64_t occ = (uint64_t) (ring_fill(e->pcm_ring) / (ENGINE_CHANNELS * sizeof(float)));
      if (occ < e->lead_low) {
        e->lead_low = occ;
      }
    }

    if (n < 0) {
      Event ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind     = EV_DECODE_ERROR;
      ev.track_id = e->cur_track_id;
      ev.averror  = (int) n;
      evt_push(e, &ev);
      park(e, ENG_STOPPED);
      continue;
    }

    /* n == 0 needs no branch of its own: bytes is 0, the fill loop
       does not execute, and the short-read check after it posts EV_EOF
       -- one EOF site, not two. DESIGN.md V.4: EOF does not stop the
       device and does not touch the ring. No CommandKind exists yet
       for T1 to hand T2 a next track ahead of EOF, so the no-flush
       transition of V.3 is never reached in this phase -- see this
       file's header comment. */
    size_t bytes = (size_t) n * ENGINE_CHANNELS * sizeof(float);
    size_t off   = 0;

    /* Sampled before the first write, re-checked after every command
       handled below: a CMD_SEEK or CMD_PLAY serviced mid-chunk resets
       pcm_ring and rewinds both frame counters, which makes every byte
       still sitting in `chunk` pre-flush audio. Writing it after the
       reset would put a burst of the old position at the seek target
       and leave write_frames past the resync. Neither shows up in the
       state check on its own -- both commands leave the engine
       ENG_PLAYING -- and with a 2 s ring T2 is in the backpressure wait
       almost continuously, so this is the path a seek normally takes,
       not an edge case. */
    uint64_t gen0 = e->flush_gen;
    int      flushed = 0;

    while (off < bytes) {
      size_t w = ring_write(e->pcm_ring, (const uint8_t *) chunk + off, bytes - off);
      off += w;
      e->write_frames += w / (ENGINE_CHANNELS * sizeof(float));
      if (e->lead_armed) {
        e->lead_frames += w / (ENGINE_CHANNELS * sizeof(float));
      }

      if (off < bytes) {
        if (!e->lead_armed) { /* pcm_ring is full: steady state from here */
          e->lead_armed = 1;
          e->lead_u0    = sink_underrun_count();
        }
        /* pcm_ring is full. DESIGN.md IV.5: wait up to 10 ms on the
           command condvar, then re-check free space by looping back to
           ring_write above. */
        Command bcmd;
        if (cmd_pop_wait(e, &bcmd, BACKPRESSURE_TIMEOUT_MS)) {
          handle_command(e, &bcmd, &quit);
          st = (EngineState) atomic_load_explicit(&e->state, memory_order_relaxed);
          if (quit || st != ENG_PLAYING) {
            break;
          }
          if (e->flush_gen != gen0) {
            flushed = 1;
            break;
          }
        }
      }
    }

    /* DESIGN.md VI: a short read IS the EOF signal, not just a zero
       one. Posted after the frames are written so the tail of the file
       is never dropped, and suppressed if the chunk was abandoned or
       the engine left ENG_PLAYING -- in both of those cases the read
       that produced n no longer describes what T2 is doing. */
    if (!quit && !flushed && st == ENG_PLAYING && n < READ_CHUNK_FRAMES) {
      /* Amendment 09: with a prefetched next track, EOF is not the end
         of anything T1 needs to hear about -- T2 opens the next file and
         keeps writing into the same ring, contiguously (DESIGN.md V.3
         steps 1-3). Only when there is nothing next, or opening it
         failed, does this fall through to the end-of-queue path below. */
      if (advance_to_next(e)) {
        continue;
      }

      Event ev;
      memset(&ev, 0, sizeof(ev));
      ev.kind     = EV_EOF;
      ev.track_id = e->cur_track_id;
      /* Amendment 06: the counter boundary_ring entries are keyed on, read
         here on T2 and crossing to T1 inside the mutex-protected event
         queue, so it needs no synchronisation of its own. Sampled after
         the write loop above, so it names the last frame of the file. */
      ev.end_frame = e->write_frames;
      evt_push(e, &ev);
      park(e, ENG_STOPPED);
    }
  }

  return NULL;
} /* engine_thread_main */

/* ---------------------------------------------------------------------
 * Lifecycle and T1-side convenience wrappers.
 * ------------------------------------------------------------------- */

/* Reserved, post-v1: the EQ's coefficient buffers belong in THIS file,
   not in audio/sink.c (DESIGN.md IX.3). The filter itself runs on T3, in
   audio/dsp.c, at the seam between callback steps 6 and 7 -- a band
   adjustment has to be audible within one device period, and anything on
   T2 is two seconds late. But the buffers T1 writes and T3 reads are
   published from here, through a double-buffered atomic index: T1 fills
   the inactive set, then release-stores the index; T3 acquire-loads it
   once per invocation and uses that set for the whole block.

   The ownership is the point, and it is why this comment exists in v1
   with nothing under it. Putting the coefficients in sink.c would be the
   obvious shortcut -- that is where the callback is -- and it would drag
   miniaudio into the EQ and break the confinement that makes sink.c
   replaceable at all (DESIGN.md III.2). Declared here so the next person
   to build it finds the claim already staked. */

/* engine_create: see include/audio/engine.h. */
Engine *
engine_create(const SessionFormat * fmt, AudioSource src, AudioSink * sink)
{
  if (fmt == NULL || sink == NULL || src.self == NULL) {
    /* A source that failed to construct arrives here with an all-NULL
       vtable (decoder_create's contract), so there is nothing to destroy
       and nothing to leak. */
    return NULL;
  }

  Engine * e = calloc(1, sizeof(Engine));
  if (e == NULL) {
    src.destroy(src.self);
    return NULL;
  }

  e->src = src; /* owned from here; released by engine_destroy */

  if (ring_init(&e->boundary_ring, BOUNDARY_RING_BYTES) != 0) {
    e->src.destroy(e->src.self);
    free(e);
    return NULL;
  }

  e->sink         = sink;
  e->pcm_ring     = sink_pcm_ring();
  e->sample_rate  = fmt->sample_rate;
  e->event_pipe_w = -1;
  e->parked       = 1; /* T2 starts parked with no track, IV.9.a step 7 */
  e->lead_low     = UINT64_MAX;
  atomic_store_explicit(&e->state, ENG_STOPPED, memory_order_relaxed);

  pthread_mutex_init(&e->cmd_mtx, NULL);
  pthread_cond_init(&e->cmd_cond, NULL);
  pthread_mutex_init(&e->evt_mtx, NULL);

  return e;
} /* engine_create */

/* engine_destroy: see include/audio/engine.h. */
void
engine_destroy(Engine * e)
{
  if (e == NULL) {
    return;
  }
  if (e->has_track) {
    e->src.close(e->src.self);
  }
  if (e->src.destroy != NULL) {
    e->src.destroy(e->src.self);
  }
  memset(&e->src, 0, sizeof(e->src));
  ring_free(&e->boundary_ring);
  pthread_mutex_destroy(&e->cmd_mtx);
  pthread_cond_destroy(&e->cmd_cond);
  pthread_mutex_destroy(&e->evt_mtx);
  free(e);
} /* engine_destroy */

/* engine_set_event_pipe: see include/audio/engine.h. */
void
engine_set_event_pipe(Engine * e, int fd)
{
  e->event_pipe_w = fd;
} /* engine_set_event_pipe */

/* engine_start: see include/audio/engine.h. */
int
engine_start(Engine * e)
{
  if (e == NULL || e->pcm_ring == NULL) {
    return -1;
  }
  if (pthread_create(&e->thread, NULL, engine_thread_main, e) != 0) {
    return -1;
  }
  e->thread_started = 1;
  return 0;
} /* engine_start */

/* engine_join: see include/audio/engine.h. */
void
engine_join(Engine * e)
{
  if (e == NULL || !e->thread_started) {
    return;
  }
  pthread_join(e->thread, NULL);
  e->thread_started = 0;
} /* engine_join */

/* engine_frames_account: see include/audio/engine.h. */
void
engine_frames_account(const Engine * e, uint64_t * written,
                      uint64_t * in_ring)
{
  *written = e->write_frames;
  *in_ring = ring_fill(e->pcm_ring) / (ENGINE_CHANNELS * sizeof(float));
} /* engine_frames_account */

/* engine_lead_stats: see include/audio/engine.h. */
int
engine_lead_stats(const Engine * e, uint64_t * low, uint64_t * steady,
                  unsigned * underruns, uint64_t * cap)
{
  *low       = e->lead_low;
  *steady    = e->lead_frames;
  *underruns = e->lead_underruns;
  *cap       = (uint64_t) (e->pcm_ring->cap / (ENGINE_CHANNELS * sizeof(float)));
  return e->lead_low != UINT64_MAX;
} /* engine_lead_stats */

/* engine_play: see include/audio/engine.h. */
void
engine_play(Engine * e, const char * path, int64_t track_id)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_PLAY;
  snprintf(cmd.path, sizeof(cmd.path), "%s", path);
  cmd.track_id = track_id;
  cmd_push(e, &cmd);
} /* engine_play */

/* engine_set_next: see include/audio/engine.h. */
void
engine_set_next(Engine * e, const char * path, int64_t track_id)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_SET_NEXT;
  /* A NULL path is how T1 says "nothing follows": it clears the slot,
     which is what makes the end-of-queue path reachable again after a
     prefetch was pushed and then invalidated. */
  snprintf(cmd.path, sizeof(cmd.path), "%s", (path != NULL) ? path : "");
  cmd.track_id = track_id;
  cmd_push(e, &cmd);
} /* engine_set_next */

/* engine_pause: see include/audio/engine.h. */
void
engine_pause(Engine * e)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_PAUSE;
  cmd_push(e, &cmd);
} /* engine_pause */

/* engine_resume: see include/audio/engine.h. */
void
engine_resume(Engine * e)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_RESUME;
  cmd_push(e, &cmd);
} /* engine_resume */

/* engine_park_sync: see include/audio/engine.h. */
void
engine_park_sync(Engine * e)
{
  if (e == NULL) {
    return;
  }

  /* A never-started T2 can never set the flag, so waiting for it would
     hang. It is also already true that no thread is writing pcm_ring in
     that case, which is the whole postcondition this function owes its
     caller. */
  if (!e->thread_started) {
    return;
  }

  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind   = CMD_PAUSE;
  cmd.ticket = ++e->park_ticket;
  cmd_push(e, &cmd);

  pthread_mutex_lock(&e->cmd_mtx);
  while (e->pause_ack < cmd.ticket) {
    pthread_cond_wait(&e->cmd_cond, &e->cmd_mtx);
  }
  pthread_mutex_unlock(&e->cmd_mtx);
} /* engine_park_sync */

/* engine_seek: see include/audio/engine.h. */
void
engine_seek(Engine * e, int64_t ms)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind   = CMD_SEEK;
  cmd.arg_ms = ms;
  cmd_push(e, &cmd);
} /* engine_seek */

/* engine_stop: see include/audio/engine.h. */
void
engine_stop(Engine * e)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_STOP;
  cmd_push(e, &cmd);
} /* engine_stop */

/* engine_quit: see include/audio/engine.h. */
void
engine_quit(Engine * e)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_QUIT;
  cmd_push(e, &cmd);
} /* engine_quit */

/* engine_set_gain: see include/audio/engine.h. */
void
engine_set_gain(Engine * e, float gain)
{
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.kind = CMD_SET_GAIN;
  cmd.gain = gain;
  cmd_push(e, &cmd);
} /* engine_set_gain */

/* engine_state: see include/audio/engine.h. */
EngineState
engine_state(const Engine * e)
{
  return (EngineState) atomic_load_explicit(&e->state, memory_order_relaxed);
} /* engine_state */

/* engine_boundary_ring: see include/audio/engine.h. */
Ring *
engine_boundary_ring(Engine * e)
{
  return &e->boundary_ring;
} /* engine_boundary_ring */
