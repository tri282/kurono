/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/nullsink_test.c -- standalone test binary for NullSink in
   src/audio/sink.c (amendment 10, E3 and E4). Links sink.c, ring.c and
   trace.c under TSan; no device, no kurono process. Modes (a) and (b):
   T1 is the only pcm_ring producer, NullSink's T4 the only consumer.
   Mode (c) links engine.c for the park handshake (amendment 11).
   Mode (d) links engine.c for the steady-state headroom rule.

   Usage: nullsink_test [seconds]   (live-producer mode, default 10) */

#include "../include/audio/engine.h"
#include "../include/audio/format.h"
#include "../include/audio/ring.h"
#include "../include/audio/sink.h"
#include "../include/audio/trace.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
      fprintf(stderr, __VA_ARGS__);                             \
      fprintf(stderr, "\n");                                    \
      g_failures++;                                             \
    }                                                           \
  } while (0)

#define PRELOAD_PERIODS   20   /* K in E4 */
#define RUN_PERIODS       60   /* target N; the real N is read back (G5) */
#define LIVE_SECONDS      10   /* mode (b) default; long enough for M2 (G4) */
#define MAX_BLOCK_FRAMES  2048u
#define MAX_CHANNELS      2u   /* f32 stereo is pinned (format.h) */
#define LIVE_SEED         12345u
#define LIVE_DRY_PERIODS  5u   /* underruns per starve phase before refilling */
#define PARK_HOLD_MS      100  /* mode (c): park_sync must still be waiting after this */
#define PARK_LIMIT_MS     2000 /* mode (c): bound on every other wait */
#define LEAD_SETTLE_MS    1000 /* mode (d): >= 1 s of steady state before and after the seeks */
#define LEAD_SEEKS        10

static float g_buf[MAX_BLOCK_FRAMES * MAX_CHANNELS]; /* silence */

/* sleep_ns: nanosleep for ns nanoseconds, retrying on EINTR with the
   remainder. Takes the duration, returns nothing. */
static void
sleep_ns(long ns)
{
  struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
  while (nanosleep(&ts, &ts) != 0) {
  }
} /* sleep_ns */

/* now_s: CLOCK_MONOTONIC in seconds. Takes nothing, returns it. */
static double
now_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
} /* now_s */

/* period_ns: the length of one NullSink period, from the format open
   reported. Takes the format, returns nanoseconds. */
static long
period_ns(const SessionFormat * f)
{
  return (long) ((uint64_t) f->period_frames * 1000000000u / f->sample_rate);
} /* period_ns */

/* test_e4: mode (a), ruling 3 literal (E4, D-2). Preloads K periods,
   runs N > K periods with no producer, stops, and checks
   consumed + underrun_count * P == N * P plus pacing (E3) and the stop
   contract. Takes the open sink and its format, returns nothing. */
static void
test_e4(AudioSink * s, const SessionFormat * f)
{
  Ring *         r     = sink_pcm_ring();
  const uint64_t P     = f->period_frames;
  const uint64_t K     = PRELOAD_PERIODS;
  const size_t   fb    = f->channels * sizeof(float);
  size_t         want  = (size_t) (K * P) * fb;
  size_t         wrote = 0;

  while (wrote < want) {
    size_t chunk = want - wrote < sizeof(g_buf) ? want - wrote : sizeof(g_buf);
    size_t n     = ring_write(r, g_buf, chunk);
    if (n == 0) {
      break;
    }
    wrote += n;
  }
  CHECK(wrote == want, "preload wrote %zu of %zu bytes", wrote, want);

  CHECK(s->start(s->self) == 0, "start failed");
  CHECK(sink_is_started(s) == 1, "sink_is_started is 0 after start");
  CHECK(sink_callback_may_run(s) == 1, "sink_callback_may_run is 0 after start");
  sleep_ns(RUN_PERIODS * period_ns(f));
  s->stop(s->self);

  uint64_t N        = atomic_load_explicit(sink_cb_seq(), memory_order_acquire);
  uint64_t periods  = 0;
  long     late     = 0;
  int      pace_ok  = sink_null_pacing(&periods, &late);
  uint64_t consumed = atomic_load_explicit(sink_frames_consumed(), memory_order_acquire);
  unsigned u        = sink_underrun_count();

  CHECK(N == periods, "g_cb_seq %" PRIu64 " != pacing periods %" PRIu64, N, periods);
  CHECK(N > K, "N %" PRIu64 " not greater than K %" PRIu64, N, K);
  CHECK(consumed == K * P, "consumed %" PRIu64 ", expected %" PRIu64, consumed, K * P);
  CHECK(u == N - K, "underruns %u, expected %" PRIu64, u, N - K);
  CHECK(consumed + (uint64_t) u * P == N * P,
        "E4: consumed %" PRIu64 " + underruns %u * P != N %" PRIu64 " * P",
        consumed, u, N);
  CHECK(pace_ok, "E3 pacing out of band (late_max %ld ns)", late);
  CHECK(sink_is_started(s) == 0, "sink_is_started is 1 after stop");
  CHECK(sink_callback_may_run(s) == 0, "sink_callback_may_run is 1 after stop");
  CHECK(sink_cb_alive() == 0, "callback alive after stop");
  printf("e4: N=%" PRIu64 " K=%" PRIu64 " underruns=%u late_max=%ld ns\n",
         N, K, u, late);
} /* test_e4 */

