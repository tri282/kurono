/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "../../include/ui/browser.h"
#include "../../include/ui/listgeom.h"

#include "../../include/library/db.h"
#include "../../include/library/query.h"

#include <dirent.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
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
 * row_cmp: directories before files, then query_name_cmp within each
 * group -- ASCII case-insensitive with a strcmp tiebreak, which is the
 * same ordering db.c's COLLATE NOCASE clauses produce and the same one
 * query_dirs_collapse sorts by. See query.h for why it is not strcmp
 * and not strcoll. qsort(3) comparator over BrowserRow.
 */
static int
row_cmp(const void * a, const void * b)
{
  const BrowserRow * x = (const BrowserRow *) a;
  const BrowserRow * y = (const BrowserRow *) b;

  if (x->is_dir != y->is_dir) {
    return y->is_dir - x->is_dir; /* is_dir 1 sorts before is_dir 0 */
  }
  return query_name_cmp(x->name, y->name);
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
 * fs_parent_key: the filesystem source's key decomposition -- the
 * dirname of a path. Takes the key, returns an owned parent key, or NULL
 * at "/" and for a key with no separator at all.
 *
 * This is the logic browser_ascend used to hold inline. Moving it here
 * changes nothing for this source and is what lets a source whose keys
 * are not paths exist at all (browser.h, BrowserSource.parent_key).
 */
static char *
fs_parent_key(void * self, const char * key)
{
  (void) self;

  if (key == NULL) {
    return NULL;
  }

  const char * slash = strrchr(key, '/');
  if (slash == NULL || slash == key) {
    return NULL; /* already at "/" or a key with no separator */
  }

  size_t plen   = (size_t) (slash - key);
  char * parent = malloc(plen + 1);
  if (parent == NULL) {
    return NULL;
  }
  memcpy(parent, key, plen);
  parent[plen] = '\0';
  return parent;
} /* fs_parent_key */

/*
 * browser_fs_source: see include/ui/browser.h.
 */
void
browser_fs_source(BrowserSource * out, char * const * ignore, size_t nignore)
{
  g_fs_source.ignore  = ignore;
  g_fs_source.nignore = nignore;

  out->load       = fs_load;
  out->free_rows  = fs_free_rows;
  out->parent_key = fs_parent_key;
  out->self       = &g_fs_source;
} /* browser_fs_source */


/* ---------------------------------------------------------------------
 * The library source (phase 16).
 *
 * One source serving three views over library/db.c. Everything specific
 * to a view is chosen at construction and read from LibSource; nothing
 * in the widget, the draw code, or lib_load's control flow branches on
 * "which view is this" -- lib_load branches on what a key *is*, which is
 * a property of the row the user is pointing at, not of the screen.
 *
 * Keys are self-describing and length-prefixed, never slash-joined:
 *
 *   R                     the top of this view
 *   D<path>               a directory (Directory view)
 *   A<len>:<name>         an artist, or an album artist
 *   B<len>:<a><len>:<alb> an album, qualified by the name above it
 *   I<id>                 one track, by primary key
 *
 * The length prefix is the point. A slash-joined key cannot survive
 * "AC/DC" or an album called "Live 1/2", and the failure is a listing of
 * the wrong thing rather than an error -- see
 * plans/kurono/05-impl/phase-16.md 0.C. Segment text is copied verbatim,
 * so no character in a tag is special here.
 * ------------------------------------------------------------------- */

#define LIB_KEY_ROOT   'R'
#define LIB_KEY_DIR    'D'
#define LIB_KEY_NAME   'A'
#define LIB_KEY_ALBUM  'B'
#define LIB_KEY_TRACK  'I'

/*
 * LibSource: one per view. `roots` is Config's array, borrowed, and is
 * only consulted at the synthetic top level of the Directory view
 * (amendment 01). `truncated` latches XI.7's out-of-memory signal from
 * whichever query last ran, for the status bar to drain.
 */
typedef struct {
  Db *   db;
  int    view;
  const char * const * roots;
  size_t nroots;
  int    truncated;
} LibSource;

static LibSource g_lib_source[BROWSER_VIEW_COUNT];

/*
 * seg_len: how many bytes "<len>:<text>" needs for a segment of `n`
 * bytes. Takes the segment length, returns the encoded length.
 */
static size_t
seg_len(size_t n)
{
  char tmp[32];
  return (size_t) snprintf(tmp, sizeof tmp, "%zu:", n) + n;
} /* seg_len */

/*
 * seg_put: write one length-prefixed segment. Takes the write cursor and
 * the segment, returns the cursor past what it wrote.
 */
static char *
seg_put(char * p, const char * s)
{
  size_t n = strlen(s);
  int    w = snprintf(p, seg_len(n) + 1, "%zu:", n);
  memcpy(p + w, s, n);
  return p + w + n;
} /* seg_put */

/*
 * seg_get: read one length-prefixed segment. Takes the read cursor and
 * an out-param for a freshly allocated copy of the segment; returns the
 * cursor past it, or NULL if the encoding is malformed -- which can only
 * happen if a key was built by something other than lib_key, so it is
 * treated as "no such row" rather than diagnosed.
 */
static const char *
seg_get(const char * p, char ** out)
{
  char * end = NULL;
  unsigned long n;

  if (p == NULL || *p == '\0') {
    return NULL;
  }
  n = strtoul(p, &end, 10);
  if (end == p || end == NULL || *end != ':') {
    return NULL;
  }
  end++;
  if (strlen(end) < n) {
    return NULL;
  }

  *out = malloc(n + 1);
  if (*out == NULL) {
    return NULL;
  }
  memcpy(*out, end, n);
  (*out)[n] = '\0';
  return end + n;
} /* seg_get */

/*
 * lib_key: build a key of the given kind. Takes the kind and up to two
 * segments (NULL for unused ones); returns an owned key or NULL on
 * allocation failure. LIB_KEY_DIR and LIB_KEY_TRACK carry their payload
 * raw in `a` because neither is composite and neither needs to be split
 * back apart by anything but strlen.
 */
static char *
lib_key(char kind, const char * a, const char * b)
{
  size_t need = 2; /* kind + NUL */
  char * key;
  char * p;

  if (kind == LIB_KEY_DIR || kind == LIB_KEY_TRACK) {
    need += (a != NULL) ? strlen(a) : 0;
    key = malloc(need);
    if (key == NULL) {
      return NULL;
    }
    key[0] = kind;
    strcpy(key + 1, (a != NULL) ? a : "");
    return key;
  }

  if (a != NULL) {
    need += seg_len(strlen(a));
  }
  if (b != NULL) {
    need += seg_len(strlen(b));
  }

  key = malloc(need);
  if (key == NULL) {
    return NULL;
  }
  key[0] = kind;
  p = key + 1;
  if (a != NULL) {
    p = seg_put(p, a);
  }
  if (b != NULL) {
    p = seg_put(p, b);
  }
  *p = '\0';
  return key;
} /* lib_key */

/*
 * rows_push: append one row, growing the array. Takes the array, its
 * count and capacity by reference plus the row's three fields (name and
 * key are taken over on success); returns 0, or -1 having freed the two
 * strings it was handed, since a caller that cannot append also cannot
 * keep them.
 */
static int
rows_push(BrowserRow ** rows, size_t * count, size_t * cap,
          char * name, char * key, int is_dir)
{
  if (name == NULL || key == NULL) {
    free(name);
    free(key);
    return -1;
  }
  if (*count == *cap) {
    size_t ncap = (*cap == 0) ? 16 : *cap * 2;
    BrowserRow * grown = realloc(*rows, ncap * sizeof **rows);
    if (grown == NULL) {
      free(name);
      free(key);
      return -1;
    }
    *rows = grown;
    *cap  = ncap;
  }
  (*rows)[*count].name   = name;
  (*rows)[*count].key    = key;
  (*rows)[*count].is_dir = is_dir;
  (*count)++;
  return 0;
} /* rows_push */

/*
 * track_label: the display name for a track row -- "05  Hunter", or the
 * title alone when there is no track number, or the path's basename when
 * there is no title (DESIGN.md XI.1: a missing tag is NULL, and the file
 * still has to be listed). Takes the row, returns an owned string.
 */
static char *
track_label(const DbTrackRow * t)
{
  const char * title = t->title;
  char         buf[512];

  if (title == NULL) {
    const char * slash = (t->path != NULL) ? strrchr(t->path, '/') : NULL;
    title = (slash != NULL) ? slash + 1 : ((t->path != NULL) ? t->path : "(untitled)");
  }

  if (t->has_track_no) {
    snprintf(buf, sizeof buf, "%2" PRId64 "  %s", t->track_no, title);
    return strdup(buf);
  }
  return strdup(title);
} /* track_label */

/*
 * push_tracks: turn a DbTrackRow array into track rows. Takes the
 * out-array and its bookkeeping plus the query result; returns 0 or -1.
 * The DbTrackRow array is the caller's to free either way.
 */
static int
push_tracks(BrowserRow ** rows, size_t * count, size_t * cap,
            const DbTrackRow * tracks, size_t n)
{
  char idbuf[32];

  for (size_t i = 0; i < n; i++) {
    snprintf(idbuf, sizeof idbuf, "%" PRId64, tracks[i].id);
    if (rows_push(rows, count, cap, track_label(&tracks[i]),
                  lib_key(LIB_KEY_TRACK, idbuf, NULL), 0) != 0) {
      return -1;
    }
  }
  return 0;
} /* push_tracks */

/*
 * detail_line: one "label  value" row of the tag detail preview. Takes
 * the out-array bookkeeping, the label and the value (NULL prints as a
 * dash, since a missing tag is information); returns 0 or -1.
 */
static int
detail_line(BrowserRow ** rows, size_t * count, size_t * cap,
            const char * label, const char * value)
{
  char buf[600];

  snprintf(buf, sizeof buf, "%-9s %s", label, (value != NULL) ? value : "--");
  return rows_push(rows, count, cap, strdup(buf), strdup(""), 0);
} /* detail_line */

/*
 * lib_preview_track: the tag detail of one track, as rows. This is
 * gap 10's ruling (plans/kurono/05-impl/phase-16.md 0.B): a container
 * previews as its contents, and a track -- which has none -- previews as
 * what is known about it. Takes the source, the track id and the
 * out-array bookkeeping; returns 0 or -1.
 */
static int
lib_preview_track(LibSource * s, int64_t id,
                  BrowserRow ** rows, size_t * count, size_t * cap)
{
  DbTrackRow t;
  char       buf[128];
  int        rc = 0;

  if (db_query_track_by_id(s->db, id, &t) != 1) {
    return 0; /* gone since the listing was built: an empty preview */
  }

  rc |= detail_line(rows, count, cap, "title", t.title);
  rc |= detail_line(rows, count, cap, "artist", t.artist);
  rc |= detail_line(rows, count, cap, "album", t.album);
  rc |= detail_line(rows, count, cap, "albumart", t.album_artist);
  rc |= detail_line(rows, count, cap, "genre", t.genre);

  if (t.has_year) {
    snprintf(buf, sizeof buf, "%" PRId64, t.year);
    rc |= detail_line(rows, count, cap, "year", buf);
  }
  if (t.has_duration_ms) {
    int64_t sec = t.duration_ms / 1000;
    snprintf(buf, sizeof buf, "%" PRId64 ":%02" PRId64, sec / 60, sec % 60);
    rc |= detail_line(rows, count, cap, "length", buf);
  }
  if (t.has_sample_rate || t.has_channels || t.codec != NULL) {
    snprintf(buf, sizeof buf, "%s %" PRId64 " Hz %" PRId64 "ch",
             (t.codec != NULL) ? t.codec : "?",
             t.has_sample_rate ? t.sample_rate : 0,
             t.has_channels ? t.channels : 0);
    rc |= detail_line(rows, count, cap, "format", buf);
  }
  if (t.has_bitrate) {
    snprintf(buf, sizeof buf, "%" PRId64 " kbps", t.bitrate / 1000);
    rc |= detail_line(rows, count, cap, "bitrate", buf);
  }
  rc |= detail_line(rows, count, cap, "path", t.path);

  db_track_row_free_fields(&t);
  return rc;
} /* lib_preview_track */

/*
 * lib_load_dir: the Directory view's listing for one directory -- the
 * immediate child directories, then the tracks in it. The children come
 * from db_query_dirs, which returns descendants at every depth, through
 * query_dirs_collapse, which is the half of that split ui/ owns.
 * Takes the source, the directory and the out-array bookkeeping;
 * returns 0 or -1.
 */
static int
lib_load_dir(LibSource * s, const char * dir,
             BrowserRow ** rows, size_t * count, size_t * cap)
{
  char **      paths = NULL;
  size_t       npaths = 0;
  char **      names = NULL;
  size_t       nnames = 0;
  DbTrackRow * tracks = NULL;
  size_t       ntracks = 0;
  int          rc = 0;

  db_query_dirs(s->db, dir, &paths, &npaths);
  s->truncated |= db_result_truncated(s->db);
  s->truncated |= query_dirs_collapse(dir, paths, npaths, &names, &nnames);
  db_names_free(paths, npaths);

  for (size_t i = 0; i < nnames && rc == 0; i++) {
    char   child[PATH_MAX];
    size_t dl = strlen(dir);

    snprintf(child, sizeof child, "%s%s%s", dir,
             (dl > 0 && dir[dl - 1] == '/') ? "" : "/", names[i]);
    rc = rows_push(rows, count, cap, strdup(names[i]),
                   lib_key(LIB_KEY_DIR, child, NULL), 1);
  }
  db_names_free(names, nnames);
  if (rc != 0) {
    return -1;
  }

  db_query_tracks_in_dir(s->db, dir, &tracks, &ntracks);
  s->truncated |= db_result_truncated(s->db);
  rc = push_tracks(rows, count, cap, tracks, ntracks);
  db_track_rows_free(tracks, ntracks);
  return rc;
} /* lib_load_dir */

/*
 * lib_load: see BrowserSource.load. Branches on what the key names, not
 * on which column asked and not on which view it belongs to -- the view
 * only ever decides *which* query answers "the albums of this name",
 * which is a two-line difference and not a shape difference.
 */
static int
lib_load(void * self, int column, const char * parent_key,
         BrowserRow ** out, size_t * n)
{
  (void) column;

  LibSource *  s = (LibSource *) self;
  BrowserRow * rows = NULL;
  size_t       count = 0, cap = 0;
  int          rc = 0;

  if (s == NULL || s->db == NULL || parent_key == NULL || out == NULL || n == NULL) {
    return -1;
  }

  switch (parent_key[0]) {
    case LIB_KEY_ROOT: {
      if (s->view == BROWSER_VIEW_DIRECTORY) {
        /* Amendment 01's synthetic level. A root that did not resolve is
           listed and marked, never hidden: a missing mount must be
           visible. */
        for (size_t i = 0; i < s->nroots && rc == 0; i++) {
          rc = rows_push(&rows, &count, &cap, strdup(s->roots[i]),
                         lib_key(LIB_KEY_DIR, s->roots[i], NULL), 1);
        }
      } else {
        char ** names = NULL;
        size_t  nn = 0;

        if (s->view == BROWSER_VIEW_ARTISTS) {
          db_query_artists(s->db, &names, &nn);
        } else {
          db_query_album_artists(s->db, &names, &nn);
        }
        s->truncated |= db_result_truncated(s->db);

        for (size_t i = 0; i < nn && rc == 0; i++) {
          rc = rows_push(&rows, &count, &cap, strdup(names[i]),
                         lib_key(LIB_KEY_NAME, names[i], NULL), 1);
        }
        db_names_free(names, nn);
      }
      break;
    }

    case LIB_KEY_DIR:
      rc = lib_load_dir(s, parent_key + 1, &rows, &count, &cap);
      break;

    case LIB_KEY_NAME: {
      char *  name = NULL;
      char ** albums = NULL;
      size_t  na = 0;

      if (seg_get(parent_key + 1, &name) == NULL) {
        return -1;
      }
      if (s->view == BROWSER_VIEW_ARTISTS) {
        db_query_albums_for(s->db, name, &albums, &na);
      } else {
        db_query_albums_for_album_artist(s->db, name, &albums, &na);
      }
      s->truncated |= db_result_truncated(s->db);

      for (size_t i = 0; i < na && rc == 0; i++) {
        rc = rows_push(&rows, &count, &cap, strdup(albums[i]),
                       lib_key(LIB_KEY_ALBUM, name, albums[i]), 1);
      }
      db_names_free(albums, na);
      free(name);
      break;
    }

    case LIB_KEY_ALBUM: {
      char *       name = NULL;
      char *       album = NULL;
      const char * p = seg_get(parent_key + 1, &name);
      DbTrackRow * tracks = NULL;
      size_t       nt = 0;

      if (p == NULL || seg_get(p, &album) == NULL) {
        free(name);
        return -1;
      }
      if (s->view == BROWSER_VIEW_ARTISTS) {
        db_query_tracks_for_artist_album(s->db, name, album, &tracks, &nt);
      } else {
        db_query_tracks_for_album(s->db, name, album, &tracks, &nt);
      }
      s->truncated |= db_result_truncated(s->db);

      rc = push_tracks(&rows, &count, &cap, tracks, nt);
      db_track_rows_free(tracks, nt);
      free(name);
      free(album);
      break;
    }

    case LIB_KEY_TRACK:
      rc = lib_preview_track(s, (int64_t) strtoll(parent_key + 1, NULL, 10),
                             &rows, &count, &cap);
      break;

    default:
      return -1;
  }

  if (rc != 0) {
    rows_free(rows, count);
    return -1;
  }

  *out = rows;
  *n   = count;
  return 0;
} /* lib_load */

/*
 * lib_parent_key: see BrowserSource.parent_key. A track has no parent
 * that can be browsed into, so it answers NULL and the widget stays put.
 */
static char *
lib_parent_key(void * self, const char * key)
{
  LibSource * s = (LibSource *) self;

  if (s == NULL || key == NULL) {
    return NULL;
  }

  switch (key[0]) {
    case LIB_KEY_DIR: {
      const char * path = key + 1;

      /* A root's parent is the synthetic level, and only when there is
         one -- with a single root the root is the ceiling and the level
         does not exist (amendment 01). */
      for (size_t i = 0; i < s->nroots; i++) {
        if (strcmp(path, s->roots[i]) == 0) {
          return (s->nroots > 1) ? lib_key(LIB_KEY_ROOT, NULL, NULL) : NULL;
        }
      }

      const char * slash = strrchr(path, '/');
      if (slash == NULL || slash == path) {
        return NULL;
      }
      {
        size_t plen   = (size_t) (slash - path);
        char * parent = malloc(plen + 2);
        if (parent == NULL) {
          return NULL;
        }
        parent[0] = LIB_KEY_DIR;
        memcpy(parent + 1, path, plen);
        parent[plen + 1] = '\0';
        return parent;
      }
    }

    case LIB_KEY_NAME:
      return lib_key(LIB_KEY_ROOT, NULL, NULL);

    case LIB_KEY_ALBUM: {
      char * name = NULL;
      char * parent;

      if (seg_get(key + 1, &name) == NULL) {
        return NULL;
      }
      parent = lib_key(LIB_KEY_NAME, name, NULL);
      free(name);
      return parent;
    }

    default:
      return NULL; /* the root, a track, or a key we did not write */
  }
} /* lib_parent_key */

/*
 * browser_library_locate: see include/ui/browser.h. The inverse of the
 * key builders above -- given a track, produce the pair of keys that
 * name where it sits in this source's view. Takes the source, the
 * track's row and two out-params; returns 0 with both keys owned by the
 * caller, or -1 with neither set.
 *
 * It lives here, next to lib_key, because it is the only other function
 * that has to know the encoding. A caller that built these keys itself
 * would be the second owner of the format, which is exactly the leak
 * BrowserSource.parent_key was added to close -- the same argument,
 * applied in the other direction.
 *
 * The row key is the same in all three views: a track is addressed by
 * id. The parent differs, and the difference is the whole point of the
 * views -- the same track is inside a directory, inside its artist's
 * album, and inside its album artist's album, and those are three
 * different rows in three different listings.
 *
 * A track whose tags do not place it in the current view -- no artist in
 * the Artists view, no album anywhere -- returns -1. That is "this track
 * has no position here", not a failure: the caller reports it and stays
 * where it is.
 */
int
browser_library_locate(const BrowserSource * src, const DbTrackRow * t,
                       char ** out_parent, char ** out_row)
{
  char idbuf[32];

  if (src == NULL || t == NULL || out_parent == NULL || out_row == NULL) {
    return -1;
  }
  /* Only a library source has an encoding to invert. Checked against the
     load function rather than a flag, so a source that is not one cannot
     be mistaken for one by an uninitialised field. */
  if (src->load != lib_load || src->self == NULL) {
    return -1;
  }

  LibSource * s      = (LibSource *) src->self;
  char *      parent = NULL;

  switch (s->view) {
    case BROWSER_VIEW_DIRECTORY:
      if (t->parent_dir == NULL) {
        return -1;
      }
      parent = lib_key(LIB_KEY_DIR, t->parent_dir, NULL);
      break;

    case BROWSER_VIEW_ARTISTS:
      if (t->artist == NULL || t->album == NULL) {
        return -1;
      }
      parent = lib_key(LIB_KEY_ALBUM, t->artist, t->album);
      break;

    case BROWSER_VIEW_ALBUM_ARTISTS:
      if (t->album_artist == NULL || t->album == NULL) {
        return -1;
      }
      parent = lib_key(LIB_KEY_ALBUM, t->album_artist, t->album);
      break;

    default:
      return -1;
  }

  if (parent == NULL) {
    return -1;
  }

  snprintf(idbuf, sizeof idbuf, "%" PRId64, t->id);
  char * row = lib_key(LIB_KEY_TRACK, idbuf, NULL);
  if (row == NULL) {
    free(parent);
    return -1;
  }

  *out_parent = parent;
  *out_row    = row;
  return 0;
} /* browser_library_locate */

/*
 * browser_library_track_id: see include/ui/browser.h. The other
 * direction of the same inversion browser_library_locate performs, and
 * it lives beside it for the same reason. Takes the source and a row
 * key, returns the library id or 0 when the key does not name a track.
 */
int64_t
browser_library_track_id(const BrowserSource * src, const char * row_key)
{
  if (src == NULL || row_key == NULL || src->load != lib_load) {
    return 0;
  }
  if (row_key[0] != LIB_KEY_TRACK) {
    return 0;
  }
  return (int64_t) strtoll(row_key + 1, NULL, 10);
} /* browser_library_track_id */

/*
 * rows_take: move `n` DbTrackRows from `add` onto the end of `*acc`,
 * growing it. Takes the accumulator, its count, and the batch; returns 0
 * or -1 on allocation failure, and consumes `add` either way -- the
 * owned strings are moved by struct assignment, so only the batch's
 * array is freed, never its fields. On failure the accumulator keeps
 * whatever it already had and the caller frees it as one.
 */
static int
rows_take(DbTrackRow ** acc, size_t * n, DbTrackRow * add, size_t nadd)
{
  if (nadd == 0) {
    free(add);
    return 0;
  }

  DbTrackRow * grown = realloc(*acc, (*n + nadd) * sizeof *grown);
  if (grown == NULL) {
    db_track_rows_free(add, nadd);
    return -1;
  }

  memcpy(grown + *n, add, nadd * sizeof *add);
  *acc = grown;
  *n  += nadd;
  free(add); /* fields moved above; only the array itself is ours to drop */
  return 0;
} /* rows_take */

/*
 * browser_library_expand: see include/ui/browser.h. Every kind that can
 * hold tracks answers here, so main.c never learns what a key looks
 * like -- the same reason browser_library_locate and
 * browser_library_track_id exist.
 *
 * A track key answers zero rows rather than itself: player_row_target
 * already handles that case and answering it twice would put one
 * decision in two places. The root answers zero too -- "queue the whole
 * library" is a different action from "queue this row", and silently
 * making it this one is worse than refusing.
 */
int
browser_library_expand(const BrowserSource * src, const char * row_key,
                       DbTrackRow ** out_rows, size_t * out_n)
{
  if (src == NULL || row_key == NULL || out_rows == NULL || out_n == NULL ||
      src->load != lib_load) {
    return -1;
  }

  LibSource *  s     = (LibSource *) src->self;
  DbTrackRow * acc   = NULL;
  size_t       count = 0;
  int          rc    = 0;

  switch (row_key[0]) {
    case LIB_KEY_DIR: {
      DbTrackRow * tracks = NULL;
      size_t       nt = 0;
      db_query_tracks_under_dir(s->db, row_key + 1, &tracks, &nt);
      s->truncated |= db_result_truncated(s->db);
      rc = rows_take(&acc, &count, tracks, nt);
      break;
    }

    case LIB_KEY_ALBUM: {
      char *       name  = NULL;
      char *       album = NULL;
      const char * p     = seg_get(row_key + 1, &name);
      DbTrackRow * tracks = NULL;
      size_t       nt = 0;

      if (p == NULL || seg_get(p, &album) == NULL) {
        free(name);
        return -1;
      }
      /* The same view split lib_load makes, and for the same reason: an
         Album Artists row filtered on `artist` misses every
         compilation. */
      if (s->view == BROWSER_VIEW_ARTISTS) {
        db_query_tracks_for_artist_album(s->db, name, album, &tracks, &nt);
      } else {
        db_query_tracks_for_album(s->db, name, album, &tracks, &nt);
      }
      s->truncated |= db_result_truncated(s->db);
      rc = rows_take(&acc, &count, tracks, nt);
      free(name);
      free(album);
      break;
    }

    case LIB_KEY_NAME: {
      /* An artist is its albums, in album order, each album in its own
         disc/track order. There is no one-statement form of this and it
         does not need one: it is the album query in a loop over what
         lib_load already lists under this row, so the queue order is
         exactly the browse order. */
      char *  name = NULL;
      char ** albums = NULL;
      size_t  na = 0;

      if (seg_get(row_key + 1, &name) == NULL) {
        return -1;
      }
      if (s->view == BROWSER_VIEW_ARTISTS) {
        db_query_albums_for(s->db, name, &albums, &na);
      } else {
        db_query_albums_for_album_artist(s->db, name, &albums, &na);
      }
      s->truncated |= db_result_truncated(s->db);

      for (size_t i = 0; i < na && rc == 0; i++) {
        DbTrackRow * tracks = NULL;
        size_t       nt = 0;
        if (s->view == BROWSER_VIEW_ARTISTS) {
          db_query_tracks_for_artist_album(s->db, name, albums[i], &tracks, &nt);
        } else {
          db_query_tracks_for_album(s->db, name, albums[i], &tracks, &nt);
        }
        s->truncated |= db_result_truncated(s->db);
        rc = rows_take(&acc, &count, tracks, nt);
      }
      db_names_free(albums, na);
      free(name);
      break;
    }

    default:
      break; /* ROOT, TRACK, and anything malformed: zero rows, not an error */
  }

  if (rc != 0) {
    db_track_rows_free(acc, count);
    return -1;
  }

  *out_rows = acc;
  *out_n    = count;
  return 0;
} /* browser_library_expand */

/*
 * lib_free_rows: see BrowserSource.free_rows.
 */
static void
lib_free_rows(void * self, BrowserRow * rows, size_t n)
{
  (void) self;
  rows_free(rows, n);
} /* lib_free_rows */

/*
 * browser_library_source / browser_library_root / browser_library_truncated:
 * see include/ui/browser.h.
 */
void
browser_library_source(BrowserSource * out, int view, Db * db,
                       const char * const * roots, size_t nroots)
{
  LibSource * s = &g_lib_source[(view >= 0 && view < BROWSER_VIEW_COUNT) ? view : 0];

  s->db     = db;
  s->view   = view;
  s->roots  = roots;
  s->nroots = nroots;

  out->load       = lib_load;
  out->free_rows  = lib_free_rows;
  out->parent_key = lib_parent_key;
  out->self       = s;
} /* browser_library_source */

char *
browser_library_root(int view, const char * const * roots, size_t nroots)
{
  if (view == BROWSER_VIEW_DIRECTORY && nroots == 1) {
    return lib_key(LIB_KEY_DIR, roots[0], NULL);
  }
  return lib_key(LIB_KEY_ROOT, NULL, NULL);
} /* browser_library_root */

int
browser_library_truncated(const BrowserSource * src)
{
  LibSource * s;

  if (src == NULL || src->self == NULL) {
    return 0;
  }
  s = (LibSource *) src->self;
  if (s->truncated) {
    s->truncated = 0;
    return 1;
  }
  return 0;
} /* browser_library_truncated */

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

  /* The two derived columns (DESIGN.md VIII.4's miller layout). Column 1
     is `rows` above and is the only listing with a cursor; column 0 is
     what contains it and column 2 is what is inside the selection, so
     both are functions of (cwd, cursor) and neither is navigable.
     Rebuilt by browser_refresh_derived, never here: a cursor move sets
     the flag and the redraw pays for it, so holding a key down costs one
     pair of loads per frame instead of one per repeat. */
  BrowserRow * prows;         /* column 0: the parent's listing */
  size_t       pn;
  size_t       pcursor;       /* index of cwd within prows, for the highlight */
  BrowserRow * vrows;         /* column 2: preview of the selection */
  size_t       vn;
  int          derived_dirty;
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

  /* Every mutator ends here, which makes this the one place that cannot
     forget to invalidate the derived columns. A resize also lands here
     and marks them dirty without needing to -- one wasted pair of loads
     per resize, against a whole class of stale-column bug. */
  b->derived_dirty = 1;

  if (b->n == 0) {
    b->cursor = 0;
    b->scroll = 0;
    return;
  }

  if (b->cursor >= b->n) {
    b->cursor = b->n - 1;
  }

  b->scroll = list_scroll_for(b->cursor, b->n, vp);
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

  if (b->src.load(b->src.self, BROWSER_COL_MAIN, key, &rows, &n) != 0) {
    return -1;
  }

  b->src.free_rows(b->src.self, b->rows, b->n);
  b->rows = rows;
  b->n    = n;
  b->derived_dirty = 1;
  return 0;
} /* load_into */

