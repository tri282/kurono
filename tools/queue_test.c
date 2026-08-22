/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/queue_test.c -- standalone test binary for src/queue.c
   (DESIGN.md XII.2, phase-9-queue.md I.6). Pure, dependency-free,
   deterministic: links only against queue.c and libc.

   Every appended/inserted entry gets a globally unique track_id (a
   monotonic serial, never reused, even across queue_remove) and a path
   derived from it. Since queue.c is opaque from here (Queue is an
   incomplete type -- there is no way to peek at its internal storage
   index or permutation array), this file treats "each live serial
   appears in queue_at(0..count) exactly once, matching the known live
   set" as the externally observable proxy for the internal invariant
   phase-9-queue.md I.4 states directly: "the permutation is always a
   bijection over [0, N)". If that internal bijection ever breaks --
   duplicated, lost, or aliased storage indices -- it manifests exactly
   as a duplicated, lost, or wrong track_id here, so the proxy check
   catches the same class of bug the internal statement describes.

   The fuzz harness (fuzz_run) additionally mirrors the fully
   deterministic parts of play order (insert/remove/move on `perm`
   never involve randomness) in its own `play` array, so it can assert
   *exact* agreement after every insert/remove/move/cursor op, not just
   structural properties -- this is what actually exercises the
   remove/move fixup logic the phase file calls out as the fiddliest
   code in queue.c. Only genuinely random events (shuffle-on, and a
   forward REPEAT_ALL wrap while shuffled) are verified structurally
   (multiset-preserving, prefix-preserving, self-consistent between
   queue_peek_advance and queue_advance) and then the driver resyncs its
   `play` mirror from the real queue before continuing.

   A second driver-maintained mirror, `base_mirror`, tracks the
   *arranged* order (the order queue_set_shuffle(q, 0) restores) by the
   same maintenance rules src/queue.c applies to its own internal
   base[] array (phase-9-queue.md I.4, round 2). This is the one that
   matters most: the first version of this file compared only a
   *multiset* of live ids ("in any order") plus an elementwise `play`
   check that, on shuffle-off, asserted against *append* order -- the
   exact same wrong assumption the round-1 implementation bug made. A
   self-confirming oracle: the test and the bug agreed, so 10,000 fuzz
   ops and every discrete case passed. `base_mirror` is tracked
   independently of append order (see case 1/3's shuffle-state-aware
   updates below), so the shuffle-off assertion in fuzz_run's case 5 is
   now an actual, independent check of the round trip, not a restatement
   of the bug. */

#include "../include/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                         \
  do {                                                                                            \
    if (!(cond)) {                                                                                \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                        \
      fprintf(stderr, __VA_ARGS__);                                                                \
      fprintf(stderr, "\n");                                                                       \
      g_failures++;                                                                                \
    }                                                                                              \
  } while (0)

/* path_for: deterministic path string for a given serial track_id. */
static void
path_for(int64_t id, char * buf, size_t bufsz)
{
  snprintf(buf, bufsz, "track-%lld.flac", (long long) id);
} /* path_for */

/* append_serial: append a fresh, uniquely-serialled entry. Returns the
   serial used (the caller's *next_id is post-incremented). */
static int64_t
append_serial(Queue * q, int64_t * next_id)
{
  int64_t id = (*next_id)++;
  char    buf[32];
  path_for(id, buf, sizeof(buf));
  int rc = queue_append(q, id, buf);
  CHECK(rc == 0, "queue_append(serial %lld) failed", (long long) id);
  return id;
} /* append_serial */

/* bijection_proxy_check: assert queue_at(0..count) is exactly the
   caller-supplied set of `expected` track_ids, each exactly once, in
   any order. This is the externally observable form of "perm is a
   bijection over [0, N)" -- see the file header. */
static void
bijection_proxy_check(const Queue * q, const int64_t * expected, size_t n_expected, const char * where)
{
  size_t n = queue_count(q);
  CHECK(n == n_expected, "%s: count=%zu expected=%zu", where, n, n_expected);

  for (size_t i = 0; i < n; i++) {
    const QueueEntry * e = queue_at(q, i);
    CHECK(e != NULL, "%s: queue_at(%zu) returned NULL within count", where, i);
    if (e == NULL) {
      continue;
    }
    int seen_in_expected = 0;
    for (size_t k = 0; k < n_expected; k++) {
      if (expected[k] == e->track_id) {
        seen_in_expected = 1;
        break;
      }
    }
    CHECK(seen_in_expected, "%s: queue_at(%zu)->track_id=%lld not in expected set", where, i,
          (long long) e->track_id);

    size_t dup = 0;
    for (size_t j = 0; j < n; j++) {
      if (queue_at(q, j)->track_id == e->track_id) {
        dup++;
      }
    }
    CHECK(dup == 1, "%s: track_id=%lld appears %zu times in play order", where,
          (long long) e->track_id, dup);
  }
} /* bijection_proxy_check */

/* ---------------------------------------------------------------------
 * Discrete cases, phase-9-queue.md I.6
 * --------------------------------------------------------------------- */