/* test_live_conservation: mode (b), E2 against a live producer. T1
   alternates a fill phase (random whole-frame blocks until the ring is
   full) with a starve phase (no writes until T4 has underrun
   LIVE_DRY_PERIODS times), so T4 sees full reads, partial reads and
   underruns throughout. Then stops and checks every written frame was
   consumed or is still in the ring, plus pacing (E3) and the stop
   contract. Takes the open sink, its format and the run length, returns
   nothing. */
static void
test_live_conservation(AudioSink * s, const SessionFormat * f, double seconds)
{
  Ring *       r    = sink_pcm_ring();
  const size_t fb   = f->channels * sizeof(float);
  unsigned     seed = LIVE_SEED;
  uint64_t     p0   = 0;
  long         late = 0;
  uint64_t     written = 0;

  (void) sink_null_pacing(&p0, &late);
  uint64_t c0 = atomic_load_explicit(sink_frames_consumed(), memory_order_acquire);
  unsigned u0 = sink_underrun_count();
  CHECK(ring_fill(r) == 0, "ring not empty before mode (b): %zu bytes", ring_fill(r));
  printf("live: seed=%u seconds=%.1f\n", seed, seconds);

  CHECK(s->start(s->self) == 0, "start failed");
  double   t0       = now_s();
  int      starving = 0;
  unsigned u_starve = 0;
  unsigned dry      = 0; /* completed starve phases */
  while (now_s() - t0 < seconds) {
    if (starving) {
      if (sink_underrun_count() - u_starve >= LIVE_DRY_PERIODS) {
        starving = 0;
        dry++;
      } else {
        sleep_ns(period_ns(f));
      }
      continue;
    }
    size_t frames = 1 + (size_t) rand_r(&seed) % MAX_BLOCK_FRAMES;
    size_t n      = ring_write(r, g_buf, frames * fb);
    CHECK(n % fb == 0, "partial frame written: %zu bytes", n);
    written += n / fb;
    if (n < frames * fb) {
      starving = 1;
      u_starve = sink_underrun_count();
    } else {
      sleep_ns((long) (rand_r(&seed) % 4) * 1000000L);
    }
  }
  s->stop(s->self); /* T1 is producer and stopper: no write overlaps the join */

  uint64_t periods  = 0;
  int      pace_ok  = sink_null_pacing(&periods, &late);
  uint64_t consumed = atomic_load_explicit(sink_frames_consumed(), memory_order_acquire);
  uint64_t in_ring  = ring_fill(r) / fb;

  CHECK(written == (consumed - c0) + in_ring,
        "E2: written %" PRIu64 " != consumed %" PRIu64 " + in ring %" PRIu64,
        written, consumed - c0, in_ring);
  CHECK(pace_ok, "E3 pacing out of band (late_max %ld ns)", late);
  CHECK(sink_is_started(s) == 0, "sink_is_started is 1 after stop");
  CHECK(sink_cb_alive() == 0, "callback alive after stop");
  if (seconds >= LIVE_SECONDS) {
    CHECK(dry > 0, "ring never ran dry in %.1f s", seconds);
  }
  printf("live: periods=%" PRIu64 " underruns=%u dry_phases=%u written=%" PRIu64
         " late_max=%ld ns\n",
         periods - p0, sink_underrun_count() - u0, dry, written, late);
} /* test_live_conservation */

/* Gate: mode (c)'s AudioSource. open() and seek() block until the test
   opens the gate, so T2 can be held inside do_play or do_seek while its
   parked flag still reads 1. read() is endless silence. */
typedef struct {
  pthread_mutex_t mtx;
  pthread_cond_t  cond;
  int             inside; /* T2 is blocked in open or seek */
  int             open;   /* gate open: open and seek return */
} Gate;

static Gate g_gate = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0 };

