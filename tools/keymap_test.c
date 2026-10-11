/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/keymap_test.c -- standalone test binary for src/ui/keymap.c and
   src/ui/theme.c (DESIGN.md XII.2 testable tier,
   phase-10-theme-keymap.md I.5). Pure and dependency-free: links only
   against keymap.c, theme.c and libc. vendor/termbox2.h is included for
   its declarations only (TB_IMPL is not defined anywhere here), so no
   terminal is opened and nothing links against termbox2's
   implementation.

   The events fed to keymap_resolve are synthesised the way termbox2's
   extract_event actually fills them, not the way the binding table is
   written: printable characters (space included) arrive as ch with
   key = 0, and ASCII control bytes below 0x20 arrive as key with
   TB_MOD_CTRL already set. synth_event below is the single place that
   models that, so the TB_MOD_CTRL normalisation in keymap.c is covered
   by every row-level check rather than by one special case. */

#include "../include/ui/keymap.h"
#include "../include/ui/layout.h"
#include "../include/ui/listgeom.h"
#include "../include/ui/theme.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                 \
  do {                                                   \
    if (!(cond)) {                                       \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                      \
      fprintf(stderr, "\n");                             \
      g_failures++;                                      \
    }                                                    \
  } while (0)

/*
 * synth_event: builds the tb_event termbox2 would deliver for a given
 * (key, ch, mod) binding triple. Takes the triple, returns the event by
 * value. The TB_MOD_CTRL that termbox2 ORs into every ASCII control byte
 * is added here so the table's mod = 0 rows are exercised through
 * keymap.c's normalisation rather than around it.
 */
static struct tb_event
synth_event(uint16_t key, uint32_t ch, uint8_t mod)
{
  struct tb_event ev;
  memset(&ev, 0, sizeof ev);
  ev.type = TB_EVENT_KEY;
  ev.key = key;
  ev.ch = ch;
  ev.mod = mod;
  if ((key != 0 && key < TB_KEY_SPACE) || key == TB_KEY_BACKSPACE2) {
    ev.mod = (uint8_t) (ev.mod | TB_MOD_CTRL);
  }
  return ev;
} /* synth_event */

/*
 * resolve_view: resolve `ev` through view `v`'s MODE_NORMAL chain. Takes
 * the view, the event and the arg out-param; returns the Action.
 */
static Action
resolve_view(UiView v, const struct tb_event * ev, int32_t * arg)
{
  KeyCtx chain[KEYCTX_CHAIN_MAX];
  size_t n = keymap_chain(MODE_NORMAL, 0, v, chain);
  return keymap_resolve(chain, n, ev, arg);
} /* resolve_view */

/*
 * resolve_ctx: resolve `ev` through the one-element chain {c}. Takes the
 * context, the event and the arg out-param; returns the Action.
 */
static Action
resolve_ctx(KeyCtx c, const struct tb_event * ev, int32_t * arg)
{
  return keymap_resolve(&c, 1, ev, arg);
} /* resolve_ctx */

/*
 * test_every_row_resolves: every row of the compiled-in table must
 * resolve to its own action and arg from a faithfully synthesised event.
 * Takes nothing, returns nothing.
 */
static void
test_every_row_resolves(void)
{
  for (int c = 0; c < KEYCTX_COUNT; c++) {
    size_t n = 0;
    const Binding * tab = keymap_table((KeyCtx) c, &n);
    CHECK((tab == NULL) == (n == 0), "context %d: rows %p with count %zu",
          c, (const void *) tab, n);

    for (size_t i = 0; i < n; i++) {
      struct tb_event ev = synth_event(tab[i].key, tab[i].ch, tab[i].mod);
      int32_t arg = -12345;
      Action a = resolve_ctx((KeyCtx) c, &ev, &arg);
      CHECK(a == tab[i].action,
            "context %d row %zu (key=%u ch=%u mod=%u) resolved to %s, expected %s",
            c, i, (unsigned) tab[i].key, (unsigned) tab[i].ch,
            (unsigned) tab[i].mod, action_name(a), action_name(tab[i].action));
      CHECK(arg == tab[i].arg, "context %d row %zu resolved arg %d, expected %d",
            c, i, (int) arg, (int) tab[i].arg);
    }
  }
} /* test_every_row_resolves */

/*
 * test_no_duplicate_triples: two rows with the same (key, ch, mod) mean
 * the second is unreachable -- the table walk returns the first match. A
 * config merge that appends rather than replaces would produce exactly
 * this, silently. Takes nothing, returns nothing.
 */
static void
test_no_duplicate_triples(void)
{
  for (int c = 0; c < KEYCTX_COUNT; c++) {
    size_t n = 0;
    const Binding * tab = keymap_table((KeyCtx) c, &n);

    for (size_t i = 0; i < n; i++) {
      for (size_t j = i + 1; j < n; j++) {
        CHECK(!(tab[i].key == tab[j].key && tab[i].ch == tab[j].ch &&
                tab[i].mod == tab[j].mod),
              "context %d rows %zu and %zu share (key=%u ch=%u mod=%u): "
              "%s is unreachable",
              c, i, j, (unsigned) tab[i].key, (unsigned) tab[i].ch,
              (unsigned) tab[i].mod, action_name(tab[j].action));
      }
    }
  }
} /* test_no_duplicate_triples */

/*
 * test_unbound_and_non_key: unbound keys, modified variants of bound
 * keys, non-key event types and a NULL event all resolve to ACT_NONE with
 * arg 0. Takes nothing, returns nothing.
 */
static void
test_unbound_and_non_key(void)
{
  int32_t arg = -1;

  struct tb_event ev = synth_event(0, 'Z', 0);
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE, "'Z' should be unbound");
  CHECK(arg == 0, "unbound key must zero *out_arg, got %d", (int) arg);

  ev = synth_event(TB_KEY_F1, 0, 0);
  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE, "F1 should be unbound");
  CHECK(arg == 0, "unbound key must zero *out_arg");

  /* A modifier must not be ignored: alt-j and ctrl-j are not j. */
  ev = synth_event(0, 'j', TB_MOD_ALT);
  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE,
        "alt-j must not collide with the j binding");
  ev = synth_event(0, 'j', TB_MOD_CTRL);
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE,
        "ctrl-modified j must not collide with the j binding");

  /* Alt-Enter: Enter's own TB_MOD_CTRL is normalised away, but the ALT
     bit must survive and leave the row unmatched. */
  ev = synth_event(TB_KEY_ENTER, 0, TB_MOD_ALT);
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE,
        "alt-enter must not collide with the enter binding");

  ev = synth_event(0, 'q', 0);
  ev.type = TB_EVENT_RESIZE;
  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE, "a resize event binds nothing");
  CHECK(arg == 0, "non-key event must zero *out_arg");

  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, NULL, &arg) == ACT_NONE, "NULL event resolves to none");
  CHECK(arg == 0, "NULL event must zero *out_arg");
} /* test_unbound_and_non_key */

