/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * audio/sink.c -- the AudioSink implementation over miniaudio, and the
 * project's rank-1 invariant: the data callback (DESIGN.md IV.2) and
 * the notification callback (DESIGN.md IV.3), both running on T3.
 *
 * Every miniaudio type and every miniaudio call in the whole tree lives
 * in this one file (CLAUDE.md; DESIGN.md III.2, XII.4 item 6/checklist
 * scope). MINIAUDIO_IMPLEMENTATION is defined exactly once, here.
 *
 * There is exactly one device per process (DESIGN.md VI), so the state
 * the callbacks touch is a set of file-scope statics rather than fields
 * reached through AudioSink.self -- this is what lets the callback body
 * name g_pcm_ring, g_flushing, g_frames_consumed, etc. directly, the
 * same names DESIGN.md IV.2 and plan.md phase-5-sink.md I.3 use.
 *
 * Any diff touching this file is checked against DESIGN.md XII.4 (all
 * thirteen items) before anything else.
 */

/* -Wconversion/-Wsign-conversion (Makefile EXTRA_WARN, DESIGN.md XII.5)
   fire throughout miniaudio.h's own implementation once
   MINIAUDIO_IMPLEMENTATION pulls its body into this translation unit --
   vendor/ is never edited (CLAUDE.md), so the two flags are suppressed
   for exactly this #include and restored immediately after. Everything
   below this block -- all of it code this file owns -- stays checked
   under the full flag set. */
#define MINIAUDIO_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#include "miniaudio.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "../../include/audio/format.h"
#include "../../include/audio/ring.h"
#include "../../include/audio/sink.h"
#include "../../include/audio/trace.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Pinned by DESIGN.md II.2 -- channels = 2 moves any device-native
   channel conversion inside miniaudio, ahead of our callback, and makes
   frameCount * SINK_CHANNELS * sizeof(float) correct by construction. */
#define SINK_CHANNELS 2u

/* DESIGN.md IV.4 rule 6: "a single #define PCM_LEAD_SECONDS in
   audio/engine.c". engine.c does not exist until phase 7 and sink.c is
   the only owner of pcm_ring until then (nothing else allocates it in
   this phase) -- defined here for now, to move verbatim into engine.c
   when phase 7 lands. See phase-5.md "Deviations" for the full note. */
#define PCM_LEAD_SECONDS 2

/* ~170 ms at 48 kHz f32 stereo -- more than the ~33 ms redraw tick
   needs, so the (post-v1) visualiser never starves (DESIGN.md IV.4
   rule 6). */
#define VIZ_RING_BYTES (64u * 1024u)

/* Still a placeholder value (DESIGN.md IV.4 rule 6, IV.2 step 3; plan.md
   VII gap 12): no numeric threshold is given anywhere in DESIGN.md or
   the phase files, and phase 5b's own job is to turn this into a real
   measurement via TRACE_RING_LOW, not to pick the number -- that is
   deferred to phase 18 once real playback (phase 7+) generates data to
   look at. trace_emit is real as of phase 5b, so this threshold now has
   an observable effect (it gates when TRACE_RING_LOW's edge fires).
   ~100 ms at 48 kHz. */
#define RING_LOW_WATER_FRAMES 4800u
#define RING_LOW_WATER_BYTES \
  ((size_t) RING_LOW_WATER_FRAMES * SINK_CHANNELS * sizeof(float))

/* DESIGN.md XI.2: browse-only allocates the rings from a default
   session format of f32 / 48000 / 2 so every pointer stays non-NULL when
   no device was ever obtained. The rate is a placeholder for sizing and
   for T1's position arithmetic; nothing consumes these rings in that
   state. */
#define BROWSE_ONLY_SAMPLE_RATE 48000u

/* --tone (plan.md VI.8): a 441 Hz SineSource with no ffmpeg and no
   file, per DESIGN.md VI. */
#define TONE_HZ               441.0
#define TONE_DURATION_SECONDS 3
#define TONE_AMPLITUDE        0.2f
#define TONE_CHUNK_FRAMES     1024

