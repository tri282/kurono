#include "../include/queue.h"

#include <stdlib.h>
#include <string.h>

/*
 * Growth constant for the two parallel arrays below (doubling from 16).
 */
#define QUEUE_INITIAL_CAP 16

/*
 * The playback order model (DESIGN.md III.2, phase-9-queue.md).
 *
 * `entries` is a growable array of QueueEntry in *creation order* --
 * the order things were appended/inserted, mutated only by
 * queue_insert (which always appends the new entry to the tail of this
 * array; see below) and queue_remove (which compacts it). Its index
 * space is "storage index" and is never exposed to callers. Creation
 * order is an implementation detail only -- it is NOT the order
 * restored by queue_set_shuffle(q, 0); see `base` below.
 *
 * `perm` is a growable array of size_t, always exactly `count` long,
 * mapping a *play-order* position to a storage index:
 * entries[perm[i]] is the i-th entry in play order. `perm` is a
 * bijection over [0, count) at all times, in both shuffle states, after
 * every operation -- this is the load-bearing invariant the whole file
 * is built to preserve (phase-9-queue.md I.4).
 *
 * `base` is a second growable array of size_t, always exactly `count`
 * long and always a bijection over [0, count), holding the *arranged*
 * play order -- the order queue_set_shuffle(q, 0) restores. While
 * shuffle is off, `base` and `perm` are kept identical (every operation
 * that touches one touches the other the same way), so play order and
 * arranged order are the same thing, as they should be. While shuffle
 * is on, `perm` gets randomised and walked by advance/skip, but `base`
 * is left untouched except for the structural updates queue_insert and
 * queue_remove must still apply to keep it a valid bijection over the
 * new `count` (phase-9-queue.md I.4, round 2's maintenance table) --
 * `queue_move` specifically must NOT touch `base` while shuffled, or a
 * shuffled-session reorder would silently overwrite the order the user
 * returns to on shuffle-off. This split exists because the first
 * implementation of this file conflated "arranged order" with "creation
 * order" and rebuilt `perm` as the identity on shuffle-off, silently
 * discarding queue_insert's `at` position and every queue_move made
 * while unshuffled -- found on session review, not by any test written
 * at the time (see tools/queue_test.c's file header on why the fuzz
 * suite could not see it).
 *
 * `cursor` is a play-order position (an index into `perm`), valid
 * whenever count > 0, and left at whatever it was (clamped) when
 * count == 0.
 */
struct Queue {
  QueueEntry * entries;
  size_t *     perm;
  size_t *     base;
  size_t       count;
  size_t       cap; /* capacity of entries[]/perm[]/base[] -- always equal */
  size_t       cursor;
  int          shuffle;
  RepeatMode   repeat;
  unsigned     rng_seed; /* rand_r state; never touched by anything but this file */
};

/*
 * ensure_capacity: grow q->entries/q->perm/q->base (doubling from
 * QUEUE_INITIAL_CAP) so all three can hold at least `needed` elements.
 * Returns 0 on success. On failure, returns -1 and leaves all three
 * arrays exactly as they were -- realloc() never frees the original
 * block on failure, so there is nothing to unwind, and q->cap is only
 * committed once every realloc below has succeeded.
 */
static int
ensure_capacity(Queue * q, size_t needed)
{
  if (needed <= q->cap) {
    return 0;
  }

  size_t new_cap = (q->cap == 0) ? QUEUE_INITIAL_CAP : q->cap * 2;
  while (new_cap < needed) {
    new_cap *= 2;
  }

  QueueEntry * new_entries = realloc(q->entries, new_cap * sizeof(QueueEntry));
  if (new_entries == NULL) {
    return -1;
  }
  q->entries = new_entries;

  /* entries[]/perm[] are now larger than q->cap reflects; harmless --
   * q->cap is only committed once base[] has grown too, below. */
  size_t * new_perm = realloc(q->perm, new_cap * sizeof(size_t));
  if (new_perm == NULL) {
    return -1;
  }
  q->perm = new_perm;

  size_t * new_base = realloc(q->base, new_cap * sizeof(size_t));
  if (new_base == NULL) {
    return -1;
  }
  q->base = new_base;

  q->cap = new_cap;
  return 0;
} /* ensure_capacity */

