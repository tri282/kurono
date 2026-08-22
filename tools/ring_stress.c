/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/ring_stress.c -- standalone producer/consumer stress harness for
   audio/ring.c (DESIGN.md XII.3). Dependency-free by design: ring.c,
   pthread, and libc only. Compiles without the Makefile, which is why it
   can precede phase 2.

   Two modes:

   1. Default: one producer thread and one consumer thread hammer a Ring
      with randomised block sizes for a fixed duration (argv[1] seconds,
      default 5). The logical byte stream is a deterministic counter
      sequence (payload_byte()), so the consumer detects reorder,
      duplication, and loss BY VALUE, not merely by count -- a ring that
      duplicated one block and dropped another would still pass a pure
      byte-count check.

   2. `--latest [seconds]`: exercises ring_read_latest() instead of
      ring_read(). The consumer verifies that every chunk it receives is
      a contiguous suffix of the logical stream, and that the implied
      read position never moves backwards.

   Exit code 0 and no output but the final PASS line means success. Any
   mismatch prints the first failing index (or a diagnostic for --latest)
   to stderr and exits non-zero. */

#include "../include/audio/ring.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING_CAP_BYTES ((size_t) 1 << 16) /* 64 KiB -- ample headroom */
#define MAX_BLOCK      8192u              /* per phase-1 spec: rand_r in [1, 8192] */
#define ZERO_CHANCE    32u                /* 1-in-32 chance of a zero-length block */
#define HASH_MULT      2654435761u        /* Knuth multiplicative hash constant */

/* Shared, read-mostly state visible to both threads. Only one field is
   ever written after threads start (producer_done, by the producer), so
   it alone is atomic. */
typedef struct {
  Ring *      ring;
  atomic_bool producer_done;
  double      duration_sec;
  /* Total bytes the producer has published, monotonic. Read only by the
     --latest consumer, to bound its search by where the writer actually
     is rather than by a guess at how far ahead it can get. */
  atomic_uint_fast64_t written;
} Shared;

typedef struct {
  Shared *     shared;
  unsigned int seed; /* rand_r state, private to this thread */
} ThreadArgs;

/* payload_byte: the deterministic value of byte `idx` of the logical
   stream. Pure function of the absolute index, per phase-1 spec: any
   reorder, duplication, or loss changes the byte a given index maps to
   and is therefore caught by comparison, not just by counting bytes. */
static uint8_t
payload_byte(uint64_t idx)
{
  uint32_t v = (uint32_t) idx * HASH_MULT;
  return (uint8_t) (v >> 24);
} /* payload_byte */

/* pick_block_size: a random block length in [1, MAX_BLOCK], or 0 with
   probability 1/ZERO_CHANCE -- exercises both sides' empty-request
   paths, per phase-1 spec. */
static size_t
pick_block_size(unsigned int * seed)
{
  if ((unsigned int) rand_r(seed) % ZERO_CHANCE == 0) {
    return 0;
  }
  return 1 + ((size_t) rand_r(seed) % MAX_BLOCK);
} /* pick_block_size */

/* short_sleep: a brief, bounded pause used only to back off when the
   ring is momentarily full or empty. Not modelling the real backpressure
   policy (DESIGN.md IV.5 uses a condvar) -- this harness only hammers
   the cursors. */
static void
short_sleep(void)
{
  struct timespec ts;
  ts.tv_sec  = 0;
  ts.tv_nsec = 200000; /* 200 us */
  nanosleep(&ts, NULL);
} /* short_sleep */

/* elapsed_seconds: wall-clock seconds since `start`, via CLOCK_MONOTONIC
   so it is immune to time-of-day adjustments mid-run. */
static double
elapsed_seconds(const struct timespec * start)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double sec  = (double) (now.tv_sec - start->tv_sec);
  double nsec = (double) (now.tv_nsec - start->tv_nsec) / 1e9;
  return sec + nsec;
} /* elapsed_seconds */

/* producer_thread: writes the deterministic payload sequence into the
   ring for shared->duration_sec seconds, then sets producer_done and
   returns. Used by both harness modes -- ring_read_latest's producer
   side is identical to ring_read's. */
