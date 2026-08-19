/*
 * ui/keymap.c -- the one compiled-in Binding table and the table walk
 * that resolves a termbox2 key event to an Action.
 *
 * CLAUDE.md's architectural rule: no raw key switch statements in the
 * input path. There is deliberately no switch on ev->key or ev->ch
 * anywhere in this file; a config parser will one day fill the same table
 * from a file, and a switch would have to be rewritten instead.
 */

#include <stddef.h>
#include <stdio.h>

#include "../../include/ui/keymap.h"

/* README.md V.2. Volume moves in 5-percent steps; the two seek pairs are
   the whole reason Binding carries an arg. */
#define VOL_STEP_PCT     5
#define SEEK_SMALL_MS    5000
#define SEEK_LARGE_MS    30000

/*
 * The binding table, matched in order.
 *
 * key/ch/mod are matched together; see the Binding comment in keymap.h
 * for which of key and ch termbox2 populates. Space is a printable
 * character to termbox2 (ch = 0x20, key = 0), not TB_KEY_SPACE, even
 * though that constant exists -- extract_event only assigns `key` below
 * 0x20.
 *
 * Enter and Tab are ASCII control bytes (0x0d, 0x09) and therefore arrive
 * with TB_MOD_CTRL already set by termbox2. The rows below carry mod = 0
 * because keymap_resolve strips that bit for control keys -- see the
 * comment there.
 */
