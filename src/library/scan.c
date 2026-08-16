#include "../../include/library/scan.h"
#include "../../include/audio/decoder.h"

#include <tag_c.h>

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/*
 * Extension prefilter (DESIGN.md V.5 step 3, phase-14-scan.md I.4). Cheap
 * on purpose -- the real gate on whether a file is indexable is whether
 * decoder_probe succeeds, not this list.
 */
static const char * const SCAN_EXT_TABLE[] = {
  ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus",
  ".wav", ".wv", ".ape", ".alac", ".aiff", ".mpc"
};
#define SCAN_EXT_COUNT (sizeof(SCAN_EXT_TABLE) / sizeof(SCAN_EXT_TABLE[0]))

/*
 * Defensive cap on recursion depth (phase-14-scan.md: "watch for
 * unbounded recursion on deep trees"). A real music tree is
 * artist/album/track deep; this only bites a pathological or
 * maliciously deep directory structure.
 */
#define SCAN_MAX_DEPTH 64

/*
 * ScanCtx: everything threaded through the recursive walk -- the
 * database handle, the caller's options (for on_progress and
 * ignore_names), the generation this scan stamps rows with, and the
 * running done/total counters plus the skipped-file tally that becomes
 * scan_run's return value.
 */
typedef struct {
  Db *             db;
  const ScanOpts * opts;
  int64_t          gen;
  int              done;
  int              total;
  int              skipped;
} ScanCtx;

/*
 * has_audio_ext: case-insensitive compare of name's extension against
 * SCAN_EXT_TABLE. Takes a basename, returns 1 if it matches, 0 otherwise
 * (no extension included).
 */
static int
has_audio_ext(const char * name)
{
  const char * dot = strrchr(name, '.');
  if (dot == NULL) {
    return 0;
  }
  for (size_t i = 0; i < SCAN_EXT_COUNT; i++) {
    if (strcasecmp(dot, SCAN_EXT_TABLE[i]) == 0) {
      return 1;
    }
  }
  return 0;
} /* has_audio_ext */

/*
 * dir_is_ignored: exact, case-sensitive basename compare against
 * opts->ignore_names. Takes a directory's own name (not a path) and the
 * options, returns 1 if it must not be descended into.
 */
static int
dir_is_ignored(const char * name, const ScanOpts * opts)
{
  for (size_t i = 0; i < opts->nignore; i++) {
    if (strcmp(name, opts->ignore_names[i]) == 0) {
      return 1;
    }
  }
  return 0;
} /* dir_is_ignored */

/*
 * path_join: "parent/child" into out, snprintf'd and truncation-checked.
 * Takes the destination buffer and its size plus both path halves,
 * returns 1 on success or 0 if the result would not fit (the caller
 * skips the entry, not the whole walk).
 */
static int
path_join(char * out, size_t out_sz, const char * parent, const char * name)
{
  int n = snprintf(out, out_sz, "%s/%s", parent, name);
  return (n >= 0 && (size_t) n < out_sz) ? 1 : 0;
} /* path_join */

/*
 * path_dirname: copy the portion of path before its final '/' into out.
 * Takes the full path and a destination buffer/size, returns 1 on
 * success or 0 if path has no '/' or the result does not fit -- every
 * path this walk produces has one, so failure here means truncation.
 */
static int
path_dirname(const char * path, char * out, size_t out_sz)
{
  const char * slash = strrchr(path, '/');
  if (slash == NULL) {
    return 0;
  }
  size_t len = (size_t) (slash - path);
  if (len == 0) {
    len = 1; /* the root "/" itself */
  }
  if (len >= out_sz) {
    return 0;
  }
  memcpy(out, path, len);
  out[len] = '\0';
  return 1;
} /* path_dirname */

/* Defined below walk_dir, which calls it; the two are mutually ordered
   only by this declaration. */
static void process_leaf(const char * path, const struct stat * st, ScanCtx * ctx);

