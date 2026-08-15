/*
 * ui/tui.c -- the terminal: termbox2's lifecycle, the drain loop folded
 * into the phase-4 epoll, DESIGN.md VIII.4's layout, and VIII.3's
 * one-present-per-wakeup redraw discipline.
 *
 * This is the only translation unit that defines TB_IMPL and the only one
 * that calls a tb_* function (`grep -rln termbox2 src` must name this file
 * and nothing else). Phase 12 fills the three columns with the browser;
 * this phase draws the frame they will sit in.
 */

#define TB_IMPL
/* The vendored implementation is not ours and is not edited (CLAUDE.md).
   The pragma is scoped to exactly the #include, the same treatment
   audio/sink.c gives miniaudio's implementation block. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "../../vendor/termbox2.h"
#pragma GCC diagnostic pop

/* termbox2 v2.5.0 puts its TB_IMPL block *outside* its TERMBOX_H_INCL
   include guard: the guard covers the declarations only. ui/theme.h and
   ui/keymap.h include the same header for uintattr_t and struct tb_event,
   and with TB_IMPL still defined that second include re-expands every
   definition -- ~60 redefinition errors. Undefining it here is what makes
   this file the one and only implementation site while still letting it
   use our own headers. */
#undef TB_IMPL

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../../include/ui/keymap.h"
#include "../../include/ui/theme.h"
#include "../../include/ui/tui.h"

/* DESIGN.md VIII.1: TB_OUTPUT_TRUECOLOR is not even defined at the
   default attribute width of 16. This is the assertion the whole Theme
   model rests on -- a Makefile that ever drops -DTB_OPT_ATTR_W=32 fails
   the build here instead of silently producing a colourless player. */
_Static_assert(TB_OPT_ATTR_W == 32, "kurono requires 32-bit termbox attributes");

/* Below this, the layout has no room for a header, one body row, and a
   status bar with a readable progress area. DESIGN.md VIII.4 /
   phase-11-tui.md I.7. */
#define TUI_MIN_W 60
#define TUI_MIN_H 15

/* Column width shares, in percent of the body width left after the
   separators and the (currently zero-width) cover pane. Column 3 takes
   the remainder so rounding never loses or invents a cell. */
#define COL1_PCT 25
#define COL2_PCT 30

/* Reserved cover pane (DESIGN.md IX.1, phase-11-tui.md II). v1 draws
   nothing there and the pane takes no width, so the three columns use the
   full body; the constant and the branch below are the whole obligation
   this phase has to cover art -- arriving later must not require the
   four-region layout to be re-derived after three widgets were written
   against a three-region one. */
#define COVER_PANE_W 0

/* Progress bar width in the status bar, and the space its clock needs. */
#define PROGRESS_W 24

/* The mode indicator's fixed field at the left of the status bar
   (amendment 04 decision 8). Always drawn, MODE_NORMAL included: a field
   that disappears shifts every element right of it, and the status bar's
   remaining width feeds the progress bar's cell count. Twelve columns
   fits "-- SEARCH --" exactly. */
#define MODE_FIELD_W 12

/* The help overlay's box, and the margin it needs around itself. An
   overlay that would not fit refuses to open rather than drawing a
   clipped box (amendment 04 decision 9). */
#define OVERLAY_MARGIN   2
#define OVERLAY_MAX_W    56
#define OVERLAY_KEY_W    14

static Theme     g_theme;
static TuiLayout g_layout;
static int       g_active = 0;
static int       g_dirty  = 1;

/*
 * The mode stack (amendment 04 decision 3). MODE_NORMAL sits at the
 * bottom and the stack is never empty, so ui_mode() never reads out of
 * bounds: a push at the ceiling is dropped and a pop at depth 1 is a
 * no-op. v1 never exceeds depth 2, and a silently truncated stack is a
 * better failure than an abort in a music player.
 *
 * Push and pop are static. Nothing outside this file writes the stack --
 * main.c reads the top through ui_mode() and asks for a change through
 * ui_mode_toggle()/ui_mode_pop(), which is what keeps input routing
 * inside ui/.
 */