static const Binding g_bindings_normal[] = {
  /* navigation. The arrows own it outright as of phase 12b
     (amendment-04-input-modes.md decision 1): h/j/k/l are unbound and
     reserved, because a keymap that answers to j, k and l but opens a
     window on h fails silently on the most-practised motion in the set. */
  { TB_KEY_ARROW_UP,    0, 0, ACT_CURSOR_UP,   0 },
  { TB_KEY_ARROW_DOWN,  0, 0, ACT_CURSOR_DOWN, 0 },
  { TB_KEY_ARROW_LEFT,  0, 0, ACT_COL_LEFT,    0 },
  { TB_KEY_ARROW_RIGHT, 0, 0, ACT_COL_RIGHT,   0 },

  /* transport */
  { 0,           ' ', 0, ACT_PLAY_PAUSE, 0 },
  { TB_KEY_ENTER, 0,  0, ACT_PLAY_NOW,   0 },
  { 0, 'n',  0, ACT_NEXT,             0 },
  { 0, 'p',  0, ACT_PREV,             0 },
  { 0, '+',  0, ACT_VOL_UP,           VOL_STEP_PCT },
  { 0, '-',  0, ACT_VOL_DOWN,         VOL_STEP_PCT },

  /* Seek. shift+arrow is the whole set of shifted bindings termbox2
     v2.5.0 can express -- its header states TB_MOD_CTRL and TB_MOD_SHIFT
     are only ever set on TB_KEY_ARROW_*, so shift+p is unreachable no
     matter how this table is written. shift+up is forward and shift+down
     is backward, matching mpv.

     The four unmodified rows below them are not legacy: a terminal that
     emits bare CSI A..D instead of CSI 1;2A..D delivers an unmodified
     arrow, and seek would silently read as a cursor move. They cost four
     rows and keep seek reachable there. H and L do not collide with the
     unbound h -- termbox2 delivers them as distinct codepoints. */
  { TB_KEY_ARROW_RIGHT, 0, TB_MOD_SHIFT, ACT_SEEK_FWD,  SEEK_SMALL_MS },
  { TB_KEY_ARROW_LEFT,  0, TB_MOD_SHIFT, ACT_SEEK_BACK, SEEK_SMALL_MS },
  { TB_KEY_ARROW_UP,    0, TB_MOD_SHIFT, ACT_SEEK_FWD,  SEEK_LARGE_MS },
  { TB_KEY_ARROW_DOWN,  0, TB_MOD_SHIFT, ACT_SEEK_BACK, SEEK_LARGE_MS },
  { 0, '.',  0, ACT_SEEK_FWD,         SEEK_SMALL_MS },
  { 0, ',',  0, ACT_SEEK_BACK,        SEEK_SMALL_MS },
  { 0, 'L',  0, ACT_SEEK_FWD,         SEEK_LARGE_MS },
  { 0, 'H',  0, ACT_SEEK_BACK,        SEEK_LARGE_MS },
  { 0, 's',  0, ACT_TOGGLE_SHUFFLE,   0 },
  { 0, 'r',  0, ACT_CYCLE_REPEAT,     0 },
  { 0, 'x',  0, ACT_STOP,             0 },
  /* Shift-M rather than `m`: phase 12b takes `m` for the settings menu
     (amendment-04-input-modes.md), and a binding that has to move one
     phase later is worse than the shifted key now. */
  { 0, 'M',  0, ACT_MUTE,             0 },

  /* overlays. `h` is free because navigation moved to the arrows; `m` is
     bound here so the keymap settles in one change rather than leaving
     the key silently unbound between 12b and the settings menu. */
  { 0, 'h',  0, ACT_HELP_TOGGLE,      0 },
  { 0, 'm',  0, ACT_MENU_TOGGLE,      0 },

  /* queue. `v` opens the pane -- one of the few unshifted keys left that
     is not h/j/k/l, which amendment 04 decision 1 keeps reserved. `d`
     and `C` stay bound here as well as in the queue's own table: both
     act on the queue rather than on what is under the browser cursor,
     so they work with the pane closed and report what they did. */
  { 0, 'a',  0, ACT_ENQUEUE,          0 },
  { 0, 'd',  0, ACT_REMOVE,           0 },
  { 0, 'C',  0, ACT_CLEAR_QUEUE,      0 },
  { 0, 'o',  0, ACT_JUMP_TO_PLAYING,  0 },
  { 0, 'v',  0, ACT_QUEUE_TOGGLE,     0 },

  /* device. Shift-R because `r` is repeat; 12b's settings menu offers the
     same action to anyone who does not know the key. */
  { 0, 'R',  0, ACT_RETRY_DEVICE,     0 },

  /* library. Ctrl-R rather than a bare letter: a rescan blocks the UI
     for its duration (DESIGN.md V.5), so it should not be one keystroke
     away from `r` (repeat) and `R` (retry device), which are the two
     keys a hand reaching for it will already be near. */
  { TB_KEY_CTRL_R, 0, 0, ACT_RESCAN,  0 },

  /* search */
  { 0, '/',  0, ACT_SEARCH,           0 },

  /* views */
  { TB_KEY_TAB, 0, 0, ACT_VIEW_NEXT,  0 },
  { 0, '1',  0, ACT_VIEW_SET,         0 },
  { 0, '2',  0, ACT_VIEW_SET,         1 },
  { 0, '3',  0, ACT_VIEW_SET,         2 },

  { 0, 'q',  0, ACT_QUIT,             0 },

  /* Ctrl-C is a key here, not a signal. termbox2's init_term_attrs calls
     cfmakeraw, which clears ISIG, so the terminal never generates SIGINT
     from the keyboard -- the byte 0x03 arrives on the tty like any other
     control key. DESIGN.md VIII.2's "SIGINT -> unconditional shutdown" is
     unaffected and still covers `kill -INT`; this row is what makes the
     gesture a user actually performs reach the same shutdown_all. Without
     it Ctrl-C is silently unbound and the player looks hung. */
  { TB_KEY_CTRL_C, 0, 0, ACT_QUIT,    0 },

  /* Esc is deliberately absent here (amendment 04 decision 5). It arrives
     from stray escape sequences and from a terminal that half-parses a
     key, and an Esc that can end the process is an infuriating bug. It is
     bound in every non-normal table, where popping is all it can do. */
};

/*
 * The overlay tables. Both are lists to move a cursor through and a way
 * out, and the way out is three keys plus the toggle that opened it, so
 * the same key closes what it opened.
 */