/* ---------------------------------------------------------------------
 * Module state. Ownership per DESIGN.md IV.1: T3 exclusively owns
 * pcm_ring's read cursor, viz_ring's write cursor, g_frames_consumed,
 * and g_cb_seq (each ring's SPSC contract enforces the rest). T1 owns
 * everything else here through the AudioSink calls.
 * ------------------------------------------------------------------- */

static ma_context g_context;
static ma_device  g_device;

/* Written by T1 only (open/close), but READ from T2 as well, through
   sink_is_started's gate (phase 7's seek fence). A plain int here is a
   data race the moment phase 7 lands -- and sink_is_started is required
   to be a pure atomic load with no other effect (plan.md VII gap 2,
   closed 2026-08-08), which a non-atomic gate would violate. */
static atomic_bool g_device_open = false;

static Ring g_pcm_ring;
static Ring g_viz_ring;

/* Whether g_pcm_ring/g_viz_ring currently hold an allocation. Distinct
   from g_device_open on purpose: browse-only (DESIGN.md XI.2) has rings
   and no device, and the reopen ladder (IV.8.b) briefly has neither.
   T1-only -- every writer (sink_open_impl, sink_alloc_default_rings,
   sink_free_rings) and the one reader that matters (sink_pcm_ring, from
   T1 and from T2 at engine_create/engine_device_reflush) run while the
   rings are provably not being resized, but atomic anyway for the same
   reason g_device_open is: a plain int read from two threads is a data
   race whatever the surrounding argument says. */
static atomic_bool g_rings_ready = false;

static atomic_bool           g_flushing        = false;
static atomic_uint_least64_t g_frames_consumed = 0;
static atomic_uint           g_cb_seq          = 0;
static atomic_uint           g_underrun_count  = 0;
static atomic_bool           g_cb_alive        = false; /* residual-risk-2 mitigation, phase-5-sink.md I.3 */
static atomic_int            g_device_event    = -1;    /* last ma_device_notification_type, or -1 = none */

/* The best (lowest) pcm_ring occupancy seen so far this device lifetime,
   in bytes. TRACE_RING_LOW fires only when occupancy sets a new minimum
   below RING_LOW_WATER_FRAMES, which is what makes the record stream
   both bounded and truthful:

     - Level-triggered (emit on every callback below the threshold)
       measures the real minimum but floods a 4096-record trace_ring
       during pre-fill and every quiet stretch, dropping the very
       records the PCM_LEAD_SECONDS measurement depends on.
     - Purely edge-triggered (emit once on the way below) is bounded but
       reports the threshold back at you -- the record carries occupancy
       at the crossing, so the low-water mark reads ~RING_LOW_WATER_FRAMES
       on every run no matter how close to empty playback actually came.

   New-minimum-only is self-limiting: each record must be strictly lower
   than the last, so a burst converges in a handful of records rather
   than thousands, and the final drain-to-empty at the end of playback --
   the case a rising-edge scheme misses entirely -- is captured.

   T3-owned exclusively: only kurono_data_callback reads or writes it, so
   it needs no atomic. Reset in sink_open_impl for each new device
   lifetime, before T3 can run. */
static size_t g_ring_low_best = SIZE_MAX;

/* Phase 7 carry-in item (plans/kurono/05-impl/phase-7.md): gates
   TRACE_RING_LOW so it measures playback headroom, not the terminal
   drain-to-empty every run ends with once T2 parks. T3-read (relaxed,
   inside the callback), T2-written via sink_set_producer_active on
   every producing/parked transition -- see that function's comment in
   sink.h. Defaults false: before engine.c's first CMD_PLAY succeeds,
   the ring is provably not being actively filled. */
static atomic_bool g_producer_active = false;

/* IEEE-754 bit pattern of the current gain, DESIGN.md IV.2: "One
   representation, no lock-freedom check, no fallback branch." */
static _Atomic uint32_t g_gain_bits;
_Static_assert(sizeof(float) == 4, "gain bit-pattern atomic requires a 4-byte float");

/* Cached, never owned -- see sink_set_event_pipe and DESIGN.md IV.9.b
   step 9's warning. -1 means "no wakeup pipe registered". */