/*
 * browser_refresh_derived: see include/ui/browser.h. Rebuilds columns 0
 * and 2 if anything they depend on moved. Takes the Browser, returns
 * nothing -- a derived column that fails to load is simply empty, which
 * is the correct rendering of "nothing to show here" and never an error
 * the user has to dismiss.
 */
void
browser_refresh_derived(Browser * b)
{
  if (b == NULL || !b->derived_dirty) {
    return;
  }
  b->derived_dirty = 0;

  /* Column 0: the listing that contains cwd. Absent at the root, which
     is not a failure -- the column simply draws empty. */
  b->src.free_rows(b->src.self, b->prows, b->pn);
  b->prows   = NULL;
  b->pn      = 0;
  b->pcursor = 0;

  if (strcmp(b->cwd, b->root) != 0 && b->src.parent_key != NULL) {
    char * parent = b->src.parent_key(b->src.self, b->cwd);
    if (parent != NULL) {
      BrowserRow * rows = NULL;
      size_t       n    = 0;
      if (b->src.load(b->src.self, BROWSER_COL_PARENT, parent, &rows, &n) == 0) {
        b->prows = rows;
        b->pn    = n;
        for (size_t i = 0; i < n; i++) {
          if (strcmp(rows[i].key, b->cwd) == 0) {
            b->pcursor = i;
            break;
          }
        }
      }
      free(parent);
    }
  }

  /* Column 2: what is inside the selection. The source decides what
     that means for a row that is not a container -- the library source
     answers with tag detail, the filesystem source with nothing. */
  b->src.free_rows(b->src.self, b->vrows, b->vn);
  b->vrows = NULL;
  b->vn    = 0;

  if (b->n > 0 && b->cursor < b->n) {
    BrowserRow * rows = NULL;
    size_t       n    = 0;
    if (b->src.load(b->src.self, BROWSER_COL_PREVIEW, b->rows[b->cursor].key,
                    &rows, &n) == 0) {
      b->vrows = rows;
      b->vn    = n;
    }
  }
} /* browser_refresh_derived */

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
  b->src.free_rows(b->src.self, b->prows, b->pn);
  b->src.free_rows(b->src.self, b->vrows, b->vn);
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
 * browser_cursor_move: see include/ui/browser.h. Wraps on |delta| == 1,
 * clamps otherwise.
 */