static const Binding g_bindings_help[] = {
  { TB_KEY_ARROW_UP,   0, 0, ACT_CURSOR_UP,        0 },
  { TB_KEY_ARROW_DOWN, 0, 0, ACT_CURSOR_DOWN,      0 },
  { TB_KEY_PGUP,       0, 0, ACT_CURSOR_PAGE_UP,   0 },
  { TB_KEY_PGDN,       0, 0, ACT_CURSOR_PAGE_DOWN, 0 },
  { TB_KEY_HOME,       0, 0, ACT_CURSOR_HOME,      0 },
  { TB_KEY_END,        0, 0, ACT_CURSOR_END,       0 },
  { 0, 'h',  0, ACT_HELP_TOGGLE,   0 },
  { 0, 'q',  0, ACT_QUIT,          0 },
  { TB_KEY_ESC,    0, 0, ACT_QUIT, 0 },
  { TB_KEY_CTRL_C, 0, 0, ACT_QUIT, 0 },
};

/*
 * MODE_MENU is written now and pushed by nothing until phase 17b. The
 * arrows adjust a value there as well as move the cursor, which is why
 * per-mode tables exist at all: no ACT_MENU_UP family is needed for the
 * same key to mean a different thing in a different list.
 */
static const Binding g_bindings_menu[] = {
  { TB_KEY_ARROW_UP,    0, 0, ACT_CURSOR_UP,        0 },
  { TB_KEY_ARROW_DOWN,  0, 0, ACT_CURSOR_DOWN,      0 },
  { TB_KEY_ARROW_LEFT,  0, 0, ACT_VOL_DOWN,         VOL_STEP_PCT },
  { TB_KEY_ARROW_RIGHT, 0, 0, ACT_VOL_UP,           VOL_STEP_PCT },
  { TB_KEY_PGUP,        0, 0, ACT_CURSOR_PAGE_UP,   0 },
  { TB_KEY_PGDN,        0, 0, ACT_CURSOR_PAGE_DOWN, 0 },
  { TB_KEY_HOME,        0, 0, ACT_CURSOR_HOME,      0 },
  { TB_KEY_END,         0, 0, ACT_CURSOR_END,       0 },
  { TB_KEY_ENTER,       0, 0, ACT_CONFIRM,          0 },
  { 0, 'm',  0, ACT_MENU_TOGGLE,  0 },
  { 0, 'q',  0, ACT_QUIT,         0 },
  { TB_KEY_ESC,    0, 0, ACT_QUIT, 0 },
  { TB_KEY_CTRL_C, 0, 0, ACT_QUIT, 0 },
};

/*
 * MODE_SEARCH binds the keys below and nothing else, and the omission is
 * the design (amendment 04 decision 4): every other event resolves ACT_NONE,
 * and tui.c reads ACT_NONE with a non-zero ev->ch as "insert this
 * codepoint into the query". That is what keeps text entry inside the
 * binding table instead of bypassing it. Esc cancels rather than pops --
 * cancelling a search also has to restore column 3 (phase 16), which is
 * more than a pop -- though phase 16 found there is nothing to restore,
 * because the search draws over column 3 without disturbing it.
 * tui.c pushes this mode on ACT_SEARCH.
 */
static const Binding g_bindings_search[] = {
  { TB_KEY_ESC,        0, 0, ACT_SEARCH_CANCEL, 0 },
  { TB_KEY_ENTER,      0, 0, ACT_CONFIRM,       0 },
  { TB_KEY_BACKSPACE2, 0, 0, ACT_REMOVE,        0 },
  { TB_KEY_ARROW_UP,   0, 0, ACT_CURSOR_UP,     0 },
  { TB_KEY_ARROW_DOWN, 0, 0, ACT_CURSOR_DOWN,   0 },
  /* Ctrl-C is bound here for the same reason it is bound in every other
     table: it is the one key that must never be swallowed as text. Every
     other unbound key in this mode is literal input, and without this row
     Ctrl-C would be one of them. */
  { TB_KEY_CTRL_C,     0, 0, ACT_QUIT,          0 },
};

/*
 * MODE_QUEUE. The pane is a list to move a cursor through and edit, so
 * it takes the overlay tables' shape and adds the four edits. Two rows
 * are worth their own note:
 *
 * `d` and `C` repeat their normal-mode bindings rather than being moved
 * here, because both act on the queue and neither needs the pane open --
 * the pane changes which entry `d` means, not whether it works.
 *
 * ACT_MOVE_UP / ACT_MOVE_DOWN get their first bindings in the program
 * here. K and J rather than shift+arrow: termbox2 v2.5.0 sets
 * TB_MOD_SHIFT only on TB_KEY_ARROW_*, and those four are already seek.
 */