static int g_event_pipe_w = -1;

/* The T1-side notification handler registered through AudioSink.open
   (format.h's SinkNotifyCallback), and its user pointer. Written by T1
   in sink_open_impl, read by T1 in sink_dispatch_notifications -- never
   by T3, which only records the notification type into g_device_event
   and rings the doorbell. Keeping the *call* off T3 is what makes
   DESIGN.md IV.3's "it must never call ma_device_stop/uninit/start"
   enforceable at all: the policy that wants to do exactly that runs on
   the thread that may. */
static SinkNotifyCallback g_on_notify   = NULL;
static void *             g_notify_user = NULL;

/* apply_gain: multiply n interleaved f32 samples in buf by the current
   gain, in place. T3-only. One relaxed atomic load, one union
   reinterpretation (DESIGN.md IV.2: "read relaxed and reinterpreted
   through a union"), one loop bounded by n -- no allocation, no lock,
   no syscall. */
static void
apply_gain(float * buf, ma_uint32 n)
{
  union {
    uint32_t bits;
    float    f;
  } u;
  u.bits = atomic_load_explicit(&g_gain_bits, memory_order_relaxed);

  for (ma_uint32 i = 0; i < n; i++) {
    buf[i] *= u.f;
  }
} /* apply_gain */

/* kurono_data_callback: miniaudio's playback data callback. Runs on T3.
   Fills out with frameCount stereo f32 frames at the session format.
   The list of things this function does is closed at four (DESIGN.md
   IV.2, CLAUDE.md): pop from pcm_ring, apply gain, memcpy into
   viz_ring (before the gain), trace_emit on underrun/low occupancy.
   Never blocks, never allocates, never takes a lock, never makes a
   syscall other than trace_emit's bounded memcpy-and-store. */
static void
kurono_data_callback(ma_device * dev, void * out, const void * in,
                      ma_uint32 frameCount)
{
  (void) dev;
  (void) in;

  atomic_store_explicit(&g_cb_alive, true, memory_order_relaxed);

  const size_t want = (size_t) frameCount * SINK_CHANNELS * sizeof(float);

  /* 1 */ bool flush = atomic_load_explicit(&g_flushing, memory_order_acquire);
  /* 2 */ if (flush) {
            memset(out, 0, want);
            goto seq; /* touch nothing else while flushing -- DESIGN.md IV.7.6 */
          }

  /* 3 */ size_t got = ring_read(&g_pcm_ring, out, want);
          size_t occ = ring_fill(&g_pcm_ring);
          /* New minimum only, not every callback below the threshold and
             not just the crossing -- see g_ring_low_best above. Also
             gated on g_producer_active (phase 7 carry-in item, sink.h's
             comment on sink_set_producer_active): without this gate the
             low-water mark always converges to 0 by the end of any run,
             because T2 parking at EOF drains the ring to empty on
             purpose, which is not a measurement of playback headroom. */
          if (occ < RING_LOW_WATER_BYTES && occ < g_ring_low_best
              && atomic_load_explicit(&g_producer_active, memory_order_relaxed)) {
            g_ring_low_best = occ;
            trace_emit(TRACE_RING_LOW,
                       (uint32_t) (occ / (SINK_CHANNELS * sizeof(float))), 0, 0);
          }

  /* 4 */ if (got < want) {
            memset((uint8_t *) out + got, 0, want - got);
            atomic_fetch_add_explicit(&g_underrun_count, 1u, memory_order_relaxed);
            trace_emit(TRACE_UNDERRUN,
                       (uint32_t) (got / (SINK_CHANNELS * sizeof(float))),
                       frameCount, 0);
          }

  /* 5 */ (void) ring_write(&g_viz_ring, out, got); /* PRE-GAIN. DESIGN.md IX.2. */

  /* 6 */ atomic_fetch_add_explicit(&g_frames_consumed,
            (uint_least64_t) (got / (SINK_CHANNELS * sizeof(float))),
            memory_order_release); /* got, NEVER frameCount -- checklist item 7 */

  /* --- EQ SEAM: audio/dsp.c goes here, post-v1. DESIGN.md IX.3. -------- */

  /* 7 */ apply_gain((float *) out, frameCount * SINK_CHANNELS);

seq:
  /* 8 */ atomic_fetch_add_explicit(&g_cb_seq, 1u, memory_order_release);
  atomic_store_explicit(&g_cb_alive, false, memory_order_relaxed);
} /* kurono_data_callback */