/*
 * test_readme_bindings: spot-checks the rows README.md V.2 promises, by
 * key rather than by table index, through the Directories chain,
 * including the two seek pairs that are the reason Binding carries an arg
 * at all. The view-set rows are K10's (test_view_set_range); d and C are
 * Playlists-only and K14's (test_view_coverage). Takes nothing, returns
 * nothing.
 */
static void
test_readme_bindings(void)
{
  struct { uint16_t key; uint32_t ch; Action want; int32_t want_arg; } cases[] = {
    /* Navigation, on the arrows as of phase 12b. h/j/k/l are checked for
       being unbound in test_mode_tables below, which is the half of the
       change that can regress silently. */
    { TB_KEY_ARROW_UP,    0, ACT_CURSOR_UP,   0 },
    { TB_KEY_ARROW_DOWN,  0, ACT_CURSOR_DOWN, 0 },
    { TB_KEY_ARROW_LEFT,  0, ACT_COL_LEFT,    0 },
    { TB_KEY_ARROW_RIGHT, 0, ACT_COL_RIGHT,   0 },
    { 0, ' ', ACT_PLAY_PAUSE,      0 },
    { TB_KEY_ENTER, 0, ACT_PLAY_NOW, 0 },
    { 0, 'a', ACT_ENQUEUE,         0 },
    { 0, 'n', ACT_NEXT,            0 },
    { 0, 'p', ACT_PREV,            0 },
    { 0, '+', ACT_VOL_UP,          5 },
    { 0, '-', ACT_VOL_DOWN,        5 },
    /* The unmodified seek fallbacks. These exist for terminals that
       swallow the shift modifier on an arrow; the modified rows are
       checked in test_mode_tables. */
    { 0, '.', ACT_SEEK_FWD,        5000 },
    { 0, ',', ACT_SEEK_BACK,       5000 },
    { 0, 'L', ACT_SEEK_FWD,        30000 },
    { 0, 'H', ACT_SEEK_BACK,       30000 },
    { 0, 'h', ACT_HELP_TOGGLE,     0 },
    { 0, 'm', ACT_MENU_TOGGLE,     0 },
    { 0, '/', ACT_SEARCH,          0 },
    { 0, 's', ACT_TOGGLE_SHUFFLE,  0 },
    { 0, 'r', ACT_CYCLE_REPEAT,    0 },
    /* The phase-12 transport rows. Each of these is the only way to reach
       its action from a keyboard, and two of them (mute, retry-device)
       are the only way to drive a phase-12 verify row at all. Shifted
       where the lowercase key is already taken now or by phase 12b. */
    { 0, 'x', ACT_STOP,            0 },
    { 0, 'M', ACT_MUTE,            0 },
    { 0, 'o', ACT_JUMP_TO_PLAYING, 0 },
    { 0, 'R', ACT_RETRY_DEVICE,    0 },
    { 0, '?', ACT_HELP_TOGGLE,     0 },
    { 0, 'v', ACT_QUEUE_TOGGLE,    0 },
    { 0, 'q', ACT_QUIT,            0 },
    /* Raw mode means the terminal does not turn this into SIGINT -- see
       the row's comment in keymap.c. Checked here because an unbound
       Ctrl-C makes the player look hung, and nothing else would catch
       its removal. */
    { TB_KEY_CTRL_C, 0, ACT_QUIT,  0 },
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    struct tb_event ev = synth_event(cases[i].key, cases[i].ch, 0);
    int32_t arg = -1;
    Action a = resolve_view(VIEW_DIRECTORIES, &ev, &arg);
    CHECK(a == cases[i].want, "key=%u ch=%u gave %s, expected %s",
          (unsigned) cases[i].key, (unsigned) cases[i].ch,
          action_name(a), action_name(cases[i].want));
    CHECK(arg == cases[i].want_arg, "key=%u ch=%u gave arg %d, expected %d",
          (unsigned) cases[i].key, (unsigned) cases[i].ch,
          (int) arg, (int) cases[i].want_arg);
  }
} /* test_readme_bindings */

/*
 * test_mode_tables: the phase-12b rules that can regress without any
 * other check noticing -- the modifier actually discriminating, the
 * vim keys being gone, Esc's asymmetry between normal and the overlays,
 * and [FILTER]'s "unresolved means literal text" contract. Takes
 * nothing, returns nothing.
 */
static void
test_mode_tables(void)
{
  int32_t arg = -1;
  struct tb_event ev;

  /* The pair that proves mod discriminates rather than being ignored:
     the same key, once bare and once shifted, must reach two different
     actions. Injectable end to end as CSI 1;2C by the pty harness. */
  ev  = synth_event(TB_KEY_ARROW_RIGHT, 0, TB_MOD_SHIFT);
  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_SEEK_FWD,
        "shift+right must seek forward");
  CHECK(arg == 5000, "shift+right seeks %d ms, expected 5000", (int) arg);

  ev  = synth_event(TB_KEY_ARROW_RIGHT, 0, 0);
  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_COL_RIGHT,
        "a bare right arrow must move a column, not seek");

  ev  = synth_event(TB_KEY_ARROW_UP, 0, TB_MOD_SHIFT);
  arg = -1;
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_SEEK_FWD &&
            arg == 30000,
        "shift+up must be the large forward seek");

  /* The vim keys are unbound and reserved, and h is help in both the
     mode that opens it and the mode it opens. */
  const uint32_t gone[] = { 'j', 'k', 'l' };
  for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
    ev  = synth_event(0, gone[i], 0);
    arg = -1;
    CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE,
          "'%c' must be unbound in normal mode", (char) gone[i]);
  }

  ev = synth_event(0, 'h', 0);
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_HELP_TOGGLE,
        "h opens help");
  CHECK(resolve_ctx(KEYCTX_HELP, &ev, &arg) == ACT_HELP_TOGGLE,
        "h closes the help it opened");

  /* Esc: unbound in normal mode on purpose (a stray escape sequence must
     not end the process), a pop everywhere else. */
  ev = synth_event(TB_KEY_ESC, 0, 0);
  CHECK(resolve_view(VIEW_DIRECTORIES, &ev, &arg) == ACT_NONE,
        "Esc must be unbound in normal mode");
  CHECK(resolve_ctx(KEYCTX_HELP, &ev, &arg) == ACT_QUIT,
        "Esc must quit (pop) in help mode");
  CHECK(resolve_ctx(KEYCTX_FILTER, &ev, &arg) == ACT_SEARCH_CANCEL,
        "Esc cancels a search rather than popping -- column 3 has to come back");

  /* The text-entry contract, now [FILTER]'s (design III.2; plan gap
     G8): anything not in its own table resolves ACT_NONE, which tui.c
     reads as "insert this codepoint". */
  const uint32_t typed[] = { 'j', 'q', 'h', ' ', '/' };
  for (size_t i = 0; i < sizeof typed / sizeof typed[0]; i++) {
    ev  = synth_event(0, typed[i], 0);
    arg = -1;
    CHECK(resolve_ctx(KEYCTX_FILTER, &ev, &arg) == ACT_NONE,
          "'%c' in search mode is literal text, not an action",
          (char) typed[i]);
  }

  /* Every overlay table must offer a way out, or the mode is a trap. */
  const KeyCtx closable[] = { KEYCTX_HELP, KEYCTX_MENU };
  for (size_t i = 0; i < sizeof closable / sizeof closable[0]; i++) {
    ev = synth_event(0, 'q', 0);
    CHECK(resolve_ctx(closable[i], &ev, &arg) == ACT_QUIT,
          "context %d has no q binding", (int) closable[i]);
    ev = synth_event(TB_KEY_CTRL_C, 0, 0);
    CHECK(resolve_ctx(closable[i], &ev, &arg) == ACT_QUIT,
          "context %d has no ctrl-c binding", (int) closable[i]);
  }
} /* test_mode_tables */