void
browser_cursor_move(Browser * b, int delta)
{
  if (b == NULL || b->n == 0 || delta == 0) {
    return;
  }

  b->cursor = list_cursor_move(b->cursor, b->n, delta);
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

  /* Only the source knows how its keys decompose (browser.h). A NULL
     here means "no parent", which is also what an allocation failure
     inside the source looks like -- the two are deliberately not
     distinguished, because both mean the same thing to the user and the
     safe direction for either is to stay put. */
  char * parent = (b->src.parent_key != NULL)
                    ? b->src.parent_key(b->src.self, b->cwd)
                    : NULL;
  if (parent == NULL) {
    return 0;
  }

  /* Remember what we are leaving before the rows it lives in go away.
     The key, not the name: a key identifies a row to its source by
     construction, while two rows in one listing can share a name (a
     directory and a symlink to it elsewhere in the tree). */
  char * leaving = strdup(b->cwd);
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

  for (size_t i = 0; i < b->n; i++) {
    if (strcmp(b->rows[i].key, leaving) == 0) {
      b->cursor = i;
      break;
    }
  }
  free(leaving);

  clamp(b);
  return 1;
} /* browser_ascend */

/* Bounds the walk below. A key format that cycled -- a parent_key that
   never reaches NULL -- would otherwise hang the UI thread, and no
   library or filesystem hierarchy in this project is anywhere near this
   deep. Reaching the bound is treated as "not under the root". */
