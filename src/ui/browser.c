#include "../../include/ui/browser.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BROWSER_MIN_VIEWPORT 1

/* ---------------------------------------------------------------------
 * The M1 filesystem source.
 *
 * Everything between here and the widget section is what phase 16
 * replaces with a library/query.c source. Nothing below this section
 * knows that opendir exists.
 * ------------------------------------------------------------------- */

/*
 * FsSource: the filesystem source's own state. The ignore list is
 * borrowed from Config, which outlives every widget, so it is a pointer
 * and not a copy.
 */
typedef struct {
  char * const * ignore;
  size_t         nignore;
} FsSource;

/* One instance is enough: the source is stateless apart from the
   borrowed ignore list, and a second browser would want the same one. */
static FsSource g_fs_source;

/*
 * ext_is_playable: does this filename end in an extension the decoder
 * will attempt? Deliberately a allow-list rather than "anything ffmpeg
 * might open" -- a music tree full of .log, .cue and .jpg should list
 * as tracks only what is a track. Takes the filename, returns 1 or 0.
 */
static int
ext_is_playable(const char * name)
{
  static const char * const ext[] = {
    ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus",
    ".wav", ".wv", ".ape", ".alac", ".aiff", ".aif", ".mp4", ".wma"
  };

  const char * dot = strrchr(name, '.');
  if (dot == NULL || dot == name) {
    return 0; /* no extension, or a dotfile that got this far */
  }

  for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) {
    /* Case-insensitive without <ctype.h>'s locale surprises: the set is
       ASCII by construction, so a manual fold is both correct and
       shorter than tolower on every byte. */
    const char * a = dot;
    const char * b = ext[i];
    while (*a != '\0' && *b != '\0') {
      char ca = *a;
      char cb = *b;
      if (ca >= 'A' && ca <= 'Z') {
        ca = (char) (ca - 'A' + 'a');
      }
      if (ca != cb) {
        break;
      }
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') {
      return 1;
    }
  }
  return 0;
} /* ext_is_playable */

/*
 * fs_is_ignored: does this directory basename appear in the ignore
 * list? DESIGN.md's KURONO_IGNORE_DIRS is a list of basenames, not
 * paths, so this is an exact string match and applies at every depth.
 * Takes the source and the basename, returns 1 or 0.
 */
static int
fs_is_ignored(const FsSource * s, const char * name)
{
  for (size_t i = 0; i < s->nignore; i++) {
    if (strcmp(s->ignore[i], name) == 0) {
      return 1;
    }
  }
  return 0;
} /* fs_is_ignored */

/*
 * row_cmp: directories before files, then byte order within each group.
 * Byte order rather than strcoll because the locale is not set and a
 * deterministic listing is worth more here than a linguistically
 * correct one -- for UTF-8 this is codepoint order, which is stable.
 * qsort(3) comparator over BrowserRow.
 */
static int
row_cmp(const void * a, const void * b)
{
  const BrowserRow * x = (const BrowserRow *) a;
  const BrowserRow * y = (const BrowserRow *) b;

  if (x->is_dir != y->is_dir) {
    return y->is_dir - x->is_dir; /* is_dir 1 sorts before is_dir 0 */
  }
  return strcmp(x->name, y->name);
} /* row_cmp */

/*
 * rows_free: release a row array and every string in it. Shared by the
 * source's free_rows and by its own failure paths, which is why it
 * tolerates NULL members -- a partially built array is freed through
 * here after an allocation failure partway.
 * Takes the array and its length, returns nothing.
 */
static void
rows_free(BrowserRow * rows, size_t n)
{
  if (rows == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    free(rows[i].name);
    free(rows[i].key);
  }
  free(rows);
} /* rows_free */

/*
 * path_join: "parent/child" into a fresh allocation, collapsing the
 * double slash when parent already ends in one (the root "/" case).
 * Takes both halves, returns the joined path or NULL on allocation
 * failure or if the result would exceed PATH_MAX.
 */