/* kurono_notification_callback: miniaudio's notification callback for
   started/stopped/rerouted/etc. Runs on T3, the same thread as the data
   callback (DESIGN.md IV.1, IV.3) -- bound by the identical contract,
   with exactly one documented exception: the one-byte write below. It
   must never call ma_device_stop, ma_device_uninit, or ma_device_start
   on its own device; all three self-deadlock from this thread. All
   policy (reopen, degrade, notify) is decided on T1 (phase 8). */
static void
kurono_notification_callback(const ma_device_notification * n)
{
  atomic_store_explicit(&g_device_event, (int) n->type, memory_order_release);
  trace_emit(TRACE_NOTIFY, (uint32_t) n->type,
             (uint32_t) ma_device_get_state(n->pDevice), 0);
  if (g_event_pipe_w >= 0) {
    (void) write(g_event_pipe_w, "\0", 1); /* O_NONBLOCK; EAGAIN ignored */
  }
} /* kurono_notification_callback */

/* sink_set_event_pipe: see include/audio/sink.h. */
void
sink_set_event_pipe(int fd)
{
  g_event_pipe_w = fd;
} /* sink_set_event_pipe */

/* notify_from_ma: translate a ma_device_notification_type into the
   backend-independent SinkNotification of format.h. Runs on T1, inside
   sink_dispatch_notifications -- deliberately not on T3, so the
   notification callback gains no line at all in this phase. An
   unrecognised type (a miniaudio upgrade adding one) maps to
   SINK_NOTIFY_UNKNOWN rather than onto one of the four that carry
   policy. Takes the raw ma type, returns the SinkNotification. */
static SinkNotification
notify_from_ma(int ma_type)
{
  switch (ma_type) {
    case ma_device_notification_type_started:           return SINK_NOTIFY_STARTED;
    case ma_device_notification_type_stopped:           return SINK_NOTIFY_STOPPED;
    case ma_device_notification_type_rerouted:          return SINK_NOTIFY_REROUTED;
    case ma_device_notification_type_interruption_began: return SINK_NOTIFY_INTERRUPTION_BEGAN;
    case ma_device_notification_type_interruption_ended: return SINK_NOTIFY_INTERRUPTION_ENDED;
    default:                                            return SINK_NOTIFY_UNKNOWN;
  }
} /* notify_from_ma */

/* sink_dispatch_notifications: see include/audio/sink.h. */
int
sink_dispatch_notifications(void)
{
  /* Exchange, not load-then-clear: T3 may store a new notification at
     any moment, and taking the slot in one operation means the worst
     case is dispatching a notification twice's worth of staleness, never
     losing one that arrived during the read-modify-write. */
  int raw = atomic_exchange_explicit(&g_device_event, -1, memory_order_acquire);
  if (raw < 0) {
    return 0;
  }
  if (g_on_notify != NULL) {
    g_on_notify(g_notify_user, (int) notify_from_ma(raw));
  }
  return 1;
} /* sink_dispatch_notifications */

/* sink_cb_alive: see include/audio/sink.h. */
int
sink_cb_alive(void)
{
  return atomic_load_explicit(&g_cb_alive, memory_order_relaxed) ? 1 : 0;
} /* sink_cb_alive */

/* session_arm: arm a fresh sink lifetime at session rate r, shared by
   both AudioSink backends (amendment 10). Frees any previous rings,
   allocates pcm_ring and viz_ring sized from r, resets gain to unity and
   every callback-side counter, and registers g_frames_consumed and r
   with audio/trace.c. Caller guarantees neither ring has a live toucher.
   Takes the session rate, returns 0, or -1 with neither ring allocated. */