/*
 * walk_dir: recursive opendir/readdir walk of one directory, in the
 * order DESIGN.md V.5 step 3 and phase-14-scan.md I.2 describe --
 * lstat first, symlinked directories are never descended into (never
 * follow a symlink out of the tree), a symlinked *file* is followed once
 * via stat to get its target's mtime/size, ignored directory basenames
 * are skipped without descent, and every regular file (real or resolved
 * through a symlink) that passes the extension filter is handed to
 * `leaf`. An unopenable directory is skipped, not an error (phase-14
 * amendment: "a root you cannot opendir is skipped, not an error",
 * applied uniformly to every subdirectory too). Takes the directory
 * path, the scan context, and the current recursion depth; returns
 * nothing.
 *
 * Each directory is counted before it is processed, so ctx->total grows
 * as the walk descends rather than being computed by a full pre-walk of
 * every root. ScanProgressFn's contract permits either, and this is the
 * cheaper one: a pre-count pass would lstat every file in the tree
 * twice, which doubles the syscall cost of exactly the warm rescan
 * DESIGN.md V.5 step 4 exists to keep stat-bound. The count loop itself
 * costs nothing extra -- rewinddir re-reads the same getdents buffer and
 * the extension test looks only at the name.
 */
static void
walk_dir(const char * dir, ScanCtx * ctx, int depth)
{
  if (depth > SCAN_MAX_DEPTH) {
    return; /* pathologically deep tree -- stop descending, not fatal */
  }

  DIR * d = opendir(dir);
  if (d == NULL) {
    return; /* unreadable/gone: skip this (sub)directory, walk continues */
  }

  struct dirent * de;

  /* Name-only prepass over this one directory: no stat, no d_type (the
     latter is not POSIX and is DT_UNKNOWN on some filesystems anyway).
     The consequence of judging by name alone is a denominator one too
     high for a directory named like an audio file, or for a file that
     later fails to stat -- never a file missed or scanned twice. */
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') {
      continue;
    }
    if (has_audio_ext(de->d_name)) {
      ctx->total++;
    }
  }
  rewinddir(d);

  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') {
      continue; /* ".", "..", and every hidden entry */
    }

    char path[PATH_MAX];
    if (!path_join(path, sizeof(path), dir, de->d_name)) {
      continue; /* over PATH_MAX: skip the entry, not the directory */
    }

    struct stat lst;
    if (lstat(path, &lst) != 0) {
      continue; /* unreadable/gone by the time we got here: skip */
    }

    if (S_ISLNK(lst.st_mode)) {
      struct stat rst;
      if (stat(path, &rst) != 0) {
        continue; /* broken symlink: skip */
      }
      if (S_ISDIR(rst.st_mode)) {
        continue; /* never follow a symlinked directory out of the tree */
      }
      if (S_ISREG(rst.st_mode) && has_audio_ext(de->d_name)) {
        process_leaf(path, &rst, ctx);
      }
      continue;
    }

    if (S_ISDIR(lst.st_mode)) {
      if (dir_is_ignored(de->d_name, ctx->opts)) {
        continue; /* exact, case-sensitive basename match, no descent */
      }
      walk_dir(path, ctx, depth + 1);
      continue;
    }

    if (S_ISREG(lst.st_mode) && has_audio_ext(de->d_name)) {
      process_leaf(path, &lst, ctx);
    }
  }

  closedir(d);
} /* walk_dir */

/*
 * scan_index_file: DESIGN.md V.5 step 5 and VII.2's metadata split for
 * one file that is new or changed -- taglib_c for tags, decoder_probe
 * for stream properties, then db_track_upsert. Any failure here (zero-
 * length file, decoder_probe rejecting the container/codec, an invalid
 * or unreadable tag) is per-file and never fatal (DESIGN.md XI.1): it is
 * reported to the caller as a non-zero return, which the caller counts
 * as skipped and moves on from. Takes the database, the file's path,
 * its mtime/size (already stat'd by the walk), and the generation to
 * stamp; returns 0 on success, non-zero on any per-file failure.
 */