/*
 * test_key_names: the help overlay is generated from the binding table
 * through key_name and action_name, so a row either of them cannot render
 * is a blank line in the only place the keymap is documented at runtime.
 * Takes nothing, returns nothing.
 */
static void
test_key_names(void)
{
  size_t n = 0;
  const Binding * tab = keymap_table(KEYCTX_GLOBAL, &n);

  for (size_t i = 0; i < n; i++) {
    const char * k = key_name(&tab[i]);
    CHECK(k != NULL && k[0] != '\0', "row %zu has no key name", i);
    const char * a = action_name(tab[i].action);
    CHECK(a != NULL && strcmp(a, "invalid") != 0,
          "row %zu binds an unnamed action", i);
  }

  CHECK(key_name(NULL) != NULL, "key_name(NULL) must not return NULL");

  Binding b = { TB_KEY_ARROW_UP, 0, 0, ACT_CURSOR_UP, 0 };
  CHECK(strcmp(key_name(&b), "up") == 0, "arrow up should render as \"up\"");
  b.mod = TB_MOD_SHIFT;
  CHECK(strcmp(key_name(&b), "shift+up") == 0,
        "a shifted arrow should carry the shift+ prefix");

  Binding sp = { 0, ' ', 0, ACT_PLAY_PAUSE, 0 };
  CHECK(strcmp(key_name(&sp), "space") == 0, "space should render as \"space\"");

  Binding ch = { 0, 'q', 0, ACT_QUIT, 0 };
  CHECK(strcmp(key_name(&ch), "q") == 0, "a printable should render as itself");
} /* test_key_names */

/*
 * test_action_names: every Action below ACT_COUNT has a distinct name,
 * and out-of-range values report "invalid" rather than reading past the
 * table. This is the check that catches an Action appended to the enum
 * without a config-file name. Takes nothing, returns nothing.
 */
static void
test_action_names(void)
{
  for (int a = 0; a < ACT_COUNT; a++) {
    const char * n = action_name((Action) a);
    CHECK(n != NULL && strcmp(n, "invalid") != 0,
          "action %d has no name", a);
    for (int b = a + 1; b < ACT_COUNT; b++) {
      CHECK(strcmp(n, action_name((Action) b)) != 0,
            "actions %d and %d share the name \"%s\"", a, b, n);
    }
  }
  CHECK(strcmp(action_name((Action) ACT_COUNT), "invalid") == 0,
        "ACT_COUNT itself must report invalid");
} /* test_action_names */

/*
 * test_reserved_actions_present: the post-v1 block is load-bearing
 * plumbing (phase-10-theme-keymap.md II) and a future reader must not
 * delete it as dead code. Naming the members here makes the deletion a
 * build failure. Takes nothing, returns nothing.
 */
static void
test_reserved_actions_present(void)
{
  Action reserved[] = {
    ACT_TOGGLE_COVER, ACT_TOGGLE_VIZ, ACT_CYCLE_VIZ_MODE,
    ACT_PLAYLIST_SAVE, ACT_PLAYLIST_ADD, ACT_PLAYLIST_LOAD,
    ACT_PLAYLIST_DELETE, ACT_EQ_TOGGLE, ACT_EQ_SET_BAND, ACT_EQ_LOAD_PRESET,
  };
  for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
    CHECK(reserved[i] > ACT_NONE && reserved[i] < ACT_COUNT,
          "reserved action %zu is out of range", i);
  }

  /* No reserved action may be bound in v1 -- binding one would dispatch
     into a case nothing implements. */
  for (int c = 0; c < KEYCTX_COUNT; c++) {
    size_t n = 0;
    const Binding * tab = keymap_table((KeyCtx) c, &n);
    for (size_t i = 0; i < n; i++) {
      for (size_t r = 0; r < sizeof reserved / sizeof reserved[0]; r++) {
        CHECK(tab[i].action != reserved[r],
              "context %d row %zu binds reserved post-v1 action %s",
              c, i, action_name(tab[i].action));
      }
    }
  }
} /* test_reserved_actions_present */

/*
 * test_key_names_are_named: every binding in every mode table must
 * render through key_name() as a name a person recognises, never as
 * key_name's fallback spelling.
 *
 * The help overlay is generated from the binding tables rather than
 * hand-written, which is what keeps it from drifting -- but it means an
 * unnamed key silently reaches the screen as "key#18" instead of
 * "ctrl-r". That is exactly what shipped: TB_KEY_CTRL_R was bound in
 * two tables and absent from g_key_name, so help advertised a keystroke
 * nobody could type (reported 2026-08-21). Nothing failed; the wrong
 * text was simply drawn.
 *
 * Both fallbacks are checked. "key#" catches a special key missing from
 * g_key_name, and "U+" catches a non-ASCII codepoint binding, which
 * nothing in v1 has but a config file could add. Takes nothing, returns
 * nothing.
 */
static void
test_key_names_are_named(void)
{
  for (int c = 0; c < KEYCTX_COUNT; c++) {
    size_t          n = 0;
    const Binding * t = keymap_table((KeyCtx) c, &n);

    if (t == NULL) {
      continue;   /* a context with no rows (KEYCTX_LIBRARY until phase 5) */
    }

    for (size_t i = 0; i < n; i++) {
      const char * name = key_name(&t[i]);

      CHECK(name != NULL && name[0] != '\0',
            "context %d row %zu (action %s) has an empty key name",
            c, i, action_name(t[i].action));
      if (name == NULL) {
        continue;
      }
      CHECK(strstr(name, "key#") == NULL,
            "context %d row %zu (action %s) renders as \"%s\" -- the key is "
            "missing from g_key_name and help will show it that way",
            c, i, action_name(t[i].action), name);
      CHECK(strstr(name, "U+") == NULL,
            "context %d row %zu (action %s) renders as \"%s\" -- a bare "
            "codepoint is not a key name a user can act on",
            c, i, action_name(t[i].action), name);
    }
  }
} /* test_key_names_are_named */