static void
test_append_insert_remove_move_clear_oob(void)
{
  Queue * q = queue_create(1u);
  CHECK(q != NULL, "queue_create failed");

  int64_t next_id = 1;
  int64_t a = append_serial(q, &next_id);
  int64_t b = append_serial(q, &next_id);
  int64_t c = append_serial(q, &next_id);
  CHECK(queue_count(q) == 3, "count after 3 appends");
  CHECK(queue_at(q, 0)->track_id == a && queue_at(q, 1)->track_id == b &&
            queue_at(q, 2)->track_id == c,
        "insertion order after plain appends");

  /* insert in the middle */
  char buf[32];
  int64_t d = next_id++;
  path_for(d, buf, sizeof(buf));
  CHECK(queue_insert(q, 1, d, buf) == 0, "insert at 1");
  CHECK(queue_at(q, 0)->track_id == a && queue_at(q, 1)->track_id == d &&
            queue_at(q, 2)->track_id == b && queue_at(q, 3)->track_id == c,
        "order after insert at 1");

  /* out-of-bounds insert leaves the queue unchanged */
  CHECK(queue_insert(q, 99, 999, "x") != 0, "insert OOB must fail");
  CHECK(queue_count(q) == 4, "count unchanged after failed insert");

  /* out-of-bounds remove/move leave the queue unchanged */
  CHECK(queue_remove(q, 99) != 0, "remove OOB must fail");
  CHECK(queue_count(q) == 4, "count unchanged after failed remove");
  CHECK(queue_move(q, 0, 99) != 0, "move OOB (to) must fail");
  CHECK(queue_move(q, 99, 0) != 0, "move OOB (from) must fail");
  CHECK(queue_count(q) == 4, "count unchanged after failed move");

  /* move c (idx 3) to the front */
  CHECK(queue_move(q, 3, 0) == 0, "move 3->0");
  CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a &&
            queue_at(q, 2)->track_id == d && queue_at(q, 3)->track_id == b,
        "order after move 3->0");

  /* remove the entry now at idx 2 (d) */
  CHECK(queue_remove(q, 2) == 0, "remove idx 2");
  CHECK(queue_count(q) == 3, "count after remove");
  CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a &&
            queue_at(q, 2)->track_id == b,
        "order after remove idx 2");

  queue_clear(q);
  CHECK(queue_count(q) == 0, "count after clear");
  CHECK(queue_cursor(q) == 0, "cursor after clear");

  queue_destroy(q);
  printf("test_append_insert_remove_move_clear_oob: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_append_insert_remove_move_clear_oob */

static void
test_cursor_traversal_all_modes(void)
{
  Queue * q = queue_create(2u);
  int64_t next_id = 1;
  int64_t ids[4];
  for (int i = 0; i < 4; i++) {
    ids[i] = append_serial(q, &next_id);
  }

  /* REPEAT_OFF: advance walks 0->1->2->3->NULL, cursor stays at 3 */
  queue_set_repeat(q, REPEAT_OFF);
  CHECK(queue_cursor(q) == 0, "starts at 0");
  for (int i = 1; i < 4; i++) {
    const QueueEntry * e = queue_advance(q);
    CHECK(e != NULL && e->track_id == ids[i], "REPEAT_OFF advance step %d", i);
  }
  CHECK(queue_advance(q) == NULL, "REPEAT_OFF advance past end returns NULL");
  CHECK(queue_cursor(q) == 3, "REPEAT_OFF cursor unmoved past end");

  /* REPEAT_ALL: wraps back to 0 */
  queue_set_cursor(q, 3);
  queue_set_repeat(q, REPEAT_ALL);
  const QueueEntry * wrapped = queue_advance(q);
  CHECK(wrapped != NULL && wrapped->track_id == ids[0], "REPEAT_ALL wraps to 0");
  CHECK(queue_cursor(q) == 0, "REPEAT_ALL cursor at 0 after wrap");

  /* REPEAT_ONE: advance keeps returning the same entry */
  queue_set_cursor(q, 1);
  queue_set_repeat(q, REPEAT_ONE);
  for (int i = 0; i < 3; i++) {
    const QueueEntry * e = queue_advance(q);
    CHECK(e != NULL && e->track_id == ids[1], "REPEAT_ONE stays at cursor, rep %d", i);
    CHECK(queue_cursor(q) == 1, "REPEAT_ONE cursor unmoved, rep %d", i);
  }

  /* backward traversal via skip_prev, REPEAT_OFF stops at 0 */
  queue_set_cursor(q, 1);
  queue_set_repeat(q, REPEAT_OFF);
  const QueueEntry * back = queue_skip_prev(q);
  CHECK(back != NULL && back->track_id == ids[0], "skip_prev to 0");
  CHECK(queue_skip_prev(q) == NULL, "skip_prev at 0 under REPEAT_OFF returns NULL");
  CHECK(queue_cursor(q) == 0, "cursor stays at 0");

  /* skip_prev wraps under REPEAT_ALL */
  queue_set_repeat(q, REPEAT_ALL);
  const QueueEntry * wrapped_back = queue_skip_prev(q);
  CHECK(wrapped_back != NULL && wrapped_back->track_id == ids[3], "skip_prev wraps to last");

  queue_destroy(q);
  printf("test_cursor_traversal_all_modes: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_cursor_traversal_all_modes */

static void
test_shuffle_identity(void)
{
  Queue * q = queue_create(42u);
  int64_t next_id = 1;
  int64_t ids[8];
  for (int i = 0; i < 8; i++) {
    ids[i] = append_serial(q, &next_id);
  }

  queue_set_cursor(q, 2);
  int64_t current_before = queue_at(q, 2)->track_id;

  queue_set_shuffle(q, 1);
  CHECK(queue_shuffle(q) == 1, "shuffle reports on");
  CHECK(queue_at(q, queue_cursor(q))->track_id == current_before,
        "current entry unchanged immediately after shuffle-on");
  /* prefix [0, cursor] must be untouched */
  for (size_t i = 0; i <= 2; i++) {
    CHECK(queue_at(q, i)->track_id == ids[i], "prefix entry %zu unchanged by shuffle-on", i);
  }

  queue_set_shuffle(q, 0);
  CHECK(queue_shuffle(q) == 0, "shuffle reports off");
  for (int i = 0; i < 8; i++) {
    CHECK(queue_at(q, (size_t) i)->track_id == ids[i], "insertion order restored exactly, idx %d", i);
  }
  CHECK(queue_at(q, queue_cursor(q))->track_id == current_before,
        "cursor repositioned onto the entry that was playing");

  queue_destroy(q);
  printf("test_shuffle_identity: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_shuffle_identity */

static void
test_remove_current_and_last(void)
{
  Queue * q = queue_create(7u);
  int64_t next_id = 1;
  int64_t a = append_serial(q, &next_id);
  (void) append_serial(q, &next_id); /* b: only its removal matters below */
  int64_t c = append_serial(q, &next_id);
  (void) a;

  /* remove the currently playing entry (cursor must stay valid) */
  queue_set_cursor(q, 1); /* b is current */
  CHECK(queue_remove(q, 1) == 0, "remove current entry");
  CHECK(queue_cursor(q) < queue_count(q), "cursor valid after removing current entry");
  CHECK(queue_at(q, queue_cursor(q))->track_id == c, "cursor now on the entry that followed");

  queue_destroy(q);

  /* remove the last entry while the cursor is on it */
  q       = queue_create(8u);
  next_id = 1;
  int64_t x = append_serial(q, &next_id);
  (void) append_serial(q, &next_id); /* the last entry, made current below */
  queue_set_cursor(q, 1); /* the last entry is current */
  CHECK(queue_remove(q, 1) == 0, "remove last entry while cursor is on it");
  CHECK(queue_count(q) == 1, "count after removing the last entry");
  CHECK(queue_cursor(q) == 0, "cursor clamped to the new last valid index");
  CHECK(queue_at(q, 0)->track_id == x, "remaining entry is the one before it");

  queue_destroy(q);
  printf("test_remove_current_and_last: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_remove_current_and_last */

static void
test_negative_track_id_roundtrip(void)
{
  Queue * q = queue_create(9u);
  CHECK(queue_append(q, -1, "not-in-library/foo.mp3") == 0, "append with track_id -1");
  const QueueEntry * e = queue_at(q, 0);
  CHECK(e != NULL, "entry present");
  if (e != NULL) {
    CHECK(e->track_id == -1, "track_id round-trips as -1");
    CHECK(e->path != NULL && strcmp(e->path, "not-in-library/foo.mp3") == 0, "path round-trips intact");
  }
  queue_destroy(q);
  printf("test_negative_track_id_roundtrip: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_negative_track_id_roundtrip */

static void
test_repeat_one_skip_next_traverses(void)
{
  Queue * q = queue_create(11u);
  int64_t next_id = 1;
  int64_t ids[5];
  for (int i = 0; i < 5; i++) {
    ids[i] = append_serial(q, &next_id);
  }

  queue_set_repeat(q, REPEAT_ONE);
  queue_set_cursor(q, 0);

  /* the trap case: REPEAT_ONE + queue_skip_next repeatedly must
     traverse the whole queue, never stall on the current entry */
  for (int lap = 0; lap < 2; lap++) {
    for (int i = 1; i <= 5; i++) {
      const QueueEntry * e = queue_skip_next(q);
      int                expect_idx = i % 5;
      CHECK(e != NULL && e->track_id == ids[expect_idx],
            "lap %d step %d: skip_next under REPEAT_ONE must move on", lap, i);
    }
  }

  queue_destroy(q);
  printf("test_repeat_one_skip_next_traverses: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_repeat_one_skip_next_traverses */

static void
test_peek_agrees_with_advance(void)
{
  RepeatMode modes[3] = { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

  for (int m = 0; m < 3; m++) {
    for (int shuffled = 0; shuffled <= 1; shuffled++) {
      Queue * q = queue_create(100u + (unsigned) m * 10u + (unsigned) shuffled);
      int64_t next_id = 1;
      int64_t ids[6];
      for (int i = 0; i < 6; i++) {
        ids[i] = append_serial(q, &next_id);
      }
      (void) ids;

      queue_set_repeat(q, modes[m]);
      if (shuffled) {
        queue_set_shuffle(q, 1);
      }

      /* interior position: peek must not move the cursor, and must
         agree with the following advance */
      queue_set_cursor(q, 2);
      size_t              cursor_before = queue_cursor(q);
      const QueueEntry *  peeked        = queue_peek_advance(q);
      CHECK(queue_cursor(q) == cursor_before, "peek must not move the cursor (interior)");
      const QueueEntry * advanced = queue_advance(q);
      if (peeked == NULL || advanced == NULL) {
        CHECK(peeked == advanced, "peek/advance both NULL or both non-NULL (interior)");
      } else {
        CHECK(peeked->track_id == advanced->track_id, "peek agrees with advance (interior), mode=%d shuffled=%d",
              m, shuffled);
      }
      queue_destroy(q);

      /* boundary: cursor on the last entry */
      q       = queue_create(200u + (unsigned) m * 10u + (unsigned) shuffled);
      next_id = 1;
      for (int i = 0; i < 6; i++) {
        append_serial(q, &next_id);
      }
      queue_set_repeat(q, modes[m]);
      if (shuffled) {
        queue_set_shuffle(q, 1);
      }
      queue_set_cursor(q, queue_count(q) - 1);

      cursor_before = queue_cursor(q);
      peeked        = queue_peek_advance(q);
      CHECK(queue_cursor(q) == cursor_before, "peek must not move the cursor (boundary)");
      advanced = queue_advance(q);
      if (peeked == NULL || advanced == NULL) {
        CHECK(peeked == advanced, "peek/advance both NULL or both non-NULL (boundary), mode=%d shuffled=%d", m,
              shuffled);
      } else {
        CHECK(peeked->track_id == advanced->track_id, "peek agrees with advance (boundary), mode=%d shuffled=%d",
              m, shuffled);
      }
      queue_destroy(q);
    }
  }

  printf("test_peek_agrees_with_advance: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_peek_agrees_with_advance */

static void
test_remove_move_while_shuffled(void)
{
  Queue * q = queue_create(55u);
  int64_t next_id = 1;
  int64_t ids[7];
  for (int i = 0; i < 7; i++) {
    ids[i] = append_serial(q, &next_id);
  }

  queue_set_cursor(q, 2);
  queue_set_shuffle(q, 1);

  int64_t current_id = queue_at(q, queue_cursor(q))->track_id;

  /* remove an entry after the cursor */
  size_t after_idx = queue_count(q) - 1;
  if (after_idx > queue_cursor(q)) {
    /* Play position count-1 is an arbitrary entry once shuffled, not the
       highest serial, so the survivor set has to be derived from the entry
       actually removed rather than assumed to be ids[0..5]. */
    int64_t removed_id  = queue_at(q, after_idx)->track_id;
    int64_t survivors[7];
    size_t  n_survivors = 0;
    for (int i = 0; i < 7; i++) {
      if (ids[i] != removed_id) {
        survivors[n_survivors++] = ids[i];
      }
    }
    CHECK(n_survivors == 6, "exactly one entry removed from the expected set");
    CHECK(queue_remove(q, after_idx) == 0, "remove entry after cursor while shuffled");
    CHECK(queue_at(q, queue_cursor(q))->track_id == current_id,
          "current entry unchanged after removing an entry after it");
    bijection_proxy_check(q, survivors, n_survivors, "after remove-after (shuffled)");
  }

  /* remove an entry before the cursor */
  if (queue_cursor(q) > 0) {
    CHECK(queue_remove(q, 0) == 0, "remove entry before cursor while shuffled");
    CHECK(queue_at(q, queue_cursor(q))->track_id == current_id,
          "current entry unchanged after removing an entry before it");
  }

  /* remove the currently playing entry */
  size_t cur = queue_cursor(q);
  CHECK(queue_remove(q, cur) == 0, "remove currently playing entry while shuffled");
  CHECK(queue_cursor(q) < queue_count(q), "cursor stays valid after removing current, shuffled");

  /* move while shuffled */
  if (queue_count(q) >= 2) {
    int64_t moved_id = queue_at(q, 0)->track_id;
    CHECK(queue_move(q, 0, queue_count(q) - 1) == 0, "move while shuffled");
    CHECK(queue_at(q, queue_count(q) - 1)->track_id == moved_id, "moved entry landed at target position");
  }

  queue_destroy(q);
  printf("test_remove_move_while_shuffled: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_remove_move_while_shuffled */

static void
test_repeat_all_wrap_shuffled_different_permutation(void)
{
  /* Fixed seed -> deterministic and assertable. */
  Queue * q = queue_create(777u);
  int64_t next_id = 1;
  for (int i = 0; i < 10; i++) {
    append_serial(q, &next_id);
  }

  queue_set_repeat(q, REPEAT_ALL);
  queue_set_shuffle(q, 1);
  queue_set_cursor(q, queue_count(q) - 1);

  int64_t pass1[10];
  for (int i = 0; i < 10; i++) {
    pass1[i] = queue_at(q, (size_t) i)->track_id;
  }

  const QueueEntry * w1 = queue_advance(q); /* triggers the first wrap+reshuffle */
  CHECK(w1 != NULL, "first wrap produced an entry");

  int64_t pass2[10];
  for (int i = 0; i < 10; i++) {
    pass2[i] = queue_at(q, (size_t) i)->track_id;
  }

  int identical = 1;
  for (int i = 0; i < 10; i++) {
    if (pass1[i] != pass2[i]) {
      identical = 0;
      break;
    }
  }
  CHECK(!identical, "REPEAT_ALL wrap while shuffled produces a different permutation (fixed seed)");

  /* run the queue out to the second wrap and check again for good measure */
  for (int i = 0; i < 9; i++) {
    queue_advance(q);
  }
  int64_t pass2_snapshot[10];
  memcpy(pass2_snapshot, pass2, sizeof(pass2));
  queue_advance(q); /* second wrap */
  int64_t pass3[10];
  for (int i = 0; i < 10; i++) {
    pass3[i] = queue_at(q, (size_t) i)->track_id;
  }
  int identical2 = 1;
  for (int i = 0; i < 10; i++) {
    if (pass2_snapshot[i] != pass3[i]) {
      identical2 = 0;
      break;
    }
  }
  CHECK(!identical2, "second REPEAT_ALL wrap also reshuffles (fixed seed)");

  queue_destroy(q);
  printf("test_repeat_all_wrap_shuffled_different_permutation: %s\n",
         g_failures == 0 ? "ok" : "FAILED so far");
} /* test_repeat_all_wrap_shuffled_different_permutation */

/* ---------------------------------------------------------------------
 * Round-2 AMENDED discrete cases (phase-9-queue.md I.6, round 2):
 * insert-at-position and unshuffled-move must survive a shuffle
 * round trip; a shuffled move must not; backward wrap must not
 * reshuffle. These are the direct, single-scenario regression tests
 * for the two round-2 fixes, clearer on failure than a fuzz seed.
 * --------------------------------------------------------------------- */

static void
test_shuffle_round_trip_preserves_insert_position(void)
{
  Queue * q = queue_create(31u);
  int64_t next_id = 1;
  int64_t a = append_serial(q, &next_id);
  int64_t b = append_serial(q, &next_id);

  /* insert C at a non-tail position: play order becomes C A B */
  char    buf[32];
  int64_t c = next_id++;
  path_for(c, buf, sizeof(buf));
  CHECK(queue_insert(q, 0, c, buf) == 0, "insert C at 0");
  CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a && queue_at(q, 2)->track_id == b,
        "order is C A B before any shuffle");

  queue_set_shuffle(q, 1);
  queue_set_shuffle(q, 0);

  CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a && queue_at(q, 2)->track_id == b,
        "insert-at-position survives a shuffle-on/shuffle-off round trip");

  queue_destroy(q);
  printf("test_shuffle_round_trip_preserves_insert_position: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_shuffle_round_trip_preserves_insert_position */

static void
test_move_survives_round_trip_only_when_unshuffled(void)
{
  /* queue_move with shuffle OFF, then shuffle on, then off: the move survives. */
  {
    Queue * q = queue_create(32u);
    int64_t next_id = 1;
    int64_t a = append_serial(q, &next_id);
    int64_t b = append_serial(q, &next_id);
    int64_t c = append_serial(q, &next_id);

    CHECK(queue_move(q, 2, 0) == 0, "move C (idx 2) to 0 while unshuffled");
    CHECK(queue_at(q, 0)->track_id == c, "order is C A B after the unshuffled move");

    queue_set_shuffle(q, 1);
    queue_set_shuffle(q, 0);

    CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a && queue_at(q, 2)->track_id == b,
          "a move made while unshuffled survives a shuffle-on/shuffle-off round trip");
    queue_destroy(q);
  }

  /* queue_move with shuffle ON, then shuffle off: the move does NOT survive. */
  {
    Queue * q = queue_create(33u);
    int64_t next_id = 1;
    int64_t a = append_serial(q, &next_id);
    int64_t b = append_serial(q, &next_id);
    int64_t c = append_serial(q, &next_id);

    queue_set_shuffle(q, 1);
    CHECK(queue_move(q, 2, 0) == 0, "move idx 2 to 0 while shuffled");
    queue_set_shuffle(q, 0);

    CHECK(queue_at(q, 0)->track_id == a && queue_at(q, 1)->track_id == b && queue_at(q, 2)->track_id == c,
          "a move made while shuffled does NOT survive shuffle-off -- arranged order is untouched");

    queue_destroy(q);
  }

  printf("test_move_survives_round_trip_only_when_unshuffled: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_move_survives_round_trip_only_when_unshuffled */

/*
 * test_remove_base_duplicate_search_hazard: round-3 regression for the
 * defect found on session review of round 2's `base` fix. queue_remove
 * used to locate the removed entry's slot in base[] by searching for
 * its value AFTER the storage-index fixup loop had already run. That
 * fixup decrements every base[] value greater than the removed storage
 * index, which can transiently create a DUPLICATE of that index -- the
 * live entry that held storage_idx + 1 now also reads storage_idx.
 * Searching by value at that point can find the wrong slot (a live
 * entry's, not the removed one's) whenever the duplicate is
 * non-adjacent, and the result still looks like a valid bijection
 * afterward -- nothing downstream can detect it structurally, only an
 * order-aware check against a known-correct expected order can.
 *
 * The exact repro: append A, B, C (storage 0, 1, 2); move C to the
 * front while unshuffled (base becomes [2,0,1], play order C A B);
 * shuffle on (base frozen at [2,0,1]); remove B (storage index 1,
 * whose live successor -- C, now storage index 1 after B's removal --
 * sits at base position 0, *before* B's own base position 2). The old
 * code's post-fixup search returned position 0 (C's slot) instead of
 * position 2 (B's), leaving the arranged order reversed (A, C instead
 * of the correct C, A) with no structural signal that anything broke.
 */
static void
test_remove_base_duplicate_search_hazard(void)
{
  Queue * q = queue_create(41u);
  int64_t next_id = 1;
  int64_t a = append_serial(q, &next_id);
  int64_t b = append_serial(q, &next_id);
  int64_t c = append_serial(q, &next_id);

  CHECK(queue_move(q, 2, 0) == 0, "move C to the front while unshuffled");
  CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a && queue_at(q, 2)->track_id == b,
        "play order is C A B after the move");

  queue_set_shuffle(q, 1); /* base frozen at the arranged order [C, A, B] */

  /* find B's current play-order position (shuffle may have moved it,
     though with only one entry after the cursor there is nothing to
     shuffle here -- located by value regardless, to not assume that) */
  size_t b_idx = queue_count(q);
  for (size_t i = 0; i < queue_count(q); i++) {
    if (queue_at(q, i)->track_id == b) {
      b_idx = i;
      break;
    }
  }
  CHECK(b_idx < queue_count(q), "B is present before removal");

  CHECK(queue_remove(q, b_idx) == 0, "remove B while shuffled");
  CHECK(queue_count(q) == 2, "count after removing B");

  queue_set_shuffle(q, 0); /* restore the arranged order with B gone */

  CHECK(queue_at(q, 0)->track_id == c && queue_at(q, 1)->track_id == a,
        "arranged order after removing B is C, A -- not reversed to A, C");

  queue_destroy(q);
  printf("test_remove_base_duplicate_search_hazard: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_remove_base_duplicate_search_hazard */

static void
test_skip_prev_backward_wrap_no_reshuffle(void)
{
  Queue * q = queue_create(34u);
  int64_t next_id = 1;
  for (int i = 0; i < 6; i++) {
    append_serial(q, &next_id);
  }

  queue_set_repeat(q, REPEAT_ALL);
  queue_set_shuffle(q, 1);
  queue_set_cursor(q, 0);

  int64_t before[6];
  for (int i = 0; i < 6; i++) {
    before[i] = queue_at(q, (size_t) i)->track_id;
  }

  const QueueEntry * landed = queue_skip_prev(q);

  int64_t after[6];
  for (int i = 0; i < 6; i++) {
    after[i] = queue_at(q, (size_t) i)->track_id;
  }

  int unchanged = 1;
  for (int i = 0; i < 6; i++) {
    if (before[i] != after[i]) {
      unchanged = 0;
      break;
    }
  }
  CHECK(unchanged, "backward wrap while shuffled leaves the permutation unchanged");
  CHECK(landed != NULL && landed->track_id == before[5],
        "backward wrap lands on the entry at the last play-order position");
  CHECK(queue_cursor(q) == 5, "cursor is at the last position after the backward wrap");

  queue_destroy(q);
  printf("test_skip_prev_backward_wrap_no_reshuffle: %s\n", g_failures == 0 ? "ok" : "FAILED so far");
} /* test_skip_prev_backward_wrap_no_reshuffle */

/* ---------------------------------------------------------------------
 * Fuzz harness: 10,000 random operations against a driver-maintained
 * reference model, with the bijection-proxy check run after every
 * single operation (not just at the end) -- phase-9-queue.md I.6 AMENDED.
 * --------------------------------------------------------------------- */

#define FUZZ_OPS      10000
#define FUZZ_MAX_LIVE 64

typedef struct {
  int64_t ids[FUZZ_MAX_LIVE]; /* live count is capped at FUZZ_MAX_LIVE below */
  size_t  n;
} IdArray;

static void
ida_insert(IdArray * a, size_t at, int64_t id)
{
  for (size_t i = a->n; i > at; i--) {
    a->ids[i] = a->ids[i - 1];
  }
  a->ids[at] = id;
  a->n++;
} /* ida_insert */

static void
ida_remove_at(IdArray * a, size_t idx)
{
  for (size_t i = idx; i + 1 < a->n; i++) {
    a->ids[i] = a->ids[i + 1];
  }
  a->n--;
} /* ida_remove_at */

static void
ida_remove_value(IdArray * a, int64_t id)
{
  for (size_t i = 0; i < a->n; i++) {
    if (a->ids[i] == id) {
      ida_remove_at(a, i);
      return;
    }
  }
} /* ida_remove_value */

static void
ida_move(IdArray * a, size_t from, size_t to)
{
  int64_t v = a->ids[from];
  if (from < to) {
    for (size_t i = from; i < to; i++) {
      a->ids[i] = a->ids[i + 1];
    }
  } else {
    for (size_t i = from; i > to; i--) {
      a->ids[i] = a->ids[i - 1];
    }
  }
  a->ids[to] = v;
} /* ida_move */

static size_t
ida_index_of(const IdArray * a, int64_t id)
{
  for (size_t i = 0; i < a->n; i++) {
    if (a->ids[i] == id) {
      return i;
    }
  }
  return (size_t) -1;
} /* ida_index_of */

/* resync_play: adopt the real queue's current play order into `play`
   (used after a genuinely random event -- shuffle-on or a shuffled
   wrap -- that the driver cannot predict exactly). */
static void
resync_play(const Queue * q, IdArray * play)
{
  play->n = queue_count(q);
  for (size_t i = 0; i < play->n; i++) {
    play->ids[i] = queue_at(q, i)->track_id;
  }
} /* resync_play */

/*
 * fuzz_check_consistency: run after every fuzz op. `play` is the
 * driver's ordered mirror of the real play order -- kept exactly
 * accurate at all times (either predicted deterministically, or
 * resynced immediately after a genuinely random event such as
 * shuffle-on or a forward shuffled wrap), so both an elementwise
 * ordered comparison against it AND a set/multiset comparison
 * (bijection_proxy_check) are meaningful on every single op, not just
 * while shuffle is off -- stronger than the phase file's stated
 * minimum ("track the exact order while shuffle is off... fall back to
 * the multiset check while shuffle is on"), which this satisfies as a
 * special case. `base_mirror`'s correctness is checked separately, at
 * the moment shuffle actually toggles off (see fuzz_run case 5) --
 * that is the one assertion that would have caught the round-trip
 * defect this file's header describes.
 */
static void
fuzz_check_consistency(const Queue * q, const IdArray * play, unsigned seed, long op)
{
  CHECK(queue_count(q) == play->n, "seed=%u op=%ld: count mismatch (queue=%zu mirror=%zu)", seed, op,
        queue_count(q), play->n);

  bijection_proxy_check(q, play->ids, play->n, "fuzz");

  for (size_t i = 0; i < play->n; i++) {
    const QueueEntry * e = queue_at(q, i);
    CHECK(e != NULL && e->track_id == play->ids[i],
          "seed=%u op=%ld: play-order mismatch at %zu (queue=%lld mirror=%lld)", seed, op, i,
          e ? (long long) e->track_id : -1, (long long) play->ids[i]);
  }
} /* fuzz_check_consistency */

static void
fuzz_run(unsigned seed)
{
  Queue * q = queue_create(seed);
  CHECK(q != NULL, "fuzz: queue_create failed");

  unsigned drive_seed = seed ^ 0xA5A5A5A5u; /* separate stream from the queue's own rng_seed */
  int64_t  next_id    = 1;

  /* play: mirrors the real, current play order (perm) at all times.
   * base_mirror: mirrors the real *arranged* order (base) -- the order
   * shuffle-off restores -- maintained by the exact same rules
   * src/queue.c applies to its own base[] (phase-9-queue.md I.4, round
   * 2's maintenance table). This is the piece the first implementation
   * of this harness did not have: it asserted shuffle-off against
   * append order, which was wrong in exactly the same way the
   * production bug was wrong, so it could not catch it. */
  IdArray play        = { .n = 0 };
  IdArray base_mirror  = { .n = 0 };
  int64_t current_id  = -1;

  for (long op = 0; op < FUZZ_OPS; op++) {
    int choice = (int) (rand_r(&drive_seed) % 9);

    switch (choice) {
    case 0: { /* append */
      if (play.n >= FUZZ_MAX_LIVE) {
        break;
      }
      int64_t id = append_serial(q, &next_id);
      size_t  at = play.n; /* append == insert at play.n */
      ida_insert(&play, at, id);
      /* append always lands at base's tail too, regardless of shuffle
       * state (matches src/queue.c: append == insert at count, and
       * insert's shuffled-tail rule and unshuffled-at-`at` rule agree
       * at at==count). */
      ida_insert(&base_mirror, base_mirror.n, id);
      if (play.n == 1) {
        current_id = id;
      }
      break;
    }
    case 1: { /* insert at a random play-order position */
      if (play.n >= FUZZ_MAX_LIVE) {
        break;
      }
      size_t  at = (play.n == 0) ? 0 : (size_t) (rand_r(&drive_seed) % (play.n + 1));
      char    buf[32];
      int64_t id = next_id++;
      path_for(id, buf, sizeof(buf));
      int shuffled_now = queue_shuffle(q);
      int rc            = queue_insert(q, at, id, buf);
      CHECK(rc == 0, "seed=%u op=%ld: queue_insert failed", seed, op);
      ida_insert(&play, at, id);
      /* base_mirror: at `at` while unshuffled (base == play then), at
       * the tail while shuffled -- mirrors src/queue.c's queue_insert. */
      if (shuffled_now) {
        ida_insert(&base_mirror, base_mirror.n, id);
      } else {
        ida_insert(&base_mirror, at, id);
      }
      if (play.n == 1) {
        current_id = id;
      }
      break;
    }
    case 2: { /* remove at a random play-order position */
      if (play.n == 0) {
        break;
      }
      size_t  idx        = (size_t) (rand_r(&drive_seed) % play.n);
      int64_t removed_id = play.ids[idx];
      int     rc         = queue_remove(q, idx);
      CHECK(rc == 0, "seed=%u op=%ld: queue_remove failed", seed, op);
      ida_remove_at(&play, idx);
      /* base_mirror: remove by value, same as src/queue.c's base[]
       * fixup -- regardless of shuffle state, an entry that is gone is
       * gone from the arranged order too. */
      ida_remove_value(&base_mirror, removed_id);
      if (removed_id == current_id) {
        /* unpredictable exactly (depends on play order); adopt the
           observed result as the new oracle value */
        current_id = (queue_count(q) > 0) ? queue_at(q, queue_cursor(q))->track_id : -1;
      }
      break;
    }
    case 3: { /* move */
      if (play.n < 2) {
        break;
      }
      size_t from         = (size_t) (rand_r(&drive_seed) % play.n);
      size_t to           = (size_t) (rand_r(&drive_seed) % play.n);
      int    shuffled_now = queue_shuffle(q);
      int    rc           = queue_move(q, from, to);
      CHECK(rc == 0, "seed=%u op=%ld: queue_move failed", seed, op);
      if (from != to) {
        ida_move(&play, from, to);
        /* base_mirror: the move applies only while unshuffled -- a
         * move made mid-shuffle must not survive shuffle-off. */
        if (!shuffled_now) {
          ida_move(&base_mirror, from, to);
        }
      }
      break;
    }
    case 4: { /* set_cursor */
      if (play.n == 0) {
        break;
      }
      size_t idx = (size_t) (rand_r(&drive_seed) % play.n);
      int    rc  = queue_set_cursor(q, idx);
      CHECK(rc == 0, "seed=%u op=%ld: queue_set_cursor failed", seed, op);
      current_id = play.ids[idx];
      break;
    }
    case 5: { /* toggle shuffle */
      int on  = (int) (rand_r(&drive_seed) % 2);
      int was = queue_shuffle(q);
      queue_set_shuffle(q, on);
      if (on && !was) {
        /* structural: prefix unchanged, current unchanged. base_mirror
         * needs no update -- it stays whatever it already was, exactly
         * like the real base[], which is frozen the instant shuffle
         * turns on. */
        size_t cur = queue_cursor(q);
        for (size_t i = 0; i <= cur && i < play.n; i++) {
          CHECK(queue_at(q, i)->track_id == play.ids[i],
                "seed=%u op=%ld: shuffle-on disturbed prefix at %zu", seed, op, i);
        }
        resync_play(q, &play);
      } else if (!on && was) {
        /* THE assertion this round's fix exists for: play order must
         * become the *arranged* order (base_mirror) exactly, not
         * append order. This is the round-trip check the phase file
         * calls out by name -- an insert-at-position or a move made
         * while unshuffled must survive a shuffle-on/shuffle-off
         * round trip. */
        for (size_t i = 0; i < base_mirror.n; i++) {
          CHECK(queue_at(q, i)->track_id == base_mirror.ids[i],
                "seed=%u op=%ld: shuffle-off did not restore the arranged order at %zu (queue=%lld expected=%lld)",
                seed, op, i, (long long) queue_at(q, i)->track_id, (long long) base_mirror.ids[i]);
        }
        play = base_mirror;
        if (current_id >= 0) {
          size_t idx = ida_index_of(&base_mirror, current_id);
          CHECK(idx == queue_cursor(q), "seed=%u op=%ld: cursor not repositioned onto current entry", seed,
                op);
        }
      }
      if (queue_count(q) > 0) {
        CHECK(queue_at(q, queue_cursor(q))->track_id == current_id,
              "seed=%u op=%ld: current entry changed by a shuffle toggle", seed, op);
      }
      break;
    }
    case 6: { /* set_repeat */
      RepeatMode modes[3] = { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };
      queue_set_repeat(q, modes[rand_r(&drive_seed) % 3]);
      break;
    }
    case 7: { /* advance (and peek-agrees-with-advance check) */
      if (play.n == 0) {
        break;
      }
      const QueueEntry * peeked = queue_peek_advance(q);
      size_t              cur_before = queue_cursor(q);
      RepeatMode          mode       = queue_repeat(q);
      const QueueEntry *  advanced   = queue_advance(q);

      if (peeked == NULL || advanced == NULL) {
        CHECK(peeked == advanced, "seed=%u op=%ld: peek/advance NULL-agreement", seed, op);
      } else {
        CHECK(peeked->track_id == advanced->track_id, "seed=%u op=%ld: peek disagrees with advance", seed, op);
      }

      if (mode == REPEAT_ONE) {
        CHECK(queue_cursor(q) == cur_before, "seed=%u op=%ld: REPEAT_ONE advance moved cursor", seed, op);
        CHECK(advanced != NULL && advanced->track_id == current_id,
              "seed=%u op=%ld: REPEAT_ONE advance changed current entry", seed, op);
      } else {
        size_t next = cur_before + 1;
        if (next < play.n) {
          CHECK(advanced != NULL && advanced->track_id == play.ids[next],
                "seed=%u op=%ld: advance landed on wrong deterministic entry", seed, op);
          current_id = play.ids[next];
        } else if (mode == REPEAT_OFF) {
          CHECK(advanced == NULL, "seed=%u op=%ld: REPEAT_OFF advance past end must be NULL", seed, op);
        } else if (!queue_shuffle(q)) {
          CHECK(advanced != NULL && advanced->track_id == play.ids[0],
                "seed=%u op=%ld: unshuffled REPEAT_ALL wrap must land on play[0]", seed, op);
          current_id = play.ids[0];
        } else {
          /* shuffled wrap: unpredictable exactly; multiset must be preserved */
          resync_play(q, &play);
          CHECK(advanced != NULL && advanced->track_id == play.ids[0],
                "seed=%u op=%ld: post-wrap cursor/head mismatch", seed, op);
          if (advanced != NULL) {
            current_id = advanced->track_id;
          }
        }
      }
      break;
    }
    case 8: { /* skip_next / skip_prev */
      if (play.n == 0) {
        break;
      }
      int         forward   = (int) (rand_r(&drive_seed) % 2);
      RepeatMode  mode      = queue_repeat(q);
      RepeatMode  wrap_mode = (mode == REPEAT_ONE) ? REPEAT_ALL : mode;
      size_t      cur_before = queue_cursor(q);

      if (forward) {
        const QueueEntry * skipped = queue_skip_next(q);
        size_t              next    = cur_before + 1;
        if (next < play.n) {
          CHECK(skipped != NULL && skipped->track_id == play.ids[next],
                "seed=%u op=%ld: skip_next landed wrong (deterministic)", seed, op);
          current_id = play.ids[next];
        } else if (wrap_mode == REPEAT_OFF) {
          CHECK(skipped == NULL, "seed=%u op=%ld: skip_next REPEAT_OFF boundary must be NULL", seed, op);
        } else if (!queue_shuffle(q)) {
          CHECK(skipped != NULL && skipped->track_id == play.ids[0],
                "seed=%u op=%ld: unshuffled skip_next wrap must land on play[0]", seed, op);
          current_id = play.ids[0];
        } else {
          resync_play(q, &play);
          CHECK(skipped != NULL && skipped->track_id == play.ids[0],
                "seed=%u op=%ld: post-wrap skip_next mismatch", seed, op);
          if (skipped != NULL) {
            current_id = skipped->track_id;
          }
        }
      } else {
        const QueueEntry * skipped = queue_skip_prev(q);
        if (cur_before > 0) {
          CHECK(skipped != NULL && skipped->track_id == play.ids[cur_before - 1],
                "seed=%u op=%ld: skip_prev landed wrong (deterministic)", seed, op);
          current_id = play.ids[cur_before - 1];
        } else if (wrap_mode == REPEAT_OFF) {
          CHECK(skipped == NULL, "seed=%u op=%ld: skip_prev REPEAT_OFF boundary must be NULL", seed, op);
        } else {
          /* Backward wrap NEVER reshuffles (phase-9-queue.md I.4, round
           * 2 -- Fix 2): perm is unchanged regardless of shuffle state,
           * so this is fully deterministic against the driver's own
           * `play` mirror with no resync needed. */
          CHECK(skipped != NULL && skipped->track_id == play.ids[play.n - 1],
                "seed=%u op=%ld: skip_prev backward wrap must land on the last play-order entry, unchanged "
                "permutation",
                seed, op);
          if (skipped != NULL) {
            current_id = skipped->track_id;
          }
        }
      }
      break;
    }
    default:
      break;
    }

    fuzz_check_consistency(q, &play, seed, op);
    if (g_failures > 0) {
      fprintf(stderr, "fuzz_run: stopping early at op=%ld after first failure (seed=%u)\n", op, seed);
      break;
    }
  }

  queue_destroy(q);
} /* fuzz_run */

int
main(int argc, char ** argv)
{
  unsigned seed = 424242u;
  if (argc > 1) {
    seed = (unsigned) strtoul(argv[1], NULL, 10);
  }
  printf("queue_test: fuzz seed=%u (pass this seed as argv[1] to replay)\n", seed);

  test_append_insert_remove_move_clear_oob();
  test_cursor_traversal_all_modes();
  test_shuffle_identity();
  test_remove_current_and_last();
  test_negative_track_id_roundtrip();
  test_repeat_one_skip_next_traverses();
  test_peek_agrees_with_advance();
  test_remove_move_while_shuffled();
  test_repeat_all_wrap_shuffled_different_permutation();
  test_shuffle_round_trip_preserves_insert_position();
  test_move_survives_round_trip_only_when_unshuffled();
  test_remove_base_duplicate_search_hazard();
  test_skip_prev_backward_wrap_no_reshuffle();

  fuzz_run(seed);

  if (g_failures == 0) {
    printf("queue_test: PASS (seed=%u)\n", seed);
    return 0;
  }
  fprintf(stderr, "queue_test: FAIL (%d failures, seed=%u)\n", g_failures, seed);
  return 1;
} /* main */