static char *
path_join(const char * parent, const char * child)
{
  size_t pl = strlen(parent);
  size_t cl = strlen(child);
  int    need_sep = (pl > 0 && parent[pl - 1] != '/');

  size_t total = pl + (size_t) (need_sep ? 1 : 0) + cl + 1;
  if (total > PATH_MAX) {
    return NULL;
  }

  char * out = malloc(total);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, parent, pl);
  if (need_sep) {
    out[pl] = '/';
  }
  memcpy(out + pl + (need_sep ? 1 : 0), child, cl);
  out[total - 1] = '\0';
  return out;
} /* path_join */

/*
 * fs_load: AudioSource-style load for the filesystem. Lists parent_key,
 * keeping directories that are not ignored and files whose extension is
 * playable, skipping every dotfile. The column index is ignored -- M1
 * has one listing and every column would show the same thing -- but it
 * stays in the signature because phase 16's source runs a different
 * query per column.
 *
 * A directory that cannot be opened is not fatal: it yields zero rows
 * and a -1 the caller reports, which is CLAUDE.md's per-file rule
 * applied to a directory. Returns 0 with the out-params set, or -1 having
 * allocated nothing.
 */
static int
fs_load(void * self, int column, const char * parent_key,
        BrowserRow ** out, size_t * n)
{
  (void) column;

  FsSource * s = (FsSource *) self;
  if (parent_key == NULL || out == NULL || n == NULL) {
    return -1;
  }

  DIR * d = opendir(parent_key);
  if (d == NULL) {
    return -1; /* unreadable or gone -- reported by the caller, not fatal */
  }

  BrowserRow * rows = NULL;
  size_t       count = 0, cap = 0;

  struct dirent * de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') {
      continue; /* "." and ".." and every hidden entry */
    }

    char * key = path_join(parent_key, de->d_name);
    if (key == NULL) {
      continue; /* over PATH_MAX or out of memory: skip the entry, not the dir */
    }

    /* d_type is not portable across filesystems -- it is DT_UNKNOWN on
       some -- so stat decides. lstat rather than stat would exclude
       symlinked album directories, which a music tree legitimately has. */
    struct stat st;
    if (stat(key, &st) != 0) {
      free(key);
      continue;
    }

    int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    if (is_dir) {
      if (fs_is_ignored(s, de->d_name)) {
        free(key);
        continue;
      }
    } else if (!S_ISREG(st.st_mode) || !ext_is_playable(de->d_name)) {
      free(key);
      continue;
    }

    if (count == cap) {
      size_t ncap = (cap == 0) ? 16 : cap * 2;
      BrowserRow * grown = realloc(rows, ncap * sizeof *grown);
      if (grown == NULL) {
        free(key);
        rows_free(rows, count);
        closedir(d);
        return -1;
      }
      rows = grown;
      cap  = ncap;
    }

    char * name = strdup(de->d_name);
    if (name == NULL) {
      free(key);
      rows_free(rows, count);
      closedir(d);
      return -1;
    }

    rows[count].name   = name;
    rows[count].key    = key;
    rows[count].is_dir = is_dir;
    count++;
  }
  closedir(d);

  if (count > 1) {
    qsort(rows, count, sizeof rows[0], row_cmp);
  }

  *out = rows;
  *n   = count;
  return 0;
} /* fs_load */

/*
 * fs_free_rows: AudioSource-style free for the filesystem source.
 * Takes the array and its length, returns nothing.
 */
static void
fs_free_rows(void * self, BrowserRow * rows, size_t n)
{
  (void) self;
  rows_free(rows, n);
} /* fs_free_rows */

/*
 * browser_fs_source: see include/ui/browser.h.
 */
void
browser_fs_source(BrowserSource * out, char * const * ignore, size_t nignore)
{
  g_fs_source.ignore  = ignore;
  g_fs_source.nignore = nignore;

  out->load      = fs_load;
  out->free_rows = fs_free_rows;
  out->self      = &g_fs_source;
} /* browser_fs_source */

