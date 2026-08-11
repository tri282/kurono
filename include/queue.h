#ifndef KURONO_QUEUE_H
#define KURONO_QUEUE_H

#include <stddef.h>
#include <stdint.h>

/*
 * A single queue entry. `path` is owned by the queue (strdup'd on
 * insertion, freed on removal/clear/destroy). `track_id <= 0` means the
 * entry is not in the library and `path` is authoritative; in v1 every
 * entry has a valid `track_id` (DESIGN.md III.2), but the field is
 * carried from day one so post-v1 playlist import is a new file rather
 * than a rewrite of queue.c, ui/queueview.c, and ui/statusbar.c.
 */
typedef struct {
  int64_t track_id;
  char *  path;
} QueueEntry;

/*
 * off -> all -> one -> off (ruled 2026-08-11, phase-9-queue.md III.3):
 * matches VLC/Spotify/foobar muscle memory and puts the common case
 * (off -> all) at one keypress.
 */
typedef enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE } RepeatMode;

typedef struct Queue Queue;

/*
 * queue_create: allocate an empty queue. `seed` seeds the internal
 * rand_r state used for Fisher-Yates shuffling -- the caller decides
 * where it comes from (main may pass time(NULL); a test harness passes
 * a fixed, printed seed so a failure is replayable). queue.c never
 * calls time() or the global rand() itself. Returns NULL on allocation
 * failure.
 */
Queue * queue_create(unsigned seed);

/*
 * queue_destroy: free every entry's owned path, the queue's internal
 * storage, and *q itself. A NULL q is a no-op.
 */
void queue_destroy(Queue * q);

/*
 * queue_append: append a new entry (track_id, strdup'd path) to the end
 * of play order. Equivalent to queue_insert(q, queue_count(q), ...).
 * Returns 0 on success, non-zero on failure (NULL q/path, or allocation
 * failure), leaving the queue unchanged.
 */
int queue_append(Queue * q, int64_t track_id, const char * path);

/*
 * queue_insert: insert a new entry at play-order position `at`, which
 * must be in [0, queue_count(q)] -- `at == queue_count(q)` appends. The
 * new entry is always appended to the queue's internal creation-order
 * storage (an implementation detail, never exposed), but `at` also
 * controls where it lands in the *arranged* order -- the order
 * queue_set_shuffle(q, 0) restores -- when shuffle is currently off:
 * while unshuffled, the arranged order and play order are the same
 * thing, so the new entry's arranged-order position is `at` too. While
 * shuffled, there is no established arranged-order position for a
 * brand-new entry, so it is appended to the tail of the arranged order
 * (as if it had been queued after everything already there), while
 * still landing at play-order position `at` for immediate playback.
 * The cursor is adjusted so it keeps referring to the same entry that
 * was current before the insert. Returns 0 on success, non-zero on
 * failure (NULL q/path, out-of-range `at`, or allocation failure),
 * leaving the queue unchanged on failure.
 */
int queue_insert(Queue * q, size_t at, int64_t track_id, const char * path);

/*
 * queue_remove: remove the entry at play-order position `idx`, which
 * must be in [0, queue_count(q)). Frees the entry's path. If the
 * removed entry was current, the cursor lands on the entry that was
 * next in play order (or is clamped to the new last entry if the
 * removed entry was both current and last). Returns 0 on success,
 * non-zero on an out-of-range `idx` (queue unchanged).
 */
int queue_remove(Queue * q, size_t idx);

/*
 * queue_move: move the entry at play-order position `from` to
 * play-order position `to` (both in [0, queue_count(q))), shifting the
 * entries between. Always updates play order. Whether it also updates
 * the *arranged* order (the order queue_set_shuffle(q, 0) restores)
 * depends on whether shuffle is on at the time of the call: while
 * unshuffled, play order and arranged order are the same thing, so the
 * move applies to both, and survives a later shuffle-on/shuffle-off
 * round trip. While shuffled, the arranged order is left untouched --
 * rearranging what plays next during a shuffled session must not
 * silently rewrite the order you return to when shuffle goes off, so a
 * move made while shuffled does not survive a shuffle-off. The cursor
 * is adjusted so it keeps referring to whichever entry was current
 * before the move. Returns 0 on success, non-zero on an out-of-range
 * index (queue unchanged).
 */
int queue_move(Queue * q, size_t from, size_t to);

/*
 * queue_clear: remove every entry (freeing each path) and reset the
 * cursor to 0. Shuffle and repeat settings are untouched.
 */
void queue_clear(Queue * q);

/* queue_count: number of entries currently queued. */
size_t queue_count(const Queue * q);

/*
 * queue_at: the entry at play-order position `idx`, or NULL if `idx >=
 * queue_count(q)`. The returned pointer is valid until the next
 * mutating call on `q`.
 */
