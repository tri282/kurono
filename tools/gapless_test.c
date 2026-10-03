/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/gapless_test.c -- decodes split-chirp fixture pairs through the
   AudioSource API and checks each file's frame count and the chirp's
   alignment across the boundary. Links decoder.o and the ffmpeg
   libraries; calls no ffmpeg symbol itself.

   Usage: gapless_test [--report] [fixture-dir]   (default tools/fixtures) */

#include "../include/audio/decoder.h"
#include "../include/audio/format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SESSION_RATE          48000u
#define READ_CHUNK_FRAMES     1024      /* engine.c's fill-loop request size */
#define CHIRP_F0              200.0
#define CHIRP_F1              6000.0
#define LAG_WINDOW            8192
#define LAG_MAX               4096
#define LAG_MIN_CORR          0.5       /* below this the search found no peak */
#define UNTRIMMED_MAX_SURPLUS 16384L    /* untrimmed path: at most this many extra frames */
#define GAPLESS_RESAMPLE_TOL  0L        /* set in phase 2 from the phase 0 table */
#define MAX_FRAMES            131072L   /* both files of a pair plus two max surpluses */
#define PATH_MAX_LEN          1024

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

typedef enum { EXPECT_EXACT, EXPECT_RESAMPLED, EXPECT_UNTRIMMED } Expect;

typedef struct {
  const char * set;   /* <set>-a.<ext> and <set>-b.<ext>, or <set>.<ext> when n_b == 0 */
  const char * ext;
  unsigned     rate;  /* source rate */
  long         n_a;
  long         n_b;
  Expect       expect;
} Fixture;

static const Fixture g_fixtures[] = {
  { "mp3",        "mp3",  48000, 24337, 21111, EXPECT_EXACT },
  { "m4a",        "m4a",  48000, 24337, 21111, EXPECT_EXACT },
  { "opus",       "opus", 48000, 24337, 21111, EXPECT_EXACT },
  { "ogg",        "ogg",  48000, 24337, 21111, EXPECT_EXACT },
  { "mp3-44k",    "mp3",  44100, 22337, 19411, EXPECT_RESAMPLED },
  { "mp3-noxing", "mp3",  48000, 24337, 0,     EXPECT_UNTRIMMED },
  { "adts",       "aac",  48000, 24337, 0,     EXPECT_UNTRIMMED },
};

static float  g_out[MAX_FRAMES * 2];
static double g_ref[LAG_WINDOW];

/* decode_into: open path on src and decode it to EOF into g_out starting
   at frame offset at, in READ_CHUNK_FRAMES requests. Takes the source, the
   path, the offset, and out-params for the frame count and stream info.
   Returns 0 with *got set, or -1 after printing a FAIL line (missing file,
   negative read, or no room left in g_out). */
static int
decode_into(AudioSource * src, const char * path, long at, long * got,
            StreamInfo * info)
{
  int err = src->open(src->self, path, info);
  if (err < 0) {
    fprintf(stderr, "FAIL %s: open %d\n", path, err);
    return -1;
  }

  long total = 0;
  for (;;) {
    long room = MAX_FRAMES - at - total;
    long want = room < READ_CHUNK_FRAMES ? room : READ_CHUNK_FRAMES;
    if (want <= 0) {
      fprintf(stderr, "FAIL %s: more than %ld frames\n", path, MAX_FRAMES);
      src->close(src->self);
      return -1;
    }
    long n = src->read(src->self, g_out + (at + total) * 2, want);
    if (n < 0) {
      fprintf(stderr, "FAIL %s: read %ld\n", path, n);
      src->close(src->self);
      return -1;
    }
    total += n;
    if (n < want) {
      break;
    }
  }

  src->close(src->self);
  *got = total;
  return 0;
} /* decode_into */

/* chirp: the fixture signal at time t seconds, for a chirp of total length
   T seconds. Returns the sample value, matching gen_fixtures.sh. */
