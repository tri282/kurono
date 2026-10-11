/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_UI_TUI_H
#define KURONO_UI_TUI_H

#include <stdint.h>

#include "browser.h"
#include "keymap.h"   /* UiMode, for the mode-stack accessors below */
#include "layout.h"   /* TuiLayout, for tui_layout() */

/*
 * ui/tui.c owns the terminal: it is the only translation unit that
 * defines TB_IMPL, and the only one that calls a tb_* function. Everything
 * it needs about the running player arrives in a TuiModel filled by
 * main.c, so tui.c calls into neither audio/ nor library/ -- the
 * dependency arrow points one way even though CLAUDE.md would permit the
 * other (ui/ may call audio/). Keeping it out means phase 12 can hand the
 * browser the same struct without untangling engine calls from draw code.
 */

/*
 * What the status bar and header show. main.c fills one of these per
 * repaint from the Player it already owns. Every pointer may be NULL and
 * every count may be zero -- an idle player with no device is the normal
 * phase-11 case, not an error.
 */
typedef struct {
  const char * now_playing;   /* NULL when nothing is loaded */
  const char * message;       /* device state / last error, may be NULL */
  int64_t      pos_ms;
  int64_t      dur_ms;        /* <= 0 when unknown */
  int          playing;
  int          paused;
  int          shuffle;
  int          repeat;        /* 0 off, 1 all, 2 one */
  int          volume_pct;
  int          muted;

  /* Borrowed, may be NULL before the browser exists or in a run that
     has none. tui.c reads it only through browser.h's const accessors:
     ui/ calling ui/ is inside the dependency graph, and browser.c
     itself depends on nothing but libc, so this does not weaken the
     rule that tui.c reaches neither audio/ nor library/. */
  const Browser * browser;

  /* Which view `browser` belongs to, for the column labels only. The
     draw code must not branch on it for anything else: a view is a
     source plus a label, and a switch here is the leak DESIGN.md VIII.4
     forbids (phase-16-browser.md I.1). */
  int             view;

  /* The resolved music directory, for DESIGN.md XI.5's first-run state:
     an empty index shows the path and the variable that changes it.
     Borrowed from Config; NULL on a run with no browser. */
  const char *    music_dir;

  /* Search results, for column 2 while MODE_SEARCH is on top. Borrowed
     from the Player, which owns the rows and frees them when the search
     ends. Column 2's own preview cache is deliberately NOT touched
     while a search is up: the results are drawn instead of it, so
     cancelling restores it byte-for-byte by doing nothing at all
     (plans/kurono/05-impl/phase-16.md step 6). */
  const BrowserRow * search_rows;
  size_t             search_n;

  /* The queue pane's rows, for column 2 while MODE_QUEUE is on top.
     Borrowed from ui/queueview.c, which owns them and the cursor;
     main.c passes them through for the same reason it passes the search
     results through, and column 2's preview cache is untouched here as
     well. `queue_playing` is the play-order position of the entry that
     is sounding, or -1 when nothing is. */
  const BrowserRow * queue_rows;
  size_t             queue_n;
  size_t             queue_cursor;
  int                queue_playing;

  /* DESIGN.md XI.6: a visible underrun counter is worth more than any
     amount of reasoning about whether underruns are happening. Shown
     only when non-zero. */
  unsigned      underruns;

  /* Status bar only, never arithmetic (DESIGN.md II.2's warning). */
  uint32_t      rate;
  int           internal_format;
  uint32_t      internal_channels;
} TuiModel;

int  tui_init(void);
void tui_shutdown(void);
int  tui_active(void);

/*
 * The input mode stack (amendment-04-input-modes.md decision 3). The
 * stack itself is file-scope in tui.c and nothing outside writes it --
 * main.c reads the top and asks for a change through these two calls,
 * which is what keeps input routing inside ui/.
 *
 * ui_mode returns the top of the stack; MODE_NORMAL when nothing is
 * pushed, and never anything else at depth 1.
 *
 * ui_mode_toggle pushes `m` if it is not already on top and pops it if it
 * is. Returns 1 when it pushed, 0 when it popped, and -1 when it refused:
 * the terminal is too small for an overlay to draw without clipping, the
 * stack is at its ceiling, or the mode is not one that can be pushed.
 * The caller reports a refusal; it is never fatal.
 *
 * ui_mode_pop pops one level, and is a no-op at depth 1 -- that is what
 * makes ACT_QUIT's "close the overlay, then quit" rule a dispatch rule
 * rather than a per-overlay binding.
 */
UiMode ui_mode(void);
int    ui_mode_toggle(UiMode m);
void   ui_mode_pop(void);

void tui_get_fds(int * ttyfd, int * resizefd);

void tui_drain(void);
void tui_mark_dirty(void);
int  tui_dirty(void);

void tui_draw(const TuiModel * m);

const TuiLayout * tui_layout(void);

#endif // KURONO_UI_TUI_H
