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

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
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

/* Placeholder pending phase 5b's real measurement (DESIGN.md IV.4 rule
   6, IV.2 step 3; plan.md VII gap 12). trace_emit is a no-op in this
   phase, so this value has no observable effect yet -- only that the
   call site exists at the right spot. ~100 ms at 48 kHz. */
#define RING_LOW_WATER_FRAMES 4800u
#define RING_LOW_WATER_BYTES \
  ((size_t) RING_LOW_WATER_FRAMES * SINK_CHANNELS * sizeof(float))

/* trace event ids -- local placeholders. audio/trace.c (phase 5b)
   defines the real TraceEvent enum (DESIGN.md IV.10); when it lands,
   these three #defines and the no-op trace_emit below are deleted and
   replaced by "#include "../../include/audio/trace.h"". The call
   sites in the two callbacks below do not change. */
#define TRACE_UNDERRUN 0u
#define TRACE_RING_LOW 1u
#define TRACE_NOTIFY   2u

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

static atomic_bool           g_flushing        = false;
static atomic_uint_least64_t g_frames_consumed = 0;
static atomic_uint           g_cb_seq          = 0;
static atomic_uint           g_underrun_count  = 0;
static atomic_bool           g_cb_alive        = false; /* residual-risk-3 mitigation, phase-5-sink.md I.3 */
static atomic_int            g_device_event    = -1;    /* last ma_device_notification_type, or -1 = none */

/* IEEE-754 bit pattern of the current gain, DESIGN.md IV.2: "One
   representation, no lock-freedom check, no fallback branch." */
static _Atomic uint32_t g_gain_bits;
_Static_assert(sizeof(float) == 4, "gain bit-pattern atomic requires a 4-byte float");

/* Cached, never owned -- see sink_set_event_pipe and DESIGN.md IV.9.b
   step 9's warning. -1 means "no wakeup pipe registered". */
static int g_event_pipe_w = -1;

/* trace_emit: no-op in phase 5. audio/trace.c (phase 5b) replaces this
   with the real SPSC trace ring (DESIGN.md IV.10). The call sites are
   written now, at the exact points the callback and notification-
   callback contracts specify (DESIGN.md IV.2 step 3/4, IV.3), so the
   callback is never reopened later just to add them. */
static void
trace_emit(uint32_t event, uint32_t a, uint32_t b, uint32_t c)
{
  (void) event;
  (void) a;
  (void) b;
  (void) c;
} /* trace_emit */

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
          if (occ < RING_LOW_WATER_BYTES) {
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
  (void) on_notify; /* stored/dispatched starting phase 8; unused until then */
  (void) user;

  if (atomic_load_explicit(&g_device_open, memory_order_acquire)) {
    return -1; /* one device per process; already open */
  }

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

  /* M1 open item (plan.md phase-5-sink.md III.1): the real negotiated
     internalPeriodSizeInFrames is not knowable without running. Legal
     here -- it runs before tb_init ever claims the terminal, the
     documented exception to the no-printf rule (CLAUDE.md), not a
     violation of it. Remove or downgrade before phase 12. */
  fprintf(stderr,
          "device: rate=%u internalRate=%u internalChannels=%u "
          "internalFormat=%d periodFrames=%u\n",
          g_device.sampleRate, g_device.playback.internalSampleRate,
          g_device.playback.internalChannels,
          (int) g_device.playback.internalFormat,
          g_device.playback.internalPeriodSizeInFrames);

  uint32_t r = g_device.playback.internalSampleRate;

  inout_fmt->sample_rate       = r;
  inout_fmt->channels          = SINK_CHANNELS;
  inout_fmt->period_frames     = (uint32_t) g_device.playback.internalPeriodSizeInFrames;
  inout_fmt->internal_rate     = (uint32_t) g_device.playback.internalSampleRate;
  inout_fmt->internal_format   = (int) g_device.playback.internalFormat;
  inout_fmt->internal_channels = (uint32_t) g_device.playback.internalChannels;

  size_t pcm_bytes = (size_t) PCM_LEAD_SECONDS * r * SINK_CHANNELS * sizeof(float);
  if (ring_init(&g_pcm_ring, pcm_bytes) != 0) {
    ma_device_uninit(&g_device);
    ma_context_uninit(&g_context);
    return -1;
  }
  if (ring_init(&g_viz_ring, VIZ_RING_BYTES) != 0) {
    ring_free(&g_pcm_ring);
    ma_device_uninit(&g_device);
    ma_context_uninit(&g_context);
    return -1;
  }

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
   DESIGN.md IV.9.b step 3), then frees pcm_ring and viz_ring. Safe to
   fold ring_free in here for phase 5's only caller (--tone, single-
   threaded besides T3, which is provably gone once ma_device_uninit
   returns). See phase-5.md "Open questions" for why phase 7/8's fuller
   teardown -- where T2 also touches pcm_ring -- may need to split this
   into a separate step ordered after the T2 join (DESIGN.md IV.9.b
   step 6 is after step 5's join, not bundled with step 3's uninit).
   Never closes g_event_pipe_w -- that fd is cached, not owned. */
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
  ring_free(&g_pcm_ring);
  ring_free(&g_viz_ring);
} /* sink_close_impl */

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

