/*
 * ui/theme.c -- the one compiled-in Theme and the 256-colour degrade.
 *
 * Every colour literal in the program lives in this file (CLAUDE.md: no
 * literal colours in draw code). `make check-theme` greps for 6-digit hex
 * outside it.
 */

#include "../../include/ui/theme.h"

/* DESIGN.md VIII.1: TB_OUTPUT_TRUECOLOR is not even defined at the
   default attribute width of 16, and Style's fields silently truncate
   TB_BOLD. Every target passes -DTB_OPT_ATTR_W=32; this makes a dropped
   flag a build failure rather than a colourless run. */
_Static_assert(TB_OPT_ATTR_W == 32, "kurono requires -DTB_OPT_ATTR_W=32");

/* The array is StyleSlot-indexed; a slot added to the enum without a row
   below would otherwise initialise to {0,0,0} -- terminal default on both
   channels -- and look plausible. */
_Static_assert(sizeof(((Theme *) 0)->slot) / sizeof(Style) == STYLE_COUNT,
               "Theme.slot must be exactly STYLE_COUNT entries");

/* xterm-256 6x6x6 colour cube levels, and the 24-step grey ramp
   (indices 232..255) whose value is 8 + 10*i. Used only by
   theme_degrade_to_256. */
#define CUBE_STEPS 6
#define GREY_STEPS 24

static const int g_cube_level[CUBE_STEPS] = { 0, 95, 135, 175, 215, 255 };

/*
 * kurono-dark. Designated initialisers deliberately: the slot index is
 * the contract, so a reordering of the enum can never silently shift a
 * colour onto the wrong slot.
 *
 * No slot uses exact 0x000000 -- that value means "terminal default" to
 * termbox2 rather than black (see theme.h).
 */
const Theme g_theme_default = {
  .name = "kurono-dark",
  .slot = {
    [STYLE_NORMAL]         = { 0xc8ccd4, 0x11131a, 0 },
    [STYLE_DIM]            = { 0x6b7280, 0x11131a, 0 },
    [STYLE_SELECTED]       = { 0x11131a, 0x7aa2f7, 0 },
    [STYLE_CURSOR]         = { 0x11131a, 0xe0af68, TB_BOLD },
    [STYLE_HEADER]         = { 0x9ece6a, 0x1a1d26, TB_BOLD },
    [STYLE_STATUSBAR]      = { 0xc8ccd4, 0x1a1d26, 0 },
    [STYLE_BORDER]         = { 0x2f3440, 0x11131a, 0 },
    [STYLE_COL_INACTIVE]   = { 0x565f73, 0x11131a, 0 },
    [STYLE_PROGRESS_FILL]  = { 0x7aa2f7, 0x11131a, 0 },
    [STYLE_PROGRESS_EMPTY] = { 0x2f3440, 0x11131a, 0 },
    [STYLE_SPECTRUM_LOW]   = { 0x3d59a1, 0x11131a, 0 },
    [STYLE_SPECTRUM_HIGH]  = { 0x7dcfff, 0x11131a, 0 },
    [STYLE_SPECTRUM_PEAK]  = { 0xff9e64, 0x11131a, 0 },
    [STYLE_PLAYING]        = { 0x9ece6a, 0x11131a, TB_BOLD },
    [STYLE_ERROR]          = { 0xf7768e, 0x11131a, TB_BOLD },
    /* A foreground accent, not a filled bar. This slot was
       { 0x11131a, 0xe0af68, 0 } -- dark text on the accent background,
       which is STYLE_CURSOR's colour exactly, differing only by
       TB_BOLD. Every result row therefore drew as an orange bar and the
       cursor row drew as the same orange bar, so the arrow keys moved
       an invisible cursor (reported 2026-08-21). The filled bar belongs
       to the cursor alone; a match earns its emphasis from the
       foreground. Same accent as STYLE_MODE, which is where this pairing
       is already proven legible. */
    [STYLE_SEARCH_MATCH]   = { 0xe0af68, 0x11131a, 0 },
    [STYLE_SCROLLBAR]      = { 0x565f73, 0x11131a, 0 },
    [STYLE_POPUP]          = { 0xc8ccd4, 0x232733, 0 },
    [STYLE_COVER_BORDER]   = { 0x565f73, 0x11131a, 0 },
    /* Blue and bold, the convention every file manager already trained
       the user on. Distinct from STYLE_SELECTED's blue background so a
       directory under the cursor still reads as the cursor. */
    [STYLE_DIR]            = { 0x7aa2f7, 0x11131a, TB_BOLD },
    /* The mode indicator sits inside the status bar, so it keeps that
       row's background and earns its separation from the fields beside
       it with the accent foreground and bold. */
    [STYLE_MODE]           = { 0xe0af68, 0x1a1d26, TB_BOLD },
  }
};