static int
scan_index_file(Db * db, const char * path, int64_t mtime, int64_t size, int64_t gen)
{
  if (size == 0) {
    return -1; /* zero-length file: never fatal, just skipped */
  }

  StreamInfo info;
  if (decoder_probe(path, &info) != 0) {
    return -1; /* unreadable, not a container ffmpeg knows, or no audio stream */
  }

  TagLib_File * tf = taglib_file_new(path);
  if (tf == NULL || !taglib_file_is_valid(tf)) {
    if (tf != NULL) {
      taglib_file_free(tf);
    }
    return -1; /* corrupt/unreadable as far as taglib is concerned */
  }

  char parent[PATH_MAX];
  if (!path_dirname(path, parent, sizeof(parent))) {
    taglib_file_free(tf);
    return -1; /* should not happen -- every path here came from a "/" join */
  }

  TagLib_Tag * tag = taglib_file_tag(tf);

  /*
   * taglib_c's simple getters return "", not NULL, for an absent tag
   * (verified against the installed /usr/include/taglib/tag_c.h, which
   * documents the getters as always-non-NULL strings and only the
   * numeric year()/track() as 0-means-absent) -- an empty string must
   * become SQL NULL, never stored as "", per DESIGN.md XI.1 and
   * DbTrackWrite's contract.
   */
  char * title  = NULL;
  char * artist = NULL;
  char * album  = NULL;
  char * genre  = NULL;
  if (tag != NULL) {
    char * v;
    v = taglib_tag_title(tag);
    if (v != NULL && v[0] != '\0') { title = v; }
    v = taglib_tag_artist(tag);
    if (v != NULL && v[0] != '\0') { artist = v; }
    v = taglib_tag_album(tag);
    if (v != NULL && v[0] != '\0') { album = v; }
    v = taglib_tag_genre(tag);
    if (v != NULL && v[0] != '\0') { genre = v; }
  }

  int64_t year_v = 0, track_v = 0;
  const int64_t * year = NULL;
  const int64_t * track_no = NULL;
  if (tag != NULL) {
    unsigned int y   = taglib_tag_year(tag);
    unsigned int trk = taglib_tag_track(tag);
    if (y != 0)   { year_v = (int64_t) y;   year = &year_v; }
    if (trk != 0) { track_v = (int64_t) trk; track_no = &track_v; }
  }

  /*
   * album_artist is not a simple-API getter (DESIGN.md VII.2 note,
   * phase-14-scan.md's task brief) -- taglib_property_get's ALBUMARTIST
   * key is the multi-value-aware path; the first value is what a
   * single-valued column needs. DISCNUMBER is the same story: no
   * taglib_tag_disc() exists in tag_c.h at all, so it goes through the
   * property interface too, with a leading-integer parse since the tag
   * value can be "N" or "N/total".
   */
  char **          albumartist_vals = taglib_property_get(tf, "ALBUMARTIST");
  char *           album_artist = NULL;
  if (albumartist_vals != NULL && albumartist_vals[0] != NULL && albumartist_vals[0][0] != '\0') {
    album_artist = albumartist_vals[0];
  }

  char **          disc_vals = taglib_property_get(tf, "DISCNUMBER");
  int64_t          disc_v = 0;
  const int64_t *  disc_no = NULL;
  if (disc_vals != NULL && disc_vals[0] != NULL && disc_vals[0][0] != '\0') {
    char * endptr = NULL;
    long   d = strtol(disc_vals[0], &endptr, 10);
    if (endptr != disc_vals[0] && d > 0) {
      disc_v = (int64_t) d;
      disc_no = &disc_v;
    }
  }

  int64_t duration_ms_v = info.duration_ms;
  int64_t bitrate_v     = info.bitrate;
  int64_t sample_rate_v = (int64_t) info.sample_rate;
  int64_t channels_v    = (int64_t) info.channels;

  DbTrackWrite w = {
    .path          = path,
    .parent_dir    = parent,
    .mtime         = mtime,
    .size          = size,
    .title         = title,
    .artist        = artist,
    .album         = album,
    .album_artist  = album_artist,
    .genre         = genre,
    .year          = year,
    .track_no      = track_no,
    .disc_no       = disc_no,
    .duration_ms   = &duration_ms_v,
    .bitrate       = &bitrate_v,
    .sample_rate   = &sample_rate_v,
    .channels      = &channels_v,
    .codec         = info.codec[0] != '\0' ? info.codec : NULL,
    .scan_gen      = gen,
  };

  int rc = db_track_upsert(db, &w);

  if (albumartist_vals != NULL) {
    taglib_property_free(albumartist_vals);
  }
  if (disc_vals != NULL) {
    taglib_property_free(disc_vals);
  }
  taglib_tag_free_strings();
  taglib_file_free(tf);

  return rc == 0 ? 0 : -1;
} /* scan_index_file */

