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
#include "../include/ui/theme.h"

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
 * test_every_row_resolves: every row of the compiled-in table must
 * resolve to its own action and arg from a faithfully synthesised event.
 * Takes nothing, returns nothing.
 */
static void
test_every_row_resolves(void)
{
  for (int m = 0; m < MODE_COUNT; m++) {
    size_t n = 0;
    const Binding * tab = keymap_table((UiMode) m, &n);
    CHECK(tab != NULL && n > 0, "mode %d has an empty table", m);

    for (size_t i = 0; i < n; i++) {
      struct tb_event ev = synth_event(tab[i].key, tab[i].ch, tab[i].mod);
      int32_t arg = -12345;
      Action a = keymap_resolve((UiMode) m, &ev, &arg);
      CHECK(a == tab[i].action,
            "mode %d row %zu (key=%u ch=%u mod=%u) resolved to %s, expected %s",
            m, i, (unsigned) tab[i].key, (unsigned) tab[i].ch,
            (unsigned) tab[i].mod, action_name(a), action_name(tab[i].action));
      CHECK(arg == tab[i].arg, "mode %d row %zu resolved arg %d, expected %d",
            m, i, (int) arg, (int) tab[i].arg);
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
  for (int m = 0; m < MODE_COUNT; m++) {
    size_t n = 0;
    const Binding * tab = keymap_table((UiMode) m, &n);

    for (size_t i = 0; i < n; i++) {
      for (size_t j = i + 1; j < n; j++) {
        CHECK(!(tab[i].key == tab[j].key && tab[i].ch == tab[j].ch &&
                tab[i].mod == tab[j].mod),
              "mode %d rows %zu and %zu share (key=%u ch=%u mod=%u): "
              "%s is unreachable",
              m, i, j, (unsigned) tab[i].key, (unsigned) tab[i].ch,
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
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE, "'Z' should be unbound");
  CHECK(arg == 0, "unbound key must zero *out_arg, got %d", (int) arg);

  ev = synth_event(TB_KEY_F1, 0, 0);
  arg = -1;
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE, "F1 should be unbound");
  CHECK(arg == 0, "unbound key must zero *out_arg");

  /* A modifier must not be ignored: alt-j and ctrl-j are not j. */
  ev = synth_event(0, 'j', TB_MOD_ALT);
  arg = -1;
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE,
        "alt-j must not collide with the j binding");
  ev = synth_event(0, 'j', TB_MOD_CTRL);
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE,
        "ctrl-modified j must not collide with the j binding");

  /* Alt-Enter: Enter's own TB_MOD_CTRL is normalised away, but the ALT
     bit must survive and leave the row unmatched. */
  ev = synth_event(TB_KEY_ENTER, 0, TB_MOD_ALT);
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE,
        "alt-enter must not collide with the enter binding");

  ev = synth_event(0, 'q', 0);
  ev.type = TB_EVENT_RESIZE;
  arg = -1;
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE, "a resize event binds nothing");
  CHECK(arg == 0, "non-key event must zero *out_arg");

  arg = -1;
  CHECK(keymap_resolve(MODE_NORMAL, NULL, &arg) == ACT_NONE, "NULL event resolves to none");
  CHECK(arg == 0, "NULL event must zero *out_arg");
} /* test_unbound_and_non_key */

