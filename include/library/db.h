/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_LIBRARY_DB_H
#define KURONO_LIBRARY_DB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Opaque handle over the SQLite connection and every prepared statement.
 * No sqlite3.h type appears anywhere in this header -- every sqlite3_*
 * call lives in db.c (CLAUDE.md, phase-13-db.md scope). Callers in
 * library/scan.c and library/query.c never see a sqlite3_stmt or a
 * sqlite3*, only these typed functions.
 */
typedef struct Db Db;

/*
 * One track row as read back from `tracks`, mirroring the schema's
 * nineteen columns (DESIGN.md VII.1) minus none. String fields are NULL
 * when the column is NULL (a missing tag, per DESIGN.md XI.1 -- stored
 * as NULL, not ""); the caller must not assume non-NULL. Nullable
 * INTEGER columns use an explicit has_* flag since int64_t has no NULL
 * of its own. Every owned string here is freed by db_track_row_free.
 */
typedef struct {
  int64_t id;
  char *  path;
  char *  parent_dir;
  int64_t mtime;
  int64_t size;
  char *  title;          /* NULL if absent */
  char *  artist;          /* NULL if absent */
  char *  album;           /* NULL if absent */
  char *  album_artist;    /* NULL if absent */
  char *  genre;           /* NULL if absent */
  int     has_year;
  int64_t year;
  int     has_track_no;
  int64_t track_no;
  int     has_disc_no;
  int64_t disc_no;
  int     has_duration_ms;
  int64_t duration_ms;
  int     has_bitrate;
  int64_t bitrate;
  int     has_sample_rate;
  int64_t sample_rate;
  int     has_channels;
  int64_t channels;
  char *  codec;           /* NULL if absent */
  int64_t scan_gen;
} DbTrackRow;

/*
 * The fields scan.c (phase 14) has in hand after a tag/stream-property
 * read, for db_track_upsert. Nullable numeric fields are pointers --
 * NULL means "tag absent", matching the row-side has_* convention
 * without a second flag field on the write side. All pointers are
 * borrowed for the duration of the call only; db_track_upsert does not
 * retain any of them.
 */
typedef struct {
  const char *    path;
  const char *    parent_dir;
  int64_t         mtime;
  int64_t         size;
  const char *    title;          /* NULL if absent */
  const char *    artist;         /* NULL if absent */
  const char *    album;          /* NULL if absent */
  const char *    album_artist;   /* NULL if absent */
  const char *    genre;          /* NULL if absent */
  const int64_t * year;           /* NULL if absent */
  const int64_t * track_no;       /* NULL if absent */
  const int64_t * disc_no;        /* NULL if absent */
  const int64_t * duration_ms;    /* NULL if absent */
  const int64_t * bitrate;        /* NULL if absent */
  const int64_t * sample_rate;    /* NULL if absent */
  const int64_t * channels;       /* NULL if absent */
  const char *    codec;          /* NULL if absent */
  int64_t         scan_gen;
} DbTrackWrite;

/*
 * db_open: open (or create) the SQLite database at db_path, apply the
 * schema verbatim (DESIGN.md VII.1) on a fresh file, run forward
 * migrations from schema_version on an old one, and reconcile the
 * `music_dirs` meta key against `roots`/`nroots` (the config-resolved
 * music roots, in any order -- db_open sorts its own copy before
 * joining and comparing, per amendment 01). PRAGMA journal_mode=WAL and
 * synchronous=NORMAL are set before anything else.
 *
 * Every failure mode in DESIGN.md XI.4 is handled here and does not
 * surface as an error return:
 *   - cache dir absent is config.c's problem, not this function's;
 *   - library.db absent -> schema applied, meta seeded, empty database;
 *   - schema_version older than KURONO_SCHEMA_VERSION -> migrated;
 *   - schema_version newer -> kurono_die("database written by a newer
 *     kurono ..."), never returns;
 *   - SQLITE_CORRUPT / SQLITE_NOTADB -> the file is unlinked and
 *     rebuilt from scratch, once;
 *   - an unrecoverable error (e.g. a second corruption immediately
 *     after rebuild, or an allocation failure) -> kurono_die, never
 *     returns.
 *
 * This function therefore never returns NULL -- every failure is either
 * self-healing or fatal via kurono_die. *out_rescanned, if out_rescanned
 * is non-NULL, is set to 1 when the music_dirs reconciliation cleared
 * `tracks` (the caller should treat this exactly like a missing
 * database and run a full scan) and to 0 otherwise, including on a
 * freshly created database (nothing to clear, but nothing to preserve
 * either -- the caller's normal "library.db absent -> full scan" path
 * already covers that case; the flag is not consulted there).
 */