static const Binding g_bindings_queue[] = {
  { TB_KEY_ARROW_UP,   0, 0, ACT_CURSOR_UP,        0 },
  { TB_KEY_ARROW_DOWN, 0, 0, ACT_CURSOR_DOWN,      0 },
  { TB_KEY_PGUP,       0, 0, ACT_CURSOR_PAGE_UP,   0 },
  { TB_KEY_PGDN,       0, 0, ACT_CURSOR_PAGE_DOWN, 0 },
  { TB_KEY_HOME,       0, 0, ACT_CURSOR_HOME,      0 },
  { TB_KEY_END,        0, 0, ACT_CURSOR_END,       0 },
  { TB_KEY_ENTER,      0, 0, ACT_CONFIRM,          0 },
  { 0, 'd',  0, ACT_REMOVE,           0 },
  { 0, 'C',  0, ACT_CLEAR_QUEUE,      0 },
  { 0, 'K',  0, ACT_MOVE_UP,          0 },
  { 0, 'J',  0, ACT_MOVE_DOWN,        0 },
  { 0, 's',  0, ACT_TOGGLE_SHUFFLE,   0 },
  { 0, 'r',  0, ACT_CYCLE_REPEAT,     0 },
  { 0, ' ',  0, ACT_PLAY_PAUSE,       0 },
  { 0, 'v',  0, ACT_QUEUE_TOGGLE,     0 },
  { 0, 'q',  0, ACT_QUIT,             0 },
  { TB_KEY_ESC,    0, 0, ACT_QUIT,    0 },
  { TB_KEY_CTRL_C, 0, 0, ACT_QUIT,    0 },
};

#define TABLE_ROWS(t) (sizeof(t) / sizeof((t)[0]))

/*
 * The table-of-tables, indexed by UiMode. Sized by MODE_COUNT so a mode
 * added without a table fails the build rather than resolving every key
 * to ACT_NONE at runtime.
 */
static const struct {
  const Binding * rows;
  size_t          count;
} g_mode_tables[MODE_COUNT] = {
  [MODE_NORMAL] = { g_bindings_normal, TABLE_ROWS(g_bindings_normal) },
  [MODE_HELP]   = { g_bindings_help,   TABLE_ROWS(g_bindings_help)   },
  [MODE_MENU]   = { g_bindings_menu,   TABLE_ROWS(g_bindings_menu)   },
  [MODE_SEARCH] = { g_bindings_search, TABLE_ROWS(g_bindings_search) },
  [MODE_QUEUE]  = { g_bindings_queue,  TABLE_ROWS(g_bindings_queue)  },
};

/*
 * Action names, for diagnostics and for the config parser's name table
 * later. Sized by its designated initialisers rather than by ACT_COUNT so
 * that the assert below fails when an Action is appended without a name.
 */