/*
 * test_theme_slots: every StyleSlot is populated (a slot left at
 * {0,0,0} means "terminal default on both channels", which looks
 * plausible on screen and is impossible to spot in a screenshot), the
 * accessors OR the attribute bits into fg and never into bg, and the
 * reserved post-v1 slots exist. Takes nothing, returns nothing.
 */
static void
test_theme_slots(void)
{
  const Theme * t = &g_theme_default;
  CHECK(t->name != NULL && t->name[0] != '\0', "theme has no name");

  for (int s = 0; s < STYLE_COUNT; s++) {
    CHECK(t->slot[s].fg != 0, "slot %d has no foreground colour", s);
    CHECK(t->slot[s].fg <= 0xffffff, "slot %d fg is not a 24-bit RGB value", s);
    CHECK(t->slot[s].bg <= 0xffffff, "slot %d bg is not a 24-bit RGB value", s);
    /* A slot that wants real black must set TB_HI_BLACK; plain zero is
       "terminal default" (theme.h). The shipped theme uses neither, so a
       bare zero here is an unpopulated slot, not a deliberate black. */
    CHECK(t->slot[s].bg != 0,
          "slot %d bg is bare 0x000000 (terminal default, not black)", s);
  }

  CHECK(style_fg(t, STYLE_CURSOR) ==
          (g_theme_default.slot[STYLE_CURSOR].fg | TB_BOLD),
        "style_fg must OR the attribute bits into the colour");
  CHECK(style_bg(t, STYLE_CURSOR) == g_theme_default.slot[STYLE_CURSOR].bg,
        "style_bg must not carry attribute bits");

  /* The cursor row must be distinguishable from every base style a
     non-cursor row in the same column can carry -- and distinguishable
     by colour, not by attribute. STYLE_SEARCH_MATCH was
     { 0x11131a, 0xe0af68, 0 } against STYLE_CURSOR's
     { 0x11131a, 0xe0af68, TB_BOLD }: the same two colours, differing
     only in bold, which is invisible on dark text over a light bar. The
     result was a search column where the arrow keys moved a cursor
     nobody could see (2026-08-21). Attributes are deliberately excluded
     from the comparison here -- that is the whole point. */
  StyleSlot base_slots[] = {
    STYLE_NORMAL, STYLE_DIM, STYLE_DIR, STYLE_PLAYING,
    STYLE_SEARCH_MATCH, STYLE_COL_INACTIVE, STYLE_SELECTED,
  };
  for (size_t i = 0; i < sizeof base_slots / sizeof base_slots[0]; i++) {
    StyleSlot b = base_slots[i];
    CHECK(!(t->slot[b].fg == t->slot[STYLE_CURSOR].fg &&
            t->slot[b].bg == t->slot[STYLE_CURSOR].bg),
          "slot %d is STYLE_CURSOR's exact colours; the cursor would be "
          "invisible on a row drawn with it", (int) b);
  }

  /* Reserved slots: named here so deleting one fails the build. */
  StyleSlot reserved[] = {
    STYLE_SPECTRUM_LOW, STYLE_SPECTRUM_HIGH, STYLE_SPECTRUM_PEAK,
    STYLE_COVER_BORDER, STYLE_POPUP, STYLE_SCROLLBAR,
    STYLE_SEARCH_MATCH, STYLE_COL_INACTIVE,
  };
  for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
    CHECK(reserved[i] < STYLE_COUNT, "reserved slot %zu is out of range", i);
  }
} /* test_theme_slots */

/*
 * test_theme_degrade: after theme_degrade_to_256 every colour is a legal
 * xterm-256 index in the cube or grey ramp (never 0, which TB_OUTPUT_256
 * reads as the terminal default), and the attribute bits are untouched.
 * Takes nothing, returns nothing.
 */
static void
test_theme_degrade(void)
{
  Theme t = g_theme_default;
  theme_degrade_to_256(&t);

  for (int s = 0; s < STYLE_COUNT; s++) {
    CHECK(t.slot[s].fg >= 16 && t.slot[s].fg <= 255,
          "slot %d degraded fg %u is not an xterm-256 index",
          s, (unsigned) t.slot[s].fg);
    CHECK(t.slot[s].bg >= 16 && t.slot[s].bg <= 255,
          "slot %d degraded bg %u is not an xterm-256 index",
          s, (unsigned) t.slot[s].bg);
    CHECK(t.slot[s].attr == g_theme_default.slot[s].attr,
          "slot %d lost its attribute bits in the degrade", s);
  }

  CHECK(t.name == g_theme_default.name, "degrade must not change the name");

  /* Distinguishable colours must stay distinguishable: the background
     and the selection bar collapsing to one index would make the cursor
     invisible in 256-colour mode. */
  CHECK(t.slot[STYLE_NORMAL].bg != t.slot[STYLE_SELECTED].bg,
        "normal and selected backgrounds collapsed to the same index");
  CHECK(t.slot[STYLE_NORMAL].fg != t.slot[STYLE_NORMAL].bg,
        "normal fg and bg collapsed to the same index");
} /* test_theme_degrade */

/*
 * test_list_scroll_for: invariant, centring, and end behaviour of
 * list_scroll_for (include/ui/listgeom.h) over n in [0,40], viewport
 * in [1,20], cursor in [0,n). Takes nothing, returns nothing.
 */
static void
test_list_scroll_for(void)
{
  for (size_t n = 0; n <= 40; n++) {
    for (int vp = 1; vp <= 20; vp++) {
      size_t h = (size_t) vp;
      for (size_t cursor = 0; cursor < (n == 0 ? 1 : n); cursor++) {
        if (n == 0) {
          continue;
        }
        size_t scroll = list_scroll_for(cursor, n, vp);

        CHECK(scroll <= cursor, "n=%zu vp=%d cursor=%zu: scroll %zu > cursor",
              n, vp, cursor, scroll);
        CHECK(cursor < scroll + h, "n=%zu vp=%d cursor=%zu: cursor not in viewport",
              n, vp, cursor);
        if (n <= h) {
          CHECK(scroll == 0, "n=%zu vp=%d cursor=%zu: scroll != 0 when n <= viewport",
                n, vp, cursor);
        } else {
          CHECK(scroll + h <= n, "n=%zu vp=%d cursor=%zu: scroll + viewport > n",
                n, vp, cursor);
        }

        if (n > h && cursor >= h / 2 && cursor <= n - 1 - h / 2) {
          CHECK(cursor - scroll == h / 2,
                "n=%zu vp=%d cursor=%zu: not centred (scroll=%zu)", n, vp, cursor, scroll);
        }
        if (n > h && cursor < h / 2) {
          CHECK(scroll == 0, "n=%zu vp=%d cursor=%zu: left end not scroll=0", n, vp, cursor);
        }
        if (n > h && cursor > n - 1 - h / 2) {
          CHECK(scroll == n - h, "n=%zu vp=%d cursor=%zu: right end not scroll=n-h", n, vp, cursor);
        }
      }
    }
  }
} /* test_list_scroll_for */