Db * db_open(const char * db_path, const char * const * roots, size_t nroots, int * out_rescanned);

/*
 * db_close: finalise every prepared statement, then sqlite3_close_v2,
 * then free *db. Statements are finalised before the close call because
 * sqlite3_close (not _v2) would otherwise return SQLITE_BUSY with any
 * of them still live (DESIGN.md IV.9.b step 7) -- db_close uses close_v2
 * regardless, as the belt-and-braces form the design calls for. A NULL
 * db is a no-op.
 */
void db_close(Db * db);

/*
 * db_is_corrupt: 1 once any statement on this handle has terminated
 * with SQLITE_CORRUPT or SQLITE_NOTADB, 0 otherwise. Every query in
 * this file degrades a database error to zero results rather than
 * reporting it (DESIGN.md XI.7), which is right for the caller's
 * control flow but would otherwise make post-open corruption invisible
 * -- the library would simply appear empty forever. This flag is how
 * T1 sees it: queries keep returning empty so the UI stays alive, and
 * the caller decides when to close, unlink, and rebuild (DESIGN.md
 * XI.4). Sticky -- never cleared, since the handle is not recoverable
 * in place. Corruption found at open is handled inside db_open and
 * never reaches this flag.
 */
int db_is_corrupt(Db * db);

/*
 * db_result_truncated: 1 if the most recent array-returning query ran
 * out of memory and returned fewer rows than the query actually
 * matched, 0 otherwise. Both sources of that count: this file's own
 * realloc/strdup failing while growing the result array, and SQLite
 * itself returning SQLITE_NOMEM from the statement (it allocates too --
 * a DISTINCT builds a temp B-tree). The caller cannot act differently
 * on the two, so they set the same flag. DESIGN.md XI.7 requires
 * exactly this: after
 * startup, an allocation failure in the UI path degrades to "out of
 * memory, result truncated" rather than aborting. Without the flag the
 * degrade is invisible and a browser column silently shows a short
 * list.
 *
 * Unlike db_is_corrupt this is NOT sticky: it is cleared at the start
 * of every array-returning query, so it describes only the query that
 * just returned and must be read immediately after it. That is safe
 * because T1 is the only caller and runs these synchronously; a second
 * query before the check overwrites the answer. Queries returning a
 * single row (db_query_track_by_id) never set it -- a failed field
 * allocation there surfaces as a NULL column, which is the same
 * degrade applied per field rather than per result set.
 */
int db_result_truncated(Db * db);

/* db_begin / db_commit / db_rollback: wrap BEGIN / COMMIT / ROLLBACK. Return 0 on success, -1 on error. */
int db_begin(Db * db);
int db_commit(Db * db);
int db_rollback(Db * db);

/*
 * db_meta_get: look up `key` in the `meta` table. Returns an owned,
 * caller-freed copy of the value, or NULL if the key is absent or on
 * error -- the two are not distinguished, matching every meta key's use
 * in this design (a missing key and a key that errored both mean
 * "treat as unknown / force the conservative path").
 */
char * db_meta_get(Db * db, const char * key);

/*
 * db_meta_set: upsert `key` = `value` in the `meta` table. Returns 0 on
 * success, -1 on error.
 */
int db_meta_set(Db * db, const char * key, const char * value);

/*
 * db_track_unchanged: DESIGN.md V.5 step 4. Returns 1 if a row exists in
 * `tracks` with this exact path, mtime, and size (the caller should
 * UPDATE scan_gen and skip the tag read), 0 if no such row exists (the
 * caller should read tags and upsert), or -1 on a database error (the
 * caller should treat this like 0 -- re-reading tags is safe, silently
 * skipping a file is not).
 */
