/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/nullsink_test.c -- standalone test binary for NullSink in
   src/audio/sink.c (amendment 10, E3 and E4). Links sink.c, ring.c and
   trace.c under TSan; no engine, no device, no kurono process. T1 is
   the only pcm_ring producer, NullSink's T4 the only consumer.

   Usage: nullsink_test [seconds]   (live-producer mode, default 10) */

#include "../include/audio/format.h"
#include "../include/audio/ring.h"
#include "../include/audio/sink.h"
#include "../include/audio/trace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

/* main: open NullSink, run mode (a) then mode (b), tear down in
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
