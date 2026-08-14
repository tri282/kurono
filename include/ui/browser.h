#ifndef KURONO_UI_BROWSER_H
#define KURONO_UI_BROWSER_H

#include <stddef.h>

/*
 * The browser widget and the indirection that keeps it from ever
 * knowing where its rows came from (DESIGN.md XIV.1, phase-12 I.1,
 * plan.md VII gap 8).
 *
 * M1 lists a directory tree with opendir. M2 (phase 16) lists the
 * SQLite index. CLAUDE.md's rule is that a new view must be "a query
 * plus a keybind, not a new file in ui/", and that rule only holds if
 * the widget never learned where rows come from -- so the indirection
 * is built now, one phase before the second source exists, rather than
 * retrofitted around a widget that hardcoded opendir in three places.
 */

/*
 * BrowserRow: one line in a column. `name` is what is drawn, `key` is
 * what identifies the row to its source -- a filesystem path for the
 * M1 source, and whatever phase 16's queries want for theirs. Both
 * strings are owned by the source that produced them and stay valid
 * until that source's free_rows is called on the array.
 */
typedef struct {
  char * name;
  char * key;
  int    is_dir;    /* 1 when descending into it is meaningful */
} BrowserRow;

/*
 * BrowserSource: how a column gets its rows.
 *
 * load() fills *out with a freshly allocated array of *n rows for the
 * given column under the given parent key, and returns 0. It returns -1
 * on failure, having allocated nothing -- a failure is reported and
 * skipped, never fatal (CLAUDE.md: per-file failures are never fatal).
 * A parent that exists but yields nothing is success with *n == 0, not
 * a failure; an empty directory is not an error.
 *
 * `column` is passed even though the M1 source ignores it, because
 * phase 16's source runs a different query per column and the widget
 * must not grow a switch when that lands.
 *
 * free_rows() releases what load() produced, including every string.
 * Calling it with n == 0 or rows == NULL is a no-op.
 */
typedef struct {
  int  (* load)(void * self, int column, const char * parent_key,
                BrowserRow ** out, size_t * n);
  void (* free_rows)(void * self, BrowserRow * rows, size_t n);
  void * self;
} BrowserSource;

typedef struct Browser Browser;

/*
 * browser_fs_source: the M1 filesystem source. Lists directories and
 * playable files under a path, hiding dotfiles, skipping any directory
 * whose basename appears in `ignore`, and sorting directories before
 * files with each group in byte order.
 *
 * Takes the ignore list (may be NULL with nignore 0) and an out-param
 * for the source. The ignore array is borrowed, not copied, and must
 * outlive the source -- it is Config's, which outlives every widget.
 */
void browser_fs_source(BrowserSource * out, char * const * ignore, size_t nignore);

/*
 * browser_create / browser_destroy: the widget. `root` is the key the
 * first column loads and the point above which ascending stops.
 * Returns NULL on allocation failure or if the root will not load.
 */
Browser * browser_create(const BrowserSource * src, const char * root);
void      browser_destroy(Browser * b);

/*
 * browser_reload: re-run the current directory's load. Returns 0, or
 * -1 with the previous rows left intact -- a failed reload must not
 * empty a column that was fine a moment ago.
 */
int browser_reload(Browser * b);

/*
 * Cursor movement. All of these clamp; none can move the cursor out of
 * range or leave the scroll offset inconsistent with it. A delta past
 * either end stops at that end rather than wrapping.
 */
void browser_cursor_move(Browser * b, int delta);
void browser_cursor_home(Browser * b);
void browser_cursor_end(Browser * b);

/*
 * browser_descend: enter the selected row if it is a directory.
 * browser_ascend: go to the parent, stopping at the root, and place the
 * cursor on the directory just left so that left-then-right returns to
 * where the user was.
 *
 * Both return 1 when the listing changed, 0 when the move was not
 * possible (a file, or already at the root), and -1 when the new
 * listing failed to load -- in which case the browser is unchanged.
 */
int browser_descend(Browser * b);
int browser_ascend(Browser * b);

/*
 * browser_goto: list `parent_key` and put the cursor on the row whose
 * key is `row_key` -- the jump-to-playing move, which descend and
 * ascend cannot express because the target is usually neither a child
 * nor the parent of the current listing.
 *
 * The caller supplies both keys rather than just the row, because only
 * the caller knows how a key decomposes: the M1 source's keys are
 * filesystem paths and main.c takes their dirname, while phase 16's
 * queries hand back a parent of their own. The widget stays ignorant of
 * where rows come from, which is the whole point of BrowserSource.
 *
 * Refuses any parent outside the root. Returns 0 on success and -1 when
 * the parent will not load, lies outside the root, or does not contain
 * the row -- and on -1 the browser is left as it was, so a failed jump
 * never strands the user in a reloaded column.
 */
int browser_goto(Browser * b, const char * parent_key, const char * row_key);

/*
 * browser_set_viewport: tell the widget how many rows the column can
 * show, so it can keep the cursor visible. Called on every resize.
 * A viewport of 0 or less is treated as 1.
 */
void browser_set_viewport(Browser * b, int rows);

/* Read-only accessors for the draw code. browser_row returns NULL for
   an out-of-range index; browser_selection returns NULL on an empty
   listing. browser_cwd is never NULL. */
size_t             browser_count(const Browser * b);
size_t             browser_cursor(const Browser * b);
size_t             browser_scroll(const Browser * b);
const BrowserRow * browser_row(const Browser * b, size_t i);
const BrowserRow * browser_selection(const Browser * b);
const char *       browser_cwd(const Browser * b);

#endif // KURONO_UI_BROWSER_H