/* sleep_ms: nanosleep for ms milliseconds. Main-thread (T1) helper for
   --tone's producer loop only -- never called from T2 or T3. */
static void
sleep_ms(long ms)
{
  struct timespec ts;
  ts.tv_sec  = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
} /* sleep_ms */

/* tone_gen_chunk: write n interleaved stereo f32 frames of a 441 Hz
   sine into chunk, advancing *phase. Pure generation, no I/O -- the
   main-thread analogue of what audio/decoder.c's T2 will produce from
   a real file. */
static void
tone_gen_chunk(float * chunk, int n, double * phase, double phase_step)
{
  for (int i = 0; i < n; i++) {
    float s = TONE_AMPLITUDE * (float) sin(*phase);
    *phase += phase_step;
    if (*phase >= 2.0 * MA_PI) {
      *phase -= 2.0 * MA_PI;
    }
    chunk[i * 2]     = s;
    chunk[i * 2 + 1] = s;
  }
} /* tone_gen_chunk */

/* sink_run_tone: see include/audio/sink.h. */
int
sink_run_tone(void)
{
  AudioSink sink;
  sink_get(&sink);

  SessionFormat fmt;
  memset(&fmt, 0, sizeof(fmt));

  if (sink.open(sink.self, &fmt, NULL, NULL) != 0) {
    fprintf(stderr, "kurono: --tone: sink open failed\n");
    return -1;
  }

  printf("session format: f32 / %u / %u\n", fmt.sample_rate, fmt.channels);

  /* MA_PI (miniaudio.h), not M_PI (math.h): M_PI is not guaranteed
     visible under this project's strict -D_POSIX_C_SOURCE=200809L, and
     miniaudio.h is already included here. */
  double phase      = 0.0;
  double phase_step = 2.0 * MA_PI * TONE_HZ / (double) fmt.sample_rate;
  float  chunk[TONE_CHUNK_FRAMES * SINK_CHANNELS];

  long total_frames = (long) fmt.sample_rate * TONE_DURATION_SECONDS;
  long written      = 0;
  int  started      = 0;

  /* Feed the ring exactly as a decoder thread would: generate a chunk,
     write it, and if the ring is full, start the device (the first
     time only) so the callback begins draining it, then retry. */
  while (written < total_frames) {
    int n = (int) ((total_frames - written < TONE_CHUNK_FRAMES)
                      ? (total_frames - written)
                      : TONE_CHUNK_FRAMES);
    tone_gen_chunk(chunk, n, &phase, phase_step);

    size_t bytes = (size_t) n * SINK_CHANNELS * sizeof(float);
    size_t off   = 0;
    while (off < bytes) {
      size_t w = ring_write(&g_pcm_ring, (uint8_t *) chunk + off, bytes - off);
      off += w;
      if (off < bytes) {
        if (!started) {
          if (sink.start(sink.self) != 0) {
            fprintf(stderr, "kurono: --tone: sink start failed\n");
            sink.close(sink.self);
            return -1;
          }
          started = 1;
        }
        sleep_ms(5);
      }
    }
    written += n;
  }

  if (!started && sink.start(sink.self) != 0) {
    fprintf(stderr, "kurono: --tone: sink start failed\n");
    sink.close(sink.self);
    return -1;
  }

  /* Drain on the frame counter, not on a fixed sleep. pcm_ring is sized
     from PCM_LEAD_SECONDS but ring_init rounds the capacity up to a
     power of two, so at 48 kHz it actually holds ~2.7 s of the 3 s tone
     -- a fixed PCM_LEAD_SECONDS sleep would stop the device with most
     of a second still unplayed and make the run's own output ambiguous.
     Bounded by a wall-clock cap so a dead device cannot hang --tone. */
  long waited_ms = 0;
  while (atomic_load_explicit(&g_frames_consumed, memory_order_acquire)
           < (uint_least64_t) total_frames
         && waited_ms < 1000L * (TONE_DURATION_SECONDS + PCM_LEAD_SECONDS + 2)) {
    sleep_ms(20);
    waited_ms += 20;
  }

  sink.stop(sink.self);

  printf("underruns: %u\n",
         atomic_load_explicit(&g_underrun_count, memory_order_relaxed));

  sink.close(sink.self);
  return 0;
} /* sink_run_tone */