static void *
producer_thread(void * arg_v)
{
  ThreadArgs * args = (ThreadArgs *) arg_v;
  Shared *     sh   = args->shared;
  unsigned int seed = args->seed;
  uint64_t     idx  = 0;
  uint8_t      block_buf[MAX_BLOCK];

  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);

  while (elapsed_seconds(&start) < sh->duration_sec) {
    size_t block = pick_block_size(&seed);
    for (size_t k = 0; k < block; k++) {
      block_buf[k] = payload_byte(idx + k);
    }

    /* Published BEFORE the bytes are written, which is the only order
       that makes this counter usable as a bound.

       Publishing it after -- either per block or per partial write --
       looks more natural and is wrong: the producer's ring_write makes
       bytes readable before the store lands, so a consumer that reads
       in that gap holds data newer than the counter it then loads. The
       consumer would compute an upper bound BELOW what it legitimately
       received and report a defect that is not there. Measured, not
       reasoned: chunks ending 1121 and 6154 bytes past the counter.

       Published first, the value can only ever OVERSTATE what is
       readable, by at most one block. An upper bound that is too
       generous costs a slightly wider search; one that is too tight
       costs a false failure on the project's rank-1 invariant. */
    atomic_store_explicit(&sh->written, (uint_fast64_t) (idx + block),
                          memory_order_release);

    size_t off = 0;
    while (off < block) {
      size_t n = ring_write(sh->ring, block_buf + off, block - off);
      off += n;
      if (n == 0) {
        short_sleep();
      }
    }
    idx += block;
  }

  atomic_store_explicit(&sh->producer_done, true, memory_order_release);
  return NULL;
} /* producer_thread */

/* consumer_thread: default-mode reader. Reads randomised blocks with
   ring_read(), verifies every byte against payload_byte() at its
   absolute stream index, and exits(1) with the first mismatching index
   printed to stderr on failure. Runs until the producer is done and the
   ring has drained. */
static void *
consumer_thread(void * arg_v)
{
  ThreadArgs * args = (ThreadArgs *) arg_v;
  Shared *     sh   = args->shared;
  unsigned int seed = args->seed;
  uint64_t     idx  = 0;
  uint8_t      block_buf[MAX_BLOCK];

  for (;;) {
    size_t block = pick_block_size(&seed);
    size_t got   = (block == 0) ? 0 : ring_read(sh->ring, block_buf, block);

    for (size_t k = 0; k < got; k++) {
      uint8_t expected = payload_byte(idx + k);
      if (block_buf[k] != expected) {
        fprintf(stderr,
                "MISMATCH at absolute index %" PRIu64 ": expected 0x%02x, got 0x%02x\n",
                idx + k, expected, block_buf[k]);
        exit(1);
      }
    }
    idx += got;

    if (got == 0) {
      bool done = atomic_load_explicit(&sh->producer_done, memory_order_acquire);
      if (done && ring_fill(sh->ring) == 0) {
        break;
      }
      short_sleep();
    }
  }
  return NULL;
} /* consumer_thread */

/* consumer_thread_latest: --latest mode reader. Reads randomised blocks
   with ring_read_latest() and verifies two properties without any
   access to Ring internals beyond the public `cap` field:

   1. The returned bytes are a contiguous suffix of the logical stream:
      there exists a starting index idx0 such that buf[k] ==
      payload_byte(idx0 + k) for every k in the chunk.
   2. idx0 is never less than the previous chunk's end (read_pos never
      moves backwards) -- enforced structurally by only ever searching
      idx0 at or after read_pos, the value this same thread mirrors
      exactly (it is the ring's sole consumer of viz_ring-style state).

   The search window is bounded by the ring's capacity plus one block,
   not by elapsed time or bytes produced. ring_read_latest drains the
   ring, so `read_pos` below is the writer's position at the moment of
   the previous read; the producer can then write a full `cap` before it
   blocks on a full ring (it retries rather than dropping), which is the
   ordering is checked against where the writer actually is, not against
   a guess at how far ahead of the reader it can get.

   THAT GUESS IS WHY THIS CHECK HAS BEEN WRONG TWICE. It was originally
   `cap`; on 2026-08-20 a `+ MAX_BLOCK` term was added for the producer's
   in-flight block, after `--latest 60` failed with the true match 206
   and 911 bytes past the old limit. That fix was real but treated a
   symptom. On 2026-08-22 CI failed on both distros at `--latest 20`,
   and the true match was 1528 to 2136 bytes past the widened limit --
   reproducible locally under `taskset -c 0`, which is what a contended
   shared runner looks like.

   The actual defect is in how the position was recovered, not in how
   wide the window was. payload_byte() maps an index to ONE byte, so a
   short chunk has many false matches: for got == 1 a wrong candidate
   turns up within ~256 bytes essentially always, and the loop takes the
   FIRST match it finds. read_pos then advances a few hundred bytes when
   the writer has moved tens of thousands, and that lag is permanent and
   cumulative. Any fixed window is eventually exceeded; widening it only
   buys iterations. Both earlier failures were this, at different stages
   of the same drift.

   So the window is no longer guessed. The producer publishes its byte
   count, and the consumer loads it AFTER the read returns. The chunk it
   holds must end at or before that value, so its start lies in
   [read_pos, written - got] -- a range that is correct by construction
   under any scheduling, and that still contains the true position even
   when a previous iteration took a false match. A short-chunk
   ambiguity now self-heals: the next chunk long enough to be unique
   snaps read_pos back to the truth.

   It is also a STRONGER check than before. A fixed window could not
   tell "the writer got further ahead than I assumed" from "the ring
   returned something it should not have"; anchoring to the writer means
   data claiming to be past the writer's own position is now a failure
   rather than something a wider window would hide.

   A window that is too tight reports a ring defect that is not there,
   which is the worse direction to be wrong in on this particular file:
   ring.c holds the project's rank-1 invariant (DESIGN.md XVI risk 1)
   and a false positive here costs a day of looking for a memory-
   ordering bug that does not exist. Both times, that is exactly what it
   cost. */