static const char * const g_action_name[] = {
  [ACT_NONE]             = "none",
  [ACT_QUIT]             = "quit",
  [ACT_PLAY_PAUSE]       = "play_pause",
  [ACT_STOP]             = "stop",
  [ACT_NEXT]             = "next",
  [ACT_PREV]             = "prev",
  [ACT_SEEK_FWD]         = "seek_fwd",
  [ACT_SEEK_BACK]        = "seek_back",
  [ACT_SEEK_ABS]         = "seek_abs",
  [ACT_VOL_UP]           = "vol_up",
  [ACT_VOL_DOWN]         = "vol_down",
  [ACT_MUTE]             = "mute",
  [ACT_TOGGLE_SHUFFLE]   = "toggle_shuffle",
  [ACT_CYCLE_REPEAT]     = "cycle_repeat",
  [ACT_CURSOR_UP]        = "cursor_up",
  [ACT_CURSOR_DOWN]      = "cursor_down",
  [ACT_CURSOR_PAGE_UP]   = "cursor_page_up",
  [ACT_CURSOR_PAGE_DOWN] = "cursor_page_down",
  [ACT_CURSOR_HOME]      = "cursor_home",
  [ACT_CURSOR_END]       = "cursor_end",
  [ACT_COL_LEFT]         = "col_left",
  [ACT_COL_RIGHT]        = "col_right",
  [ACT_VIEW_NEXT]        = "view_next",
  [ACT_VIEW_PREV]        = "view_prev",
  [ACT_VIEW_SET]         = "view_set",
  [ACT_JUMP_TO_PLAYING]  = "jump_to_playing",
  [ACT_ENQUEUE]          = "enqueue",
  [ACT_PLAY_NOW]         = "play_now",
  [ACT_REMOVE]           = "remove",
  [ACT_CLEAR_QUEUE]      = "clear_queue",
  [ACT_MOVE_UP]          = "move_up",
  [ACT_MOVE_DOWN]        = "move_down",
  [ACT_SEARCH]           = "search",
  [ACT_SEARCH_NEXT]      = "search_next",
  [ACT_SEARCH_CANCEL]    = "search_cancel",
  [ACT_RESCAN]           = "rescan",
  [ACT_RETRY_DEVICE]     = "retry_device",
  [ACT_TOGGLE_COVER]     = "toggle_cover",
  [ACT_TOGGLE_VIZ]       = "toggle_viz",
  [ACT_CYCLE_VIZ_MODE]   = "cycle_viz_mode",
  [ACT_PLAYLIST_SAVE]    = "playlist_save",
  [ACT_PLAYLIST_ADD]     = "playlist_add",
  [ACT_PLAYLIST_LOAD]    = "playlist_load",
  [ACT_PLAYLIST_DELETE]  = "playlist_delete",
  [ACT_EQ_TOGGLE]        = "eq_toggle",
  [ACT_EQ_SET_BAND]      = "eq_set_band",
  [ACT_EQ_LOAD_PRESET]   = "eq_load_preset",
  [ACT_HELP_TOGGLE]      = "help_toggle",
  [ACT_MENU_TOGGLE]      = "menu_toggle",
  [ACT_CONFIRM]          = "confirm",
  [ACT_QUEUE_TOGGLE]     = "queue_toggle",
};

_Static_assert(sizeof(g_action_name) / sizeof(g_action_name[0]) == ACT_COUNT,
               "every Action needs a name in g_action_name");

/*
 * Normalises the modifier bitmask of a key event before matching.
 *
 * termbox2's extract_event ORs TB_MOD_CTRL into every ASCII control byte
 * (anything below 0x20, plus 0x7f) because Ctrl-M and Enter are the same
 * byte on a terminal -- the distinction does not exist in the input
 * stream. Carrying that bit into the table would make every control-key
 * row read `TB_MOD_CTRL` for no information gain, and a config file would
 * have to spell "ctrl-enter" to bind Enter. The key value already encodes
 * which control key it was, so the bit is dropped. Takes the event,
 * returns the mask to match on.
 */
static uint8_t
normalized_mod(const struct tb_event * ev)
{
  uint8_t mod = ev->mod;
  if (ev->key != 0 && ev->key < TB_KEY_SPACE) {
    mod = (uint8_t) (mod & (uint8_t) ~TB_MOD_CTRL);
  } else if (ev->key == TB_KEY_BACKSPACE2) {
    mod = (uint8_t) (mod & (uint8_t) ~TB_MOD_CTRL);
  }
  return mod;
} /* normalized_mod */

/*
 * Resolves a termbox2 event to an Action by walking the binding table in
 * order. Takes the event and an out-parameter for the row's arg, returns
 * the Action, or ACT_NONE with *out_arg = 0 for a non-key event or an
 * unbound key. out_arg may not be NULL.
 */
Action
keymap_resolve(UiMode mode, const struct tb_event * ev, int32_t * out_arg)
{
  *out_arg = 0;

  if (ev == NULL || ev->type != TB_EVENT_KEY) {
    return ACT_NONE;
  }

  size_t          count = 0;
  const Binding * rows  = keymap_table(mode, &count);
  uint8_t         mod   = normalized_mod(ev);

  for (size_t i = 0; i < count; i++) {
    const Binding * b = &rows[i];
    if (b->key == ev->key && b->ch == ev->ch && b->mod == mod) {
      *out_arg = b->arg;
      return b->action;
    }
  }

  return ACT_NONE;
} /* keymap_resolve */

