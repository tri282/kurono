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
   `cap` term. The `+ MAX_BLOCK` is the producer's in-flight block: the
   read is not atomic against the writer, so one block may land between
   the previous read's snapshot and its return, and that block is
   reported by the *next* read rather than being lost.

   That second term was missing until 2026-08-20, and its absence is a
   textbook example of why a duration-bounded stress test proves less
   than it looks like it does. `--latest 10` passed every time; the
   failure needs the producer to both fill the ring completely and land
   an extra block between two reads, which is rare per iteration and
   near-certain over a minute. Phase 1 accepted this harness on a 10 s
   run. `--latest 60` fails on it reproducibly, with the true match
   sitting 206 and 911 bytes past the old limit on the two runs that
   were measured -- both well inside one MAX_BLOCK, which is what
   identifies the missing term rather than merely widening until green.

   A window that is too tight reports a ring defect that is not there,
   which is the worse direction to be wrong in on this particular file:
   ring.c holds the project's rank-1 invariant (DESIGN.md XVI risk 1)
   and a false positive here costs a day of looking for a memory-
   ordering bug that does not exist. */
static void *
consumer_thread_latest(void * arg_v)
{
  ThreadArgs * args     = (ThreadArgs *) arg_v;
  Shared *     sh       = args->shared;
  unsigned int seed     = args->seed;
  uint64_t     read_pos = 0;
  uint8_t      buf[MAX_BLOCK];
  uint64_t     window = (uint64_t) sh->ring->cap + MAX_BLOCK;

  for (;;) {
    size_t n   = pick_block_size(&seed);
    size_t got = (n == 0) ? 0 : ring_read_latest(sh->ring, buf, n);

    if (got > 0) {
      uint64_t idx0  = UINT64_MAX;
      uint64_t limit = read_pos + window;

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
        fprintf(stderr,
                "ring_read_latest: no contiguous suffix match in [%" PRIu64 ", %" PRIu64 "]\n",
                read_pos, limit);
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
