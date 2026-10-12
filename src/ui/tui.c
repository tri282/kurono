/* SPDX-License-Identifier: GPL-3.0-or-later */

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
#include "../../include/ui/layout.h"
#include "../../include/ui/listgeom.h"
#include "../../include/ui/theme.h"
#include "../../include/ui/tui.h"

/* DESIGN.md VIII.1: TB_OUTPUT_TRUECOLOR is not even defined at the
   default attribute width of 16. This is the assertion the whole Theme
   model rests on -- a Makefile that ever drops -DTB_OPT_ATTR_W=32 fails
   the build here instead of silently producing a colourless player. */
_Static_assert(TB_OPT_ATTR_W == 32, "kurono requires 32-bit termbox attributes");

/* Progress bar width in the status bar, and the space its clock needs. */
#define PROGRESS_W 24

/* The mode indicator's fixed field at the left of the status bar
   (amendment 04 decision 8). Always drawn, MODE_NORMAL included: a field
   that disappears shifts every element right of it, and the status bar's
   remaining width feeds the progress bar's cell count. Twelve columns
   fits "-- FILTER --" exactly. */
#define MODE_FIELD_W 12

/* The view indicator's fixed field at the left of the header. Always
   drawn, playing or idle: losing it the moment playback starts is the
   defect this exists to fix (v1.0.1 #14), and it is worst on a narrow
   terminal, which is where a drop-below-floor rule would drop it.
   Fourteen columns leaves room for the longest view label (11, "now
   playing") plus its separator. */
#define VIEW_FIELD_W 14

/* Below this budget the ellipsis is more of the field than the content
   is, so a narrow field truncates silently the way it always did. Four
   columns is the narrowest field in the program that still carries a
   word, and draw_text is genuinely called with budgets this small --
   column 1 at TUI_MIN_W is around fourteen usable columns and the
   overlay's key column is fourteen. */
#define ELLIPSIS_MIN_W 4

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

/* The current view (design II.1, II.7). Input-routing state like the
   mode stack, so it lives here; main.c reads it through ui_view(). The
   startup view is assigned by main.c, not taken from this initialiser. */
static UiView g_view = VIEW_LIBRARY;

/* The overlay scroll offset, in rows. Reset whenever a mode is pushed or
   popped so an overlay never opens halfway down. */
static int g_overlay_scroll = 0;

/*
 * Library input state (design II.7, IV.7a): which of FACET and MAIN has
 * the cursor, whether the artist column is switched on, whether a filter
 * is being edited, and the two filter buffers. Terminal text state, so it
 * lives here; main.c reads it through ui_focus/ui_facet_on/ui_filter at
 * repaint. Fixed buffers keep text entry out of the allocator; a full
 * buffer drops the keystroke.
 */
#define FILTER_MAX 256
static RegionRole g_focus    = ROLE_MAIN;
static int        g_facet_on = 1;
static int        g_editing  = 0;
static char       g_filter_facet[FILTER_MAX];
static char       g_filter_main[FILTER_MAX];

/* The `200+` marker on MAIN's filter line (design IV.5). */
#define FILTER_CAP_MARK "200+"

/*
 * filter_buf: the writable filter buffer for `r`. Takes a role, returns
 * the FACET or MAIN buffer, or NULL for any other role.
 */
static char *
filter_buf(RegionRole r)
{
  switch (r) {
    case ROLE_FACET: return g_filter_facet;
    case ROLE_MAIN:  return g_filter_main;
    default:         return NULL;
  }
} /* filter_buf */

/*
 * tui_compute_layout: recompute every rectangle from the current view
 * and terminal size. Called at init, on every TB_EVENT_RESIZE and by
 * ui_view_set, and by nothing else -- no widget reads tb_width/tb_height
 * itself. Takes and returns nothing; the result is g_layout.
 */
