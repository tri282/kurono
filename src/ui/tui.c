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

#include <stdio.h>
#include <string.h>

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

static Theme     g_theme;
static TuiLayout g_layout;
static int       g_active = 0;
static int       g_dirty  = 1;

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
 * draw_text: write str at (x, y), clipped to w cells, in the given slot.
 * Decodes UTF-8 so a multibyte character costs one cell rather than
 * several, which is what keeps a clipped track title from splitting a
 * codepoint. Takes the position, the width budget, the slot and the
 * string (NULL draws nothing); returns nothing.
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
    tb_set_cell(x + col, y, uni, fg, bg);
    p += len;
    col++;
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
  const char * what = (m->now_playing != NULL) ? m->now_playing : "kurono";
  draw_text(1, 0, g_layout.w - 2,
            (m->now_playing != NULL) ? STYLE_PLAYING : STYLE_HEADER, what);
} /* draw_header */

/*
 * draw_body: the three empty columns, their separators, and the reserved
 * cover pane. Phase 12 replaces the placeholder text with the browser;
 * the rectangles it draws into are these. Takes nothing, returns nothing.
 */
static void
draw_body(void)
{
  static const char * const col_title[3] = { "artists", "albums", "tracks" };

  for (int c = 0; c < 3; c++) {
    /* The active column is column 1 until the browser owns a cursor
       (phase 12); the inactive slot is what the other two use, which is
       what gives STYLE_COL_INACTIVE a call site now rather than a
       retrofit later. */
    StyleSlot slot = (c == 0) ? STYLE_NORMAL : STYLE_COL_INACTIVE;
    draw_text(g_layout.col_x[c], g_layout.body_y, g_layout.col_w[c], slot,
              col_title[c]);
    draw_text(g_layout.col_x[c], g_layout.body_y + 2, g_layout.col_w[c],
              STYLE_DIM, "(empty)");
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
 * draw_statusbar: the last row -- progress bar, clock, volume, shuffle
 * and repeat indicators, and the current message. Takes the model,
 * returns nothing.
 */
static void
draw_statusbar(const TuiModel * m)
{
  int y = g_layout.status_y;
  fill_row(0, y, g_layout.w, STYLE_STATUSBAR);

  /* Progress bar. An unknown duration leaves it empty rather than full:
     a bar that reads 100% for an unseekable stream is worse than a bar
     that reads nothing. */
  int filled = 0;
  if (m->dur_ms > 0 && m->pos_ms > 0) {
    int64_t f = (int64_t) PROGRESS_W * m->pos_ms / m->dur_ms;
    filled = (f < 0) ? 0 : (f > PROGRESS_W) ? PROGRESS_W : (int) f;
  }
  for (int i = 0; i < PROGRESS_W && i + 1 < g_layout.w; i++) {
    StyleSlot slot = (i < filled) ? STYLE_PROGRESS_FILL : STYLE_PROGRESS_EMPTY;
    tb_set_cell(1 + i, y, 0x2500 /* box drawings light horizontal */,
                style_fg(&g_theme, slot), style_bg(&g_theme, slot));
  }

  char pos[16], dur[16];
  fmt_clock(m->playing || m->pos_ms > 0 ? m->pos_ms : -1, pos, sizeof pos);
  fmt_clock(m->dur_ms > 0 ? m->dur_ms : -1, dur, sizeof dur);

  char right[128];
  snprintf(right, sizeof right, "%s / %s  vol %d%%  %s  %s",
           pos, dur, m->volume_pct,
           m->shuffle ? "shuffle" : "-",
           (m->repeat == 2) ? "repeat one" : (m->repeat == 1) ? "repeat all" : "-");

  int x = PROGRESS_W + 3;
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
    draw_body();
    draw_statusbar(m);
  }

  tb_present();
  g_dirty = 0;
} /* tui_draw */

/*
 * dispatch_event: route one termbox event by its type. Key events resolve
 * through the phase-10 binding table to an Action and go to
 * action_dispatch; resize recomputes the layout. Every path marks the
 * screen dirty -- the single present happens later, at the bottom of the
 * loop iteration. Takes the event, returns nothing.
 */
static void
dispatch_event(const struct tb_event * ev)
{
  switch (ev->type) {
    case TB_EVENT_KEY: {
      int32_t arg = 0;
      Action  a   = keymap_resolve(ev, &arg);
      if (a != ACT_NONE) {
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
