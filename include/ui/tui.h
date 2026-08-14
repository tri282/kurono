#ifndef KURONO_UI_TUI_H
#define KURONO_UI_TUI_H

#include <stdint.h>

#include "browser.h"

/*
 * ui/tui.c owns the terminal: it is the only translation unit that
 * defines TB_IMPL, and the only one that calls a tb_* function. Everything
 * it needs about the running player arrives in a TuiModel filled by
 * main.c, so tui.c calls into neither audio/ nor library/ -- the
 * dependency arrow points one way even though CLAUDE.md would permit the
 * other (ui/ may call audio/). Keeping it out means phase 12 can hand the
 * browser the same struct without untangling engine calls from draw code.
 */

/* DESIGN.md VIII.4's layout, in rows and columns. Recomputed on resize
   and on nothing else; no widget reads tb_width/tb_height itself. */
typedef struct {
  int w, h;              /* terminal size at the last layout */
  int too_small;         /* 1 when below TUI_MIN_W x TUI_MIN_H */
  int body_y, body_h;    /* the three-column region */
  int col_x[3];          /* left edge of each column */
  int col_w[3];          /* width of each column, separators excluded */
  int cover_x, cover_w;  /* reserved cover pane; cover_w is 0 in v1 */
  int status_y;          /* the status bar row */
} TuiLayout;

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

void tui_get_fds(int * ttyfd, int * resizefd);

void tui_drain(void);
void tui_mark_dirty(void);
int  tui_dirty(void);

void tui_draw(const TuiModel * m);

const TuiLayout * tui_layout(void);

#endif // KURONO_UI_TUI_H