#define UI_MODE_STACK_MAX 4
static UiMode g_mode_stack[UI_MODE_STACK_MAX] = { MODE_NORMAL };
static int    g_mode_depth = 1;

/* The overlay scroll offset, in rows. Reset whenever a mode is pushed or
   popped so an overlay never opens halfway down. */
static int g_overlay_scroll = 0;

/*
 * tui_compute_layout: recompute every rectangle from the current
 * terminal size. Called at init and on every TB_EVENT_RESIZE, and by
 * nothing else -- no widget reads tb_width/tb_height itself. Takes and
 * returns nothing; the result is g_layout.
 */
static void
tui_compute_layout(void)
{
  TuiLayout * l = &g_layout;
  memset(l, 0, sizeof *l);

  l->w = tb_width();
  l->h = tb_height();

  if (l->w < TUI_MIN_W || l->h < TUI_MIN_H) {
    l->too_small = 1;
    return;
  }

  /* Row 0 is the header, the last row is the status bar, everything
     between is the body. */
  l->body_y   = 1;
  l->body_h   = l->h - 2;
  l->status_y = l->h - 1;

  /* Two single-column separators between the three columns, plus one
     more before the cover pane when it has width. */
  int cover_w = COVER_PANE_W;
  /* The comparison is always false in v1 by construction -- COVER_PANE_W
     is 0 and the branch exists so the arithmetic is already right when it
     stops being 0 (phase-11-tui.md II). Suppressed rather than deleted;
     deleting it is the retrofit this phase is paying to avoid. */
  /* cppcheck-suppress knownConditionTrueFalse */
  int seps    = 2 + (cover_w > 0 ? 1 : 0);
  int avail   = l->w - seps - cover_w;
  if (avail < 3) {
    l->too_small = 1;
    return;
  }

  l->col_w[0] = avail * COL1_PCT / 100;
  l->col_w[1] = avail * COL2_PCT / 100;
  l->col_w[2] = avail - l->col_w[0] - l->col_w[1];

  l->col_x[0] = 0;
  l->col_x[1] = l->col_w[0] + 1;
  l->col_x[2] = l->col_x[1] + l->col_w[1] + 1;

  l->cover_w = cover_w;
  l->cover_x = (cover_w > 0) ? (l->col_x[2] + l->col_w[2] + 1) : l->w;
} /* tui_compute_layout */

/*
 * draw_text: write str at (x, y), clipped to w columns, in the given
 * slot. Decodes UTF-8 so a multibyte character is one character rather
 * than several, which is what keeps a clipped track title from
 * splitting a codepoint.
 *
 * Advances by the character's DISPLAY WIDTH, not by one per codepoint.
 * termbox2 v2.5.0 has no wide-character cell model -- one cell holds
 * one codepoint and it advances the terminal by whatever that codepoint
 * actually renders as -- so a CJK title written one-cell-per-character
 * runs to twice its budget on screen, overlapping the column separator
 * and whatever is drawn to its right. A music library with Japanese
 * titles hits this on the first frame.
 *
 * wcwidth needs LC_CTYPE, which tui_init sets for termbox's own
 * iswprint filter; the two requirements are the same requirement. A
 * width of -1 (non-printable) is treated as 0 and the character is
 * skipped rather than drawn -- termbox would substitute U+FFFD for it
 * anyway, and a replacement glyph in a filename is noise.
 *
 * Takes the position, the column budget, the slot and the string (NULL
 * draws nothing); returns nothing.
 */
static void
draw_text(int x, int y, int w, StyleSlot slot, const char * str)
{
  if (str == NULL || w <= 0) {
    return;
  }
  uintattr_t fg = style_fg(&g_theme, slot);
  uintattr_t bg = style_bg(&g_theme, slot);

  int          col = 0;
  const char * p   = str;
  while (*p != '\0' && col < w) {
    uint32_t uni = 0;
    int      len = tb_utf8_char_to_unicode(&uni, p);
    if (len <= 0) {
      break;
    }
    p += len;

    int cw = wcwidth((wchar_t) uni);
    if (cw <= 0) {
      continue; /* combining marks, controls: consume, occupy nothing */
    }

    /* A wide character that would straddle the right edge is dropped,
       not half-drawn. Half of a glyph is worse than a space, and the
       cell it would spill into belongs to the separator. */
    if (col + cw > w) {
      break;
    }

    tb_set_cell(x + col, y, uni, fg, bg);
    col += cw;
  }
} /* draw_text */

