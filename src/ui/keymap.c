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
static const Binding g_bindings[] = {
  /* navigation */
  { 0, 'h',  0, ACT_COL_LEFT,        0 },
  { 0, 'j',  0, ACT_CURSOR_DOWN,     0 },
  { 0, 'k',  0, ACT_CURSOR_UP,       0 },
  { 0, 'l',  0, ACT_COL_RIGHT,       0 },

  /* transport */
  { 0,           ' ', 0, ACT_PLAY_PAUSE, 0 },
  { TB_KEY_ENTER, 0,  0, ACT_PLAY_NOW,   0 },
  { 0, 'n',  0, ACT_NEXT,             0 },
  { 0, 'p',  0, ACT_PREV,             0 },
  { 0, '+',  0, ACT_VOL_UP,           VOL_STEP_PCT },
  { 0, '-',  0, ACT_VOL_DOWN,         VOL_STEP_PCT },
  { TB_KEY_ARROW_RIGHT, 0, 0, ACT_SEEK_FWD,  SEEK_SMALL_MS },
  { TB_KEY_ARROW_LEFT,  0, 0, ACT_SEEK_BACK, SEEK_SMALL_MS },
  { 0, 'L',  0, ACT_SEEK_FWD,         SEEK_LARGE_MS },
  { 0, 'H',  0, ACT_SEEK_BACK,        SEEK_LARGE_MS },
  { 0, 's',  0, ACT_TOGGLE_SHUFFLE,   0 },
  { 0, 'r',  0, ACT_CYCLE_REPEAT,     0 },
  { 0, 'x',  0, ACT_STOP,             0 },
  /* Shift-M rather than `m`: phase 12b takes `m` for the settings menu
     (amendment-04-input-modes.md), and a binding that has to move one
     phase later is worse than the shifted key now. */
  { 0, 'M',  0, ACT_MUTE,             0 },

  /* queue */
  { 0, 'a',  0, ACT_ENQUEUE,          0 },
  { 0, 'd',  0, ACT_REMOVE,           0 },
  { 0, 'C',  0, ACT_CLEAR_QUEUE,      0 },
  { 0, 'o',  0, ACT_JUMP_TO_PLAYING,  0 },

  /* device. Shift-R because `r` is repeat; 12b's settings menu offers the
     same action to anyone who does not know the key. */
  { 0, 'R',  0, ACT_RETRY_DEVICE,     0 },

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
};

#define BINDING_COUNT (sizeof(g_bindings) / sizeof(g_bindings[0]))

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
keymap_resolve(const struct tb_event * ev, int32_t * out_arg)
{
  *out_arg = 0;

  if (ev == NULL || ev->type != TB_EVENT_KEY) {
    return ACT_NONE;
  }

  uint8_t mod = normalized_mod(ev);

  for (size_t i = 0; i < BINDING_COUNT; i++) {
    const Binding * b = &g_bindings[i];
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
keymap_table(size_t * out_count)
{
  if (out_count != NULL) {
    *out_count = BINDING_COUNT;
  }
  return g_bindings;
} /* keymap_table */

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
