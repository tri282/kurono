/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/testsrc_test.c -- standalone test binary for src/audio/testsrc.c
   (DESIGN.md XII.2's testable tier). Pure, dependency-free,
   deterministic: links only against testsrc.c and libc. No ffmpeg, no
   device, no audio file -- which is the property the doubles exist to
   provide, so a test of them that needed any of those would be
   self-defeating.

   What is actually under test is the EOF arithmetic, and it is worth a
   binary of its own for one reason: AudioSource.read's three-valued
   contract (DESIGN.md VI) is the thing every caller of a double relies
   on, and a double that gets it subtly wrong does not fail loudly. It
   produces a clean run that proves nothing -- a short read reported one
   frame early reads as a track boundary that is not there, and the
   harness built on top of it would then "confirm" gapless behaviour
   that never happened. */

#include <stdio.h>
#include <string.h>

#include "../include/audio/testsrc.h"

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

#define RATE   48000u
#define CH     2u
#define BUFFRAMES 1024L

static float g_buf[BUFFRAMES * CH];

/* fmt48: the session format every case here runs at. */
static SessionFormat
fmt48(void)
{
  SessionFormat f;
  memset(&f, 0, sizeof(f));
  f.sample_rate = RATE;
  f.channels    = CH;
  return f;
} /* fmt48 */

/* test_eof_is_exact: a source of a known length must produce exactly
   that many frames, signal EOF by returning short exactly once, and
   return 0 forever after. Takes nothing, returns nothing. */
static void
test_eof_is_exact(void)
{
  SessionFormat f = fmt48();
  /* 100 ms at 48 kHz is 4800 frames -- deliberately not a multiple of
     the 1024-frame read, so the final read is a genuine short read and
     not an exact-fit boundary that would hide an off-by-one. */
  AudioSource s = testsrc_create(&f, TESTSRC_SILENCE, 100, 0);
  CHECK(s.self != NULL, "testsrc_create returned a null source");
  if (s.self == NULL) {
    return;
  }

  StreamInfo info;
  CHECK(s.open(s.self, "ignored", &info) == 0, "open must succeed");
  CHECK(info.duration_ms == 100, "duration_ms reported %lld, expected 100",
        (long long) info.duration_ms);
  CHECK(info.sample_rate == (int) RATE, "sample_rate reported %d", info.sample_rate);

  long total = 0;
  int  shorts = 0;
  for (int i = 0; i < 16; i++) {
    long n = s.read(s.self, g_buf, BUFFRAMES);
    CHECK(n >= 0, "read returned %ld -- silence must never fault", n);
    if (n < BUFFRAMES) {
      shorts++;
      total += (n > 0) ? n : 0;
      break;
    }
    total += n;
  }
  CHECK(total == 4800, "produced %ld frames, expected 4800", total);
  CHECK(shorts == 1, "expected exactly one short read, saw %d", shorts);

  /* Past EOF, forever, and still not an error: EOF is not a failure. */
  for (int i = 0; i < 3; i++) {
    CHECK(s.read(s.self, g_buf, BUFFRAMES) == 0, "reads past EOF must return 0");
  }

  s.close(s.self);
  s.destroy(s.self);
} /* test_eof_is_exact */

/* test_endless_never_ends: duration 0 means the source never reaches
   EOF, which is what a ring or callback stress needs. Takes nothing,
   returns nothing. */
static void
test_endless_never_ends(void)
{
  SessionFormat f = fmt48();
  AudioSource s = testsrc_create(&f, TESTSRC_SINE, 0, 0);
  if (s.self == NULL) {
    CHECK(0, "testsrc_create returned a null source");
    return;
  }
  CHECK(s.open(s.self, NULL, NULL) == 0, "open must accept a NULL path and NULL info");

  for (int i = 0; i < 64; i++) {
    CHECK(s.read(s.self, g_buf, BUFFRAMES) == BUFFRAMES,
          "an endless source must always fill the block (read %d)", i);
  }

  /* The sine must actually be a sine: silence here would mean the
     kind switch fell through, and every listening test built on it
     would be listening to nothing and calling it a pass. */
  int nonzero = 0;
  for (long i = 0; i < BUFFRAMES * (long) CH; i++) {
    if (g_buf[i] != 0.0f) {
      nonzero = 1;
      break;
    }
  }
  CHECK(nonzero, "TESTSRC_SINE produced pure silence");

  s.close(s.self);
  s.destroy(s.self);
} /* test_endless_never_ends */

/* test_fault_is_negative_and_sticky: the injector must produce whole
   blocks up to and past its fault point, then a negative return, and
   must never recover -- the branch it exists to reach is the one that
   has to survive a source that stays broken. Takes nothing, returns
   nothing. */
static void
test_fault_is_negative_and_sticky(void)
{
  SessionFormat f = fmt48();
  AudioSource s = testsrc_create(&f, TESTSRC_FAULT, 0, 1500);
  if (s.self == NULL) {
    CHECK(0, "testsrc_create returned a null source");
    return;
  }
  CHECK(s.open(s.self, "ignored", NULL) == 0, "open must succeed");

  long n = s.read(s.self, g_buf, BUFFRAMES);
  CHECK(n == BUFFRAMES, "the first block is before the fault, got %ld", n);

  /* The fault lands mid-block: the block must still come back whole,
     since a short read is EOF to the engine and would hide the fault
     (amendment 10 D-16). */
  n = s.read(s.self, g_buf, BUFFRAMES);
  CHECK(n == BUFFRAMES, "the block spanning the fault must be whole, got %ld", n);

  for (int i = 0; i < 3; i++) {
    CHECK(s.read(s.self, g_buf, BUFFRAMES) < 0,
          "read %d after the fault must be negative and stay negative", i);
  }

  s.close(s.self);
  s.destroy(s.self);
} /* test_fault_is_negative_and_sticky */

/* test_fault_at_zero_is_immediate: --fault 0 is a legal harness
   argument (amendment 10), so a fault point of zero must fault on the
   very first read, with no frames produced. Takes nothing, returns
   nothing. */
static void
test_fault_at_zero_is_immediate(void)
{
  SessionFormat f = fmt48();
  AudioSource s = testsrc_create(&f, TESTSRC_FAULT, 0, 0);
  if (s.self == NULL) {
    CHECK(0, "testsrc_create returned a null source");
    return;
  }
  CHECK(s.open(s.self, "ignored", NULL) == 0, "open must succeed");
  CHECK(s.read(s.self, g_buf, BUFFRAMES) < 0,
        "a fault at frame 0 must fault on the first read");

  s.close(s.self);
  s.destroy(s.self);
} /* test_fault_at_zero_is_immediate */

/* test_seek_moves_position: seek is the only position this source has,
   so a seek forward must shorten what is left before EOF by exactly
   the amount seeked. Takes nothing, returns nothing. */
static void
test_seek_moves_position(void)
{
  SessionFormat f = fmt48();
  AudioSource s = testsrc_create(&f, TESTSRC_SILENCE, 1000, 0); /* 48000 frames */
  if (s.self == NULL) {
    CHECK(0, "testsrc_create returned a null source");
    return;
  }
  CHECK(s.open(s.self, "ignored", NULL) == 0, "open must succeed");
  CHECK(s.seek(s.self, 900) == 0, "seek must succeed on an open source");

  /* 100 ms left = 4800 frames. */
  long total = 0;
  for (int i = 0; i < 16; i++) {
    long n = s.read(s.self, g_buf, BUFFRAMES);
    CHECK(n >= 0, "read faulted after a seek: %ld", n);
    total += (n > 0) ? n : 0;
    if (n < BUFFRAMES) {
      break;
    }
  }
  CHECK(total == 4800, "after seeking to 900 ms, %ld frames remained, expected 4800",
        total);

  /* Seeking past the end is a legal request that leaves nothing to
     read, not an error. */
  CHECK(s.seek(s.self, 5000) == 0, "a seek past the end must still succeed");
  CHECK(s.read(s.self, g_buf, BUFFRAMES) == 0, "nothing remains past the end");

  s.close(s.self);
  s.destroy(s.self);
} /* test_seek_moves_position */

/* test_closed_source_reads_eof: reading a source that was never opened,
   or one that has been closed, is EOF and never a fault -- the engine
   parks on EOF and raises EV_DECODE_ERROR on a negative, so getting
   this backwards would turn an ordinary teardown into a reported decode
   failure. Takes nothing, returns nothing. */
static void
test_closed_source_reads_eof(void)
{
  SessionFormat f = fmt48();
  AudioSource s = testsrc_create(&f, TESTSRC_SILENCE, 1000, 0);
  if (s.self == NULL) {
    CHECK(0, "testsrc_create returned a null source");
    return;
  }

  CHECK(s.read(s.self, g_buf, BUFFRAMES) == 0, "an unopened source reads EOF");
  CHECK(s.open(s.self, "ignored", NULL) == 0, "open must succeed");
  CHECK(s.read(s.self, g_buf, BUFFRAMES) == BUFFRAMES, "an open source reads");
  s.close(s.self);
  CHECK(s.read(s.self, g_buf, BUFFRAMES) == 0, "a closed source reads EOF");

  /* Reopening restarts rather than continuing. */
  CHECK(s.open(s.self, "ignored", NULL) == 0, "reopen must succeed");
  CHECK(s.read(s.self, g_buf, BUFFRAMES) == BUFFRAMES, "reopened source reads");

  s.close(s.self);
  s.destroy(s.self);
} /* test_closed_source_reads_eof */

/* test_retarget_changes_the_clock: retarget exists for the device
   reopen ladder. Changing the rate must change how many frames a given
   duration is worth, or the ladder would be driven against a source
   that silently ignored the new format. Takes nothing, returns nothing. */
static void
test_retarget_changes_the_clock(void)
{
  SessionFormat f = fmt48();
  AudioSource s = testsrc_create(&f, TESTSRC_SILENCE, 100, 0);
  if (s.self == NULL) {
    CHECK(0, "testsrc_create returned a null source");
    return;
  }
  CHECK(s.retarget != NULL, "the double must offer retarget");
  CHECK(s.retarget(s.self, 44100u) == 0, "retarget must succeed");
  CHECK(s.open(s.self, "ignored", NULL) == 0, "open must succeed");

  /* 100 ms at 44.1 kHz is 4410 frames, not 4800. */
  long total = 0;
  for (int i = 0; i < 16; i++) {
    long n = s.read(s.self, g_buf, BUFFRAMES);
    total += (n > 0) ? n : 0;
    if (n < BUFFRAMES) {
      break;
    }
  }
  CHECK(total == 4410, "after retarget to 44100, produced %ld frames, expected 4410",
        total);

  s.close(s.self);
  s.destroy(s.self);
} /* test_retarget_changes_the_clock */

int
main(void)
{
  test_eof_is_exact();
  test_endless_never_ends();
  test_fault_is_negative_and_sticky();
  test_fault_at_zero_is_immediate();
  test_seek_moves_position();
  test_closed_source_reads_eof();
  test_retarget_changes_the_clock();

  if (g_failures != 0) {
    fprintf(stderr, "testsrc_test: FAILED (%d)\n", g_failures);
    return 1;
  }
  printf("testsrc_test: ok\n");
  return 0;
} /* main */