int db_track_unchanged(Db * db, const char * path, int64_t mtime, int64_t size);

/*
 * db_track_touch: UPDATE tracks SET scan_gen = gen WHERE path = path.
 * This is the statement phase 13's tracks_au WHEN clause exists to make
 * cheap -- it touches no FTS page because none of title/artist/album/
 * album_artist changed. Returns 0 on success, -1 on error.
 */
int db_track_touch(Db * db, const char * path, int64_t gen);

/*
 * db_track_upsert: INSERT the row described by *t, or UPDATE it in place
 * on a `path` conflict (DESIGN.md V.5 step 5). `id` is stable across a
 * rescan of a file whose mtime, size, or tags changed -- anything
 * holding the id (the queue, jump-to-playing) survives it. Returns 0 on
 * success, -1 on error.
 */
int db_track_upsert(Db * db, const DbTrackWrite * t);

/*
 * db_tracks_sweep: DELETE FROM tracks WHERE scan_gen < gen. Called once,
 * after every root has been walked (DESIGN.md V.5 step 6) -- never per
 * root. Returns 0 on success, -1 on error.
 */
int db_tracks_sweep(Db * db, int64_t gen);

/*
 * db_query_dirs: the Directory view's column 1 (DESIGN.md VIII.4).
 * Returns every distinct `parent_dir` value equal to `parent` or
 * nested under it at ANY depth (backed by idx_tracks_parent), as an
 * owned array of owned strings via out_names and out_n. Browsing
 * "/m/plain" returns "/m/plain", "/m/plain/sub", and
 * "/m/plain/sub/deeper" alike -- not just the immediate children.
 * Phase 15 collapses the returned paths into single next-segment names
 * and de-duplicates, so its work is O(descendants of `parent`), not
 * O(children); db.c does not parse paths (see phase-13.md's
 * implementation log for why this split was chosen over teaching db.c
 * path segmentation). The matching is a prefix range rather than a
 * GLOB pattern, so '[', '*' and '?' in a directory name are ordinary
 * characters -- see the comment on the prepared statement in db.c for
 * what the pattern form got wrong.
 *
 * PRECONDITION: `parent` carries no trailing slash and is not bare "/".
 * Both spellings return zero rows rather than an error, because the
 * range builds "//" against paths that cannot contain it. Nothing in
 * the tree produces either -- the browser rebuilds paths as
 * parent + "/" + name, and column 1 at multi-root level lists the roots
 * instead of querying "/" -- so this is documented and checked
 * (tools/fts_query_test.c, test_prefix_range_precondition) rather than
 * normalised here, which would cost an allocation per query for a
 * spelling no caller produces. db_query_tracks_under_dir below carries
 * the identical range and the identical precondition. Always returns 0: a
 * database error degrades to zero results rather than being reported,
 * the same never-fatal-query policy db_query_search documents below,
 * applied uniformly to every query in this file rather than only to
 * search (DESIGN.md XI.7).
 */
int db_query_dirs(Db * db, const char * parent, char *** out_names, size_t * out_n);

/* db_query_artists: every distinct non-NULL artist, sorted (backed by idx_tracks_artist). */
int db_query_artists(Db * db, char *** out_names, size_t * out_n);

/* db_query_album_artists: every distinct non-NULL album_artist, sorted (backed by idx_tracks_album_artist). */
int db_query_album_artists(Db * db, char *** out_names, size_t * out_n);

/* db_query_albums_for: every distinct non-NULL album for the given artist, sorted. */
int db_query_albums_for(Db * db, const char * artist, char *** out_names, size_t * out_n);

/*
 * db_query_albums_for_album_artist: the album_artist twin of the above,
 * for the Album Artists view. Same shape, different column, and it must
 * be a different statement: filtering on `artist` returns nothing for a
 * compilation, whose album_artist ("Various Artists") is usually not any
 * track's artist. Backed by idx_tracks_album_artist's leading column.
 */
int db_query_albums_for_album_artist(Db * db, const char * album_artist,
                                      char *** out_names, size_t * out_n);