/*
 * fill_row: paint a run of cells with a slot's background. Used for the
 * header and status bars, which must read as bars even where no text
 * reaches. Takes the position, the run length and the slot; returns
 * nothing.
 */
static void
fill_row(int x, int y, int w, StyleSlot slot)
{
  uintattr_t fg = style_fg(&g_theme, slot);
  uintattr_t bg = style_bg(&g_theme, slot);
  for (int i = 0; i < w; i++) {
    tb_set_cell(x + i, y, ' ', fg, bg);
  }
} /* fill_row */

/*
 * fmt_clock: render milliseconds as m:ss (or h:mm:ss past an hour) into
 * buf. A negative or unknown value renders as "--:--", which is the idle
 * case and not an error. Takes the value and the buffer; returns nothing.
 */
static void
fmt_clock(int64_t ms, char * buf, size_t bufsz)
{
  if (ms < 0) {
    snprintf(buf, bufsz, "--:--");
    return;
  }
  int64_t total = ms / 1000;
  int64_t h = total / 3600;
  int64_t m = (total % 3600) / 60;
  int64_t s = total % 60;
  if (h > 0) {
    snprintf(buf, bufsz, "%lld:%02lld:%02lld", (long long) h, (long long) m,
             (long long) s);
  } else {
    snprintf(buf, bufsz, "%lld:%02lld", (long long) m, (long long) s);
  }
} /* fmt_clock */

/*
 * draw_header: row 0 -- what is playing, or the program name while idle.
 * Takes the model, returns nothing.
 */
static void
draw_header(const TuiModel * m)
{
  fill_row(0, 0, g_layout.w, STYLE_HEADER);
  /* Idle, the header names where the browser is instead of repeating
     the program name -- it is the only place the current directory
     appears, and a single-column browser shows no parent (M1 draws one
     column; DESIGN.md XIV.1). */
  const char * what = "kurono";
  if (m->now_playing != NULL) {
    what = m->now_playing;
  } else if (m->browser != NULL) {
    what = browser_cwd(m->browser);
  }
  draw_text(1, 0, g_layout.w - 2,
            (m->now_playing != NULL) ? STYLE_PLAYING : STYLE_HEADER, what);
} /* draw_header */

/*
 * draw_body: the three empty columns, their separators, and the reserved
 * cover pane. Phase 12 replaces the placeholder text with the browser;
 * the rectangles it draws into are these. Takes nothing, returns nothing.
 */