/* gate_block: park the calling thread (T2) until the gate opens. Takes
   nothing, returns nothing. */
static void
gate_block(void)
{
  pthread_mutex_lock(&g_gate.mtx);
  g_gate.inside = 1;
  pthread_cond_broadcast(&g_gate.cond);
  while (!g_gate.open) {
    pthread_cond_wait(&g_gate.cond, &g_gate.mtx);
  }
  g_gate.inside = 0;
  pthread_mutex_unlock(&g_gate.mtx);
} /* gate_block */

/* gate_set: close (0) or open (1) the gate. Takes the new state,
   returns nothing. */
static void
gate_set(int open)
{
  pthread_mutex_lock(&g_gate.mtx);
  g_gate.open = open;
  pthread_cond_broadcast(&g_gate.cond);
  pthread_mutex_unlock(&g_gate.mtx);
} /* gate_set */

/* gate_wait_inside: wait up to PARK_LIMIT_MS for T2 to block in the
   gate. Takes nothing, returns 1 if it did, 0 on timeout. */
static int
gate_wait_inside(void)
{
  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += PARK_LIMIT_MS / 1000;
  pthread_mutex_lock(&g_gate.mtx);
  int rc = 0;
  while (!g_gate.inside && rc == 0) {
    rc = pthread_cond_timedwait(&g_gate.cond, &g_gate.mtx, &dl);
  }
  int inside = g_gate.inside;
  pthread_mutex_unlock(&g_gate.mtx);
  return inside;
} /* gate_wait_inside */

/* gsrc_open: AudioSource.open, blocking in the gate. Takes self (unused),
   the path (ignored) and the info to fill, returns 0. */
static int
gsrc_open(void * self, const char * path, StreamInfo * out_info)
{
  (void) self;
  (void) path;
  gate_block();
  if (out_info != NULL) {
    memset(out_info, 0, sizeof(*out_info));
    out_info->sample_rate = 48000;
    out_info->channels    = 2;
    snprintf(out_info->codec, sizeof(out_info->codec), "gate");
  }
  return 0;
} /* gsrc_open */

/* gsrc_read: AudioSource.read, endless silence. Takes self (unused), the
   destination and the frame count, returns the frame count. */
static long
gsrc_read(void * self, float * dst, long frames)
{
  (void) self;
  memset(dst, 0, (size_t) frames * MAX_CHANNELS * sizeof(float));
  return frames;
} /* gsrc_read */

/* gsrc_seek: AudioSource.seek, blocking in the gate. Takes self (unused)
   and the target, returns 0. */
static int
gsrc_seek(void * self, int64_t ms)
{
  (void) self;
  (void) ms;
  gate_block();
  return 0;
} /* gsrc_seek */

/* gsrc_nop: AudioSource.close and destroy; nothing to release. Takes
   self (unused), returns nothing. */
static void
gsrc_nop(void * self)
{
  (void) self;
} /* gsrc_nop */

static atomic_int g_park_done;

/* park_thread: call engine_park_sync from a second thread so the test
   can observe whether it returns. Takes the Engine, returns NULL. */
static void *
park_thread(void * arg)
{
  engine_park_sync((Engine *) arg);
  atomic_store_explicit(&g_park_done, 1, memory_order_release);
  return NULL;
} /* park_thread */

/* park_held: start park_thread while T2 is held in the gate, check it
   is still waiting after PARK_HOLD_MS, open the gate, and check it then
   returns with T2 paused. Takes the Engine and a label, returns nothing. */
static void
park_held(Engine * e, const char * what)
{
  pthread_t th;
  atomic_store_explicit(&g_park_done, 0, memory_order_relaxed);
  CHECK(pthread_create(&th, NULL, park_thread, e) == 0, "%s: pthread_create", what);
  sleep_ns(PARK_HOLD_MS * 1000000L);
  CHECK(atomic_load_explicit(&g_park_done, memory_order_acquire) == 0,
        "%s: engine_park_sync returned while T2 was inside the command", what);
  gate_set(1);
  for (int i = 0; i < PARK_LIMIT_MS
       && !atomic_load_explicit(&g_park_done, memory_order_acquire); i++) {
    sleep_ns(1000000L);
  }
  CHECK(atomic_load_explicit(&g_park_done, memory_order_acquire) == 1,
        "%s: engine_park_sync did not return after the command finished", what);
  pthread_join(th, NULL);
  CHECK(engine_state(e) == ENG_PAUSED, "%s: T2 not paused after park_sync", what);
} /* park_held */