static int
session_arm(uint32_t r)
{
  /* A reopen from the device-loss ladder (DESIGN.md IV.8.b step 3)
     arrives here with the previous device lifetime's rings still
     allocated, and the negotiated rate may differ, so they are sized
     afresh rather than reused. Freeing first is what keeps that from
     leaking both buffers on every reopen. Legal at this exact point and
     nowhere else: the ladder parks T2 synchronously before step 2
     (engine_park_sync), and step 2's ma_device_uninit has already joined
     T3 -- both touchers of pcm_ring are provably gone, the same
     condition IV.9.b step 6 requires. ring_free is idempotent, so the
     first-open path pays one pair of free(NULL) calls. */
  sink_free_rings();

  size_t pcm_bytes = (size_t) PCM_LEAD_SECONDS * r * SINK_CHANNELS * sizeof(float);
  if (ring_init(&g_pcm_ring, pcm_bytes) != 0) {
    return -1;
  }
  if (ring_init(&g_viz_ring, VIZ_RING_BYTES) != 0) {
    ring_free(&g_pcm_ring);
    return -1;
  }
  atomic_store_explicit(&g_rings_ready, true, memory_order_release);

  union {
    uint32_t bits;
    float    f;
  } unity;
  unity.f = 1.0f;
  atomic_store_explicit(&g_gain_bits, unity.bits, memory_order_relaxed);

  atomic_store_explicit(&g_flushing, false, memory_order_release);
  atomic_store_explicit(&g_frames_consumed, 0, memory_order_release);
  atomic_store_explicit(&g_cb_seq, 0, memory_order_release);
  atomic_store_explicit(&g_underrun_count, 0, memory_order_relaxed);
  atomic_store_explicit(&g_device_event, -1, memory_order_release);
  g_ring_low_best = SIZE_MAX; /* fresh device lifetime -- see its declaration */

  /* Register g_frames_consumed and R with audio/trace.c (trace.h: "call
     once, from sink.c ... once R is known"). Must happen before
     ma_device_start (sink_start_impl, called separately) -- T3 does not
     exist as a running consumer of these until then, but trace_emit is
     reachable from the very first pre-fill invocation on T1
     (DESIGN.md IV.1), so both registrations belong here, at the end of
     open, not deferred to start. */
  trace_set_frame_counter(&g_frames_consumed);
  trace_set_sample_rate(r);
  return 0;
} /* session_arm */

/* sink_open_impl: AudioSink.open. Opens the device once at the
   negotiated native rate (DESIGN.md II.2, sampleRate = 0), reads back
   the session format into *inout_fmt, and allocates pcm_ring/viz_ring
   sized from it -- in that order, since sizing depends on the
   negotiated rate. Does not call ma_device_start. Prints the one
   temporary diagnostic line (plan.md phase-5-sink.md III.1) before
   anything else can claim the terminal. Returns 0 on success, -1 on
   any failure (nothing is left partially allocated on -1). */