static void
draw_body(const TuiModel * m)
{
  /* M1 is a single-column directory listing (DESIGN.md XIV.1); the
     three-column browser is M2, phase 16. The listing goes in column 1
     because that is the ranger-style cursor column, and columns 0 and 2
     keep the placeholder until phase 16 gives them sources. */
  static const char * const col_title[3] = { "", "browser", "" };
  const int active_col = 1;

  for (int c = 0; c < 3; c++) {
    if (c == active_col) {
      continue; /* drawn below, from the model */
    }
    draw_text(g_layout.col_x[c], g_layout.body_y, g_layout.col_w[c],
              STYLE_COL_INACTIVE, col_title[c]);
    draw_text(g_layout.col_x[c], g_layout.body_y + 2, g_layout.col_w[c],
              STYLE_DIM, "(phase 16)");
  }

  int cx = g_layout.col_x[active_col];
  int cw = g_layout.col_w[active_col];

  /* Every branch below falls through to the separators and the cover
     pane -- an early return here silently drops them, which is exactly
     what an empty directory did before this was restructured. The frame
     is not the listing's to skip. */
  size_t n      = browser_count(m->browser);
  size_t cursor = browser_cursor(m->browser);
  size_t scroll = browser_scroll(m->browser);

  if (m->browser == NULL) {
    draw_text(cx, g_layout.body_y, cw, STYLE_DIM, "(no browser)");
  } else if (n == 0) {
    draw_text(cx, g_layout.body_y, cw, STYLE_DIM, "(empty)");
  } else {
    /* The widget was told the viewport, so scroll is already consistent
       with the cursor -- this loop draws what it is given and does no
       clamping of its own. Two places deciding what is visible is how
       they come to disagree. */
    for (int row = 0; row < g_layout.body_h; row++) {
      size_t idx = scroll + (size_t) row;
      if (idx >= n) {
        break;
      }
      const BrowserRow * r = browser_row(m->browser, idx);
      if (r == NULL) {
        break;
      }

      StyleSlot slot;
      if (idx == cursor) {
        slot = STYLE_CURSOR;
      } else if (r->is_dir) {
        slot = STYLE_DIR;
      } else {
        slot = STYLE_NORMAL;
      }

      /* The cursor row reads as a bar across the whole column, not just
         under its text, or a short name leaves the selection looking
         ragged against a long one. */
      if (idx == cursor) {
        fill_row(cx, g_layout.body_y + row, cw, STYLE_CURSOR);
      }
      draw_text(cx, g_layout.body_y + row, cw, slot, r->name);
    }
  }

  /* Vertical separators, between the columns only -- the outer edges are
     the terminal's own. */
  uintattr_t bfg = style_fg(&g_theme, STYLE_BORDER);
  uintattr_t bbg = style_bg(&g_theme, STYLE_BORDER);
  for (int c = 0; c < 2; c++) {
    int x = g_layout.col_x[c] + g_layout.col_w[c];
    for (int y = g_layout.body_y; y < g_layout.body_y + g_layout.body_h; y++) {
      tb_set_cell(x, y, 0x2502 /* box drawings light vertical */, bfg, bbg);
    }
  }

  /* Reserved cover pane. COVER_PANE_W is 0 in v1, so this never runs --
     it exists so the pane's border has a call site and the layout does
     not have to be re-derived when a cover renderer arrives
     (DESIGN.md IX.1). */
  if (g_layout.cover_w > 0) {
    uintattr_t cfg = style_fg(&g_theme, STYLE_COVER_BORDER);
    uintattr_t cbg = style_bg(&g_theme, STYLE_COVER_BORDER);
    int x = g_layout.cover_x - 1;
    for (int y = g_layout.body_y; y < g_layout.body_y + g_layout.body_h; y++) {
      tb_set_cell(x, y, 0x2502, cfg, cbg);
    }
  }
} /* draw_body */

/*
 * Mode labels for the status bar's indicator. Sized by MODE_COUNT and
 * checked by the assert below, the same mechanism g_action_name uses: a
 * mode added without a label fails the build rather than drawing an
 * empty field.
 */
static const char * const g_mode_label[MODE_COUNT] = {
  [MODE_NORMAL] = "-- NORMAL --",
  [MODE_HELP]   = "-- HELP --",
  [MODE_MENU]   = "-- MENU --",
  [MODE_SEARCH] = "-- SEARCH --",
};

_Static_assert(sizeof(g_mode_label) / sizeof(g_mode_label[0]) == MODE_COUNT,
               "every UiMode needs a label in g_mode_label");

/*
 * mode_label: takes a mode, returns its status-bar text. Never returns
 * NULL -- an out-of-range mode reads as normal, because a blank field
 * would shift the layout the fixed field exists to keep still.
 */
static const char *
mode_label(UiMode m)
{
  unsigned idx = (unsigned) m;
  if (idx >= (unsigned) MODE_COUNT || g_mode_label[idx] == NULL) {
    idx = (unsigned) MODE_NORMAL;
  }
  return g_mode_label[idx];
} /* mode_label */

/*
 * overlay_fits: 1 when an overlay has room to draw inside the current
 * layout with its margin, 0 otherwise. Takes and returns nothing else.
 *
 * Checked before a push rather than during the draw: refusing to open and
 * saying so is the behaviour amendment 04 decision 9 asks for, and a box
 * clipped by the layout is exactly what it forbids.
 */
