/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_UI_BROWSER_H
#define KURONO_UI_BROWSER_H

#include <stddef.h>
#include <stdint.h>

#include "../library/db.h"

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
 * Which column a load is for. The layout is miller (DESIGN.md VIII.4,
 * plans/kurono/05-impl/phase-16.md 0.A): one cursor on a path, with the
 * column to its left showing what contains it and the column to its
 * right showing what is inside the selection. Only BROWSER_COL_MAIN has
 * a cursor; the other two are functions of it.
 *
 * A source may serve all three identically -- the filesystem source
 * does -- or answer differently per column. BROWSER_COL_PREVIEW is the
 * one that usually differs, because its key names a row rather than a
 * container: the library source answers a track key there with its tag
 * detail. These live in the header rather than beside their user
 * because they are part of the source contract, not an internal.
 */
#define BROWSER_COL_PARENT  0
#define BROWSER_COL_MAIN    1
#define BROWSER_COL_PREVIEW 2

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
 *
 * parent_key() returns a freshly allocated key naming what contains
 * `key`, or NULL when `key` has no parent -- which is how a source says
 * "this is the ceiling". The caller frees the result.
 *
 * It exists because only the source knows how a key decomposes, the same
 * argument browser_goto below is built on. Until phase 16 the widget
 * derived the parent itself with strrchr(key, '/'), which silently
 * assumed every key is a filesystem path: correct for the one source
 * that existed, wrong for the library source, and wrong in a way that
 * produces a listing of the wrong parent rather than an error. It is
 * also wrong for a path containing a name with a slash in it, which is
 * why the library source does not encode its keys as synthetic paths --
 * "AC/DC" is not a hypothetical (plans/kurono/05-impl/phase-16.md 0.C).
 */