/* ---------------------------------------------------------------------
 * The widget.
 *
 * Nothing below here mentions a file, a directory, or a path. It moves
 * a cursor over rows a BrowserSource produced.
 * ------------------------------------------------------------------- */

/*
 * Browser: one column's worth of state. M1 draws a single column
 * (DESIGN.md XIV.1); the source signature already carries a column
 * index so phase 16 can add the other two without touching this.
 *
 * The invariant the whole struct exists to maintain, and the one worth
 * checking by hand rather than trusting a clean compile:
 *
 *   n == 0  =>  cursor == 0 and scroll == 0
 *   n  > 0  =>  cursor < n
 *               scroll <= cursor < scroll + viewport
 *               scroll == 0 whenever n <= viewport
 *               scroll + viewport <= n whenever n > viewport
 *
 * clamp() below is the only function that establishes it, and every
 * mutator ends by calling it.
 */
struct Browser {
  BrowserSource src;

  char * root;      /* the key the browser will not ascend above */
  char * cwd;       /* the key currently listed */

  BrowserRow * rows;
  size_t       n;
  size_t       cursor;
  size_t       scroll;
  int          viewport;
};

/*
 * clamp: re-establish the cursor/scroll invariant after any change to
 * the cursor, the row count, or the viewport. Idempotent, and the only
 * writer of `scroll`. Takes the Browser, returns nothing.
 */
static void
clamp(Browser * b)
{
  int vp = (b->viewport < BROWSER_MIN_VIEWPORT) ? BROWSER_MIN_VIEWPORT
                                                : b->viewport;

  if (b->n == 0) {
    b->cursor = 0;
    b->scroll = 0;
    return;
  }

  if (b->cursor >= b->n) {
    b->cursor = b->n - 1;
  }

  size_t v = (size_t) vp;

  /* A listing that fits never scrolls. Without this the branch below
     could leave a non-zero scroll after the row count shrank, showing
     blank lines under a short listing. */
  if (b->n <= v) {
    b->scroll = 0;
    return;
  }

  if (b->cursor < b->scroll) {
    b->scroll = b->cursor;               /* scrolled off the top */
  } else if (b->cursor >= b->scroll + v) {
    b->scroll = b->cursor - v + 1;       /* scrolled off the bottom */
  }

  /* Never leave blank rows below the last entry. */
  if (b->scroll + v > b->n) {
    b->scroll = b->n - v;
  }
} /* clamp */

/*
 * load_into: replace the browser's rows with the listing for `key`.
 * On failure the browser is left exactly as it was -- the new rows are
 * built before the old ones are released, so a failed load can never
 * empty a column that was fine a moment ago.
 * Takes the Browser and the key, returns 0 or -1.
 */
static int
load_into(Browser * b, const char * key)
{
  BrowserRow * rows = NULL;
  size_t       n    = 0;

  if (b->src.load(b->src.self, 0, key, &rows, &n) != 0) {
    return -1;
  }

  b->src.free_rows(b->src.self, b->rows, b->n);
  b->rows = rows;
  b->n    = n;
  return 0;
} /* load_into */

/*
 * browser_create: see include/ui/browser.h.
 */
Browser *
browser_create(const BrowserSource * src, const char * root)
{
  if (src == NULL || root == NULL) {
    return NULL;
  }

  Browser * b = calloc(1, sizeof *b);
  if (b == NULL) {
    return NULL;
  }

  b->src      = *src;
  b->viewport = BROWSER_MIN_VIEWPORT;
  b->root     = strdup(root);
  b->cwd      = strdup(root);
  if (b->root == NULL || b->cwd == NULL) {
    free(b->root);
    free(b->cwd);
    free(b);
    return NULL;
  }

  if (load_into(b, b->cwd) != 0) {
    free(b->root);
    free(b->cwd);
    free(b);
    return NULL;
  }

  clamp(b);
  return b;
} /* browser_create */

/*
 * browser_destroy: see include/ui/browser.h.
 */