static double
chirp(double t, double T)
{
  return 0.5 * sin(2 * M_PI * (CHIRP_F0 * t + (CHIRP_F1 - CHIRP_F0) * t * t / (2 * T)));
} /* chirp */

/* find_lag: cross-correlate the left channel of g_out[0, len) against the
   reference chirp over a LAG_WINDOW window centred on frame center, for
   every lag in [-LAG_MAX, LAG_MAX]. Positive lag means the output is late.
   Returns the arg-max lag and stores its normalised correlation in *corr. */
static long
find_lag(long len, long center, double T, double * corr)
{
  long   k0 = center - LAG_WINDOW / 2;
  double rr = 0.0;
  for (long i = 0; i < LAG_WINDOW; i++) {
    g_ref[i] = chirp((double) (k0 + i) / SESSION_RATE, T);
    rr += g_ref[i] * g_ref[i];
  }

  long   best_lag = 0;
  double best = -2.0;
  for (long lag = -LAG_MAX; lag <= LAG_MAX; lag++) {
    if (k0 + lag < 0 || k0 + LAG_WINDOW - 1 + lag >= len) {
      continue;
    }
    double xr = 0.0;
    double xx = 0.0;
    for (long i = 0; i < LAG_WINDOW; i++) {
      double x = g_out[(k0 + i + lag) * 2];
      xr += x * g_ref[i];
      xx += x * x;
    }
    double c = (xx > 0.0 && rr > 0.0) ? xr / sqrt(xx * rr) : 0.0;
    if (c > best) {
      best = c;
      best_lag = lag;
    }
  }

  *corr = best;
  return best_lag;
} /* find_lag */

/* check_set: decode one fixture (pair) and assert, or in report mode
   print one table row. A is decoded at frame 0 and B at got_a on the same
   source, the engine's reuse pattern. Takes the source, the fixture
   directory, the fixture and the report flag. */