#define KEY_DEPTH_MAX 64

/*
 * key_under_root: 1 when `key` is the root or lies beneath it. Takes the
 * Browser and a key, returns 0 or 1.
 *
 * Walks up with the source's parent_key rather than testing a string
 * prefix. The prefix test assumed a '/' separator, and its comment
 * claimed that was "the one shape every key format in this project
 * shares" -- which stopped being true when phase 16 shipped library keys
 * as a kind byte and length-prefixed segments. It survived because the
 * Directory view's keys are a byte plus a path, so a prefix test happens
 * to work on them; the Artists and Album Artists views, whose root is
 * the single byte "R" and whose album keys start with "B", failed every
 * check. That made browser_goto silently unreachable in two of the three
 * views -- the third place the widget assumed a key was a path, after
 * browser_ascend and the parent derivation that BrowserSource.parent_key
 * was added to fix.
 *
 * The walk costs one allocation per level, bounded by the depth of the
 * hierarchy, and runs only on a jump -- never on a cursor move.
 */
static int
key_under_root(const Browser * b, const char * key)
{
  if (strcmp(key, b->root) == 0) {
    return 1;
  }
  if (b->src.parent_key == NULL) {
    return 0;
  }

  char * cur   = strdup(key);
  int    under = 0;

  for (int depth = 0; cur != NULL && depth < KEY_DEPTH_MAX; depth++) {
    char * up = b->src.parent_key(b->src.self, cur);
    free(cur);
    cur = up;
    if (cur != NULL && strcmp(cur, b->root) == 0) {
      under = 1;
      break;
    }
  }

  free(cur);
  return under;
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

/*
 * browser_parent_count / browser_parent_cursor / browser_parent_row /
 * browser_preview_count / browser_preview_row: see include/ui/browser.h.
 * The derived columns, as last built by browser_refresh_derived.
 */
size_t
browser_parent_count(const Browser * b)
{
  return (b == NULL) ? 0 : b->pn;
} /* browser_parent_count */

size_t
browser_parent_cursor(const Browser * b)
{
  return (b == NULL) ? 0 : b->pcursor;
} /* browser_parent_cursor */

const BrowserRow *
browser_parent_row(const Browser * b, size_t i)
{
  if (b == NULL || i >= b->pn) {
    return NULL;
  }
  return &b->prows[i];
} /* browser_parent_row */

size_t
browser_preview_count(const Browser * b)
{
  return (b == NULL) ? 0 : b->vn;
} /* browser_preview_count */

const BrowserRow *
browser_preview_row(const Browser * b, size_t i)
{
  if (b == NULL || i >= b->vn) {
    return NULL;
  }
  return &b->vrows[i];
} /* browser_preview_row */


/*
 * browser_rows / browser_parent_rows / browser_preview_rows: see
 * include/ui/browser.h. Takes a browser, returns the column's array or
 * NULL when the browser or the column is empty.
 */
const BrowserRow *
browser_rows(const Browser * b)
{
  return (b == NULL || b->n == 0) ? NULL : b->rows;
} /* browser_rows */

const BrowserRow *
browser_parent_rows(const Browser * b)
{
  return (b == NULL || b->pn == 0) ? NULL : b->prows;
} /* browser_parent_rows */

const BrowserRow *
browser_preview_rows(const Browser * b)
{
  return (b == NULL || b->vn == 0) ? NULL : b->vrows;
} /* browser_preview_rows */