/*
 * rand_below: a uniform random value in [0, n) drawn from *seed via
 * rand_r, using rejection sampling rather than `% n` -- the phase spec
 * calls for this explicitly (a modulo reduction biases toward the low
 * end whenever n does not evenly divide RAND_MAX + 1). n must be >= 1.
 */
static size_t
rand_below(unsigned * seed, size_t n)
{
  if (n <= 1) {
    return 0;
  }

  unsigned long limit  = (unsigned long) RAND_MAX + 1UL;
  unsigned long bucket = limit / (unsigned long) n;
  unsigned long usable = bucket * (unsigned long) n;
  unsigned long r;

  do {
    r = (unsigned long) rand_r(seed);
  } while (r >= usable);

  return (size_t) (r / bucket);
} /* rand_below */

/*
 * fisher_yates: shuffle arr[0..n) in place using the classic
 * Durstenfeld walk, drawing each swap index from *seed via
 * rand_below(). A no-op for n < 2.
 */
static void
fisher_yates(size_t * arr, size_t n, unsigned * seed)
{
  for (size_t i = n; i-- > 1;) {
    size_t j   = rand_below(seed, i + 1);
    size_t tmp = arr[i];
    arr[i]     = arr[j];
    arr[j]     = tmp;
  }
} /* fisher_yates */

/*
 * predict_wrap_head: without mutating *q, compute the storage index
 * that a REPEAT_ALL wrap-while-shuffled reshuffle would place at play
 * position 0. Simulates the identical fisher_yates() over a scratch
 * copy of q->perm, seeded from a *copy* of q->rng_seed -- q->rng_seed
 * itself is left untouched, so a subsequent real advance (which draws
 * from the real, still-untouched seed) reproduces the exact same
 * sequence of rand_r() outputs and lands on the same result, as long as
 * nothing else touches the queue between the two calls. This is what
 * lets queue_peek_advance stay const while still agreeing with
 * queue_advance under a shuffled wrap. Returns 0 on success, -1 on
 * allocation failure (in which case *out is unchanged).
 */
static int
predict_wrap_head(const Queue * q, size_t * out)
{
  size_t * copy = malloc(q->count * sizeof(size_t));
  if (copy == NULL) {
    return -1;
  }
  memcpy(copy, q->perm, q->count * sizeof(size_t));

  unsigned seed_copy = q->rng_seed;
  fisher_yates(copy, q->count, &seed_copy);
  *out = copy[0];

  free(copy);
  return 0;
} /* predict_wrap_head */

/*
 * advance_by_one: the shared, mutating "move forward one play-order
 * position" policy used by both queue_advance (REPEAT_ONE already
 * handled by the caller before this is reached) and queue_skip_next
 * (which substitutes REPEAT_ALL for REPEAT_ONE before calling). Wraps
 * to position 0 under REPEAT_ALL, rebuilding the whole permutation via
 * fisher_yates if shuffled (phase-9-queue.md I.4: "REPEAT_ALL wrapping
 * while shuffle is on rebuilds the permutation"). Returns NULL past the
 * end under REPEAT_OFF. `wrap_mode` is never REPEAT_ONE.
 */
static const QueueEntry *
advance_by_one(Queue * q, RepeatMode wrap_mode)
{
  size_t next = q->cursor + 1;

  if (next >= q->count) {
    if (wrap_mode == REPEAT_OFF) {
      return NULL;
    }
    next = 0;
    if (q->shuffle) {
      fisher_yates(q->perm, q->count, &q->rng_seed);
    }
  }

  q->cursor = next;
  return &q->entries[q->perm[next]];
} /* advance_by_one */

