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
 * TuiList: one region's rows (design II.5). `scroll` is used as given
 * for MAIN, FACET and INFO; tui.c centres PARENT, QUEUE and SOURCES on
 * the cursor itself. A cursor >= n marks no row.
 */
typedef struct {
  const BrowserRow * rows;
  size_t             n, cursor, scroll;
  int                playing;  /* row overdrawn in STYLE_PLAYING, -1 for none */
  const char *       empty;    /* drawn dim when n == 0; may be NULL */
  int                capped;   /* filled in phase 5 */
  int                centre;   /* centre each row (Now playing) */
} TuiList;

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

  /* One list per region role (design II.5); tui.c draws whichever roles
     the current layout allocated. Rows are borrowed until the next
     repaint. */
  TuiList         list[ROLE_COUNT];

  /* Header text when idle (design II.8); NULL draws the program name. */
  const char *    crumb;

  /* DESIGN.md XI.5's first-run state: MAIN is an empty index, so the
     draw adds the music directory and the variable that changes it. */
  int             library_empty;

  /* The resolved music directory, for library_empty. Borrowed from
     Config; NULL on a run with no root. */
  const char *    music_dir;

  /* Search results, drawn into INFO while MODE_SEARCH is on top (decide
     D22). Borrowed from the Player, which owns the rows and frees them
     when the search ends. Removed in phase 5. */
  const BrowserRow * search_rows;
  size_t             search_n;

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

/*
 * View state (design II.1, II.7), owned by tui.c beside the mode stack.
 *
 * ui_view returns the current view. ui_view_set stores `v`, recomputes
 * the layout and marks the screen dirty; the caller validates the range.
 * ui_focus returns the role that owns the cursor: ROLE_MAIN in Library
 * and Directories, ROLE_QUEUE in Playlists, ROLE_COUNT (none) in Now
 * playing and Settings.
 */
UiView     ui_view(void);
void       ui_view_set(UiView v);
RegionRole ui_focus(void);

void tui_get_fds(int * ttyfd, int * resizefd);

void tui_drain(void);
void tui_mark_dirty(void);
int  tui_dirty(void);

void tui_draw(const TuiModel * m);

const TuiLayout * tui_layout(void);

#endif // KURONO_UI_TUI_H