static int
overlay_fits(void)
{
  return !g_layout.too_small
         && g_layout.w >= TUI_MIN_W + 2 * OVERLAY_MARGIN
         && g_layout.h >= TUI_MIN_H + 2 * OVERLAY_MARGIN;
} /* overlay_fits */

/*
 * draw_overlay: the help box, centred, drawn over the columns and under
 * nothing. Takes nothing, returns nothing.
 *
 * Its contents are a walk of the MODE_NORMAL binding table through
 * key_name and action_name -- there is no hand-written key list anywhere,
 * so a binding added later appears here with no second edit and cannot
 * drift from what the keymap actually does (amendment 04 decision 10).
 * Rows bound to ACT_NONE are skipped; nothing in v1's table has one, and
 * a config file could produce one as an explicit unbind.
 */
static void
draw_overlay(void)
{
  size_t          n    = 0;
  const Binding * rows = keymap_table(MODE_NORMAL, &n);

  int box_w = g_layout.w - 2 * OVERLAY_MARGIN;
  if (box_w > OVERLAY_MAX_W) {
    box_w = OVERLAY_MAX_W;
  }
  int box_h = g_layout.h - 2 * OVERLAY_MARGIN;
  int box_x = (g_layout.w - box_w) / 2;
  int box_y = (g_layout.h - box_h) / 2;

  /* One row of title, one blank beneath it, one bottom pad. */
  int list_y    = box_y + 2;
  int list_rows = box_h - 3;
  if (list_rows < 1) {
    return;
  }

  /* Clamped here rather than where the scroll is changed: the dispatch
     path does not know how many rows the table has or how tall the box
     is, and both change with the terminal size. */
  int max_scroll = (int) n - list_rows;
  if (max_scroll < 0) {
    max_scroll = 0;
  }
  if (g_overlay_scroll > max_scroll) {
    g_overlay_scroll = max_scroll;
  }
  if (g_overlay_scroll < 0) {
    g_overlay_scroll = 0;
  }

  for (int r = 0; r < box_h; r++) {
    fill_row(box_x, box_y + r, box_w, STYLE_POPUP);
  }

  char title[64];
  snprintf(title, sizeof title, "keys  (%d-%d of %zu)  h or esc closes",
           (n == 0) ? 0 : g_overlay_scroll + 1,
           (g_overlay_scroll + list_rows > (int) n) ? (int) n
                                                    : g_overlay_scroll + list_rows,
           n);
  draw_text(box_x + 2, box_y, box_w - 4, STYLE_POPUP, title);

  int drawn = 0;
  for (size_t i = (size_t) g_overlay_scroll; i < n && drawn < list_rows; i++) {
    if (rows[i].action == ACT_NONE) {
      continue;
    }
    draw_text(box_x + 2, list_y + drawn, OVERLAY_KEY_W, STYLE_POPUP,
              key_name(&rows[i]));
    draw_text(box_x + 2 + OVERLAY_KEY_W, list_y + drawn,
              box_w - 4 - OVERLAY_KEY_W, STYLE_POPUP,
              action_name(rows[i].action));
    drawn++;
  }
} /* draw_overlay */

/*
 * draw_statusbar: the last row -- progress bar, clock, volume, shuffle
 * and repeat indicators, and the current message. Takes the model,
 * returns nothing.
 */