/*
 * peek_advance_by_one: the const, non-mutating counterpart of
 * advance_by_one, sharing the exact same boundary/wrap policy so
 * queue_peek_advance and queue_advance can never disagree (see the
 * queue.h comment on queue_peek_advance and predict_wrap_head above for
 * how the shuffled-wrap case stays deterministic without mutating *q).
 * `wrap_mode` is never REPEAT_ONE. Returns NULL if it cannot predict
 * (REPEAT_OFF boundary, or an allocation failure inside
 * predict_wrap_head -- an unlikely edge case with no other way to
 * signal failure from a "what would happen" query).
 */
static const QueueEntry *
peek_advance_by_one(const Queue * q, RepeatMode wrap_mode)
{
  size_t next = q->cursor + 1;

  if (next < q->count) {
    return &q->entries[q->perm[next]];
  }
  if (wrap_mode == REPEAT_OFF) {
    return NULL;
  }
  if (!q->shuffle) {
    return &q->entries[q->perm[0]];
  }

  size_t predicted;
  if (predict_wrap_head(q, &predicted) != 0) {
    return NULL;
  }
  return &q->entries[predicted];
} /* peek_advance_by_one */

/*
 * queue_create: see include/queue.h for the contract.
 */
Queue *
queue_create(unsigned seed)
{
  Queue * q = malloc(sizeof(*q));
  if (q == NULL) {
    return NULL;
  }

  q->entries  = NULL;
  q->perm     = NULL;
  q->base     = NULL;
  q->count    = 0;
  q->cap      = 0;
  q->cursor   = 0;
  q->shuffle  = 0;
  q->repeat   = REPEAT_OFF;
  q->rng_seed = seed;

  return q;
} /* queue_create */

/*
 * queue_destroy: see include/queue.h for the contract.
 */
void
queue_destroy(Queue * q)
{
  if (q == NULL) {
    return;
  }

  for (size_t i = 0; i < q->count; i++) {
    free(q->entries[i].path);
  }
  free(q->entries);
  free(q->perm);
  free(q->base);
  free(q);
} /* queue_destroy */

/*
 * queue_insert: see include/queue.h for the contract.
 */
int
queue_insert(Queue * q, size_t at, int64_t track_id, const char * path)
{
  if (q == NULL || path == NULL || at > q->count) {
    return -1;
  }

  char * path_copy = strdup(path);
  if (path_copy == NULL) {
    return -1;
  }

  if (ensure_capacity(q, q->count + 1) != 0) {
    free(path_copy);
    return -1;
  }

  size_t old_count       = q->count;
  size_t new_storage_idx = old_count;

  /* Storage always grows at the tail -- entries[] is creation order,
   * and `at` only controls the play-order landing slot below. */
  q->entries[new_storage_idx].track_id = track_id;
  q->entries[new_storage_idx].path     = path_copy;

  if (at < old_count) {
    memmove(&q->perm[at + 1], &q->perm[at], (old_count - at) * sizeof(size_t));
  }
  q->perm[at] = new_storage_idx;

  /* base[] tracks the arranged order (phase-9-queue.md I.4, round 2).
   * While unshuffled, arranged order and play order are the same
   * thing, so the new entry lands at the same position `at` in both.
   * While shuffled, there is no established arranged-order position
   * for a brand-new entry, so it is appended to base's tail -- the
   * same rule queue_append already applies for perm's tail. */
  if (q->shuffle) {
    q->base[old_count] = new_storage_idx;
  } else {
    if (at < old_count) {
      memmove(&q->base[at + 1], &q->base[at], (old_count - at) * sizeof(size_t));
    }
    q->base[at] = new_storage_idx;
  }

  q->count = old_count + 1;

  if (old_count == 0) {
    q->cursor = 0;
  } else if (at <= q->cursor) {
    /* The entry that was current shifted one play-order slot to the
     * right; keep the cursor pointing at it, not at the new entry. */
    q->cursor++;
  }

  return 0;
} /* queue_insert */

/*
 * queue_append: see include/queue.h for the contract.
 */
