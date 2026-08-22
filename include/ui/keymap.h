/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_UI_KEYMAP_H
#define KURONO_UI_KEYMAP_H

#include <stddef.h>
#include <stdint.h>

#include "../../vendor/termbox2.h"   /* struct tb_event, TB_KEY_*, TB_MOD_* */

/*
 * Key -> Action resolution (DESIGN.md VIII.5).
 *
 * CLAUDE.md's architectural rule: no raw key switch statements in the
 * input path. Input resolves through the Binding table to an Action, and
 * the TUI dispatches on the Action alone. That is what lets a config file
 * remap every key later without touching a single input handler.
 *
 * Append-only, in the given order, ACT_NONE = 0 first and ACT_COUNT last:
 * a config file's action-name table maps names to these values, so
 * renumbering breaks every config written against an older build.
 *
 * The post-v1 block is listed but not implemented. It is NOT dead code
 * and must not be deleted -- an enum has no runtime footprint, and
 * listing it is what proved Binding needs `arg` (ACT_EQ_SET_BAND is
 * unexpressible without one). See
 * plans/kurono/04-plan/phase-10-theme-keymap.md section II.
 */
typedef enum {
  ACT_NONE = 0, ACT_QUIT,
  /* transport */
  ACT_PLAY_PAUSE, ACT_STOP, ACT_NEXT, ACT_PREV,
  ACT_SEEK_FWD, ACT_SEEK_BACK, ACT_SEEK_ABS,
  ACT_VOL_UP, ACT_VOL_DOWN, ACT_MUTE,
  ACT_TOGGLE_SHUFFLE, ACT_CYCLE_REPEAT,
  /* navigation */
  ACT_CURSOR_UP, ACT_CURSOR_DOWN, ACT_CURSOR_PAGE_UP, ACT_CURSOR_PAGE_DOWN,
  ACT_CURSOR_HOME, ACT_CURSOR_END, ACT_COL_LEFT, ACT_COL_RIGHT,
  ACT_VIEW_NEXT, ACT_VIEW_PREV, ACT_VIEW_SET, ACT_JUMP_TO_PLAYING,
  /* queue */
  ACT_ENQUEUE, ACT_PLAY_NOW, ACT_REMOVE, ACT_CLEAR_QUEUE, ACT_MOVE_UP, ACT_MOVE_DOWN,
  /* search */
  ACT_SEARCH, ACT_SEARCH_NEXT, ACT_SEARCH_CANCEL,
  /* library */
  ACT_RESCAN, ACT_RETRY_DEVICE,
  /* post-v1 -- reserved so the enum is append-only from day one */
  ACT_TOGGLE_COVER, ACT_TOGGLE_VIZ, ACT_CYCLE_VIZ_MODE,
  ACT_PLAYLIST_SAVE, ACT_PLAYLIST_ADD, ACT_PLAYLIST_LOAD, ACT_PLAYLIST_DELETE,
  ACT_EQ_TOGGLE, ACT_EQ_SET_BAND, ACT_EQ_LOAD_PRESET,
  /* Phase 12b (amendment-04-input-modes.md decision 6). Appended after
     the reserved post-v1 block rather than grouped with the navigation
     actions they resemble: the append-only rule exists so a config
     file's action-name table never has to be renumbered, and grouping
     is worth less than that. ACT_CONFIRM is generic on purpose -- it is
     Enter in MODE_MENU and in every later overlay, and reusing
     ACT_PLAY_NOW for it would make the generated help overlay print a
     lie. */
  ACT_HELP_TOGGLE, ACT_MENU_TOGGLE, ACT_CONFIRM,
  /* Phase 17b, appended for the same reason the three above were: the
     queue pane is a mode like search, so opening it is one action rather
     than a view id passed to ACT_VIEW_SET. A view is a source plus a
     label and the queue is neither -- it is a flat editable list whose
     rows have no parent to ascend to. */
  ACT_QUEUE_TOGGLE,
  ACT_COUNT
} Action;

/*
 * UiMode: which binding table an event resolves against (amendment 04
 * decision 3). The stack that holds these lives in ui/tui.c and nothing
 * outside that file writes it; main.c reads the top through ui_mode().
 *
 * MODE_COUNT is not a mode. It sizes the table-of-tables in keymap.c and
 * the name array in tui.c.
 */
typedef enum {
  MODE_NORMAL = 0, MODE_HELP, MODE_MENU, MODE_SEARCH, MODE_QUEUE, MODE_COUNT
} UiMode;

/*
 * One row of the binding table.
 *
 * A row matches an event on all three of (key, ch, mod). termbox2 sets
 * exactly one of key/ch per key event: special keys arrive as
 * key = TB_KEY_*, ch = 0; printable characters (space included) arrive as
 * ch = codepoint, key = 0.
 *
 * `arg` is used in v1 and is not speculative: two different keys bind
 * ACT_SEEK_FWD with 5000 and 30000, volume carries its step, and
 * ACT_VIEW_SET carries the view id. A bare Action would need three extra
 * enum members and a hardcoded constant in each handler.
 */
typedef struct {
  uint16_t key;
  uint32_t ch;
  uint8_t  mod;
  Action   action;
  int32_t  arg;        /* seek ms, volume step, view id, eq band, ... */
} Binding;

Action keymap_resolve(UiMode mode, const struct tb_event * ev, int32_t * out_arg);

const Binding * keymap_table(UiMode mode, size_t * out_count);

const char * action_name(Action a);

/*
 * Renders one Binding's key as text for the help overlay -- `up` for an
 * arrow, `enter`, `ctrl-c`, `space`, a `shift+` prefix for TB_MOD_SHIFT,
 * and a printable ch as itself. Returns a pointer to static storage that
 * the next call overwrites, so the caller prints it immediately. Never
 * returns NULL.
 */
const char * key_name(const Binding * b);

/* Lives in main.c -- it needs every subsystem, and ui/ never calls back
   into the layers below it except through this one entry point. */
void action_dispatch(Action a, int32_t arg);

/*
 * search_dispatch: the second entry point from ui/ into main.c, and it
 * exists for the same reason action_dispatch does -- running a search
 * needs library/, and ui/ never reaches library/ itself.
 *
 * `query` is the raw text the user has typed so far, borrowed for the
 * duration of the call. main.c puts it through query_build_match and
 * db_query_search and keeps the results for the next repaint. NULL means
 * the search ended: drop the results and show nothing.
 *
 * It is called on every keystroke, which is what DESIGN.md V.6 step 4's
 * prepared statement is for. Never fatal: an unparseable query, an empty
 * one, and a residual SQLITE_ERROR all mean zero results.
 */
void search_dispatch(const char * query);

/*
 * search_confirm: move the browser to the search result at `index`, in
 * whichever view is showing, and report if it has no row there. Takes
 * the index into the rows the last search_dispatch produced; out of
 * range is a no-op.
 *
 * Separate from search_dispatch because it is a different operation on
 * the same result set, and because it is the point where a search stops
 * being a list and becomes a position.
 */
void search_confirm(size_t index);

#endif // KURONO_UI_KEYMAP_H