/*
 * db_query_tracks_for_album: every track with this album_artist/album,
 * ordered by disc_no, track_no -- idx_tracks_album_artist(album_artist,
 * album, disc_no, track_no) covers the sort, so no separate ORDER BY
 * step is needed at the storage layer. Always returns 0 -- see
 * db_query_dirs above for the degrade-not-report-error policy. Caller
 * frees with db_track_rows_free.
 */
int db_query_tracks_for_album(Db * db, const char * album_artist, const char * album,
                               DbTrackRow ** out_rows, size_t * out_n);

/*
 * db_query_tracks_for_artist_album: the `artist` twin of the above, for
 * the Artists view, which reaches an album through a track's artist
 * rather than the album's album_artist. Ordered the same way, but
 * idx_tracks_artist covers only the lookup, so this one sorts -- an
 * album's worth of rows.
 */
int db_query_tracks_for_artist_album(Db * db, const char * artist, const char * album,
                                      DbTrackRow ** out_rows, size_t * out_n);

/* db_query_tracks_in_dir: every track whose parent_dir equals dir, ordered by path. */
int db_query_tracks_in_dir(Db * db, const char * dir, DbTrackRow ** out_rows, size_t * out_n);

/*
 * db_query_tracks_under_dir: every track at or below dir -- the
 * recursive twin of the above, ordered by path, which puts a
 * subdirectory's tracks after their parent's and each subdirectory's in
 * its own order. This is what an enqueue of a directory row wants and
 * db_query_tracks_in_dir is not: a multi-disc album keeps its tracks in
 * CD1/, CD2/ subdirectories, so the non-recursive query returns only
 * the loose files beside them. Same degrade policy and same
 * db_track_rows_free as every other row query here.
 *
 * PRECONDITION: as db_query_dirs above -- `dir` carries no trailing
 * slash and is not bare "/". The range is byte-identical, so the gap
 * is too, and both are checked together in
 * tools/fts_query_test.c (test_prefix_range_precondition). A fix
 * applied to one statement and not the other is the failure that pair
 * of checks exists to catch.
 */
int db_query_tracks_under_dir(Db * db, const char * dir, DbTrackRow ** out_rows, size_t * out_n);

/*
 * db_query_track_by_id: fetch one row by primary key. Returns 1 and
 * fills *out_row if found, 0 otherwise -- not-found and a database
 * error are not distinguished, same policy as every other query in this
 * file. *out_row is only touched when the return value is 1 -- free it
 * with db_track_row_free_fields.
 */
int db_query_track_by_id(Db * db, int64_t id, DbTrackRow * out_row);

/*
 * db_query_search: SELECT t.* FROM tracks t JOIN (SELECT rowid AS rid,
 * rank AS r FROM tracks_fts WHERE tracks_fts MATCH ?1 LIMIT 200) x ON
 * t.id = x.rid ORDER BY x.r, x.rid (DESIGN.md V.6 step 3), using the one
 * statement prepared once at open and reused for every keystroke
 * (DESIGN.md V.6 step 4). `match_string` must already be the
 * escaped/quoted form phase 15's transform produces -- db.c binds it as
 * a parameter and never concatenates it. LIMIT 200 sits inside the
 * subquery, so it bounds both the rows returned and the rows bm25
 * scores; a match set over 200 rows is truncated by rowid before
 * ranking, not by rank, so the returned 200 are not always the global
 * top 200 by relevance. A residual SQLITE_ERROR from MATCH on a
 * malformed string is reported as zero rows (return 0, *out_n = 0), not
 * as an error -- DESIGN.md V.6 warns this must never be treated as
 * fatal. Always returns 0; every other query function in this file
 * applies the same policy uniformly rather than singling search out
 * (DESIGN.md XI.7).
 */
int db_query_search(Db * db, const char * match_string, DbTrackRow ** out_rows, size_t * out_n);

/* db_names_free: free an array returned by db_query_dirs/artists/album_artists/albums_for. */
void db_names_free(char ** names, size_t n);

/* db_track_row_free_fields: free the owned strings inside *row (not row itself). */
void db_track_row_free_fields(DbTrackRow * row);

/* db_track_rows_free: free every row's owned strings, then the array itself. */
void db_track_rows_free(DbTrackRow * rows, size_t n);

#endif // KURONO_LIBRARY_DB_H