int
queue_append(Queue * q, int64_t track_id, const char * path)
{
  if (q == NULL) {
    return -1;
  }
  return queue_insert(q, q->count, track_id, path);
} /* queue_append */

/*
 * queue_remove: see include/queue.h for the contract. This is the
 * fiddly one (phase-9-queue.md I.4): removing play-order index `idx`
 * removes storage index perm[idx] from entries[], which shifts every
 * storage index above it down by one, so every permutation slot
 * pointing past the removed storage index has to be fixed up before the
 * play-order slot itself is dropped -- otherwise perm stops being a
 * bijection the moment a removed storage index is compacted out from
 * under a stale perm value. base[] takes the identical storage-index
 * fixup (it references the same compacted entries[]), then has its own
 * slot for the removed storage index dropped too -- found by value,
 * since base's own order generally differs from perm's.
 */
int
queue_remove(Queue * q, size_t idx)
{
  if (q == NULL || idx >= q->count) {
    return -1;
  }

  size_t storage_idx = q->perm[idx];
  free(q->entries[storage_idx].path);

  /* Locate storage_idx's slot in base[] BEFORE either fixup loop below
   * runs -- while every base[] value is still unique. This matters:
   * the perm[] fixup (and, symmetrically, the base[] fixup right after
   * it) decrements every slot greater than storage_idx, which can
   * transiently create a DUPLICATE of storage_idx (whatever live entry
   * held storage_idx + 1 now also reads storage_idx). Searching by
   * value AFTER that fixup can silently return the wrong slot -- a
   * still-live entry's slot instead of the removed one's -- with no
   * structural signal that anything went wrong, since base[] still
   * looks like a valid bijection afterward. perm[] never has this
   * problem because its slot is dropped by INDEX (`idx`, known up
   * front), not by value; this makes base[] symmetric with it by
   * capturing the index up front too. Found on session review, round 3
   * -- see phase-9.md's round-3 entry for the exact non-adjacent-
   * duplicate repro this was invisible without. */
  size_t base_idx = q->count;
  for (size_t k = 0; k < q->count; k++) {
    if (q->base[k] == storage_idx) {
      base_idx = k;
      break;
    }
  }
  if (base_idx >= q->count) {
    /* Unreachable in a valid bijection -- base[] always contains
     * storage_idx exactly once before any fixup touches it. A hard
     * return rather than a silent skip: silently not dropping a slot
     * would leave base[] one element too long forever. */
    return -1;
  }

  /* Fix up every permutation slot that points past the storage index
   * about to be compacted out -- including slot idx itself, harmlessly,
   * since it is dropped below regardless of its post-fixup value. */
  for (size_t k = 0; k < q->count; k++) {
    if (q->perm[k] > storage_idx) {
      q->perm[k]--;
    }
  }
  /* Same fixup on base[] -- it references the same storage indices.
   * base_idx was already captured above, before this loop can create
   * the transient duplicate described there. */
  for (size_t k = 0; k < q->count; k++) {
    if (q->base[k] > storage_idx) {
      q->base[k]--;
    }
  }

  if (storage_idx + 1 < q->count) {
    memmove(&q->entries[storage_idx], &q->entries[storage_idx + 1],
            (q->count - storage_idx - 1) * sizeof(QueueEntry));
  }
  if (idx + 1 < q->count) {
    memmove(&q->perm[idx], &q->perm[idx + 1], (q->count - idx - 1) * sizeof(size_t));
  }
  /* Drop base_idx by position, exactly as perm's slot at idx is
   * dropped by position above -- not by re-searching for a value that
   * may no longer be unique. */
  if (base_idx + 1 < q->count) {
    memmove(&q->base[base_idx], &q->base[base_idx + 1], (q->count - base_idx - 1) * sizeof(size_t));
  }

  size_t new_cursor = q->cursor;
  if (idx < q->cursor) {
    new_cursor = q->cursor - 1;
  }
  /* idx == cursor: numerically unchanged here, clamped below once the
   * new count is known. idx > cursor: unaffected either way. */

  q->count--;

  if (new_cursor >= q->count) {
    new_cursor = (q->count > 0) ? q->count - 1 : 0;
  }
  q->cursor = new_cursor;

  return 0;
} /* queue_remove */