/*
 * Exposes the compiled-in table for the test harness and, later, for the
 * help overlay and the config parser's merge. Takes an out-parameter for
 * the row count (may be NULL), returns a pointer to the first row.
 */
const Binding *
keymap_table(UiMode mode, size_t * out_count)
{
  /* Unsigned for the same reason action_name compares unsigned: the enum
     may itself be unsigned and `mode < 0` would be a dead comparison. An
     out-of-range mode falls back to the normal table rather than
     returning NULL -- every caller walks the result, and a NULL here
     would turn a bad mode into a crash in draw code. */
  unsigned idx = (unsigned) mode;
  if (idx >= (unsigned) MODE_COUNT) {
    idx = (unsigned) MODE_NORMAL;
  }
  if (out_count != NULL) {
    *out_count = g_mode_tables[idx].count;
  }
  return g_mode_tables[idx].rows;
} /* keymap_table */

/*
 * Special-key names, as a table rather than a switch. The architectural
 * rule bans a key switch in the *input* path and this is the output one,
 * but a table is what a config parser will need to read names back into
 * keys, and writing it twice in two shapes is the drift this file exists
 * to avoid.
 */
static const struct {
  uint16_t     key;
  const char * name;
} g_key_name[] = {
  { TB_KEY_ARROW_UP,    "up"        },
  { TB_KEY_ARROW_DOWN,  "down"      },
  { TB_KEY_ARROW_LEFT,  "left"      },
  { TB_KEY_ARROW_RIGHT, "right"     },
  { TB_KEY_ENTER,       "enter"     },
  { TB_KEY_TAB,         "tab"       },
  { TB_KEY_ESC,         "esc"       },
  { TB_KEY_PGUP,        "pgup"      },
  { TB_KEY_PGDN,        "pgdn"      },
  { TB_KEY_HOME,        "home"      },
  { TB_KEY_END,         "end"       },
  { TB_KEY_BACKSPACE2,  "backspace" },
  { TB_KEY_CTRL_C,      "ctrl-c"    },
};

/* key_name: see include/ui/keymap.h. */
const char *
key_name(const Binding * b)
{
  /* One call at a time, printed immediately by the caller. A file-scope
     buffer is what keeps this allocation-free; the help overlay draws one
     row at a time and never holds two of these at once. */
  static char buf[24];

  if (b == NULL) {
    return "?";
  }

  const char * pre = (b->mod & TB_MOD_SHIFT) ? "shift+" : "";

  if (b->key != 0) {
    for (size_t i = 0; i < sizeof g_key_name / sizeof g_key_name[0]; i++) {
      if (g_key_name[i].key == b->key) {
        snprintf(buf, sizeof buf, "%s%s", pre, g_key_name[i].name);
        return buf;
      }
    }
    snprintf(buf, sizeof buf, "%skey#%u", pre, (unsigned) b->key);
    return buf;
  }

  if (b->ch == ' ') {
    snprintf(buf, sizeof buf, "%sspace", pre);
    return buf;
  }
  if (b->ch >= 0x21 && b->ch <= 0x7e) {
    snprintf(buf, sizeof buf, "%s%c", pre, (char) b->ch);
    return buf;
  }

  /* Nothing in v1's tables reaches here -- every row is either a special
     key or printable ASCII. A config file could bind a non-ASCII
     codepoint one day, and printing it as U+XXXX is better than drawing
     a partial UTF-8 sequence into a fixed-width column. */
  snprintf(buf, sizeof buf, "%sU+%04X", pre, (unsigned) b->ch);
  return buf;
} /* key_name */

/*
 * Takes an Action, returns its stable config-file name, or "invalid" for
 * a value outside the enum. Never returns NULL.
 */
const char *
action_name(Action a)
{
  /* Compared as unsigned: an enum with no negative enumerator may itself
     be unsigned, and `a < 0` would be a dead comparison -Wextra warns
     about. A negative value passed in wraps to a large index and is
     caught by the same bound. */
  unsigned idx = (unsigned) a;
  if (idx >= (unsigned) ACT_COUNT || g_action_name[idx] == NULL) {
    return "invalid";
  }
  return g_action_name[idx];
} /* action_name */