const QueueEntry * queue_at(const Queue * q, size_t idx);

/* queue_cursor: the current play-order position (0 if the queue is empty). */
size_t queue_cursor(const Queue * q);

/*
 * queue_set_cursor: move the cursor directly to play-order position
 * `idx` (used when the UI jumps to an explicit selection). Returns 0 on
 * success, non-zero if `idx >= queue_count(q)` (queue unchanged).
 */
int queue_set_cursor(Queue * q, size_t idx);

/*
 * queue_advance: the natural EOF path. Honours REPEAT_ONE (returns the
 * current entry again, cursor unchanged). Under REPEAT_OFF/REPEAT_ALL,
 * moves the cursor forward one play-order position, wrapping to 0 and
 * rebuilding the shuffle permutation under REPEAT_ALL if shuffled.
 * Returns NULL past the end under REPEAT_OFF, or if the queue is empty.
 */
const QueueEntry * queue_advance(Queue * q);

/*
 * queue_peek_advance: reports exactly what queue_advance would return,
 * without moving the cursor -- gapless prefetch reads this seconds
 * before EOF (DESIGN.md V.3, phase-17-queue-transition.md). Shares the
 * same repeat-mode policy as queue_advance via an internal helper, so
 * the two can never disagree. Even in the shuffled-REPEAT_ALL-wrap
 * case, where the real advance would draw a fresh permutation, this
 * predicts that permutation from a copy of the queue's rand_r state
 * without consuming it, so the prediction matches what a following
 * queue_advance() call (with no other mutation in between) actually
 * produces. Returns NULL under the same conditions as queue_advance --
 * including, as a known and accepted limitation, the rare case where
 * predicting a shuffled wrap needs a scratch allocation that fails: a
 * caller cannot distinguish that from genuine end-of-queue. This is not
 * expected to be reachable in practice (the allocation is one
 * queue_count(q)-sized array) and is documented here rather than adding
 * an error channel to an otherwise simple query API.
 */
const QueueEntry * queue_peek_advance(const Queue * q);

/*
 * queue_skip_next: the user's ACT_NEXT. Ignores REPEAT_ONE entirely --
 * moves forward exactly one play-order position as if the mode were
 * REPEAT_ALL when it is actually REPEAT_ONE, so repeatedly pressing
 * next always traverses the whole queue rather than stalling on the
 * current entry. REPEAT_OFF/REPEAT_ALL behave as in queue_advance.
 * Returns NULL if the queue is empty, or past the end under REPEAT_OFF.
 */
const QueueEntry * queue_skip_next(Queue * q);

/*
 * queue_skip_prev: the user's ACT_PREV. Ignores REPEAT_ONE, like
 * queue_skip_next, and moves the cursor back one play-order position,
 * wrapping to the last position under REPEAT_ALL. Unlike the forward
 * wrap, a backward wrap does NOT rebuild the shuffle permutation, even
 * if shuffled: this function exists to walk back through what was just
 * heard, and reshuffling on the way back would destroy exactly that
 * history and land on an arbitrary entry instead of the one that was
 * actually last in play order. The "restart if
 * more than ~3s in" convention needs the playback position, which the
 * queue must never know (DESIGN.md III.2) -- that decision belongs to
 * the caller, between this and a seek-to-zero. Returns NULL if the
 * queue is empty, or at position 0 under REPEAT_OFF.
 */
const QueueEntry * queue_skip_prev(Queue * q);

/*
 * queue_set_shuffle: turn shuffle on or off (any non-zero `on` means
 * on). A no-op if the state does not change.
 *
 * Turning on: Fisher-Yates shuffles play-order positions strictly after
 * the cursor, leaving the already-played prefix (positions [0,
 * cursor]) exactly as it stands -- the currently playing entry is not
 * interrupted.
 *
 * Turning off: restores the *arranged* order -- the order the entries
 * were in immediately before shuffle turned on, including any
 * queue_insert `at` position or queue_move done while unshuffled --
 * and repositions the cursor onto the entry that was playing, so
 * "what's current" survives the toggle either way. This is
 * deliberately not the same as creation/append order: a queue_move
 * made while unshuffled must survive a later shuffle-on/shuffle-off
 * round trip rather than reverting, and a queue_insert at a non-tail
 * `at` must land back at that position, not at the end.
 */
void queue_set_shuffle(Queue * q, int on);

/* queue_shuffle: 1 if shuffle is on, 0 otherwise. */
int queue_shuffle(const Queue * q);

/* queue_set_repeat: set the repeat mode. */
void queue_set_repeat(Queue * q, RepeatMode m);

/* queue_repeat: the current repeat mode. */
RepeatMode queue_repeat(const Queue * q);

#endif // KURONO_QUEUE_H