static void
draw_statusbar(const TuiModel * m)
{
  int y = g_layout.status_y;
  fill_row(0, y, g_layout.w, STYLE_STATUSBAR);

  /* The mode indicator, a fixed field at the left. Dropped before the
     progress bar when the row cannot hold both, which is the order
     amendment 04 decision 8 sets: the bar is the element that has to
     survive a narrow terminal. */
  int bar_x = 1;
  if (g_layout.w >= MODE_FIELD_W + PROGRESS_W + 4) {
    draw_text(1, y, MODE_FIELD_W, STYLE_MODE, mode_label(ui_mode()));
    bar_x = MODE_FIELD_W + 1;
  }

  /* Progress bar. An unknown duration leaves it empty rather than full:
     a bar that reads 100% for an unseekable stream is worse than a bar
     that reads nothing. */
  int filled = 0;
  if (m->dur_ms > 0 && m->pos_ms > 0) {
    int64_t f = (int64_t) PROGRESS_W * m->pos_ms / m->dur_ms;
    filled = (f < 0) ? 0 : (f > PROGRESS_W) ? PROGRESS_W : (int) f;
  }
  for (int i = 0; i < PROGRESS_W && bar_x + i < g_layout.w; i++) {
    StyleSlot slot = (i < filled) ? STYLE_PROGRESS_FILL : STYLE_PROGRESS_EMPTY;
    tb_set_cell(bar_x + i, y, 0x2500 /* box drawings light horizontal */,
                style_fg(&g_theme, slot), style_bg(&g_theme, slot));
  }

  char pos[16], dur[16];
  fmt_clock(m->playing || m->pos_ms > 0 ? m->pos_ms : -1, pos, sizeof pos);
  fmt_clock(m->dur_ms > 0 ? m->dur_ms : -1, dur, sizeof dur);

  /* Underruns are shown only when non-zero (DESIGN.md XI.6). A counter
     that always reads "0 underruns" trains the eye to skip it, which
     is the opposite of why it is on screen. */
  char un[32];
  un[0] = '\0';
  if (m->underruns > 0) {
    snprintf(un, sizeof un, "  %u underrun%s", m->underruns,
             (m->underruns == 1) ? "" : "s");
  }

  char vol[24];
  if (m->muted) {
    snprintf(vol, sizeof vol, "vol muted");
  } else {
    snprintf(vol, sizeof vol, "vol %d%%", m->volume_pct);
  }

  char right[192];
  snprintf(right, sizeof right, "%s / %s  %s  %s  %s%s",
           pos, dur, vol,
           m->shuffle ? "shuffle" : "-",
           (m->repeat == 2) ? "repeat one" : (m->repeat == 1) ? "repeat all" : "-",
           un);

  int x = bar_x + PROGRESS_W + 2;
  draw_text(x, y, g_layout.w - x, STYLE_STATUSBAR, right);

  /* The message takes the row above the status bar rather than competing
     with the clock for space. Errors get the error slot; anything else
     is dim. */
  if (m->message != NULL && g_layout.body_h > 0) {
    draw_text(1, y - 1, g_layout.w - 2, STYLE_DIM, m->message);
  }
} /* draw_statusbar */

/*
 * tui_draw: repaint everything and present exactly once. This is the only
 * present call in the program (DESIGN.md VIII.3: never more than one per
 * wakeup), and it clears the dirty flag. Takes the model,
 * returns nothing. A too-small terminal draws one line and nothing else.
 */
void
tui_draw(const TuiModel * m)
{
  if (!g_active) {
    return;
  }

  tb_clear();

  if (g_layout.too_small) {
    draw_text(0, 0, g_layout.w, STYLE_ERROR, "terminal too small");
  } else {
    draw_header(m);
    draw_body(m);
    draw_statusbar(m);
    /* Last, and still before the single present -- the overlay covers the
       columns and the browser underneath keeps its geometry, knowing
       nothing about it (amendment 04 decision 9). */
    if (ui_mode() == MODE_HELP) {
      draw_overlay();
    }
  }

  tb_present();
  g_dirty = 0;
} /* tui_draw */

/*
 * overlay_scroll_action: consume the cursor actions while an overlay owns
 * the screen. Takes the action, returns 1 when it was consumed and 0 when
 * the caller should dispatch it.
 *
 * The overlay tables bind ACT_CURSOR_* deliberately -- per-mode tables
 * exist so the same action means "move down whichever list is focused"
 * without an ACT_MENU_UP family. That only holds if the focused list is
 * the one that moves, so these never reach main.c's browser while an
 * overlay is up. The clamp lives in draw_overlay, which is the only place
 * that knows the row count and the box height.
 */