void
browser_destroy(Browser * b)
{
  if (b == NULL) {
    return;
  }
  b->src.free_rows(b->src.self, b->rows, b->n);
  free(b->root);
  free(b->cwd);
  free(b);
} /* browser_destroy */

/*
 * browser_reload: see include/ui/browser.h. The cursor is kept by
 * index rather than by name: a rescan that inserted a row above the
 * cursor moves the selection, which is worth less than the code to
 * re-find it by key and is what every file manager does.
 */
int
browser_reload(Browser * b)
{
  if (b == NULL) {
    return -1;
  }
  if (load_into(b, b->cwd) != 0) {
    return -1;
  }
  clamp(b);
  return 0;
} /* browser_reload */

/*
 * browser_cursor_move: see include/ui/browser.h. Clamps at both ends.
 * The arithmetic is done signed and in one place so that a large
 * negative delta on a short listing cannot wrap through zero -- the
 * failure that would look like "page up jumps to the bottom".
 */
void
browser_cursor_move(Browser * b, int delta)
{
  if (b == NULL || b->n == 0 || delta == 0) {
    return;
  }

  long long cur  = (long long) b->cursor;
  long long want = cur + (long long) delta;

  if (want < 0) {
    want = 0;
  }
  if (want > (long long) b->n - 1) {
    want = (long long) b->n - 1;
  }

  b->cursor = (size_t) want;
  clamp(b);
} /* browser_cursor_move */

/*
 * browser_cursor_home / browser_cursor_end: see include/ui/browser.h.
 */
void
browser_cursor_home(Browser * b)
{
  if (b == NULL || b->n == 0) {
    return;
  }
  b->cursor = 0;
  clamp(b);
} /* browser_cursor_home */

void
browser_cursor_end(Browser * b)
{
  if (b == NULL || b->n == 0) {
    return;
  }
  b->cursor = b->n - 1;
  clamp(b);
} /* browser_cursor_end */

/*
 * browser_descend: see include/ui/browser.h.
 */
int
browser_descend(Browser * b)
{
  if (b == NULL || b->n == 0) {
    return 0;
  }

  const BrowserRow * sel = &b->rows[b->cursor];
  if (!sel->is_dir) {
    return 0;
  }

  /* The key is about to be freed by load_into, so copy it first. This
     is the use-after-free that a straight `load_into(b, sel->key)`
     would be. */
  char * target = strdup(sel->key);
  if (target == NULL) {
    return -1;
  }

  if (load_into(b, target) != 0) {
    free(target);
    return -1;
  }

  free(b->cwd);
  b->cwd    = target;
  b->cursor = 0;
  b->scroll = 0;
  clamp(b);
  return 1;
} /* browser_descend */

/*
 * browser_ascend: see include/ui/browser.h. The cursor lands on the
 * directory just left, so left-then-right returns the user where they
 * were rather than to the top of the parent.
 */
int
browser_ascend(Browser * b)
{
  if (b == NULL) {
    return 0;
  }
  if (strcmp(b->cwd, b->root) == 0) {
    return 0; /* the root is the ceiling */
  }

  char * slash = strrchr(b->cwd, '/');
  if (slash == NULL || slash == b->cwd) {
    return 0; /* already at "/" or a key with no separator */
  }

  size_t plen = (size_t) (slash - b->cwd);
  char * parent = malloc(plen + 1);
  if (parent == NULL) {
    return -1;
  }
  memcpy(parent, b->cwd, plen);
  parent[plen] = '\0';

  /* Remember what we are leaving before the rows it lives in go away. */
  char * leaving = strdup(slash + 1);
  if (leaving == NULL) {
    free(parent);
    return -1;
  }

  if (load_into(b, parent) != 0) {
    free(parent);
    free(leaving);
    return -1;
  }

  free(b->cwd);
  b->cwd    = parent;
  b->cursor = 0;
  b->scroll = 0;

  for (size_t i = 0; i < b->n; i++) {
    if (strcmp(b->rows[i].name, leaving) == 0) {
      b->cursor = i;
      break;
    }
  }
  free(leaving);

  clamp(b);
  return 1;
} /* browser_ascend */