/* test_park_ack: mode (c), amendment 11. Holds T2 inside a play and then
   a seek it popped while parked, and checks engine_park_sync waits for
   each to finish instead of returning on the stale parked flag. The sink
   is open but not started, so no T4 runs. Takes the open sink and its
   format, returns nothing. */
static void
test_park_ack(AudioSink * s, const SessionFormat * f)
{
  AudioSource src = { gsrc_open, gsrc_read, gsrc_seek, gsrc_nop, NULL, gsrc_nop, &g_gate };
  Engine *    e   = engine_create(f, src, s);
  if (e == NULL) {
    CHECK(0, "engine_create failed");
    return;
  }
  CHECK(engine_start(e) == 0, "engine_start failed");

  gate_set(0);
  engine_play(e, "gate", 1);
  CHECK(gate_wait_inside(), "T2 never entered open()");
  park_held(e, "play");

  gate_set(0);
  engine_seek(e, 1000);
  CHECK(gate_wait_inside(), "T2 never entered seek()");
  park_held(e, "seek");

  engine_quit(e);
  engine_join(e);
  engine_destroy(e);
  printf("park: play and seek held inside the gate\n");
} /* test_park_ack */

/* test_lead_headroom: mode (d), the #8 arming rule. Engine + NullSink +
   an endless fast source: play, settle, seek LEAD_SEEKS times, settle,
   stop, join. Steady state must exist and its low must stay above half
   the ring: every seek's empty-ring refill is excluded, so the only
   dips left are one chunk and one period. Takes the open sink and its
   format, returns nothing. */
static void
test_lead_headroom(AudioSink * s, const SessionFormat * f)
{
  AudioSource src = { gsrc_open, gsrc_read, gsrc_seek, gsrc_nop, NULL, gsrc_nop, &g_gate };
  Engine *    e;
  uint64_t    low = 0, steady = 0, cap = 0;
  unsigned    u = 0;

  gate_set(1);
  e = engine_create(f, src, s);
  if (e == NULL) {
    CHECK(0, "lead: engine_create failed");
    return;
  }
  CHECK(engine_start(e) == 0, "lead: engine_start failed");
  CHECK(s->start(s->self) == 0, "lead: sink start failed");

  engine_play(e, "lead", 2);
  sleep_ns(LEAD_SETTLE_MS * 1000000L);
  for (int i = 0; i < LEAD_SEEKS; i++) {
    engine_seek(e, (int64_t) i * 100);
  }
  sleep_ns(LEAD_SETTLE_MS * 1000000L);

  s->stop(s->self); /* IV.9.b step 2 before the join */
  engine_quit(e);
  engine_join(e);

  int armed = engine_lead_stats(e, &low, &steady, &u, &cap);
  CHECK(armed == 1, "lead: never armed");
  CHECK(steady > 0, "lead: lead_frames == 0");
  CHECK(low >= cap / 2, "lead: low %" PRIu64 " < cap/2 %" PRIu64, low, cap / 2);
  printf("lead: low=%" PRIu64 " cap=%" PRIu64 " steady=%" PRIu64 " underruns=%u\n",
         low, cap, steady, u);
  engine_destroy(e);
} /* test_lead_headroom */

/* main: open NullSink, run modes (a) to (d), tear down in
   IV.9.b order. Takes an optional mode (b) duration in seconds as
   argv[1], returns 0 if every check passed, 1 otherwise. */
int
main(int argc, char ** argv)
{
  double        seconds = LIVE_SECONDS;
  AudioSink     s;
  SessionFormat f = { 0 };

  if (argc > 1) {
    seconds = atof(argv[1]);
  }

  CHECK(trace_init(NULL) == 0, "trace_init failed");
  sink_get_null(&s);
  if (s.open(s.self, &f, NULL, NULL) != 0) {
    fprintf(stderr, "nullsink_test: FAILED (open)\n");
    trace_shutdown();
    return 1;
  }
  CHECK(f.period_frames == 900, "period_frames %" PRIu32, f.period_frames);
  CHECK(f.sample_rate == 48000, "sample_rate %" PRIu32, f.sample_rate);
  CHECK(f.channels == MAX_CHANNELS, "channels %" PRIu32, f.channels);

  test_e4(&s, &f);
  test_live_conservation(&s, &f, seconds);
  test_park_ack(&s, &f);
  test_lead_headroom(&s, &f);

  s.close(s.self);
  sink_free_rings();
  trace_shutdown();

  if (g_failures != 0) {
    fprintf(stderr, "nullsink_test: FAILED (%d)\n", g_failures);
    return 1;
  }
  printf("nullsink_test: ok\n");
  return 0;
} /* main */