static int
overlay_scroll_action(Action a)
{
  switch (a) {
    case ACT_CURSOR_UP:        g_overlay_scroll -= 1;  return 1;
    case ACT_CURSOR_DOWN:      g_overlay_scroll += 1;  return 1;
    case ACT_CURSOR_PAGE_UP:   g_overlay_scroll -= 10; return 1;
    case ACT_CURSOR_PAGE_DOWN: g_overlay_scroll += 10; return 1;
    case ACT_CURSOR_HOME:      g_overlay_scroll  = 0;  return 1;
    /* Any value past the end; draw_overlay clamps it to the real last
       page, which it computes from the current box height. */
    case ACT_CURSOR_END:       g_overlay_scroll  = 1 << 20; return 1;
    default:                   return 0;
  }
} /* overlay_scroll_action */

/*
 * dispatch_event: route one termbox event by its type. Key events resolve
 * against the table for the current mode and go to action_dispatch;
 * resize recomputes the layout. Every path marks the screen dirty -- the
 * single present happens later, at the bottom of the loop iteration.
 * Takes the event, returns nothing.
 */
static void
dispatch_event(const struct tb_event * ev)
{
  switch (ev->type) {
    case TB_EVENT_KEY: {
      int32_t arg  = 0;
      UiMode  mode = ui_mode();
      Action  a    = keymap_resolve(mode, ev, &arg);
      if (a != ACT_NONE && !(mode != MODE_NORMAL && overlay_scroll_action(a))) {
        action_dispatch(a, arg);
      }
      g_dirty = 1;
      break;
    }

    case TB_EVENT_RESIZE:
      tui_compute_layout();
      g_dirty = 1;
      break;

    case TB_EVENT_MOUSE:
    default:
      break; /* mouse input is not bound; DESIGN.md VIII.5 is key-only */
  }
} /* dispatch_event */

/*
 * tui_drain: dispatch every buffered termbox event, then return. Both the
 * tty and the resize wakeup call this same function and dispatch on
 * ev.type -- there is no fd-to-event-type correspondence, because
 * tb_peek_event internally selects on both fds regardless of which one
 * woke epoll, so a tty wakeup can legitimately yield a resize event and
 * vice versa. Two type-specific handlers would drop events (DESIGN.md
 * VIII.2). Takes and returns nothing.
 *
 * The loop condition is == TB_OK and NOT != TB_ERR_NO_EVENT. With
 * timeout_ms = 0, termbox2 v2.5.0's wait_event also returns
 * TB_ERR_NEED_MORE on a partial escape sequence and TB_ERR_POLL when its
 * select was interrupted; neither is TB_ERR_NO_EVENT, so the naive
 * condition spins at 100% CPU on a split escape sequence -- reachable on
 * a slow ssh link or a large paste. Exiting on any non-TB_OK lets the
 * next epoll wakeup retry, which is exactly what the level-triggered fd
 * guarantees will happen.
 */
void
tui_drain(void)
{
  struct tb_event ev;
  while (tb_peek_event(&ev, 0) == TB_OK) {
    dispatch_event(&ev);
  }
} /* tui_drain */

/*
 * tui_init: claim the terminal. tb_init, then truecolor (degrading the
 * theme in exactly one place if the terminal cannot do it), then
 * TB_INPUT_ESC so a bare Escape is delivered as a key rather than held
 * back waiting for a sequence, then hide the cursor. Takes nothing,
 * returns 0 on success or -1 with the terminal untouched.
 *
 * SIGWINCH is deliberately not handled here and must never be added to
 * main.c's signalfd set: termbox2 installs its own handler and delivers
 * resize through its own self-pipe, which is the fd tui_get_fds hands to
 * epoll (DESIGN.md VIII.2).
 */