/*
 * test_readme_bindings: spot-checks the rows README.md V.2 promises, by
 * key rather than by table index, including the two seek pairs and the
 * three view-set args that are the reason Binding carries an arg at all.
 * Takes nothing, returns nothing.
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
    { 0, 'd', ACT_REMOVE,          0 },
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
    { 0, 'C', ACT_CLEAR_QUEUE,     0 },
    { 0, 'o', ACT_JUMP_TO_PLAYING, 0 },
    { 0, 'R', ACT_RETRY_DEVICE,    0 },
    { TB_KEY_TAB, 0, ACT_VIEW_NEXT, 0 },
    { 0, '1', ACT_VIEW_SET,        0 },
    { 0, '2', ACT_VIEW_SET,        1 },
    { 0, '3', ACT_VIEW_SET,        2 },
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
    Action a = keymap_resolve(MODE_NORMAL, &ev, &arg);
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
 * and MODE_SEARCH's "unresolved means literal text" contract. Takes
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
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_SEEK_FWD,
        "shift+right must seek forward");
  CHECK(arg == 5000, "shift+right seeks %d ms, expected 5000", (int) arg);

  ev  = synth_event(TB_KEY_ARROW_RIGHT, 0, 0);
  arg = -1;
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_COL_RIGHT,
        "a bare right arrow must move a column, not seek");

  ev  = synth_event(TB_KEY_ARROW_UP, 0, TB_MOD_SHIFT);
  arg = -1;
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_SEEK_FWD &&
            arg == 30000,
        "shift+up must be the large forward seek");

  /* The vim keys are unbound and reserved, and h is help in both the
     mode that opens it and the mode it opens. */
  const uint32_t gone[] = { 'j', 'k', 'l' };
  for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
    ev  = synth_event(0, gone[i], 0);
    arg = -1;
    CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE,
          "'%c' must be unbound in normal mode", (char) gone[i]);
  }

  ev = synth_event(0, 'h', 0);
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_HELP_TOGGLE,
        "h opens help");
  CHECK(keymap_resolve(MODE_HELP, &ev, &arg) == ACT_HELP_TOGGLE,
        "h closes the help it opened");

  /* Esc: unbound in normal mode on purpose (a stray escape sequence must
     not end the process), a pop everywhere else. */
  ev = synth_event(TB_KEY_ESC, 0, 0);
  CHECK(keymap_resolve(MODE_NORMAL, &ev, &arg) == ACT_NONE,
        "Esc must be unbound in normal mode");
  CHECK(keymap_resolve(MODE_HELP, &ev, &arg) == ACT_QUIT,
        "Esc must quit (pop) in help mode");
  CHECK(keymap_resolve(MODE_SEARCH, &ev, &arg) == ACT_SEARCH_CANCEL,
        "Esc cancels a search rather than popping -- column 3 has to come back");

  /* MODE_SEARCH's contract: anything not in its own table resolves
     ACT_NONE, which tui.c reads as "insert this codepoint". */
  const uint32_t typed[] = { 'j', 'q', 'h', ' ', '/' };
  for (size_t i = 0; i < sizeof typed / sizeof typed[0]; i++) {
    ev  = synth_event(0, typed[i], 0);
    arg = -1;
    CHECK(keymap_resolve(MODE_SEARCH, &ev, &arg) == ACT_NONE,
          "'%c' in search mode is literal text, not an action",
          (char) typed[i]);
  }

  /* Every overlay table must offer a way out, or the mode is a trap. */
  const UiMode closable[] = { MODE_HELP, MODE_MENU };
  for (size_t i = 0; i < sizeof closable / sizeof closable[0]; i++) {
    ev = synth_event(0, 'q', 0);
    CHECK(keymap_resolve(closable[i], &ev, &arg) == ACT_QUIT,
          "mode %d has no q binding", (int) closable[i]);
    ev = synth_event(TB_KEY_CTRL_C, 0, 0);
    CHECK(keymap_resolve(closable[i], &ev, &arg) == ACT_QUIT,
          "mode %d has no ctrl-c binding", (int) closable[i]);
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
  const Binding * tab = keymap_table(MODE_NORMAL, &n);

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
  for (int m = 0; m < MODE_COUNT; m++) {
    size_t n = 0;
    const Binding * tab = keymap_table((UiMode) m, &n);
    for (size_t i = 0; i < n; i++) {
      for (size_t r = 0; r < sizeof reserved / sizeof reserved[0]; r++) {
        CHECK(tab[i].action != reserved[r],
              "mode %d row %zu binds reserved post-v1 action %s",
              m, i, action_name(tab[i].action));
      }
    }
  }
} /* test_reserved_actions_present */

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
  test_theme_slots();
  test_theme_degrade();

  size_t n = 0;
  (void) keymap_table(MODE_NORMAL, &n);
  if (g_failures == 0) {
    printf("keymap_test: ok (%zu bindings, %d actions, %d style slots)\n",
           n, (int) ACT_COUNT, (int) STYLE_COUNT);
    return 0;
  }
  fprintf(stderr, "keymap_test: %d failure(s)\n", g_failures);
  return 1;
} /* main */