/*
 * queue_move: see include/queue.h for the contract. Always operates on
 * `perm` -- entries[] (creation order) never moves, whether shuffle is
 * on or off. Whether `base` (the arranged order restored on
 * shuffle-off) also moves depends on shuffle state: while unshuffled,
 * base and perm are the same order and the move is applied to both, so
 * it survives a later shuffle-on/shuffle-off round trip; while
 * shuffled, base is left untouched (phase-9-queue.md I.4, round 2's
 * maintenance table) -- rearranging a shuffled queue must not silently
 * rewrite the order the user returns to when shuffle goes off.
 */
int
queue_move(Queue * q, size_t from, size_t to)
{
  if (q == NULL || from >= q->count || to >= q->count) {
    return -1;
  }
  if (from == to) {
    return 0;
  }

  size_t moved = q->perm[from];

  if (from < to) {
    memmove(&q->perm[from], &q->perm[from + 1], (to - from) * sizeof(size_t));
  } else {
    memmove(&q->perm[to + 1], &q->perm[to], (from - to) * sizeof(size_t));
  }
  q->perm[to] = moved;

  if (!q->shuffle) {
    /* unshuffled: base is identical to perm before this move (same
     * values, since they are kept in lockstep off-shuffle), so the
     * identical index shift applies to base too. */
    if (from < to) {
      memmove(&q->base[from], &q->base[from + 1], (to - from) * sizeof(size_t));
    } else {
      memmove(&q->base[to + 1], &q->base[to], (from - to) * sizeof(size_t));
    }
    q->base[to] = moved;
  }

  if (q->cursor == from) {
    q->cursor = to;
  } else if (from < to && q->cursor > from && q->cursor <= to) {
    q->cursor--;
  } else if (from > to && q->cursor >= to && q->cursor < from) {
    q->cursor++;
  }

  return 0;
} /* queue_move */

/*
 * queue_clear: see include/queue.h for the contract.
 */
void
queue_clear(Queue * q)
{
  if (q == NULL) {
    return;
  }

  for (size_t i = 0; i < q->count; i++) {
    free(q->entries[i].path);
  }
  q->count  = 0;
  q->cursor = 0;
} /* queue_clear */

/*
 * queue_count: see include/queue.h for the contract.
 */
size_t
queue_count(const Queue * q)
{
  return (q != NULL) ? q->count : 0;
} /* queue_count */

/*
 * queue_at: see include/queue.h for the contract.
 */
const QueueEntry *
queue_at(const Queue * q, size_t idx)
{
  if (q == NULL || idx >= q->count) {
    return NULL;
  }
  return &q->entries[q->perm[idx]];
} /* queue_at */

/*
 * queue_cursor: see include/queue.h for the contract.
 */
size_t
queue_cursor(const Queue * q)
{
  return (q != NULL) ? q->cursor : 0;
} /* queue_cursor */

/*
 * queue_set_cursor: see include/queue.h for the contract.
 */
int
queue_set_cursor(Queue * q, size_t idx)
{
  if (q == NULL || q->count == 0 || idx >= q->count) {
    return -1;
  }
  q->cursor = idx;
  return 0;
} /* queue_set_cursor */

/*
 * queue_advance: see include/queue.h for the contract.
 */
const QueueEntry *
queue_advance(Queue * q)
{
  if (q == NULL || q->count == 0) {
    return NULL;
  }
  if (q->repeat == REPEAT_ONE) {
    return &q->entries[q->perm[q->cursor]];
  }
  return advance_by_one(q, q->repeat);
} /* queue_advance */

/*
 * queue_peek_advance: see include/queue.h for the contract.
 */