int
tui_init(void)
{
  /* Load-bearing, and the failure it prevents is silent and total.
     termbox2 v2.5.0's cell_write (termbox2.h:3268) filters every
     codepoint through iswprint(3) and substitutes U+FFFD for anything
     that fails. iswprint is locale-dependent, and a C program starts in
     the "C" locale, where nothing above U+007F is printable -- so
     without this call every box-drawing separator, the progress bar,
     and every non-ASCII character in a filename renders as the
     replacement glyph. Measured before the fix: 78 U+FFFD and zero
     U+2502 in one frame of a music tree with Japanese titles.
     LC_CTYPE only: kurono formats no numbers or dates for the user, and
     a full setlocale(LC_ALL, "") would put strcoll and printf's decimal
     separator under the environment's control for no benefit.
     It belongs here rather than in main because it is a termbox
     requirement and tui.c is the file that owns termbox -- the headless
     diagnostic modes draw nothing and need no locale. */
  setlocale(LC_CTYPE, "");

  if (tb_init() != TB_OK) {
    return -1;
  }

  g_theme = g_theme_default;
  if (tb_set_output_mode(TB_OUTPUT_TRUECOLOR) != TB_OK) {
    /* The one and only degrade site (DESIGN.md VIII.5). Not idempotent,
       which is another reason it may not move into a draw path. */
    theme_degrade_to_256(&g_theme);
    (void) tb_set_output_mode(TB_OUTPUT_256);
  }

  tb_set_input_mode(TB_INPUT_ESC);
  tb_hide_cursor();

  tui_compute_layout();
  g_active = 1;
  g_dirty  = 1;
  return 0;
} /* tui_init */

/*
 * tui_shutdown: release the terminal. Teardown step 8 (DESIGN.md
 * IV.9.b): after this returns, and only after it, stderr may be written
 * again. Idempotent -- kurono_die and shutdown_all can both reach it.
 * Takes and returns nothing.
 */
void
tui_shutdown(void)
{
  if (!g_active) {
    return;
  }
  g_active = 0;
  tb_shutdown();
} /* tui_shutdown */

/* ui_mode: see include/ui/tui.h. */
UiMode
ui_mode(void)
{
  return g_mode_stack[g_mode_depth - 1];
} /* ui_mode */

/* ui_mode_pop: see include/ui/tui.h. */
void
ui_mode_pop(void)
{
  if (g_mode_depth > 1) {
    g_mode_depth--;
    g_overlay_scroll = 0;
    g_dirty = 1;
  }
} /* ui_mode_pop */

/* ui_mode_toggle: see include/ui/tui.h. */
int
ui_mode_toggle(UiMode m)
{
  if (m == MODE_NORMAL || (unsigned) m >= (unsigned) MODE_COUNT) {
    return -1;
  }

  if (ui_mode() == m) {
    ui_mode_pop();
    return 0;
  }

  if (!overlay_fits() || g_mode_depth >= UI_MODE_STACK_MAX) {
    return -1;
  }

  g_mode_stack[g_mode_depth++] = m;
  g_overlay_scroll = 0;
  g_dirty = 1;
  return 1;
} /* ui_mode_toggle */

/*
 * tui_active: whether the terminal is currently claimed. Takes nothing,
 * returns 1 while it is.
 */
int
tui_active(void)
{
  return g_active;
} /* tui_active */

/*
 * tui_get_fds: termbox2's tty and resize fds, for main.c to register with
 * the phase-4 epoll (DESIGN.md IV.9.a step 9). These are the only two fds
 * event.c does not own; it neither reads nor closes them. Takes two out
 * parameters, returns nothing -- both are set to -1 when the terminal is
 * not claimed.
 */
void
tui_get_fds(int * ttyfd, int * resizefd)
{
  if (!g_active || tb_get_fds(ttyfd, resizefd) != TB_OK) {
    *ttyfd    = -1;
    *resizefd = -1;
  }
} /* tui_get_fds */

/*
 * tui_mark_dirty / tui_dirty: the redraw flag DESIGN.md VIII.3 puts on
 * every branch of the loop. main.c sets it from the branches tui.c cannot
 * see (engine events, the reconnect ladder, the redraw tick) and reads it
 * once at the bottom of the iteration.
 */
void
tui_mark_dirty(void)
{
  g_dirty = 1;
} /* tui_mark_dirty */

int
tui_dirty(void)
{
  return g_dirty;
} /* tui_dirty */

/*
 * tui_layout: the current rectangles, for the widgets phase 12 adds.
 * Takes nothing, returns a pointer to the layout, which is valid until
 * the next resize.
 */
const TuiLayout *
tui_layout(void)
{
  return &g_layout;
} /* tui_layout */