/*
 * key_under_root: 1 when `key` is the root or lies beneath it. Takes the
 * Browser and a key, returns 0 or 1.
 *
 * A '/' separator is assumed, which is the same assumption browser_ascend
 * already makes when it takes a parent with strrchr. That is not the
 * widget knowing it is looking at a filesystem -- it is the one shape
 * every key format in this project shares, and phase 16's keys keep it.
 */
static int
key_under_root(const Browser * b, const char * key)
{
  size_t rlen = strlen(b->root);

  /* A root of "/" (or any key with a trailing separator) would otherwise
     demand a second separator at the join and reject every child. */
  while (rlen > 1 && b->root[rlen - 1] == '/') {
    rlen--;
  }

  if (strncmp(key, b->root, rlen) != 0) {
    return 0;
  }
  return key[rlen] == '\0' || key[rlen] == '/';
} /* key_under_root */

/*
 * browser_goto: see include/ui/browser.h.
 */
int
browser_goto(Browser * b, const char * parent_key, const char * row_key)
{
  if (b == NULL || parent_key == NULL || row_key == NULL) {
    return -1;
  }
  if (!key_under_root(b, parent_key)) {
    return -1;
  }

  /* Already listing the right directory is the common case -- jumping to
     the playing track while browsing its own album -- and reloading it
     would throw away rows for nothing. */
  int same = (strcmp(b->cwd, parent_key) == 0);

  char * target = strdup(parent_key);
  if (target == NULL) {
    return -1;
  }

  if (!same && load_into(b, target) != 0) {
    free(target);
    return -1;
  }

  size_t at    = 0;
  int    found = 0;
  for (size_t i = 0; i < b->n; i++) {
    if (strcmp(b->rows[i].key, row_key) == 0) {
      at    = i;
      found = 1;
      break;
    }
  }

  if (!found) {
    /* The parent listed but the row is not in it -- a track deleted or
       renamed since it was queued. Put back what was on screen. */
    if (!same && load_into(b, b->cwd) != 0) {
      /* The restore failed, so `rows` is still the target's listing and
         `cwd` must follow it: leaving the two disagreeing would draw one
         directory while descending into another. */
      free(b->cwd);
      b->cwd    = target;
      b->cursor = 0;
      b->scroll = 0;
      clamp(b);
      return -1;
    }
    free(target);
    clamp(b);
    return -1;
  }

  if (same) {
    free(target);
  } else {
    free(b->cwd);
    b->cwd = target;
  }
  b->cursor = at;
  b->scroll = 0; /* clamp scrolls the cursor back into view */
  clamp(b);
  return 0;
} /* browser_goto */

/*
 * browser_set_viewport: see include/ui/browser.h.
 */
void
browser_set_viewport(Browser * b, int rows)
{
  if (b == NULL) {
    return;
  }
  b->viewport = (rows < BROWSER_MIN_VIEWPORT) ? BROWSER_MIN_VIEWPORT : rows;
  clamp(b);
} /* browser_set_viewport */

/* Accessors. See include/ui/browser.h. */

size_t
browser_count(const Browser * b)
{
  return (b == NULL) ? 0 : b->n;
} /* browser_count */

size_t
browser_cursor(const Browser * b)
{
  return (b == NULL) ? 0 : b->cursor;
} /* browser_cursor */

size_t
browser_scroll(const Browser * b)
{
  return (b == NULL) ? 0 : b->scroll;
} /* browser_scroll */

const BrowserRow *
browser_row(const Browser * b, size_t i)
{
  if (b == NULL || i >= b->n) {
    return NULL;
  }
  return &b->rows[i];
} /* browser_row */

const BrowserRow *
browser_selection(const Browser * b)
{
  if (b == NULL || b->n == 0) {
    return NULL;
  }
  return &b->rows[b->cursor];
} /* browser_selection */

const char *
browser_cwd(const Browser * b)
{
  return (b == NULL) ? "" : b->cwd;
} /* browser_cwd */