static void
check_set(AudioSource * src, const char * dir, const Fixture * fx, int report)
{
  char       pa[PATH_MAX_LEN];
  char       pb[PATH_MAX_LEN];
  StreamInfo ia;
  StreamInfo ib;
  long       got_a = 0;
  long       got_b = 0;
  int        pair  = fx->n_b > 0;

  if (pair) {
    snprintf(pa, sizeof pa, "%s/%s-a.%s", dir, fx->set, fx->ext);
    snprintf(pb, sizeof pb, "%s/%s-b.%s", dir, fx->set, fx->ext);
  } else {
    snprintf(pa, sizeof pa, "%s/%s.%s", dir, fx->set, fx->ext);
  }

  if (decode_into(src, pa, 0, &got_a, &ia) < 0 ||
      (pair && decode_into(src, pb, got_a, &got_b, &ib) < 0)) {
    g_failures++;
    return;
  }

  long   exp_a = lround(fx->n_a * 48000.0 / fx->rate);
  long   exp_b = lround(fx->n_b * 48000.0 / fx->rate);
  double T     = (double) (fx->n_a + fx->n_b) / fx->rate;
  long   len   = got_a + got_b;
  double corr_mid = 0.0;
  double corr_bnd = 0.0;
  long   lag_mid  = 0;
  long   lag_bnd  = 0;

  if (pair && fx->expect != EXPECT_UNTRIMMED) {
    lag_mid = find_lag(len, exp_a / 2, T, &corr_mid);
    lag_bnd = find_lag(len, exp_a, T, &corr_bnd);
  }

  if (report) {
    double cmin = corr_mid < corr_bnd ? corr_mid : corr_bnd;
    printf("%-11s %-6s %5u %6ld %6ld %+5ld %6ld %6ld %+5ld %+5ld %+5ld %6.3f %8lld\n",
           fx->set, ia.codec, fx->rate, fx->n_a, got_a, got_a - exp_a,
           fx->n_b, got_b, got_b - exp_b, lag_mid, lag_bnd, cmin,
           (long long) ia.duration_ms);
    return;
  }

  switch (fx->expect) {
  case EXPECT_EXACT:
    CHECK(got_a == fx->n_a, "%s: A count %ld, want %ld", fx->set, got_a, fx->n_a);
    CHECK(got_b == fx->n_b, "%s: B count %ld, want %ld", fx->set, got_b, fx->n_b);
    CHECK(lag_mid == 0, "%s: mid lag %ld, want 0", fx->set, lag_mid);
    CHECK(lag_bnd == 0, "%s: boundary lag %ld, want 0", fx->set, lag_bnd);
    CHECK(corr_mid >= LAG_MIN_CORR, "%s: mid corr %.3f", fx->set, corr_mid);
    CHECK(corr_bnd >= LAG_MIN_CORR, "%s: boundary corr %.3f", fx->set, corr_bnd);
    break;
  case EXPECT_RESAMPLED:
    CHECK(labs(got_a - exp_a) <= GAPLESS_RESAMPLE_TOL,
          "%s: A count %ld, expected %ld", fx->set, got_a, exp_a);
    CHECK(labs(got_b - exp_b) <= GAPLESS_RESAMPLE_TOL,
          "%s: B count %ld, expected %ld", fx->set, got_b, exp_b);
    CHECK(labs(lag_mid) <= 1, "%s: mid lag %ld", fx->set, lag_mid);
    CHECK(labs(lag_bnd) <= 1, "%s: boundary lag %ld", fx->set, lag_bnd);
    CHECK(corr_mid >= LAG_MIN_CORR, "%s: mid corr %.3f", fx->set, corr_mid);
    CHECK(corr_bnd >= LAG_MIN_CORR, "%s: boundary corr %.3f", fx->set, corr_bnd);
    break;
  case EXPECT_UNTRIMMED:
    CHECK(got_a >= fx->n_a && got_a <= fx->n_a + UNTRIMMED_MAX_SURPLUS,
          "%s: A count %ld, want %ld..+%ld", fx->set, got_a, fx->n_a,
          UNTRIMMED_MAX_SURPLUS);
    if (pair) {
      CHECK(got_b >= fx->n_b && got_b <= fx->n_b + UNTRIMMED_MAX_SURPLUS,
            "%s: B count %ld, want %ld..+%ld", fx->set, got_b, fx->n_b,
            UNTRIMMED_MAX_SURPLUS);
    }
    break;
  }
} /* check_set */

/* main: gapless_test [--report] [fixture-dir]. Report mode prints the
   table and asserts nothing; assert mode exits non-zero on any failure. */
int
main(int argc, char ** argv)
{
  int          report = 0;
  const char * dir    = "tools/fixtures";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--report") == 0) {
      report = 1;
    } else {
      dir = argv[i];
    }
  }

  SessionFormat f;
  memset(&f, 0, sizeof f);
  f.sample_rate = SESSION_RATE;
  f.channels    = 2;

  AudioSource src = decoder_create(&f);
  CHECK(src.self != NULL, "decoder_create failed");
  if (src.self == NULL) {
    return 1;
  }

  if (report) {
    printf("%-11s %-6s %5s %6s %6s %5s %6s %6s %5s %5s %5s %6s %8s\n",
           "set", "codec", "rate", "N_a", "got_a", "d_a", "N_b", "got_b",
           "d_b", "lagm", "lagb", "corr", "dur_ms_a");
  }
  for (size_t i = 0; i < sizeof g_fixtures / sizeof g_fixtures[0]; i++) {
    check_set(&src, dir, &g_fixtures[i], report);
  }
  decoder_destroy(&src);

  if (report) {
    printf("gapless_test: report only, nothing asserted\n");
    return 0;
  }
  if (g_failures == 0) {
    printf("gapless_test: ok\n");
    return 0;
  }
  printf("gapless_test: FAILED (%d)\n", g_failures);
  return 1;
} /* main */
