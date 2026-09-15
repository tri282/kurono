/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_UI_LISTGEOM_H
#define KURONO_UI_LISTGEOM_H

#include <stddef.h>

/*
 * List geometry: the two formulas every scrolling list in kurono shares
 * -- the browser's main column (ui/browser.c), the queue pane
 * (ui/queueview.c), and search results (ui/tui.c). Header-only and
 * static inline rather than a new translation unit: both functions are
 * pure arithmetic with no state and no dependency, `include/ui/theme.h`'s
 * style_fg/style_bg are the existing precedent for that shape in this
 * tree, and it lets `tools/keymap_test` -- already the ui/ pure-tier
 * binary -- assert them with no Makefile or CI change.
 *
 * Keeping the formula in one place is the point: DESIGN.md VIII.4 and
 * ui/tui.c's draw code both say two places deciding what is visible is
 * how they come to disagree. This header is what makes "two places" and
 * "one formula" simultaneously true -- callers still own their own
 * cursor and scroll state, but they compute it with the same code.
 */

/*
 * list_scroll_for: the viewport offset that keeps `cursor` centred in a
 * window of `viewport` rows over `n` total rows. Takes the cursor
 * position, the row count, and the viewport height (0 or negative is
 * treated as 1), and returns the scroll offset.
 *
 * Centres whenever there is room to: `cursor - viewport / 2`. Rides out
 * of the centre in the first and last half-viewport, since there are no
 * rows on the far side of either end to centre against, and never
 * returns an offset that would leave blank rows below the last entry.
 * Returns 0 whenever the whole listing already fits in the viewport.
 *
 * Guarantees, for every reachable input: 0 <= result, result <= cursor,
 * cursor < result + viewport, and result + viewport <= n whenever
 * n > viewport. No subtraction here can underflow: every one is guarded
 * by the branch that reaches it.
 */
static inline size_t
list_scroll_for(size_t cursor, size_t n, int viewport)
{
  size_t h = (viewport > 0) ? (size_t) viewport : 1;

  if (n <= h || cursor < h / 2) {
    return 0;
  }
  if (cursor > n - 1 - h / 2) {
    return (n > h) ? n - h : 0;
  }
  return cursor - h / 2;
} /* list_scroll_for */

/*
 * list_cursor_move: the new cursor position after moving by `delta`
 * rows over a listing of `n` rows. Takes the current cursor, the row
 * count, and the signed delta, and returns the new cursor -- always
 * less than `n`, or 0 when `n` is 0. A `cursor` already out of range
 * (>= n) is treated as the last row before the move is applied.
 *
 * A delta of exactly +1 or -1 wraps: past the last row gives 0, back
 * past the first gives the last row. Every other delta clamps to
 * [0, n-1] instead of wrapping -- a page move on a short listing must
 * land on the row nearest the edge it was headed for, never read as
 * "page up jumps to the bottom" by wrapping through zero. All
 * arithmetic here is done in `long long`, so no `delta` (including
 * INT_MIN) can overflow on any path.
 */
static inline size_t
list_cursor_move(size_t cursor, size_t n, int delta)
{
  long long last;
  long long c;
  long long want;

  if (n == 0) {
    return 0;
  }

  last = (long long) n - 1;
  c    = (cursor >= n) ? last : (long long) cursor;

  if (delta == 1) {
    want = (c == last) ? 0 : c + 1;
  } else if (delta == -1) {
    want = (c == 0) ? last : c - 1;
  } else {
    want = c + (long long) delta;
    if (want < 0) {
      want = 0;
    } else if (want > last) {
      want = last;
    }
  }

  return (size_t) want;
} /* list_cursor_move */

#endif // KURONO_UI_LISTGEOM_H