typedef struct {
  int  (* load)(void * self, int column, const char * parent_key,
                BrowserRow ** out, size_t * n);
  void (* free_rows)(void * self, BrowserRow * rows, size_t n);
  char * (* parent_key)(void * self, const char * key);
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

/* Browser data-source ordinals, not view ids (design II.1). */
#define BROWSER_SRC_DIRECTORY 0
#define BROWSER_SRC_LIBRARY   1
#define BROWSER_SRC_COUNT     2

/*
 * browser_library_source: the source over library/db.c for one data source.
 * Takes an out-param for the source, the source ordinal, the database, and
 * Config's root array (borrowed, and only consulted at the Directory
 * view's synthetic top level). One instance per view is kept
 * internally, so calling this twice for the same view reconfigures that
 * instance rather than making a second.
 *
 * browser_library_root: the key browser_create should be given as its
 * root for this view -- the synthetic level, except in the Directory
 * view with exactly one music root, where the root itself is the
 * ceiling and the level never appears (amendment 01). Returns an owned
 * key; the caller frees it after browser_create has copied it.
 *
 * browser_library_truncated: reads and clears the DESIGN.md XI.7
 * out-of-memory flag latched by whatever queries have run since the
 * last call, for the status bar to report. Reading it clears it, so
 * exactly one caller may drain it.
 */
void  browser_library_source(BrowserSource * out, int src, Db * db,
                             const char * const * roots, size_t nroots);
char * browser_library_root(int src, const char * const * roots, size_t nroots);
int    browser_library_truncated(const BrowserSource * src);

/*
 * browser_library_locate: the pair of keys naming where `track` sits in
 * this source's view, for browser_goto. Takes a library source, a track
 * row, and out-params for the parent key and the row key; returns 0 with
 * both owned by the caller, or -1 with neither set.
 *
 * browser_goto takes two keys because only the caller knows how a key
 * decomposes -- and since phase 16 the caller does not, because the
 * library encoding is private to browser.c. This is how the caller asks
 * for it instead of reimplementing it. main.c building these keys itself
 * would put the encoding under two owners, which is the same leak
 * BrowserSource.parent_key exists to prevent.
 *
 * Returns -1 for a track with no position in this view -- an untagged
 * track in the Artists view has none, and that is an answer rather than
 * an error. The caller reports it and does not move.
 */
int browser_library_locate(const BrowserSource * src, const DbTrackRow * track,
                           char ** out_parent, char ** out_row);

/*
 * browser_library_track_id: the library id a track row's key names, or 0
 * when the key is not a track's (a directory, an artist, an album, or a
 * key from another source). Takes a library source and a row key.
 *
 * The inverse of browser_library_locate, and needed for the same reason:
 * a row key is the source's private encoding, so a caller that wants the
 * track behind a row has to ask rather than parse. Before this existed,
 * ACT_ENQUEUE passed row->key straight to queue_append as if it were a
 * filesystem path -- true in M1, and silently wrong from phase 16 on,
 * failing only later at open(2) time.
 */
int64_t browser_library_track_id(const BrowserSource * src, const char * row_key);

/*
 * browser_library_expand: every track a container row holds, in the
 * order the browser would list them, for enqueuing a whole directory,
 * album or artist in one keypress. Takes a library source and a row key
 * and fills out-params the caller frees with db_track_rows_free;
 * returns 0, or -1 with neither set.
 *
 * Third member of the same family as the two above, existing for the
 * same reason: main.c must not learn that a key starts with a 'D' or a
 * 'B'. A directory expands recursively, so a multi-disc album queued
 * from the Directory view brings CD1/ and CD2/ with it.
 *
 * Zero rows is a legitimate answer, not an error: a track row, the
 * synthetic root, and an empty directory all give it. "Queue the whole
 * library" is deliberately not what the root row means.
 */
int browser_library_expand(const BrowserSource * src, const char * row_key,
                           DbTrackRow ** out_rows, size_t * out_n);

/*
 * browser_library_detail: the track-detail rows lib_preview_track builds
 * for library id `id` (design IV.1, V.2). Takes the library source, the
 * id, and out-params; returns 0 with *out owned by the caller (free with
 * the source's free_rows) or -1. id 0 or an unknown id is -1.
 */
int browser_library_detail(const BrowserSource * src, int64_t id, BrowserRow ** out, size_t * n);

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
 * Cursor movement. None can move the cursor out of range or leave the
 * scroll offset inconsistent with it. browser_cursor_move treats a
 * delta of exactly +1 or -1 as a wrap -- past the last row goes to the
 * first, and back past the first goes to the last -- and clamps any
 * larger delta at the end it would otherwise cross, so a page move
 * never reads as "page up jumps to the bottom" (the formula is
 * include/ui/listgeom.h's, shared with the queue pane and search
 * results). browser_cursor_home/_end are absolute and do not wrap.
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

/*
 * browser_refresh_derived: rebuild columns 0 and 2 if the cursor or the
 * listing moved since the last call. Cheap and idempotent when nothing
 * changed, so the caller runs it unconditionally once per redraw.
 *
 * It is a separate call, and not something the cursor movers do, for
 * one reason: a held-down arrow key delivers many moves per frame, and
 * doing the loads inside the move would run a query per keypress
 * instead of a query per frame (phase-16-browser.md III, "Column
 * reload"). It is also why the draw accessors below can stay const.
 */
void browser_refresh_derived(Browser * b);

/* Read-only accessors for the draw code. browser_row returns NULL for
   an out-of-range index; browser_selection returns NULL on an empty
   listing. browser_cwd is never NULL.

   The parent_/preview_ accessors read what browser_refresh_derived
   last built: both are empty until it has run, and a derived column
   that failed to load is empty rather than an error. browser_parent_
   cursor is the index within column 0 of the row the user is inside,
   which is what the draw code highlights there. */
size_t             browser_count(const Browser * b);
size_t             browser_cursor(const Browser * b);
size_t             browser_scroll(const Browser * b);
const BrowserRow * browser_row(const Browser * b, size_t i);
const BrowserRow * browser_selection(const Browser * b);
const char *       browser_cwd(const Browser * b);

size_t             browser_parent_count(const Browser * b);
size_t             browser_parent_cursor(const Browser * b);
const BrowserRow * browser_parent_row(const Browser * b, size_t i);
size_t             browser_preview_count(const Browser * b);
const BrowserRow * browser_preview_row(const Browser * b, size_t i);

/* Contiguous arrays behind the three columns, browser_count() /
   browser_parent_count() / browser_preview_count() long. NULL for a NULL
   browser or an empty column. Borrowed until the next mutation. */
const BrowserRow * browser_rows(const Browser * b);
const BrowserRow * browser_parent_rows(const Browser * b);
const BrowserRow * browser_preview_rows(const Browser * b);

#endif // KURONO_UI_BROWSER_H
