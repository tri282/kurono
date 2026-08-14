#ifndef KURONO_UI_THEME_H
#define KURONO_UI_THEME_H

#include "../../vendor/termbox2.h"   /* uintattr_t, TB_* attribute bits */

/*
 * Semantic style slots (DESIGN.md VIII.5).
 *
 * CLAUDE.md's architectural rule: no literal colours in draw code. Draw
 * code names a StyleSlot and reads it through style_fg/style_bg; it never
 * constructs a Style inline and never writes an RGB value of its own.
 *
 * The three fields are uintattr_t, NOT uint16_t: at TB_OPT_ATTR_W == 32
 * TB_BOLD is 0x01000000 and a 16-bit field silently drops it.
 *
 * fg/bg hold a packed 24-bit RGB value (0xRRGGBB), not a termbox colour
 * index -- in TB_OUTPUT_TRUECOLOR termbox2 takes it directly. attr holds
 * the TB_BOLD/TB_UNDERLINE/... bits separately so that
 * theme_degrade_to_256 can rewrite the colour without touching them;
 * style_fg ORs the two back together because termbox2 packs colour and
 * attributes into one uintattr_t argument.
 *
 * A pure 0x000000 fg or bg means "terminal default" to termbox2, not
 * black (send_attr: cfg == 0 emits no SGR colour unless TB_HI_BLACK is
 * set). Slots that want real black must OR TB_HI_BLACK into the colour;
 * the shipped theme avoids exact black instead.
 */
typedef struct { uintattr_t fg; uintattr_t bg; uintattr_t attr; } Style;

/*
 * Append-only. A slot is never renumbered and never deleted: the enum
 * value is the index a config-file theme parser maps its key name to
 * ("spectrum_peak" -> STYLE_SPECTRUM_PEAK) through one table instead of a
 * strcmp ladder against named struct members. That is the whole mechanism
 * that makes "a theme is a new file" true (DESIGN.md VIII.5).
 *
 * The spectrum, cover-border, popup, scrollbar, search-match and
 * col-inactive slots are drawn by nothing in v1. They are NOT dead code
 * and must not be deleted -- see plans/kurono/04-plan/phase-10-theme-keymap.md
 * section II. The config parser needs a stable key for each.
 */
typedef enum {
  STYLE_NORMAL = 0, STYLE_DIM, STYLE_SELECTED, STYLE_CURSOR,
  STYLE_HEADER, STYLE_STATUSBAR, STYLE_BORDER, STYLE_COL_INACTIVE,
  STYLE_PROGRESS_FILL, STYLE_PROGRESS_EMPTY,
  STYLE_SPECTRUM_LOW, STYLE_SPECTRUM_HIGH, STYLE_SPECTRUM_PEAK,
  STYLE_PLAYING, STYLE_ERROR, STYLE_SEARCH_MATCH,
  STYLE_SCROLLBAR, STYLE_POPUP, STYLE_COVER_BORDER,
  /* Appended by phase 12. The browser must distinguish a directory from
     a track, and CLAUDE.md's rule is that a colour in draw code is a bug
     even when it looks right -- so the distinction is a slot, not a
     literal. Appended rather than inserted: amendment 04 makes this enum
     append-only, because the config parser keys on the ordinal. */
  STYLE_DIR,
  STYLE_COUNT
} StyleSlot;

typedef struct {
  const char * name;               /* config-file key for this build */
  Style        slot[STYLE_COUNT];
} Theme;

/* The compiled-in theme. Const: a caller that means to degrade it to 256
   colours copies it by value first (Theme t = g_theme_default;) and
   degrades the copy. */
extern const Theme g_theme_default;

void theme_degrade_to_256(Theme * t);

/*
 * The only accessors draw code may use. style_fg ORs the attribute bits
 * into the colour because termbox2 takes both in one uintattr_t.
 * Out-of-range slots are the caller's bug; StyleSlot is an enum and every
 * call site names a member, so no runtime check is paid per cell.
 */
static inline uintattr_t
style_fg(const Theme * t, StyleSlot s)
{
  return t->slot[s].fg | t->slot[s].attr;
} /* style_fg */

static inline uintattr_t
style_bg(const Theme * t, StyleSlot s)
{
  return t->slot[s].bg;
} /* style_bg */

#endif // KURONO_UI_THEME_H