static int
sink_open_impl(void * self, SessionFormat * inout_fmt,
               SinkNotifyCallback on_notify, void * user)
{
  (void) self;

  if (atomic_load_explicit(&g_device_open, memory_order_acquire)) {
    return -1; /* one device per process; already open */
  }

  /* Registered before ma_device_init, not after: init can itself raise a
     notification, and a handler that is not yet installed when the first
     one lands is the failure DESIGN.md IV.9.a step 5 warns about for the
     pipe fd, applied to the handler. */
  g_on_notify   = on_notify;
  g_notify_user = user;

  if (ma_context_init(NULL, 0, NULL, &g_context) != MA_SUCCESS) {
    return -1;
  }

  ma_device_config cfg      = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format       = ma_format_f32;
  cfg.playback.channels     = SINK_CHANNELS;
  cfg.sampleRate            = 0; /* 0 == device native rate, DESIGN.md II.2 */
  cfg.dataCallback          = kurono_data_callback;
  cfg.notificationCallback  = kurono_notification_callback;
  cfg.pUserData             = self;

  if (ma_device_init(&g_context, &cfg, &g_device) != MA_SUCCESS) {
    ma_context_uninit(&g_context);
    return -1;
  }

  /* The M1 open item this line existed for (plan.md phase-5-sink.md
     III.1 -- the real negotiated internalPeriodSizeInFrames) was closed
     in phase 5: 900 frames at 48000 Hz, an 18.75 ms callback deadline,
     recorded in STATUS and in DESIGN.md. Phase 12 retires the print, as
     phase 5 said it would.
     It was legal where it stood -- before tb_init, the documented
     exception to the no-printf rule -- but it printed on every single
     run, leaving a line in the scrollback above the alternate screen
     for a number nobody needs after the one time it was measured. The
     values are all still reachable: they are in SessionFormat, and the
     status bar renders internal_format and internal_channels
     (DESIGN.md II.2 -- display only, never arithmetic). */

  uint32_t r = g_device.playback.internalSampleRate;

  inout_fmt->sample_rate       = r;
  inout_fmt->channels          = SINK_CHANNELS;
  inout_fmt->period_frames     = (uint32_t) g_device.playback.internalPeriodSizeInFrames;
  inout_fmt->internal_rate     = (uint32_t) g_device.playback.internalSampleRate;
  inout_fmt->internal_format   = (int) g_device.playback.internalFormat;
  inout_fmt->internal_channels = (uint32_t) g_device.playback.internalChannels;

  if (session_arm(r) != 0) {
    ma_device_uninit(&g_device);
    ma_context_uninit(&g_context);
    return -1;
  }

  atomic_store_explicit(&g_device_open, true, memory_order_release);
  return 0;
} /* sink_open_impl */

/* sink_start_impl: AudioSink.start. Thin wrapper over ma_device_start
   (DESIGN.md IV.9.a step 8) -- not called until pcm_ring/viz_ring exist,
   which sink_open_impl already guarantees by the time this can be
   called. Returns 0 on success, -1 if not open or on device failure. */
static int
sink_start_impl(void * self)
{
  (void) self;
  if (!atomic_load_explicit(&g_device_open, memory_order_acquire)) {
    return -1;
  }
  return (ma_device_start(&g_device) == MA_SUCCESS) ? 0 : -1;
} /* sink_start_impl */

/* sink_stop_impl: AudioSink.stop. Synchronous: per DESIGN.md IV.9.b
   step 2, ma_device_stop blocks until T3 has finished its last
   invocation and will not be called again. Returns 0 on success, -1 if
   not open or on device failure. */
static int
sink_stop_impl(void * self)
{
  (void) self;
  if (!atomic_load_explicit(&g_device_open, memory_order_acquire)) {
    return -1;
  }
  return (ma_device_stop(&g_device) == MA_SUCCESS) ? 0 : -1;
} /* sink_stop_impl */

/* sink_close_impl: AudioSink.close. Joins T3 (ma_device_uninit,
   DESIGN.md IV.9.b step 3) and uninitialises the context. Does NOT
   free the rings -- that is step 6, and step 6 is after step 5's T2
   join, not bundled with step 3's uninit. Phase 5 folded the two
   together because --tone had no T2; phase 7 gives pcm_ring a second
   toucher, so the split phase-5.md's "Open questions" anticipated is
   now required. Every caller pairs this with sink_free_rings once T2
   is joined. Never closes g_event_pipe_w -- that fd is cached, not
   owned. */
static void
sink_close_impl(void * self)
{
  (void) self;
  if (!atomic_load_explicit(&g_device_open, memory_order_acquire)) {
    return;
  }
  /* Cleared before the teardown, not after: once ma_device_uninit has
     run, &g_device must not be handed to ma_device_get_state, so
     sink_is_started's gate has to already read false. */
  atomic_store_explicit(&g_device_open, false, memory_order_release);
  ma_device_uninit(&g_device); /* joins T3 */
  ma_context_uninit(&g_context);
} /* sink_close_impl */

/* sink_free_rings: see include/audio/sink.h. */
void
sink_free_rings(void)
{
  atomic_store_explicit(&g_rings_ready, false, memory_order_release);
  ring_free(&g_pcm_ring);
  ring_free(&g_viz_ring);
} /* sink_free_rings */