/*
 * Squared euclidean distance between two RGB triples. Takes six channel
 * values, returns the distance; monotonic in the real distance, which is
 * all a nearest-match search needs.
 */
static int
rgb_dist2(int r1, int g1, int b1, int r2, int g2, int b2)
{
  int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
  return dr * dr + dg * dg + db * db;
} /* rgb_dist2 */

/*
 * Maps one packed 0xRRGGBB value to the nearest xterm-256 palette index,
 * searching both the 6x6x6 cube (16..231) and the 24-step grey ramp
 * (232..255) and taking whichever is closer -- greys matter here because
 * most of this theme is desaturated. Takes the packed colour, returns the
 * index. Never returns 0, so the "index 0 means terminal default"
 * ambiguity of TB_OUTPUT_256 cannot arise from a degraded slot.
 */
static uintattr_t
rgb_to_xterm256(uintattr_t rgb)
{
  int r = (int) ((rgb >> 16) & 0xff);
  int g = (int) ((rgb >> 8) & 0xff);
  int b = (int) (rgb & 0xff);

  /* Nearest cube cell, per channel: the cube is a product of three
     identical level sets, so the nearest cell is the nearest level on
     each axis independently. */
  int ci[3] = { 0, 0, 0 };
  const int ch[3] = { r, g, b };
  for (int axis = 0; axis < 3; axis++) {
    int best = 0;
    int best_d = 256 * 256;
    for (int i = 0; i < CUBE_STEPS; i++) {
      int d = ch[axis] - g_cube_level[i];
      d = d * d;
      if (d < best_d) { best_d = d; best = i; }
    }
    ci[axis] = best;
  }
  int cube_d = rgb_dist2(r, g, b,
                         g_cube_level[ci[0]], g_cube_level[ci[1]],
                         g_cube_level[ci[2]]);

  /* Nearest grey step. The ramp is not a subset of the cube, so it is a
     separate candidate rather than a special case of it. */
  int grey_best = 0;
  int grey_d = 1 << 30;
  for (int i = 0; i < GREY_STEPS; i++) {
    int v = 8 + 10 * i;
    int d = rgb_dist2(r, g, b, v, v, v);
    if (d < grey_d) { grey_d = d; grey_best = i; }
  }

  if (grey_d < cube_d) {
    return (uintattr_t) (232 + grey_best);
  }
  return (uintattr_t) (16 + 36 * ci[0] + 6 * ci[1] + ci[2]);
} /* rgb_to_xterm256 */

/*
 * Rewrites every slot's fg and bg from packed RGB to an xterm-256 index,
 * in place, leaving the attribute bits alone. Takes the theme to degrade,
 * returns nothing.
 *
 * DESIGN.md VIII.5: ui/tui.c calls this in exactly one place -- when
 * tb_set_output_mode(TB_OUTPUT_TRUECOLOR) fails -- and never per draw
 * call. It is not idempotent: a second call would treat the small index
 * left in fg as an RGB triple and collapse the theme to near-black.
 */
void
theme_degrade_to_256(Theme * t)
{
  for (int s = 0; s < STYLE_COUNT; s++) {
    t->slot[s].fg = rgb_to_xterm256(t->slot[s].fg);
    t->slot[s].bg = rgb_to_xterm256(t->slot[s].bg);
  }
} /* theme_degrade_to_256 */