static void *
consumer_thread_latest(void * arg_v)
{
  ThreadArgs * args     = (ThreadArgs *) arg_v;
  Shared *     sh       = args->shared;
  unsigned int seed     = args->seed;
  uint64_t     read_pos = 0;
  uint8_t      buf[MAX_BLOCK];

  for (;;) {
    size_t n   = pick_block_size(&seed);
    size_t got = (n == 0) ? 0 : ring_read_latest(sh->ring, buf, n);

    if (got > 0) {
      /* Loaded after the read, so every byte just returned is covered by
         it. The chunk ends at or before `written`, so it starts at or
         before `written - got`. */
      uint64_t written = (uint64_t) atomic_load_explicit(&sh->written,
                                                         memory_order_acquire);
      uint64_t idx0  = UINT64_MAX;

      if (written < (uint64_t) got) {
        fprintf(stderr,
                "ring_read_latest: returned %zu bytes but only %" PRIu64
                " have been written\n", got, written);
        exit(1);
      }
      uint64_t limit = written - (uint64_t) got;

      if (limit < read_pos) {
        fprintf(stderr,
                "ring_read_latest: chunk ends at or before read_pos "
                "(read_pos %" PRIu64 ", written %" PRIu64 ", got %zu)\n",
                read_pos, written, got);
        exit(1);
      }

      for (uint64_t cand = read_pos; cand <= limit; cand++) {
        if (payload_byte(cand) != buf[0]) {
          continue;
        }
        bool ok = true;
        for (size_t k = 1; k < got; k++) {
          if (payload_byte(cand + k) != buf[k]) {
            ok = false;
            break;
          }
        }
        if (ok) {
          idx0 = cand;
          break;
        }
      }

      if (idx0 == UINT64_MAX) {
        /* No position in the legal range produces these bytes. Unlike
           the old fixed window, this range cannot be too small, so this
           is a ring defect rather than a harness assumption. */
        fprintf(stderr,
                "ring_read_latest: %zu bytes match no position in "
                "[%" PRIu64 ", %" PRIu64 "] (writer at %" PRIu64 ")\n",
                got, read_pos, limit, written);
        exit(1);
      }
      if (idx0 < read_pos) {
        fprintf(stderr,
                "ring_read_latest: read_pos moved backwards (was %" PRIu64 ", got %" PRIu64 ")\n",
                read_pos, idx0);
        exit(1);
      }
      read_pos = idx0 + got;
    }

    if (got == 0) {
      bool done = atomic_load_explicit(&sh->producer_done, memory_order_acquire);
      if (done && ring_fill(sh->ring) == 0) {
        break;
      }
      short_sleep();
    }
  }
  return NULL;
} /* consumer_thread_latest */

/* main: parse `--latest [seconds]` or `[seconds]`, run the producer and
   one consumer variant to completion, and report PASS. Any failure exits
   non-zero from inside a worker thread (see consumer_thread{,_latest}). */
int
main(int argc, char ** argv)
{
  bool latest_mode = false;
  int  arg_i        = 1;

  if (argc > 1 && strcmp(argv[1], "--latest") == 0) {
    latest_mode = true;
    arg_i       = 2;
  }

  double duration = 5.0;
  if (argc > arg_i) {
    duration = atof(argv[arg_i]);
  }

  Ring ring;
  if (ring_init(&ring, RING_CAP_BYTES) != 0) {
    fprintf(stderr, "ring_init failed\n");
    return 1;
  }

  Shared shared;
  shared.ring         = &ring;
  shared.duration_sec = duration;
  atomic_init(&shared.producer_done, false);
  atomic_init(&shared.written, (uint_fast64_t) 0);

  ThreadArgs producer_args = { .shared = &shared, .seed = 12345u };
  ThreadArgs consumer_args = { .shared = &shared, .seed = 67890u };

  pthread_t producer_tid;
  pthread_t consumer_tid;

  if (pthread_create(&producer_tid, NULL, producer_thread, &producer_args) != 0) {
    fprintf(stderr, "pthread_create(producer) failed\n");
    return 1;
  }

  void * (*consumer_fn)(void *) = latest_mode ? consumer_thread_latest : consumer_thread;
  if (pthread_create(&consumer_tid, NULL, consumer_fn, &consumer_args) != 0) {
    fprintf(stderr, "pthread_create(consumer) failed\n");
    return 1;
  }

  pthread_join(producer_tid, NULL);
  pthread_join(consumer_tid, NULL);

  ring_free(&ring);

  printf("ring_stress%s: PASS (%.1fs)\n", latest_mode ? " --latest" : "", duration);
  return 0;
} /* main */
