/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../../include/ui/listgeom.h"
#include "../../include/ui/queueview.h"

#include "../../include/library/db.h"
#include "../../include/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One line of the pane. Wide enough for "999. Artist - Title  12:34" at
   any column width the layout permits; draw_text truncates to the
   column, so this only has to be longer than the widest terminal a
   three-column layout is readable in. */
#define QV_LINE 512

/*
 * The rows and the cursor. File-scope for the reason queueview.h gives:
 * there is one queue in the program. `rows` is the array tui.c draws
 * through, in BrowserRow shape so draw_column's existing accessor works
 * unchanged -- the same adapter a search's results already use. `key` is
 * always NULL here and `is_dir` always 0: a queue line identifies
 * nothing to a source and nothing descends into it, and filling either
 * with something plausible is how a field starts lying.
 */
static BrowserRow * g_rows   = NULL;
static size_t       g_count  = 0;
static size_t       g_cursor = 0;

/*
 * fmt_ms: "m:ss", or "-:--" when the duration is unknown. Takes the
 * milliseconds and a buffer, returns nothing.
 */
static void
fmt_ms(int64_t ms, char * out, size_t cap)
{
  if (ms <= 0) {
    snprintf(out, cap, "-:--");
    return;
  }
  int64_t total = ms / 1000;
  snprintf(out, cap, "%lld:%02lld", (long long) (total / 60), (long long) (total % 60));
} /* fmt_ms */

/*
 * row_text: one display line for one queue entry. Takes the position,
 * the entry and the database; returns an owned string or NULL on
 * allocation failure.
 *
 * Both branches of DESIGN.md III.2, and the second one is the point.
 * v1 never takes it -- every entry is enqueued from the library and
 * carries a valid track_id -- and it is written because post-v1 playlist
 * import produces entries pointing outside KURONO_MUSIC_DIR or at files
 * deleted since the last scan, and the entry shape was chosen in phase 9
 * so that import is a new file rather than a rewrite of this one. It
 * also has a v1 consumer after all: a rescan can leave a queued entry
 * whose track_id no longer resolves, and falling back to the basename is
 * then the correct behaviour rather than dead code.
 */
static char *
row_text(size_t pos, const QueueEntry * e, Db * db)
{
  char line[QV_LINE];

  if (e->track_id > 0 && db != NULL) {
    DbTrackRow t;
    memset(&t, 0, sizeof t);
    if (db_query_track_by_id(db, e->track_id, &t) == 1) {
      char dur[16];
      fmt_ms(t.has_duration_ms ? t.duration_ms : 0, dur, sizeof dur);
      /* A missing title falls back to the path's basename rather than to
         "(unknown)": an untagged file is still identifiable by its
         filename, which is the whole reason the Directory view exists. */
      const char * title = (t.title != NULL) ? t.title : NULL;
      const char * base  = strrchr(e->path, '/');
      base = (base != NULL) ? base + 1 : e->path;

      if (title == NULL) {
        snprintf(line, sizeof line, "%3zu. %s  %s", pos + 1, base, dur);
      } else if (t.artist != NULL) {
        snprintf(line, sizeof line, "%3zu. %s - %s  %s", pos + 1, t.artist, title, dur);
      } else {
        snprintf(line, sizeof line, "%3zu. %s  %s", pos + 1, title, dur);
      }
      db_track_row_free_fields(&t);
      return strdup(line);
    }
    db_track_row_free_fields(&t);
    /* Fall through: the id no longer resolves, which is the rescan case
       above. */
  }

  const char * base = strrchr(e->path, '/');
  base = (base != NULL) ? base + 1 : e->path;
  snprintf(line, sizeof line, "%3zu. %s", pos + 1, base);
  return strdup(line);
} /* row_text */

/*
 * queueview_rebuild: see include/ui/queueview.h.
 */
void
queueview_rebuild(const Queue * q, Db * db)
{
  queueview_free();

  size_t n = (q != NULL) ? queue_count(q) : 0;
  if (n == 0) {
    return;
  }

  g_rows = calloc(n, sizeof *g_rows);
  if (g_rows == NULL) {
    return; /* an empty pane, reported by nothing: the queue itself is intact */
  }

  for (size_t i = 0; i < n; i++) {
    const QueueEntry * e = queue_at(q, i);
    if (e == NULL || e->path == NULL) {
      continue;
    }
    g_rows[g_count].name = row_text(i, e, db);
    if (g_rows[g_count].name == NULL) {
      continue; /* one unrenderable line is skipped, never the whole pane */
    }
    g_count++;
  }

  /* Clamped rather than reset: removing an entry should leave the cursor
     on whatever took its place, and rebuilding after an enqueue should
     not move it at all. */
  if (g_cursor >= g_count) {
    g_cursor = (g_count > 0) ? g_count - 1 : 0;
  }
} /* queueview_rebuild */

/*
 * queueview_free: see include/ui/queueview.h.
 */
void
queueview_free(void)
{
  for (size_t i = 0; i < g_count; i++) {
    free(g_rows[i].name);
  }
  free(g_rows);
  g_rows  = NULL;
  g_count = 0;
} /* queueview_free */

/*
 * queueview_rows / queueview_count / queueview_cursor: see the header.
 */
const BrowserRow *
queueview_rows(void)
{
  return g_rows;
} /* queueview_rows */

size_t
queueview_count(void)
{
  return g_count;
} /* queueview_count */

size_t
queueview_cursor(void)
{
  return g_cursor;
} /* queueview_cursor */

/*
 * queueview_cursor_move: see include/ui/queueview.h.
 */
void
queueview_cursor_move(int delta)
{
  g_cursor = list_cursor_move(g_cursor, g_count, delta);
} /* queueview_cursor_move */

/*
 * queueview_cursor_set: see include/ui/queueview.h.
 */
void
queueview_cursor_set(size_t idx)
{
  if (g_count == 0) {
    g_cursor = 0;
    return;
  }
  g_cursor = (idx >= g_count) ? g_count - 1 : idx;
} /* queueview_cursor_set */