const QueueEntry *
queue_peek_advance(const Queue * q)
{
  if (q == NULL || q->count == 0) {
    return NULL;
  }
  if (q->repeat == REPEAT_ONE) {
    return &q->entries[q->perm[q->cursor]];
  }
  return peek_advance_by_one(q, q->repeat);
} /* queue_peek_advance */

/*
 * queue_skip_next: see include/queue.h for the contract.
 */
const QueueEntry *
queue_skip_next(Queue * q)
{
  if (q == NULL || q->count == 0) {
    return NULL;
  }
  RepeatMode wrap_mode = (q->repeat == REPEAT_ONE) ? REPEAT_ALL : q->repeat;
  return advance_by_one(q, wrap_mode);
} /* queue_skip_next */

/*
 * queue_skip_prev: see include/queue.h for the contract. Unlike
 * advance_by_one's forward wrap, a backward wrap at position 0 does
 * NOT reshuffle, even if shuffled (phase-9-queue.md I.4, round 2 --
 * this function exists to walk back through what was just heard, and
 * reshuffling on the way back would destroy exactly that history and
 * land on an arbitrary entry instead of the true last-played one).
 */
const QueueEntry *
queue_skip_prev(Queue * q)
{
  if (q == NULL || q->count == 0) {
    return NULL;
  }
  RepeatMode wrap_mode = (q->repeat == REPEAT_ONE) ? REPEAT_ALL : q->repeat;

  if (q->cursor == 0) {
    if (wrap_mode == REPEAT_OFF) {
      return NULL;
    }
    q->cursor = q->count - 1;
  } else {
    q->cursor--;
  }

  return &q->entries[q->perm[q->cursor]];
} /* queue_skip_prev */

/*
 * queue_set_shuffle: see include/queue.h for the contract.
 *
 * Turning on: base[] is already correct -- it has been kept in
 * lockstep with perm[] the entire time shuffle was off (queue_insert,
 * queue_remove, queue_move all maintain that), so it needs no snapshot
 * here. Only perm's tail after the cursor is shuffled; base is
 * untouched.
 *
 * Turning off: perm[] is restored from base[] -- the *arranged* order,
 * not creation order (phase-9-queue.md I.4, round 2) -- and the cursor
 * is repositioned by finding the playing entry's storage index within
 * base via linear search (base's own order generally differs from the
 * shuffled perm's, so there is no shortcut past the search).
 */
void
queue_set_shuffle(Queue * q, int on)
{
  if (q == NULL) {
    return;
  }

  int want = on ? 1 : 0;
  if (want == q->shuffle) {
    return;
  }

  if (want) {
    if (q->count > 0 && q->cursor + 1 < q->count) {
      fisher_yates(&q->perm[q->cursor + 1], q->count - q->cursor - 1, &q->rng_seed);
    }
  } else {
    size_t playing_storage = (q->count > 0) ? q->perm[q->cursor] : 0;
    memcpy(q->perm, q->base, q->count * sizeof(size_t));
    if (q->count > 0) {
      size_t new_cursor = 0;
      for (size_t i = 0; i < q->count; i++) {
        if (q->base[i] == playing_storage) {
          new_cursor = i;
          break;
        }
      }
      q->cursor = new_cursor;
    }
  }

  q->shuffle = want;
} /* queue_set_shuffle */

/*
 * queue_shuffle: see include/queue.h for the contract.
 */
int
queue_shuffle(const Queue * q)
{
  return (q != NULL) ? q->shuffle : 0;
} /* queue_shuffle */

/*
 * queue_set_repeat: see include/queue.h for the contract.
 */
void
queue_set_repeat(Queue * q, RepeatMode m)
{
  if (q == NULL) {
    return;
  }
  q->repeat = m;
} /* queue_set_repeat */

/*
 * queue_repeat: see include/queue.h for the contract.
 */
RepeatMode
queue_repeat(const Queue * q)
{
  return (q != NULL) ? q->repeat : REPEAT_OFF;
} /* queue_repeat */
