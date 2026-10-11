/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_UI_QUEUEVIEW_H
#define KURONO_UI_QUEUEVIEW_H

#include <stddef.h>

#include "../library/db.h"
#include "../queue.h"
#include "browser.h"

/*
 * ui/queueview.c owns the queue pane's cursor and the display metadata
 * behind each line, and nothing else (DESIGN.md III.2's table). In
 * particular it does not draw: draw_list and draw_text are file-scope
 * in ui/tui.c and stay there, so this file hands out an array of rows
 * the same way a search hands out its results, and tui.c draws it. That
 * is what keeps tui.c calling neither audio/ nor library/ while the
 * queue's title and duration still come from library/db.c.
 *
 * There is one queue in the program and therefore one of these, so the
 * state is file-scope rather than a handle -- the same shape the search
 * state has in tui.c.
 */

/*
 * queueview_rebuild: rebuild the display rows from `q`, resolving each
 * entry's metadata through `db`. Takes the queue and the database
 * (either may be NULL, which empties the view), returns nothing.
 *
 * Called when the pane is opened and after every mutation, never per
 * repaint. The cursor is clamped to the new row count and otherwise
 * left where it was, so removing an entry leaves the cursor on what
 * took its place.
 */
void queueview_rebuild(const Queue * q, Db * db);

/*
 * queueview_free: drop the rows. Takes and returns nothing. Idempotent,
 * and safe to call on a view that was never built.
 */
void queueview_free(void);

/* queueview_rows / queueview_count: the rows to draw, borrowed until the
   next rebuild or free. NULL and 0 before the first rebuild. */
const BrowserRow * queueview_rows(void);
size_t             queueview_count(void);

/* queueview_cursor: the play-order position under the pane's cursor. 0
   when the queue is empty, which is also a valid position, so callers
   check queueview_count() before acting on it. */
size_t queueview_cursor(void);

/* queueview_cursor_move: move by `delta` rows. A delta of exactly +1 or
   -1 wraps at either end, matching the browser's columns and search
   results (include/ui/listgeom.h); a larger delta -- a page move --
   clamps at the end it would cross rather than wrapping past it.
   Accepted cost: termbox2 does not distinguish a held key from repeated
   presses, so holding the single-step key at the end of the queue now
   runs off the end and continues from the top instead of stopping. */
void queueview_cursor_move(int delta);

/* queueview_cursor_set: put the cursor at `idx`, clamped to the last
   row. Used when the queue moves under the pane -- a track transition
   follows the playing entry with it. */
void queueview_cursor_set(size_t idx);

#endif // KURONO_UI_QUEUEVIEW_H