/*
 * test_list_cursor_move: range, wrap-at-|delta|==1, and clamp-otherwise
 * behaviour of list_cursor_move (include/ui/listgeom.h), for n in [0,8]
 * and a representative set of deltas including both int extremes.
 * Takes nothing, returns nothing.
 */
static void
test_list_cursor_move(void)
{
  static const int deltas[] = {
    INT_MIN, -1000, -13, -2, -1, 0, 1, 2, 13, 1000, INT_MAX
  };
  size_t n_deltas = sizeof(deltas) / sizeof(deltas[0]);

  for (size_t n = 0; n <= 8; n++) {
    for (size_t cursor = 0; cursor < (n == 0 ? 1 : n); cursor++) {
      for (size_t di = 0; di < n_deltas; di++) {
        int delta = deltas[di];
        size_t r = list_cursor_move((n == 0) ? 0 : cursor, n, delta);

        if (n == 0) {
          CHECK(r == 0, "n=0: result %zu != 0", r);
          continue;
        }
        CHECK(r < n, "n=%zu cursor=%zu delta=%d: result %zu out of range",
              n, cursor, delta, r);

        if (n == 1) {
          CHECK(r == 0, "n=1 delta=%d: result %zu != 0", delta, r);
        }

        if (delta == 1 && cursor == n - 1) {
          CHECK(r == 0, "n=%zu cursor=%zu delta=+1: did not wrap to 0", n, cursor);
        }
        if (delta == -1 && cursor == 0) {
          CHECK(r == n - 1, "n=%zu cursor=0 delta=-1: did not wrap to n-1", n);
        }

        if (delta != 1 && delta != -1) {
          long long want = (long long) cursor + (long long) delta;
          long long last = (long long) n - 1;
          if (want < 0) want = 0;
          if (want > last) want = last;
          CHECK((long long) r == want,
                "n=%zu cursor=%zu delta=%d: got %zu, want %lld",
                n, cursor, delta, r, want);
        }
      }
    }
  }

  /* The page-up case design.md pins: a large negative delta on a short
     listing clamps to 0 rather than wrapping to the far end. */
  CHECK(list_cursor_move(1, 4, -12) == 0, "n=4 cursor=1 delta=-12 must be 0");
  CHECK(list_cursor_move(0, 5, -12) == 0, "n=5 cursor=0 delta=-12 must be 0 (not 3)");
} /* test_list_cursor_move */

/*
 * layout_expect: the test's own variant selection, written independently
 * of layout_compute. Takes the view, size and facet flag, returns the
 * index of the variant that must be chosen and fills widths[].
 */
static size_t
layout_expect(UiView v, int w, int facet_on, int * widths)
{
  size_t                n   = 0;
  const LayoutVariant * var = layout_variants(v, &n);

  for (size_t vi = 0; vi < n; vi++) {
    const LayoutVariant * lv = &var[vi];
    int ok = 1;
    int sum = 0;
    int avail = w - ((int) lv->ncols - 1);

    for (size_t i = 0; i < lv->ncols; i++) {
      if (lv->col[i].role == ROLE_FACET && !facet_on) {
        ok = 0;
      }
      widths[i] = (i + 1 < lv->ncols) ? avail * lv->col[i].pct / 100 : avail - sum;
      sum += widths[i];
      if (vi + 1 < n && widths[i] < lv->col[i].min_w) {
        ok = 0;
      }
    }
    if (ok) {
      return vi;
    }
  }
  return n;
} /* layout_expect */

/*
 * test_layout_tiling: K1-K4. Every region and separator cell of the body
 * is covered exactly once, regions stay inside the body, roles are unique
 * and match the independently selected variant, minimums hold.
 */
static void
test_layout_tiling(void)
{
  static const int hs[] = { 15, 24, 40, 100 };
  static unsigned char cov[300 * 98];

  for (int v = 0; v < (int) VIEW_COUNT; v++) {
    for (int facet = 0; facet <= 1; facet++) {
      for (int w = TUI_MIN_W; w <= 300; w++) {
        for (size_t hi = 0; hi < sizeof hs / sizeof hs[0]; hi++) {
          int h = hs[hi];
          TuiLayout l;
          layout_compute((UiView) v, w, h, facet, &l);
          CHECK(!l.too_small, "v=%d %dx%d: too_small", v, w, h);
          CHECK(l.body_y == 1 && l.body_h == h - 2 && l.status_y == h - 1,
                "v=%d %dx%d: rows %d/%d/%d", v, w, h, l.body_y, l.body_h, l.status_y);

          memset(cov, 0, sizeof cov);
          int seen[ROLE_COUNT] = { 0 };
          for (int i = 0; i < l.nregions; i++) {
            const Region * r = &l.region[i];
            CHECK(r->w >= 1 && r->h >= 1 && r->x >= 0 && r->x + r->w <= l.w,
                  "v=%d %dx%d region %d: x=%d w=%d h=%d", v, w, h, i, r->x, r->w, r->h);
            CHECK(r->y == l.body_y && r->y + r->h <= l.body_y + l.body_h,
                  "v=%d %dx%d region %d: y=%d h=%d outside body", v, w, h, i, r->y, r->h);
            CHECK(r->role != ROLE_COVER && !seen[r->role],
                  "v=%d %dx%d: role %d cover or twice", v, w, h, (int) r->role);
            seen[r->role] = 1;
            for (int y = 0; y < r->h; y++) {
              for (int x = r->x; x < r->x + r->w && x < w; x++) {
                cov[y * 300 + x]++;
              }
              if (r->x + r->w < w) {
                cov[y * 300 + r->x + r->w]++;
              }
            }
          }
          int bad = 0;
          for (int y = 0; y < l.body_h; y++) {
            for (int x = 0; x < w; x++) {
              bad += (cov[y * 300 + x] != 1);
            }
          }
          CHECK(bad == 0, "v=%d facet=%d %dx%d: %d cells not covered exactly once",
                v, facet, w, h, bad);

          int    ew[LAYOUT_MAX_COLS];
          size_t vi = layout_expect((UiView) v, w, facet, ew);
          size_t nv = 0;
          const LayoutVariant * var = layout_variants((UiView) v, &nv);
          CHECK(vi < nv, "v=%d %dx%d: no variant selected", v, w, h);
          if (vi < nv) {
            CHECK((size_t) l.nregions == var[vi].ncols,
                  "v=%d facet=%d %dx%d: %d regions, variant %zu has %zu",
                  v, facet, w, h, l.nregions, vi, var[vi].ncols);
            for (int i = 0; i < l.nregions && (size_t) i < var[vi].ncols; i++) {
              CHECK(l.region[i].role == var[vi].col[i].role && l.region[i].w == ew[i],
                    "v=%d facet=%d %dx%d col %d: role/width mismatch", v, facet, w, h, i);
              if (vi + 1 < nv) {
                CHECK(l.region[i].w >= var[vi].col[i].min_w,
                      "v=%d facet=%d %dx%d col %d: w=%d below min", v, facet, w, h, i,
                      l.region[i].w);
              }
            }
          }
        }
      }
    }
  }
} /* test_layout_tiling */