/* sink_alloc_default_rings: see include/audio/sink.h. */
int
sink_alloc_default_rings(SessionFormat * out_fmt)
{
  sink_free_rings(); /* idempotent; a ladder that gave up freed them at step 2 */

  size_t pcm_bytes = (size_t) PCM_LEAD_SECONDS * BROWSE_ONLY_SAMPLE_RATE
                     * SINK_CHANNELS * sizeof(float);
  if (ring_init(&g_pcm_ring, pcm_bytes) != 0) {
    return -1;
  }
  if (ring_init(&g_viz_ring, VIZ_RING_BYTES) != 0) {
    ring_free(&g_pcm_ring);
    return -1;
  }
  atomic_store_explicit(&g_rings_ready, true, memory_order_release);

  memset(out_fmt, 0, sizeof(*out_fmt));
  out_fmt->sample_rate = BROWSE_ONLY_SAMPLE_RATE;
  out_fmt->channels    = SINK_CHANNELS;

  /* The counters are reset even though nothing will consume them: a
     stale g_frames_consumed from the device lifetime that just died
     would make T1's position readout wrong for the rest of the process,
     and browse-only is a state the player can sit in for hours. */
  atomic_store_explicit(&g_flushing, false, memory_order_release);
  atomic_store_explicit(&g_frames_consumed, 0, memory_order_release);
  atomic_store_explicit(&g_cb_seq, 0, memory_order_release);
  g_ring_low_best = SIZE_MAX;

  trace_set_frame_counter(&g_frames_consumed);
  trace_set_sample_rate(BROWSE_ONLY_SAMPLE_RATE);
  return 0;
} /* sink_alloc_default_rings */

/* sink_get: see include/audio/sink.h. */
void
sink_get(AudioSink * out)
{
  out->open  = sink_open_impl;
  out->start = sink_start_impl;
  out->stop  = sink_stop_impl;
  out->close = sink_close_impl;
  out->self  = NULL;
} /* sink_get */

/* sink_is_started: see include/audio/sink.h. Pure atomic load, nothing
   else -- checked against plan.md VII gap 2's three constraints. */
int
sink_is_started(const AudioSink * s)
{
  (void) s;
  if (!atomic_load_explicit(&g_device_open, memory_order_acquire)) {
    return 0;
  }
  return ma_device_get_state(&g_device) == ma_device_state_started;
} /* sink_is_started */

/* sink_underrun_count: see include/audio/sink.h. Relaxed on both sides
   -- T3 increments relaxed inside the callback and this is for display
   only. */
unsigned
sink_underrun_count(void)
{
  return atomic_load_explicit(&g_underrun_count, memory_order_relaxed);
} /* sink_underrun_count */

/* sink_pcm_ring: see include/audio/sink.h. */
Ring *
sink_pcm_ring(void)
{
  if (!atomic_load_explicit(&g_rings_ready, memory_order_acquire)) {
    return NULL;
  }
  return &g_pcm_ring;
} /* sink_pcm_ring */

/* sink_flush_flag: see include/audio/sink.h. */
atomic_bool *
sink_flush_flag(void)
{
  return &g_flushing;
} /* sink_flush_flag */

/* sink_cb_seq: see include/audio/sink.h. */
const atomic_uint *
sink_cb_seq(void)
{
  return &g_cb_seq;
} /* sink_cb_seq */

/* sink_frames_consumed: see include/audio/sink.h. */
atomic_uint_least64_t *
sink_frames_consumed(void)
{
  return &g_frames_consumed;
} /* sink_frames_consumed */

/* sink_set_gain: see include/audio/sink.h. */
void
sink_set_gain(float gain)
{
  union {
    uint32_t bits;
    float    f;
  } u;
  u.f = gain;
  atomic_store_explicit(&g_gain_bits, u.bits, memory_order_relaxed);
} /* sink_set_gain */

/* sink_set_producer_active: see include/audio/sink.h. */
void
sink_set_producer_active(int active)
{
  atomic_store_explicit(&g_producer_active, active ? true : false,
                         memory_order_relaxed);
} /* sink_set_producer_active */