/*
 * process_leaf: pass-2 leaf callback -- DESIGN.md V.5 steps 4 and 5.
 * Reports progress first (done/total span every root, per ScanOpts'
 * contract), then the unchanged-row fast path (stat-only touch, no tag
 * read) or the full read-and-upsert. Any failure -- db_track_touch or
 * scan_index_file returning non-zero -- increments the skipped counter;
 * it never stops the walk (DESIGN.md XI.1).
 */
static void
process_leaf(const char * path, const struct stat * st, ScanCtx * ctx)
{
  ctx->done++;
  if (ctx->opts->on_progress != NULL) {
    ctx->opts->on_progress(ctx->opts->ctx, ctx->done, ctx->total, path);
  }

  int64_t mtime = (int64_t) st->st_mtime;
  int64_t size  = (int64_t) st->st_size;

  int unchanged = db_track_unchanged(ctx->db, path, mtime, size);
  if (unchanged == 1) {
    if (db_track_touch(ctx->db, path, ctx->gen) != 0) {
      ctx->skipped++; /* db error on touch: row keeps its old scan_gen and
                          will be swept -- a rare db-level failure that
                          would also be sinking db_commit shortly after */
    }
    return;
  }

  if (scan_index_file(ctx->db, path, mtime, size, ctx->gen) != 0) {
    ctx->skipped++;
  }
} /* process_leaf */

/*
 * scan_run: see include/library/scan.h.
 */
int
scan_run(Db * db, const ScanOpts * opts)
{
  if (db == NULL || opts == NULL) {
    return -1;
  }

  if (db_begin(db) != 0) {
    return -1;
  }

  /* Step 2: bump scan_gen. A missing/unparseable meta value starts at 0,
     matching db_meta_get's "missing key means unknown, treat
     conservatively" contract -- G is then 1 on a fresh database. */
  char *  gen_str = db_meta_get(db, "scan_gen");
  int64_t gen = 0;
  if (gen_str != NULL) {
    gen = strtoll(gen_str, NULL, 10);
    free(gen_str);
  }
  gen += 1;

  char gen_buf[32];
  snprintf(gen_buf, sizeof(gen_buf), "%lld", (long long) gen);
  if (db_meta_set(db, "scan_gen", gen_buf) != 0) {
    db_rollback(db);
    return -1;
  }

  ScanCtx ctx = { .db = db, .opts = opts, .gen = gen, .done = 0, .total = 0, .skipped = 0 };

  /*
   * Step 3, and step 3 only: for each root, in order, walk and index.
   * Step 6's sweep (below) and step 7's commit happen exactly once,
   * after this loop -- never inside it. A sweep per root would have the
   * second root's DELETE FROM tracks WHERE scan_gen < G remove every
   * track the first root just inserted, leaving a library that only
   * ever contains the last root (amendment 01's failure mode).
   */
  for (size_t i = 0; i < opts->nroots; i++) {
    walk_dir(opts->roots[i], &ctx, 0);
  }

  /*
   * Every query and write in db.c degrades a database error to an empty
   * result or a -1 return rather than reporting it (DESIGN.md XI.7), and
   * this scan turns each of those into a skipped file. That is right for
   * one bad file and wrong for a corrupt database: a scan of five
   * thousand would "succeed" with five thousand skips and then commit a
   * sweep that empties the library. db_is_corrupt is the one signal that
   * separates the two, so a corrupt handle rolls back and reports
   * failure, leaving the index exactly as it was for the caller to
   * unlink and rebuild (DESIGN.md XI.4).
   */
  if (db_is_corrupt(db)) {
    db_rollback(db);
    return -1;
  }

  /* Step 6: exactly once, after the final root. */
  if (db_tracks_sweep(db, gen) != 0) {
    db_rollback(db);
    return -1;
  }

  /* Step 7. */
  if (db_commit(db) != 0) {
    db_rollback(db);
    return -1;
  }

  return ctx.skipped;
} /* scan_run */