/*
 * layout_roles_are: 1 when *l allocated exactly the `n` roles in `want`,
 * in order. Takes the layout, the count and the roles.
 */
static int
layout_roles_are(const TuiLayout * l, int n, const RegionRole * want)
{
  if (l->nregions != n) {
    return 0;
  }
  for (int i = 0; i < n; i++) {
    if (l->region[i].role != want[i]) {
      return 0;
    }
  }
  return 1;
} /* layout_roles_are */

/*
 * test_layout_declared: K5. Variant 1 is chosen at 200x60, and the
 * narrow-terminal fallbacks at 60x15.
 */
static void
test_layout_declared(void)
{
  static const RegionRole lib3[]  = { ROLE_FACET, ROLE_MAIN, ROLE_INFO };
  static const RegionRole lib2[]  = { ROLE_MAIN, ROLE_INFO };
  static const RegionRole dir3[]  = { ROLE_PARENT, ROLE_MAIN, ROLE_INFO };
  static const RegionRole pl3[]   = { ROLE_SOURCES, ROLE_QUEUE, ROLE_INFO };
  static const RegionRole info1[] = { ROLE_INFO };
  static const RegionRole main1[] = { ROLE_MAIN };
  TuiLayout l;

  layout_compute(VIEW_LIBRARY, 200, 60, 1, &l);
  CHECK(layout_roles_are(&l, 3, lib3), "Library 200x60 facet on");
  layout_compute(VIEW_DIRECTORIES, 200, 60, 1, &l);
  CHECK(layout_roles_are(&l, 3, dir3), "Directories 200x60");
  layout_compute(VIEW_PLAYLISTS, 200, 60, 1, &l);
  CHECK(layout_roles_are(&l, 3, pl3), "Playlists 200x60");
  layout_compute(VIEW_NOW_PLAYING, 200, 60, 1, &l);
  CHECK(layout_roles_are(&l, 1, info1), "Now playing 200x60");
  layout_compute(VIEW_SETTINGS, 200, 60, 1, &l);
  CHECK(layout_roles_are(&l, 1, main1), "Settings 200x60");
  layout_compute(VIEW_LIBRARY, 200, 60, 0, &l);
  CHECK(layout_roles_are(&l, 2, lib2), "Library 200x60 facet off");
  layout_compute(VIEW_LIBRARY, 60, 15, 1, &l);
  CHECK(layout_roles_are(&l, 2, lib2), "Library 60x15: facet 11 < 12 must drop");
  layout_compute(VIEW_PLAYLISTS, 60, 15, 1, &l);
  CHECK(layout_roles_are(&l, 3, pl3), "Playlists 60x15");
  CHECK(l.region[0].w == 11 && l.region[1].w == 29 && l.region[2].w == 18,
        "Playlists 60x15 widths %d/%d/%d, want 11/29/18",
        l.region[0].w, l.region[1].w, l.region[2].w);
} /* test_layout_declared */

/*
 * test_layout_floor: K6. Below 60x15 every view is too small and holds no
 * regions; 60x15 is large enough for every view.
 */
static void
test_layout_floor(void)
{
  TuiLayout l;

  for (int v = 0; v < (int) VIEW_COUNT; v++) {
    layout_compute((UiView) v, 59, 40, 1, &l);
    CHECK(l.too_small == 1 && l.nregions == 0, "v=%d 59x40 not too small", v);
    layout_compute((UiView) v, 200, 14, 1, &l);
    CHECK(l.too_small == 1 && l.nregions == 0, "v=%d 200x14 not too small", v);
    layout_compute((UiView) v, 60, 15, 1, &l);
    CHECK(l.too_small == 0 && l.nregions > 0, "v=%d 60x15 too small", v);
  }
} /* test_layout_floor */

/*
 * test_layout_directories_golden: K7. Directories reproduces v1.3.1's
 * width formula at every width, and the design's spot rows.
 */
static void
test_layout_directories_golden(void)
{
  TuiLayout l;

  for (int w = TUI_MIN_W; w <= 300; w++) {
    int avail = w - 2;
    int w0 = avail * 25 / 100;
    int w1 = avail * 30 / 100;
    int w2 = avail - w0 - w1;
    int x1 = w0 + 1;
    int x2 = x1 + w1 + 1;

    layout_compute(VIEW_DIRECTORIES, w, 24, 1, &l);
    CHECK(l.nregions == 3 && l.region[0].role == ROLE_PARENT
            && l.region[1].role == ROLE_MAIN && l.region[2].role == ROLE_INFO,
          "w=%d: Directories roles", w);
    CHECK(l.region[0].x == 0 && l.region[0].w == w0 && l.region[1].x == x1
            && l.region[1].w == w1 && l.region[2].x == x2 && l.region[2].w == w2,
          "w=%d: Directories geometry differs from v1.3.1", w);
  }

  static const int spot[][4] = { { 60, 14, 17, 27 }, { 80, 19, 23, 36 }, { 120, 29, 35, 54 } };
  for (size_t i = 0; i < sizeof spot / sizeof spot[0]; i++) {
    layout_compute(VIEW_DIRECTORIES, spot[i][0], 24, 1, &l);
    CHECK(l.region[0].w == spot[i][1] && l.region[1].w == spot[i][2]
            && l.region[2].w == spot[i][3],
          "w=%d: widths %d/%d/%d, want %d/%d/%d", spot[i][0], l.region[0].w,
          l.region[1].w, l.region[2].w, spot[i][1], spot[i][2], spot[i][3]);
  }
} /* test_layout_directories_golden */

/*
 * test_layout_total: K8. layout_compute is defined for every size and
 * every view value, in or out of range.
 */