static void
tui_compute_layout(void)
{
  layout_compute(g_view, tb_width(), tb_height(), g_facet_on, &g_layout);

  /* Focus never rests on a region that is not on screen; moving it ends
     an edit so the next keystroke cannot land in the other buffer
     (design II.7, decide D17). */
  if (g_view == VIEW_LIBRARY && g_focus == ROLE_FACET
      && layout_find(&g_layout, ROLE_FACET) == NULL) {
    g_focus   = ROLE_MAIN;
    g_editing = 0;
  }
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
 * A string too long for its budget spends its last column on U+2026, so
 * a truncated name reads as truncated instead of as a short one. Three
 * ASCII dots would cost three columns of content and this program's
 * narrowest real field is around fourteen. Below ELLIPSIS_MIN_W the
 * marker is suppressed and the cut is silent as it always was: in four
 * columns the ellipsis is more of the field than the content.
 *
 * The marker lands on the first column the walk left free rather than on
 * the last column of the budget. Those differ exactly when the drop rule
 * above fired -- a wide glyph refused at the edge leaves a column
 * behind, and placing the marker at the far edge instead would open a
 * visible gap between the content and it.
 *
 * Takes the position, the column budget, the slot and the string (NULL
 * draws nothing); returns nothing.
 */
/*
 * text_width: display width of a UTF-8 string in terminal columns, by
 * the same codepoint walk and the same wcwidth rule draw_text uses --
 * a non-printable character occupies nothing. The two must not disagree:
 * draw_text asks this whether the content overflows its budget, and an
 * answer measured a different way would put an ellipsis on a string that
 * fits, or omit one from a string that does not.
 *
 * Takes the string (NULL is zero); returns the column count.
 */
static int
text_width(const char * str)
{
  int          width = 0;
  const char * p     = str;

  if (str == NULL) {
    return 0;
  }
  while (*p != '\0') {
    uint32_t uni = 0;
    int      len = tb_utf8_char_to_unicode(&uni, p);
    if (len <= 0) {
      break;
    }
    p += len;

    int cw = wcwidth((wchar_t) uni);
    if (cw > 0) {
      width += cw;
    }
  }
  return width;
} /* text_width */

static void
draw_text(int x, int y, int w, StyleSlot slot, const char * str)
{
  if (str == NULL || w <= 0) {
    return;
  }
  uintattr_t fg = style_fg(&g_theme, slot);
  uintattr_t bg = style_bg(&g_theme, slot);

  /* One column is reserved for the marker before the walk starts, so the
     drop rule below measures against the same budget the content will
     actually get. */
  int budget = w;
  int mark   = 0;
  if (w >= ELLIPSIS_MIN_W && text_width(str) > w) {
    budget = w - 1;
    mark   = 1;
  }

  int          col = 0;
  const char * p   = str;
  while (*p != '\0' && col < budget) {
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
    if (col + cw > budget) {
      break;
    }

    tb_set_cell(x + col, y, uni, fg, bg);
    col += cw;
  }

  if (mark) {
    tb_set_cell(x + col, y, 0x2026 /* HORIZONTAL ELLIPSIS */, fg, bg);
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
 * View labels for the header's view field (design II.8). Unsized and
 * checked below, so a view added without a label fails the build.
 */
static const char * const g_view_label[] = {
  [VIEW_LIBRARY]     = "library",
  [VIEW_DIRECTORIES] = "directories",
  [VIEW_PLAYLISTS]   = "playlists",
  [VIEW_NOW_PLAYING] = "now playing",
  [VIEW_SETTINGS]    = "settings",
};

_Static_assert(sizeof(g_view_label) / sizeof(g_view_label[0]) == VIEW_COUNT,
               "g_view_label must have one entry per view");

/*
 * draw_header: row 0 -- the view field, then what is playing, else the
 * model's crumb, else the program name (design II.8). Takes the model,
 * returns nothing.
 */
static void
draw_header(const TuiModel * m)
{
  fill_row(0, 0, g_layout.w, STYLE_HEADER);

  const char * what = "kurono";
  if (m->now_playing != NULL) {
    what = m->now_playing;
  } else if (m->crumb != NULL) {
    what = m->crumb;
  }

  /* The view label gets its own fixed field so starting playback -- which
     overwrites `what` above -- never erases it (v1.0.1 #14). */
  const char * view = ((unsigned) g_view < (unsigned) VIEW_COUNT) ? g_view_label[g_view] : "";
  draw_text(1, 0, VIEW_FIELD_W - 1, STYLE_HEADER, view);

  int x = 1 + VIEW_FIELD_W;
  draw_text(x, 0, g_layout.w - 1 - x,
            (m->now_playing != NULL) ? STYLE_PLAYING : STYLE_HEADER, what);
} /* draw_header */

/*
 * draw_list: draw one region's rows below its reserved top rows. Takes
 * the region, the reserved row count, the row array and its length, the
 * cursor index, the scroll offset, whether the region draws a cursor bar,
 * whether it only marks the cursor row, the base slot (STYLE_COUNT means
 * "decide per row from is_dir") and whether rows are centred. Returns
 * nothing.
 */
static void
draw_list(const Region * reg, int top, const BrowserRow * rows, size_t n,
          size_t cursor, size_t scroll, int is_active, int mark_only,
          StyleSlot base, int centre)
{
  for (int r = 0; r < reg->h - top; r++) {
    size_t idx = scroll + (size_t) r;
    if (idx >= n || rows == NULL) {
      break;
    }

    const BrowserRow * br = &rows[idx];

    StyleSlot slot;
    if (is_active && idx == cursor) {
      slot = STYLE_CURSOR;
    } else if (mark_only && idx == cursor) {
      slot = STYLE_SELECTED;
    } else if (base != STYLE_COUNT) {
      slot = base;
    } else if (br->is_dir) {
      slot = STYLE_DIR;
    } else {
      slot = STYLE_NORMAL;
    }

    /* The cursor row reads as a bar across the whole region, not just
       under its text, or a short name leaves the selection looking
       ragged against a long one. */
    if ((is_active || mark_only) && idx == cursor) {
      fill_row(reg->x, reg->y + top + r, reg->w, slot);
    }

    int off = 0;
    if (centre) {
      int tw = text_width(br->name);
      off = (tw < reg->w) ? (reg->w - tw) / 2 : 0;
    }
    draw_text(reg->x + off, reg->y + top + r, reg->w - off, slot, br->name);
  }
} /* draw_list */

/* How a role's cursor row draws (design II.5). */
#define CUR_NONE  0   /* no cursor row */
#define CUR_MARK  1   /* STYLE_SELECTED mark */
#define CUR_FOCUS 2   /* STYLE_CURSOR bar when focused, else a mark */

/*
 * Per-role draw style (design II.5), indexed by RegionRole: the cursor
 * rule, the slot for other rows (STYLE_COUNT: per is_dir), and whether
 * tui.c centres the scroll on the cursor rather than using the list's.
 */
static const struct {
  int       cursor;
  StyleSlot base;
  int       tui_scroll;
} g_role_style[ROLE_COUNT] = {
  [ROLE_PARENT]  = { CUR_MARK,  STYLE_COUNT,        1 },
  [ROLE_MAIN]    = { CUR_FOCUS, STYLE_COUNT,        0 },
  [ROLE_FACET]   = { CUR_FOCUS, STYLE_COUNT,        0 },
  [ROLE_INFO]    = { CUR_NONE,  STYLE_COL_INACTIVE, 0 },
  [ROLE_QUEUE]   = { CUR_FOCUS, STYLE_COUNT,        1 },
  [ROLE_COVER]   = { CUR_NONE,  STYLE_COUNT,        0 },
  [ROLE_SOURCES] = { CUR_MARK,  STYLE_NORMAL,       1 },
};

/*
 * draw_region: draw one allocated region from its list (design II.5).
 * Takes the region and the list; returns nothing. An empty list draws
 * its `empty` text dim at the first list row.
 */
static void
draw_region(const Region * reg, const TuiList * l)
{
  if ((unsigned) reg->role >= (unsigned) ROLE_COUNT) {
    return;
  }

  int top  = layout_list_top(g_view, reg->role);
  int rows = reg->h - top;

  if (l->n == 0 || l->rows == NULL) {
    if (l->empty != NULL) {
      draw_text(reg->x, reg->y + top, reg->w, STYLE_DIM, l->empty);
    }
    return;
  }

  int    rule    = g_role_style[reg->role].cursor;
  int    focused = (rule == CUR_FOCUS && ui_focus() == reg->role);
  int    mark    = (rule == CUR_MARK || (rule == CUR_FOCUS && !focused));
  size_t scroll  = g_role_style[reg->role].tui_scroll
                     ? list_scroll_for(l->cursor, l->n, rows)
                     : l->scroll;

  draw_list(reg, top, l->rows, l->n, l->cursor, scroll, focused, mark,
            g_role_style[reg->role].base, l->centre);

  /* The playing row, marked after the list rather than through it:
     draw_list decides a row's slot from the cursor and one base style,
     and the playing row is neither. Skipped on the cursor row, because
     the cursor has to win. */
  if (l->playing >= 0 && (size_t) l->playing != l->cursor) {
    size_t pi = (size_t) l->playing;
    if (pi >= scroll && pi < scroll + (size_t) rows && pi < l->n) {
      draw_text(reg->x, reg->y + top + (int) (pi - scroll), reg->w,
                STYLE_PLAYING, l->rows[pi].name);
    }
  }
} /* draw_region */

/*
 * draw_filter_line: the top row of a Library FACET or MAIN region (design
 * IV.5): `/` and the region's filter, a block cursor while that region is
 * being edited, `200+` on MAIN when the listing is capped, and a dim hint
 * when there is nothing to show. Takes the region and its list; returns
 * nothing.
 */
static void
draw_filter_line(const Region * reg, const TuiList * l)
{
  const char * text    = ui_filter(reg->role);
  int          editing = (g_editing && g_focus == reg->role);
  int          avail   = reg->w;
  int          mw      = (int) strlen(FILTER_CAP_MARK);

  if (reg->role == ROLE_MAIN && l->capped && avail > mw + 1) {
    draw_text(reg->x + reg->w - mw, reg->y, mw, STYLE_DIM, FILTER_CAP_MARK);
    avail -= mw + 1;
  }

  if (text[0] == '\0' && !editing) {
    draw_text(reg->x, reg->y, avail, STYLE_DIM, "/ filter");
    return;
  }

  char line[FILTER_MAX + 2];
  snprintf(line, sizeof line, "/%s", text);
  draw_text(reg->x, reg->y, avail - editing, STYLE_SEARCH_MATCH, line);

  if (editing && avail > 1) {
    int cx = text_width(line);
    if (cx > avail - 1) {
      cx = avail - 1;
    }
    tb_set_cell(reg->x + cx, reg->y, ' ', style_fg(&g_theme, STYLE_CURSOR),
                style_bg(&g_theme, STYLE_CURSOR));
  }
} /* draw_filter_line */

/*
 * draw_body: every allocated region by role, then the separators between
 * them (design II.5, II.6). Takes the model, returns nothing.
 */
static void
draw_body(const TuiModel * m)
{
  for (int i = 0; i < g_layout.nregions; i++) {
    const Region * r = &g_layout.region[i];

    switch (r->role) {
      case ROLE_COVER:
      case ROLE_COUNT:
        break;   /* never allocated in v2.0.0 (design II.5) */

      case ROLE_SOURCES: {
        /* One fixed row until v2.1.0 (design V.2). */
        char       name[32];
        BrowserRow row;
        TuiList    src;

        snprintf(name, sizeof name, "Queue (%zu)", m->list[ROLE_QUEUE].n);
        memset(&src, 0, sizeof src);
        row.name   = name;
        row.key    = NULL;
        row.is_dir = 0;
        src.rows    = &row;
        src.n       = 1;
        src.playing = -1;
        draw_region(r, &src);
        break;
      }

      case ROLE_MAIN: {
        const TuiList * l = &m->list[ROLE_MAIN];
        draw_region(r, l);
        if (layout_list_top(g_view, ROLE_MAIN) > 0) {
          draw_filter_line(r, l);
        }
        /* DESIGN.md XI.5: an empty index is a first-run state, not an
           error state, and the thing the user needs is the path and the
           variable that changes it. */
        if (l->n == 0 && m->library_empty && m->music_dir != NULL) {
          int top = layout_list_top(g_view, ROLE_MAIN);
          draw_text(r->x, r->y + top + 2, r->w, STYLE_DIM, m->music_dir);
          draw_text(r->x, r->y + top + 3, r->w, STYLE_DIM,
                    "set KURONO_MUSIC_DIR to change");
        }
        break;
      }

      case ROLE_FACET:
        draw_region(r, &m->list[ROLE_FACET]);
        if (layout_list_top(g_view, ROLE_FACET) > 0) {
          draw_filter_line(r, &m->list[ROLE_FACET]);
        }
        break;

      case ROLE_PARENT:
      case ROLE_INFO:
      case ROLE_QUEUE:
      default:
        draw_region(r, &m->list[r->role]);
        break;
    }
  }

  /* Vertical separators, between regions only -- the outer edges are the
     terminal's own (design II.6). */
  uintattr_t bfg = style_fg(&g_theme, STYLE_BORDER);
  uintattr_t bbg = style_bg(&g_theme, STYLE_BORDER);
  for (int i = 0; i < g_layout.nregions; i++) {
    const Region * r = &g_layout.region[i];
    if (r->x + r->w < g_layout.w) {
      for (int y = r->y; y < r->y + r->h; y++) {
        tb_set_cell(r->x + r->w, y, 0x2502 /* box drawings light vertical */, bfg, bbg);
      }
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
};

/* Filter editing is a context, not a mode (design II.8). */
#define FILTER_LABEL "-- FILTER --"

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
 * Its contents are a walk of the chain the overlay covers --
 * keymap_chain(MODE_NORMAL, 0, view), view tables first and GLOBAL last
 * (decide D23) -- through key_name and action_name. There is no
 * hand-written key list anywhere, so a binding added later appears here
 * with no second edit and cannot drift from what the keymap actually does
 * (amendment 04 decision 10). Rows bound to ACT_NONE are skipped; a
 * config file could produce one as an explicit unbind.
 */
static void
draw_overlay(void)
{
  KeyCtx chain[KEYCTX_CHAIN_MAX];
  size_t nc = keymap_chain(MODE_NORMAL, 0, g_view, chain);
  size_t n  = 0;
  for (size_t c = 0; c < nc; c++) {
    size_t cn = 0;
    (void) keymap_table(chain[c], &cn);
    n += cn;
  }

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
     path does not know how many rows the chain has or how tall the box
     is, and both change with the view and the terminal size. */
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

  size_t idx   = 0;
  int    drawn = 0;
  for (size_t c = 0; c < nc && drawn < list_rows; c++) {
    size_t          cn   = 0;
    const Binding * rows = keymap_table(chain[c], &cn);

    for (size_t i = 0; i < cn && drawn < list_rows; i++, idx++) {
      if (idx < (size_t) g_overlay_scroll || rows[i].action == ACT_NONE) {
        continue;
      }
      draw_text(box_x + 2, list_y + drawn, OVERLAY_KEY_W, STYLE_POPUP,
                key_name(&rows[i]));
      draw_text(box_x + 2 + OVERLAY_KEY_W, list_y + drawn,
                box_w - 4 - OVERLAY_KEY_W, STYLE_POPUP,
                action_name(rows[i].action));
      drawn++;
    }
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
    draw_text(1, y, MODE_FIELD_W, STYLE_MODE,
              g_editing ? FILTER_LABEL : mode_label(ui_mode()));
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

  /* The right-hand field normally shows the clock/volume/shuffle/repeat
     summary. A pending message outranks it and takes the whole field.
     Handled here rather than on the row above the status bar: that row
     is body_h's last row, shared with every column's last visible entry,
     and drawing there is the overlap this fixes (v1.0.1 #13). */
  int x     = bar_x + PROGRESS_W + 2;
  int avail = g_layout.w - x;
  if (m->message != NULL) {
    draw_text(x, y, avail, STYLE_DIM, m->message);
  } else {
    draw_text(x, y, avail, STYLE_STATUSBAR, right);
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
 * filter_key: handle one key event while a Library filter is being edited
 * (KEYCTX_FILTER, design III.5). Takes the event and the action it
 * resolved to; returns 0 for the actions keymap_filter_consumes hands on
 * (the edited region's cursor), which the caller dispatches, and 1 for
 * everything else. The listing follows the buffer at the next repaint
 * (design IV.4); nothing here calls into main.c for text.
 */
static int
filter_key(const struct tb_event * ev, Action a)
{
  char * buf = filter_buf(g_focus);

  if (!keymap_filter_consumes(a)) {
    return 0;
  }
  if (buf == NULL) {
    g_editing = 0;
    return 1;
  }

  size_t len = strlen(buf);
  switch (a) {
    case ACT_SEARCH_CANCEL:
    case ACT_QUIT:
      /* ctrl-c clears and leaves like esc; consumed, because with no mode
         to pop a dispatched ACT_QUIT ends the run (decide D16). */
      buf[0]    = '\0';
      g_editing = 0;
      return 1;

    case ACT_CONFIRM:
      g_editing = 0;
      return 1;

    case ACT_REMOVE:
      /* One codepoint, not one byte: a truncated sequence would reach the
         FTS query. */
      while (len > 0) {
        len--;
        if (((unsigned char) buf[len] & 0xc0) != 0x80) {
          break;
        }
      }
      buf[len] = '\0';
      return 1;

    case ACT_NONE: {
      if (ev->ch == 0) {
        return 1;  /* an unbound non-character key: swallowed, not text */
      }
      char enc[8];
      int  n = tb_utf8_unicode_to_char(enc, ev->ch);
      if (n > 0 && len + (size_t) n < FILTER_MAX) {
        memcpy(buf + len, enc, (size_t) n);
        buf[len + (size_t) n] = '\0';
      }
      return 1;
    }

    default:
      return 1;  /* exclusive context: nothing else gets through */
  }
} /* filter_key */

/*
 * dispatch_event: route one termbox event by its type. Key events resolve
 * through the chain for the current mode and view and go to action_dispatch;
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
      KeyCtx  chain[KEYCTX_CHAIN_MAX];
      size_t  nc   = keymap_chain(mode, g_editing, g_view, chain);
      Action  a    = keymap_resolve(chain, nc, ev, &arg);

      /* Filter text is terminal state and lives in this file; main.c
         reads the buffers at repaint (design II.7). */
      if (g_editing && filter_key(ev, a)) {
        g_dirty = 1;
        break;
      }
      if (!g_editing && mode == MODE_NORMAL && a == ACT_SEARCH) {
        /* `/` from any view edits the Library's focused filter, resuming
           its text (design III.5, decide D11). Refused where nothing can
           draw. */
        if (g_view != VIEW_LIBRARY) {
          ui_view_set(VIEW_LIBRARY);
        }
        g_editing = !g_layout.too_small;
        g_dirty   = 1;
        break;
      }

      /* Named modes rather than "not normal": overlay_scroll_action moves
         the box's own scroll offset, which only the two drawn-as-a-box
         modes have. */
      int box_overlay = (mode == MODE_HELP || mode == MODE_MENU);
      if (a != ACT_NONE && !(box_overlay && overlay_scroll_action(a))) {
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

/* ui_view: see include/ui/tui.h. */
UiView
ui_view(void)
{
  return g_view;
} /* ui_view */

/* ui_view_set: see include/ui/tui.h. */
void
ui_view_set(UiView v)
{
  g_view = v;
  if (g_active) {
    tui_compute_layout();
  }
  g_dirty = 1;
} /* ui_view_set */

/* ui_focus: see include/ui/tui.h. */
RegionRole
ui_focus(void)
{
  switch (g_view) {
    case VIEW_LIBRARY:     return g_focus;
    case VIEW_DIRECTORIES: return ROLE_MAIN;
    case VIEW_PLAYLISTS:   return ROLE_QUEUE;
    default:               return ROLE_COUNT;
  }
} /* ui_focus */

/* ui_focus_set: see include/ui/tui.h. */
void
ui_focus_set(RegionRole r)
{
  if (g_view != VIEW_LIBRARY || (r != ROLE_FACET && r != ROLE_MAIN)
      || layout_find(&g_layout, r) == NULL) {
    return;
  }
  g_focus = r;
  g_dirty = 1;
} /* ui_focus_set */

/* ui_facet_on: see include/ui/tui.h. */
int
ui_facet_on(void)
{
  return g_facet_on;
} /* ui_facet_on */

/* ui_facet_toggle: see include/ui/tui.h. */
void
ui_facet_toggle(void)
{
  g_facet_on = !g_facet_on;
  if (g_active) {
    tui_compute_layout();
  }
  g_dirty = 1;
} /* ui_facet_toggle */

/* ui_filter: see include/ui/tui.h. */
const char *
ui_filter(RegionRole r)
{
  const char * buf = filter_buf(r);
  return (buf != NULL) ? buf : "";
} /* ui_filter */

/* ui_filter_editing: see include/ui/tui.h. */
int
ui_filter_editing(void)
{
  return g_editing;
} /* ui_filter_editing */

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