static void
test_layout_total(void)
{
  TuiLayout a, b;

  for (int v = -1; v <= (int) VIEW_COUNT; v++) {
    for (int w = 1; w <= 300; w++) {
      for (int h = 1; h <= 100; h++) {
        layout_compute((UiView) v, w, h, 1, &a);
        int small = (w < TUI_MIN_W || h < TUI_MIN_H);
        if (a.nregions < 0 || a.nregions > (int) ROLE_COUNT || a.too_small != small
            || (small && a.nregions != 0) || (!small && a.nregions == 0)) {
          CHECK(0, "v=%d %dx%d: nregions=%d too_small=%d", v, w, h, a.nregions, a.too_small);
        }
      }
    }
  }

  layout_compute((UiView) VIEW_COUNT, 100, 30, 1, &a);
  layout_compute(VIEW_LIBRARY, 100, 30, 1, &b);
  CHECK(memcmp(&a, &b, sizeof a) == 0, "VIEW_COUNT must lay out as Library");
  layout_compute((UiView) -1, 100, 30, 1, &a);
  CHECK(memcmp(&a, &b, sizeof a) == 0, "view -1 must lay out as Library");
} /* test_layout_total */

/*
 * test_layout_list_top: K9. Only the Library facet and main lists
 * reserve a top row.
 */
static void
test_layout_list_top(void)
{
  for (int v = 0; v < (int) VIEW_COUNT; v++) {
    for (int r = 0; r < (int) ROLE_COUNT; r++) {
      int want = (v == VIEW_LIBRARY && (r == ROLE_FACET || r == ROLE_MAIN));
      CHECK(layout_list_top((UiView) v, (RegionRole) r) == want,
            "layout_list_top(%d, %d) != %d", v, r, want);
    }
  }
} /* test_layout_list_top */

/*
 * test_view_set_range: K10. Every ACT_VIEW_SET row in every context has
 * an arg in [0, VIEW_COUNT), and '1'..'5' resolve through GLOBAL to
 * view_set 0..4 exactly (design II.1: UiView = key - 1).
 */
static void
test_view_set_range(void)
{
  for (int c = 0; c < KEYCTX_COUNT; c++) {
    size_t          n   = 0;
    const Binding * tab = keymap_table((KeyCtx) c, &n);
    for (size_t i = 0; i < n; i++) {
      if (tab[i].action == ACT_VIEW_SET) {
        CHECK(tab[i].arg >= 0 && tab[i].arg < VIEW_COUNT,
              "context %d row %zu: view_set arg %d outside [0, %d)",
              c, i, (int) tab[i].arg, (int) VIEW_COUNT);
      }
    }
  }

  for (int v = 0; v < VIEW_COUNT; v++) {
    struct tb_event ev  = synth_event(0, (uint32_t) ('1' + v), 0);
    int32_t         arg = -1;
    CHECK(resolve_ctx(KEYCTX_GLOBAL, &ev, &arg) == ACT_VIEW_SET && arg == v,
          "'%c' must be view_set %d, got arg %d", '1' + v, v, (int) arg);
  }

  struct tb_event ev  = synth_event(TB_KEY_TAB, 0, 0);
  int32_t         arg = -1;
  CHECK(resolve_ctx(KEYCTX_GLOBAL, &ev, &arg) == ACT_VIEW_NEXT, "tab must be view_next");
} /* test_view_set_range */

/*
 * expect_chain: the design III.2 table, written out independently of
 * keymap_chain. Takes the mode, the editing flag, the view and the out
 * array; returns the length.
 */
static size_t
expect_chain(UiMode mode, int editing, int view, KeyCtx out[KEYCTX_CHAIN_MAX])
{
  if (mode == MODE_HELP) {
    out[0] = KEYCTX_HELP;
    return 1;
  }
  if (mode == MODE_MENU) {
    out[0] = KEYCTX_MENU;
    return 1;
  }
  if (editing || mode == MODE_SEARCH) {
    out[0] = KEYCTX_FILTER;
    return 1;
  }
  if (view == VIEW_LIBRARY) {
    out[0] = KEYCTX_LIBRARY; out[1] = KEYCTX_BROWSE; out[2] = KEYCTX_LIST; out[3] = KEYCTX_GLOBAL;
    return 4;
  }
  if (view == VIEW_DIRECTORIES) {
    out[0] = KEYCTX_BROWSE; out[1] = KEYCTX_LIST; out[2] = KEYCTX_GLOBAL;
    return 3;
  }
  if (view == VIEW_PLAYLISTS) {
    out[0] = KEYCTX_TRACKLIST; out[1] = KEYCTX_LIST; out[2] = KEYCTX_GLOBAL;
    return 3;
  }
  out[0] = KEYCTX_GLOBAL;
  return 1;
} /* expect_chain */

/*
 * test_chains: K11. keymap_chain matches design III.2 for every mode x
 * editing x view.
 */
static void
test_chains(void)
{
  const UiMode modes[] = { MODE_NORMAL, MODE_HELP, MODE_MENU, MODE_SEARCH };

  for (size_t mi = 0; mi < sizeof modes / sizeof modes[0]; mi++) {
    for (int ed = 0; ed <= 1; ed++) {
      for (int v = 0; v < VIEW_COUNT; v++) {
        KeyCtx got[KEYCTX_CHAIN_MAX], want[KEYCTX_CHAIN_MAX];
        size_t gn = keymap_chain(modes[mi], ed, (UiView) v, got);
        size_t wn = expect_chain(modes[mi], ed, v, want);
        CHECK(gn == wn && memcmp(got, want, wn * sizeof want[0]) == 0,
              "chain for mode %d editing %d view %d: length %zu, expected %zu",
              (int) modes[mi], ed, v, gn, wn);
      }
    }
  }
} /* test_chains */

/*
 * test_no_shadowing: K12. Within each view's MODE_NORMAL chain no
 * (key, ch, mod) triple is bound in two tables (design III.3 rule 3).
 */
static void
test_no_shadowing(void)
{
  for (int v = 0; v < VIEW_COUNT; v++) {
    KeyCtx chain[KEYCTX_CHAIN_MAX];
    size_t nc = keymap_chain(MODE_NORMAL, 0, (UiView) v, chain);

    for (size_t a = 0; a < nc; a++) {
      size_t          na = 0;
      const Binding * ta = keymap_table(chain[a], &na);
      for (size_t b = a + 1; b < nc; b++) {
        size_t          nb = 0;
        const Binding * tb = keymap_table(chain[b], &nb);
        for (size_t i = 0; i < na; i++) {
          for (size_t j = 0; j < nb; j++) {
            CHECK(!(ta[i].key == tb[j].key && ta[i].ch == tb[j].ch && ta[i].mod == tb[j].mod),
                  "view %d: contexts %d and %d both bind %s (key=%u ch=%u mod=%u)",
                  v, (int) chain[a], (int) chain[b], action_name(tb[j].action),
                  (unsigned) tb[j].key, (unsigned) tb[j].ch, (unsigned) tb[j].mod);
          }
        }
      }
    }
  }
} /* test_no_shadowing */

/*
 * test_global_reach: K13. Every action design III.3 rule 1 makes global
 * has a GLOBAL row, and every GLOBAL row resolves to the same action and
 * arg through every view's MODE_NORMAL chain.
 */
static void
test_global_reach(void)
{
  const Action global[] = {
    ACT_PLAY_PAUSE, ACT_NEXT, ACT_PREV, ACT_VOL_UP, ACT_VOL_DOWN, ACT_SEEK_FWD,
    ACT_SEEK_BACK, ACT_TOGGLE_SHUFFLE, ACT_CYCLE_REPEAT, ACT_STOP, ACT_MUTE,
    ACT_HELP_TOGGLE, ACT_MENU_TOGGLE, ACT_RETRY_DEVICE, ACT_RESCAN,
    ACT_JUMP_TO_PLAYING, ACT_SEARCH, ACT_QUEUE_TOGGLE, ACT_VIEW_NEXT,
    ACT_VIEW_SET, ACT_QUIT,
  };
  size_t          n   = 0;
  const Binding * tab = keymap_table(KEYCTX_GLOBAL, &n);

  for (size_t g = 0; g < sizeof global / sizeof global[0]; g++) {
    int found = 0;
    for (size_t i = 0; i < n; i++) {
      found |= (tab[i].action == global[g]);
    }
    CHECK(found, "%s has no GLOBAL row", action_name(global[g]));
  }

  for (int v = 0; v < VIEW_COUNT; v++) {
    for (size_t i = 0; i < n; i++) {
      struct tb_event ev  = synth_event(tab[i].key, tab[i].ch, tab[i].mod);
      int32_t         arg = -12345;
      Action          a   = resolve_view((UiView) v, &ev, &arg);
      CHECK(a == tab[i].action && arg == tab[i].arg,
            "view %d: GLOBAL row %zu (%s) resolves to %s arg %d",
            v, i, action_name(tab[i].action), action_name(a), (int) arg);
    }
  }
} /* test_global_reach */

/*
 * test_view_coverage: K14. Each view resolves the keys its regions need
 * (design III.6), and Now playing and Settings resolve no cursor key.
 */
static void
test_view_coverage(void)
{
  struct { int view; uint16_t key; uint32_t ch; Action want; } cases[] = {
    { VIEW_LIBRARY,     0,                  '/', ACT_SEARCH    },
    { VIEW_LIBRARY,     0,                  'a', ACT_ENQUEUE   },
    { VIEW_LIBRARY,     TB_KEY_ENTER,       0,   ACT_PLAY_NOW  },
    { VIEW_LIBRARY,     TB_KEY_ARROW_LEFT,  0,   ACT_COL_LEFT  },
    { VIEW_LIBRARY,     TB_KEY_ARROW_RIGHT, 0,   ACT_COL_RIGHT },
    { VIEW_DIRECTORIES, 0,                  '/', ACT_SEARCH    },
    { VIEW_DIRECTORIES, 0,                  'a', ACT_ENQUEUE   },
    { VIEW_DIRECTORIES, TB_KEY_ENTER,       0,   ACT_PLAY_NOW  },
    { VIEW_DIRECTORIES, TB_KEY_ARROW_LEFT,  0,   ACT_COL_LEFT  },
    { VIEW_DIRECTORIES, TB_KEY_ARROW_RIGHT, 0,   ACT_COL_RIGHT },
    /* d and C are Playlists-only (decide D7). */
    { VIEW_DIRECTORIES, 0,                  'd', ACT_NONE      },
    { VIEW_DIRECTORIES, 0,                  'C', ACT_NONE      },
    { VIEW_PLAYLISTS,   0,                  'd', ACT_REMOVE    },
    { VIEW_PLAYLISTS,   0,                  'C', ACT_CLEAR_QUEUE },
    { VIEW_PLAYLISTS,   0,                  'K', ACT_MOVE_UP   },
    { VIEW_PLAYLISTS,   0,                  'J', ACT_MOVE_DOWN },
    { VIEW_PLAYLISTS,   TB_KEY_ENTER,       0,   ACT_CONFIRM   },
    { VIEW_PLAYLISTS,   TB_KEY_ARROW_UP,    0,   ACT_CURSOR_UP },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    struct tb_event ev  = synth_event(cases[i].key, cases[i].ch, 0);
    int32_t         arg = -1;
    Action          a   = resolve_view((UiView) cases[i].view, &ev, &arg);
    CHECK(a == cases[i].want, "view %d key=%u ch=%u gave %s, expected %s",
          cases[i].view, (unsigned) cases[i].key, (unsigned) cases[i].ch,
          action_name(a), action_name(cases[i].want));
  }

  const UiView    bare[]   = { VIEW_NOW_PLAYING, VIEW_SETTINGS };
  const uint16_t  cursor[] = { TB_KEY_ARROW_UP, TB_KEY_ARROW_DOWN, TB_KEY_PGUP,
                               TB_KEY_PGDN, TB_KEY_HOME, TB_KEY_END };
  for (size_t v = 0; v < sizeof bare / sizeof bare[0]; v++) {
    int32_t arg = -1;
    for (size_t k = 0; k < sizeof cursor / sizeof cursor[0]; k++) {
      struct tb_event ev = synth_event(cursor[k], 0, 0);
      CHECK(resolve_view(bare[v], &ev, &arg) == ACT_NONE,
            "view %d resolves cursor key %u", (int) bare[v], (unsigned) cursor[k]);
    }
    struct tb_event ev = synth_event(0, ' ', 0);
    CHECK(resolve_view(bare[v], &ev, &arg) == ACT_PLAY_PAUSE,
          "view %d must resolve space", (int) bare[v]);
    ev = synth_event(0, 'q', 0);
    CHECK(resolve_view(bare[v], &ev, &arg) == ACT_QUIT,
          "view %d must resolve q", (int) bare[v]);
  }
} /* test_view_coverage */

/*
 * main: runs every check, returns 0 when all passed and 1 otherwise.
 */
int
main(void)
{
  test_every_row_resolves();
  test_no_duplicate_triples();
  test_unbound_and_non_key();
  test_readme_bindings();
  test_mode_tables();
  test_key_names();
  test_action_names();
  test_reserved_actions_present();
  test_key_names_are_named();
  test_theme_slots();
  test_theme_degrade();
  test_list_scroll_for();
  test_list_cursor_move();
  test_layout_tiling();
  test_layout_declared();
  test_layout_floor();
  test_layout_directories_golden();
  test_layout_total();
  test_layout_list_top();
  test_view_set_range();
  test_chains();
  test_no_shadowing();
  test_global_reach();
  test_view_coverage();

  size_t n = 0;
  (void) keymap_table(KEYCTX_GLOBAL, &n);
  if (g_failures == 0) {
    printf("keymap_test: ok (%zu bindings, %d actions, %d style slots)\n",
           n, (int) ACT_COUNT, (int) STYLE_COUNT);
    return 0;
  }
  fprintf(stderr, "keymap_test: %d failure(s)\n", g_failures);
  return 1;
} /* main */
