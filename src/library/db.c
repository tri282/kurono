#include "../../include/library/db.h"
#include "../../include/main.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ruled in plan.md VII gap 9: schema_version starts at 1, forward
 * migration is a switch on the integer with one case per step, no
 * migration table.
 */
#define KURONO_SCHEMA_VERSION 1

/*
 * DESIGN.md VII.1, transcribed verbatim: the nineteen-column tracks
 * table, its four indexes, the FTS5 virtual table, meta, and the three
 * sync triggers -- including the non-standard WHEN clause on tracks_au
 * that keeps a scan_gen-only UPDATE from touching any FTS page
 * (phase-13-db.md I.3).
 */
#define SQL_SCHEMA \
  "CREATE TABLE tracks (\n" \
  "  id           INTEGER PRIMARY KEY,\n" \
  "  path         TEXT    NOT NULL UNIQUE,\n" \
  "  parent_dir   TEXT    NOT NULL,\n" \
  "  mtime        INTEGER NOT NULL,\n" \
  "  size         INTEGER NOT NULL,\n" \
  "  title        TEXT,\n" \
  "  artist       TEXT,\n" \
  "  album        TEXT,\n" \
  "  album_artist TEXT,\n" \
  "  genre        TEXT,\n" \
  "  year         INTEGER,\n" \
  "  track_no     INTEGER,\n" \
  "  disc_no      INTEGER,\n" \
  "  duration_ms  INTEGER,\n" \
  "  bitrate      INTEGER,\n" \
  "  sample_rate  INTEGER,\n" \
  "  channels     INTEGER,\n" \
  "  codec        TEXT,\n" \
  "  scan_gen     INTEGER NOT NULL\n" \
  ");\n" \
  "CREATE INDEX idx_tracks_album_artist ON tracks(album_artist, album, disc_no, track_no);\n" \
  "CREATE INDEX idx_tracks_artist       ON tracks(artist);\n" \
  "CREATE INDEX idx_tracks_parent       ON tracks(parent_dir);\n" \
  "CREATE INDEX idx_tracks_scan_gen     ON tracks(scan_gen);\n" \
  "CREATE VIRTUAL TABLE tracks_fts USING fts5(\n" \
  "  title, artist, album, album_artist,\n" \
  "  content='tracks', content_rowid='id',\n" \
  "  tokenize='unicode61 remove_diacritics 2'\n" \
  ");\n" \
  "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);\n" \
  "CREATE TRIGGER tracks_ai AFTER INSERT ON tracks BEGIN\n" \
  "  INSERT INTO tracks_fts(rowid, title, artist, album, album_artist)\n" \
  "    VALUES (new.id, new.title, new.artist, new.album, new.album_artist);\n" \
  "END;\n" \
  "CREATE TRIGGER tracks_ad AFTER DELETE ON tracks BEGIN\n" \
  "  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, album_artist)\n" \
  "    VALUES('delete', old.id, old.title, old.artist, old.album, old.album_artist);\n" \
  "END;\n" \
  "CREATE TRIGGER tracks_au AFTER UPDATE ON tracks\n" \
  "WHEN old.title        IS NOT new.title\n" \
  "  OR old.artist       IS NOT new.artist\n" \
  "  OR old.album        IS NOT new.album\n" \
  "  OR old.album_artist IS NOT new.album_artist\n" \
  "BEGIN\n" \
  "  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, album_artist)\n" \
  "    VALUES('delete', old.id, old.title, old.artist, old.album, old.album_artist);\n" \
  "  INSERT INTO tracks_fts(rowid, title, artist, album, album_artist)\n" \
  "    VALUES(new.id, new.title, new.artist, new.album, new.album_artist);\n" \
  "END;\n"

/*
 * struct Db: the SQLite connection and every prepared statement, held
 * for the lifetime of the handle and finalised in db_close before
 * sqlite3_close_v2 (DESIGN.md IV.9.b step 7). This definition is
 * private to db.c -- include/library/db.h exposes only the opaque
 * typedef, so no sqlite3.h type can leak into any other file.
 */
struct Db {
  sqlite3 *      conn;
  sqlite3_stmt * meta_get;
  sqlite3_stmt * meta_set;
  sqlite3_stmt * track_unchanged;
  sqlite3_stmt * track_touch;
  sqlite3_stmt * track_upsert;
  sqlite3_stmt * tracks_sweep;
  sqlite3_stmt * query_dirs;
  sqlite3_stmt * query_artists;
  sqlite3_stmt * query_album_artists;
  sqlite3_stmt * query_albums_for;
sqlite3_stmt * query_albums_for_album_artist;
  sqlite3_stmt * query_tracks_for_album;
sqlite3_stmt * query_tracks_for_artist_album;
  sqlite3_stmt * query_tracks_in_dir;
  sqlite3_stmt * query_tracks_under_dir;
  sqlite3_stmt * query_track_by_id;
  sqlite3_stmt * query_search;
  int            corrupt;
  int            truncated;
};

typedef enum { DB_STATE_OK, DB_STATE_CORRUPT, DB_STATE_ERROR } DbOpenState;

/*
 * db_reset: reset `stmt` and latch corruption. sqlite3_reset returns the
 * error code of the statement's most recent sqlite3_step, which is the
 * only place the query paths in this file can observe SQLITE_CORRUPT --
 * they all drain with `while (step == SQLITE_ROW)` and report a
 * non-ROW terminal code as zero rows (DESIGN.md XI.7, deliberately: a
 * query degrades, it does not abort). Without this latch the degrade is
 * silent and a database that corrupts after open presents as a
 * permanently empty library, leaving DESIGN.md XI.4's unlink-and-rebuild
 * unreachable. The flag is sticky: once set it is never cleared, since
 * the handle it describes is not recoverable in place. Every query and
 * write path in this file routes its reset through here, so there is no
 * statement whose corruption goes unseen.
 */
static void
db_reset(Db * db, sqlite3_stmt * stmt)
{
  int rc = sqlite3_reset(stmt);

  if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
    db->corrupt = 1;
  }

  /* SQLITE_NOMEM is the other half of the truncation story. The drain
     helpers latch their own realloc/strdup failures, but SQLite can run
     out of memory inside the statement -- building the temp B-tree for
     a DISTINCT, say -- and that surfaces only as a terminal code, with
     the drain seeing a short row sequence and no error. Measured: a
     query returning one row returns zero under a capped allocator. Both
     halves mean the same thing to the caller, so both set the same
     flag. */
  if (rc == SQLITE_NOMEM) {
    db->truncated = 1;
  }
} /* db_reset */

/*
 * db_is_corrupt: see include/library/db.h.
 */
int
db_is_corrupt(Db * db)
{
  return (db != NULL) ? db->corrupt : 0;
} /* db_is_corrupt */

/*
 * db_result_truncated: see include/library/db.h.
 */
int
db_result_truncated(Db * db)
{
  return (db != NULL) ? db->truncated : 0;
} /* db_result_truncated */

/*
 * strdup_or_null: strdup(s), or NULL if s is NULL. A failed strdup is
 * also reported as NULL -- indistinguishable here from an absent tag,
 * which is the DESIGN.md XI.7 degrade-not-abort policy applied to a
 * single field rather than a whole result set.
 */
static char *
strdup_or_null(const char * s)
{
  return (s != NULL) ? strdup(s) : NULL;
} /* strdup_or_null */

/*
 * bind_null_or_text: bind `val` as TEXT at `idx`, or NULL if val is NULL.
 */
static void
bind_null_or_text(sqlite3_stmt * stmt, int idx, const char * val)
{
  if (val != NULL) {
    sqlite3_bind_text(stmt, idx, val, -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, idx);
  }
} /* bind_null_or_text */

/*
 * bind_null_or_int64: bind *val as INTEGER at `idx`, or NULL if val is NULL.
 */
static void
bind_null_or_int64(sqlite3_stmt * stmt, int idx, const int64_t * val)
{
  if (val != NULL) {
    sqlite3_bind_int64(stmt, idx, *val);
  } else {
    sqlite3_bind_null(stmt, idx);
  }
} /* bind_null_or_int64 */

/*
 * meta_get_raw / meta_set_raw: ad-hoc (not pre-prepared) meta table
 * access for the two windows where the real prepared statements do not
 * exist yet -- schema bootstrap and music_dirs reconciliation, both of
 * which run before prepare_all_statements. The public db_meta_get/
 * db_meta_set use the long-lived prepared statements instead.
 */
static char *
meta_get_raw(sqlite3 * conn, const char * key)
{
  sqlite3_stmt * stmt;
  if (sqlite3_prepare_v2(conn, "SELECT value FROM meta WHERE key = ?1", -1, &stmt, NULL) != SQLITE_OK) {
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

  char * out = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = strdup_or_null((const char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return out;
} /* meta_get_raw */

static int
meta_set_raw(sqlite3 * conn, const char * key, const char * value)
{
  sqlite3_stmt * stmt;
  if (sqlite3_prepare_v2(conn,
        "INSERT INTO meta(key, value) VALUES(?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);

  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
} /* meta_set_raw */

/*
 * apply_schema: run SQL_SCHEMA and seed schema_version/scan_gen, all
 * inside one transaction, on a database with no meta table yet
 * (DESIGN.md XI.4 "library.db absent -> create, apply schema"). Returns
 * 0 on success, -1 on error (transaction rolled back).
 */
static int
apply_schema(sqlite3 * conn)
{
  char * errmsg = NULL;

  if (sqlite3_exec(conn, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
    return -1;
  }
  if (sqlite3_exec(conn, SQL_SCHEMA, NULL, NULL, &errmsg) != SQLITE_OK) {
    fprintf(stderr, "kurono: schema creation failed: %s\n", errmsg != NULL ? errmsg : "unknown error");
    sqlite3_free(errmsg);
    sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  char version_str[16];
  snprintf(version_str, sizeof(version_str), "%d", KURONO_SCHEMA_VERSION);
  if (meta_set_raw(conn, "schema_version", version_str) != 0 ||
      meta_set_raw(conn, "scan_gen", "0") != 0) {
    sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  return (sqlite3_exec(conn, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
} /* apply_schema */

/*
 * migrate: forward migration from `from_version` to KURONO_SCHEMA_VERSION,
 * as a switch with one case per step and intentional fallthrough between
 * them -- the ruled mechanism (plan.md VII gap 9), no migration table.
 * There are no forward steps yet: KURONO_SCHEMA_VERSION is 1, so this is
 * only reachable for a from_version no version-1-or-later kurono has
 * ever written, and is treated as an error rather than guessed past.
 * Returns 0 on success, -1 on error.
 */
static int
migrate(sqlite3 * conn, int from_version)
{
  switch (from_version) {
    default:
      fprintf(stderr, "kurono: cannot migrate database from unknown schema_version %d\n", from_version);
      return -1;
  }

  char version_str[16];
  snprintf(version_str, sizeof(version_str), "%d", KURONO_SCHEMA_VERSION);
  return meta_set_raw(conn, "schema_version", version_str);
} /* migrate */

/*
 * init_schema: on `conn` (already opened, pragmas already set), decide
 * between a fresh database (apply the schema), an existing one at the
 * current version (nothing to do), an existing one at an older version
 * (migrate), or a newer one (fatal, DESIGN.md XI.4 -- via kurono_die,
 * which does not return). Detects SQLITE_CORRUPT/SQLITE_NOTADB at the
 * one query this function is guaranteed to run, which is the earliest
 * point corruption can be observed after sqlite3_open_v2 (SQLite opens
 * lazily; the file header is not read until the first real access).
 */
static DbOpenState
init_schema(sqlite3 * conn)
{
  sqlite3_stmt * stmt = NULL;
  int rc = sqlite3_prepare_v2(conn, "SELECT value FROM meta WHERE key='schema_version'", -1, &stmt, NULL);

  if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
    return DB_STATE_CORRUPT;
  }

  if (rc == SQLITE_OK) {
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
      sqlite3_finalize(stmt);
      return DB_STATE_CORRUPT;
    }
    if (rc != SQLITE_ROW) {
      /* meta exists but carries no schema_version row -- not a shape
         apply_schema ever produces (it always seeds the row in the
         same transaction that creates the table). Treated as the same
         class of corruption as a bad header, so it self-heals the same
         way rather than needing a second error path. */
      sqlite3_finalize(stmt);
      return DB_STATE_CORRUPT;
    }

    const unsigned char * txt = sqlite3_column_text(stmt, 0);
    char *                end = NULL;
    long                  version = (txt != NULL) ? strtol((const char *)txt, &end, 10) : 0;
    int                   parsed = (txt != NULL) && (end != (const char *)txt) && (*end == '\0');
    sqlite3_finalize(stmt);

    /* A schema_version that is absent, unparsable, or below the first
       version this file ever wrote is not a version this build can
       migrate from -- it is a value apply_schema cannot have produced.
       Same class as the missing-row case above, so it routes to the
       same unlink-and-rebuild rather than dying with "cannot migrate"
       on a derived cache that self-heals (DESIGN.md XI.4). */
    if (!parsed || version < 1) {
      return DB_STATE_CORRUPT;
    }

    if (version > KURONO_SCHEMA_VERSION) {
      kurono_die("database written by a newer kurono (schema_version %ld, this build understands %d)",
                 version, KURONO_SCHEMA_VERSION);
    }
    if (version < KURONO_SCHEMA_VERSION) {
      return (migrate(conn, (int)version) == 0) ? DB_STATE_OK : DB_STATE_ERROR;
    }
    return DB_STATE_OK;
  }

  if (stmt != NULL) {
    sqlite3_finalize(stmt);
  }

  /* SQLITE_ERROR here means "no such table: meta" -- the only realistic
     cause at this exact query on a file SQLITE_OPEN_CREATE just made. */
  if (rc == SQLITE_ERROR) {
    return (apply_schema(conn) == 0) ? DB_STATE_OK : DB_STATE_ERROR;
  }

  return DB_STATE_ERROR;
} /* init_schema */

/*
 * cmp_str: qsort comparator over an array of `const char *`.
 */
static int
cmp_str(const void * a, const void * b)
{
  const char * const * sa = a;
  const char * const * sb = b;
  return strcmp(*sa, *sb);
} /* cmp_str */

/*
 * join_sorted_roots: the music_dirs meta value -- roots sorted, then
 * '\n'-joined (DESIGN.md VII.1, amendment 01). Returns an owned string,
 * or NULL on allocation failure. nroots == 0 yields "".
 */
static char *
join_sorted_roots(const char * const * roots, size_t nroots)
{
  if (nroots == 0) {
    return strdup("");
  }

  const char ** sorted = malloc(nroots * sizeof(*sorted));
  if (sorted == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < nroots; i++) {
    sorted[i] = roots[i];
  }
  qsort(sorted, nroots, sizeof(*sorted), cmp_str);

  size_t total = 1; /* NUL */
  for (size_t i = 0; i < nroots; i++) {
    total += strlen(sorted[i]) + 1; /* + separator or NUL */
  }

  char * out = malloc(total);
  if (out == NULL) {
    free(sorted);
    return NULL;
  }
  out[0] = '\0';
  for (size_t i = 0; i < nroots; i++) {
    strcat(out, sorted[i]);
    if (i + 1 < nroots) {
      strcat(out, "\n");
    }
  }

  free(sorted);
  return out;
} /* join_sorted_roots */

/*
 * reconcile_music_dirs: compare the resolved `roots` against the stored
 * `music_dirs` meta key (DESIGN.md VII.1, XI.4). A mismatch -- including
 * a pre-amendment database whose `music_dirs` key reads back NULL --
 * clears `tracks` and reports it via *out_rescanned so the caller runs a
 * full scan. Always rewrites `music_dirs` to the current joined value.
 * Runs before prepare_all_statements, so it uses the ad-hoc meta
 * helpers, not the prepared meta_get/meta_set. Returns 0 on success, -1
 * on allocation or database error.
 */
static int
reconcile_music_dirs(sqlite3 * conn, const char * const * roots, size_t nroots, int * out_rescanned)
{
  char * joined = join_sorted_roots(roots, nroots);
  if (joined == NULL) {
    return -1;
  }

  char * stored = meta_get_raw(conn, "music_dirs");
  int changed = (stored == NULL) || (strcmp(stored, joined) != 0);
  free(stored);

  if (changed) {
    if (sqlite3_exec(conn, "DELETE FROM tracks;", NULL, NULL, NULL) != SQLITE_OK) {
      free(joined);
      return -1;
    }
    if (out_rescanned != NULL) {
      *out_rescanned = 1;
    }
  }

  int rc = meta_set_raw(conn, "music_dirs", joined);
  free(joined);
  return rc;
} /* reconcile_music_dirs */

/*
 * prepare_all_statements: prepare every statement db.c ever executes
 * after this point, once, kept live for the handle's lifetime and
 * finalised in db_close. A failure here is unconditionally fatal via
 * kurono_die -- the schema this file just verified/applied should make
 * every one of these strings valid SQL, so a failure means a mismatch
 * between SQL_SCHEMA and the statements below, not a runtime condition.
 */
static void
prepare_all_statements(Db * db)
{
#define PREP(field, sql) \
  do { \
    if (sqlite3_prepare_v2(db->conn, (sql), -1, &db->field, NULL) != SQLITE_OK) { \
      kurono_die("failed to prepare statement %s: %s", #field, sqlite3_errmsg(db->conn)); \
    } \
  } while (0)

  PREP(meta_get, "SELECT value FROM meta WHERE key = ?1");
  PREP(meta_set, "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
  PREP(track_unchanged, "SELECT 1 FROM tracks WHERE path = ?1 AND mtime = ?2 AND size = ?3 LIMIT 1");
  PREP(track_touch, "UPDATE tracks SET scan_gen = ?1 WHERE path = ?2");
  PREP(track_upsert,
       "INSERT OR REPLACE INTO tracks "
       "(path, parent_dir, mtime, size, title, artist, album, album_artist, genre, "
       " year, track_no, disc_no, duration_ms, bitrate, sample_rate, channels, codec, scan_gen) "
       "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18)");
  PREP(tracks_sweep, "DELETE FROM tracks WHERE scan_gen < ?1");
  /* A prefix RANGE, not a pattern. The obvious form here is a GLOB
     against the parent joined with a slash and a star, and it is wrong:
     GLOB reads '[', '*' and '?' in the bound value as metacharacters,
     so a directory named
     "[2019] Album" becomes a character class and none of its children
     ever match -- whole subtrees vanish from the Directory view, and
     bracketed album folders are common. The range form has no
     metacharacters at all, and '0' is the byte immediately after '/',
     so it selects exactly the paths under ?1. It still uses
     idx_tracks_parent: SQLite plans this as a MULTI-INDEX OR over two
     covering-index lookups. */
  PREP(query_dirs, "SELECT DISTINCT parent_dir FROM tracks "
                    "WHERE parent_dir = ?1 "
                    "   OR (parent_dir > ?1 || '/' AND parent_dir < ?1 || '0') "
                    "ORDER BY parent_dir");
  PREP(query_artists, "SELECT DISTINCT artist FROM tracks WHERE artist IS NOT NULL ORDER BY artist");
  PREP(query_album_artists,
       "SELECT DISTINCT album_artist FROM tracks WHERE album_artist IS NOT NULL ORDER BY album_artist");
  PREP(query_albums_for, "SELECT DISTINCT album FROM tracks WHERE artist = ?1 AND album IS NOT NULL ORDER BY album");
  /* The album_artist twin of the line above. Both views ask "which
     albums belong to this name", but they mean different columns, and
     one statement cannot serve both: an Album Artists view filtered on
     `artist` misses every compilation, where album_artist is "Various
     Artists" and no track carries that as its artist. Covered by
     idx_tracks_album_artist's leading column, so DISTINCT needs no
     sort. */
  PREP(query_albums_for_album_artist,
       "SELECT DISTINCT album FROM tracks WHERE album_artist = ?1 AND album IS NOT NULL ORDER BY album");
  PREP(query_tracks_for_album,
       "SELECT * FROM tracks WHERE album_artist = ?1 AND album = ?2 ORDER BY disc_no, track_no");
  /* The artist twin, for the Artists view, which reaches an album
     through a track's `artist` rather than the album's `album_artist`.
     Filtering on album_artist there returns nothing for a compilation
     -- a silent empty column 3, which is why this exists as its own
     statement. idx_tracks_artist covers the lookup but not the order,
     so this one does pay a sort; at v1 scale that is an album's worth
     of rows. */
  PREP(query_tracks_for_artist_album,
       "SELECT * FROM tracks WHERE artist = ?1 AND album = ?2 ORDER BY disc_no, track_no");
  PREP(query_tracks_in_dir, "SELECT * FROM tracks WHERE parent_dir = ?1 ORDER BY path");
  /* The recursive twin, for enqueuing a directory row. Same prefix
     RANGE as query_dirs above and for the same reason -- a GLOB would
     read '[', '*' and '?' in the bound value as metacharacters, so
     "[2019] Album" would queue nothing. The equality arm is not
     redundant with the range arm: the range covers strict descendants
     only ('/' is excluded at both ends), so ?1's own tracks need it.
     Multi-disc albums are the case that makes this rather than
     query_tracks_in_dir the right statement for an enqueue: their
     tracks live in CD1/, CD2/ subdirectories and the non-recursive
     query returns only whatever loose files sit beside them. */
  PREP(query_tracks_under_dir, "SELECT * FROM tracks "
                                "WHERE parent_dir = ?1 "
                                "   OR (parent_dir > ?1 || '/' AND parent_dir < ?1 || '0') "
                                "ORDER BY path");
  PREP(query_track_by_id, "SELECT * FROM tracks WHERE id = ?1");
  PREP(query_search,
       "SELECT t.* FROM tracks_fts f JOIN tracks t ON t.id = f.rowid "
       "WHERE tracks_fts MATCH ?1 ORDER BY rank LIMIT 200");

#undef PREP
} /* prepare_all_statements */

/*
 * db_open: see include/library/db.h for the full contract. Retries once
 * (unlink, reopen, reapply schema) on SQLITE_CORRUPT/SQLITE_NOTADB;
 * every other failure is fatal via kurono_die and does not return.
 */
Db *
db_open(const char * db_path, const char * const * roots, size_t nroots, int * out_rescanned)
{
  if (out_rescanned != NULL) {
    *out_rescanned = 0;
  }

  Db * db = calloc(1, sizeof(*db));
  if (db == NULL) {
    kurono_die("out of memory opening database %s", db_path);
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    int rc = sqlite3_open_v2(db_path, &db->conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
      const char * msg = (db->conn != NULL) ? sqlite3_errmsg(db->conn) : "unknown error";
      kurono_die("cannot open database %s: %s", db_path, msg);
    }

    sqlite3_exec(db->conn, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db->conn, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db->conn, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);

    DbOpenState state = init_schema(db->conn);

    if (state == DB_STATE_CORRUPT) {
      sqlite3_close_v2(db->conn);
      db->conn = NULL;
      if (attempt == 0) {
        unlink(db_path);
        fprintf(stderr, "kurono: %s was corrupt, rebuilding\n", db_path);
        continue;
      }
      kurono_die("database %s is corrupt and could not be rebuilt", db_path);
    }

    if (state == DB_STATE_ERROR) {
      kurono_die("failed to initialise database %s: %s", db_path, sqlite3_errmsg(db->conn));
    }

    if (reconcile_music_dirs(db->conn, roots, nroots, out_rescanned) != 0) {
      kurono_die("failed to reconcile music_dirs in %s", db_path);
    }

    prepare_all_statements(db);
    return db;
  }

  kurono_die("failed to open database %s after rebuild", db_path);
  return NULL; /* unreached -- kurono_die is _Noreturn */
} /* db_open */

/*
 * db_close: see include/library/db.h for the contract.
 */
void
db_close(Db * db)
{
  if (db == NULL) {
    return;
  }

  sqlite3_finalize(db->meta_get);
  sqlite3_finalize(db->meta_set);
  sqlite3_finalize(db->track_unchanged);
  sqlite3_finalize(db->track_touch);
  sqlite3_finalize(db->track_upsert);
  sqlite3_finalize(db->tracks_sweep);
  sqlite3_finalize(db->query_dirs);
  sqlite3_finalize(db->query_artists);
  sqlite3_finalize(db->query_album_artists);
  sqlite3_finalize(db->query_albums_for);
  sqlite3_finalize(db->query_albums_for_album_artist);
  sqlite3_finalize(db->query_tracks_for_album);
  sqlite3_finalize(db->query_tracks_for_artist_album);
  sqlite3_finalize(db->query_tracks_in_dir);
  sqlite3_finalize(db->query_tracks_under_dir);
  sqlite3_finalize(db->query_track_by_id);
  sqlite3_finalize(db->query_search);

  sqlite3_close_v2(db->conn);
  free(db);
} /* db_close */

/*
 * db_begin / db_commit / db_rollback: see include/library/db.h.
 */
int
db_begin(Db * db)
{
  return (sqlite3_exec(db->conn, "BEGIN;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
} /* db_begin */

int
db_commit(Db * db)
{
  return (sqlite3_exec(db->conn, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
} /* db_commit */

int
db_rollback(Db * db)
{
  return (sqlite3_exec(db->conn, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK) ? 0 : -1;
} /* db_rollback */

/*
 * db_meta_get: see include/library/db.h.
 */
char *
db_meta_get(Db * db, const char * key)
{
  sqlite3_stmt * stmt = db->meta_get;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

  char * out = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = strdup_or_null((const char *)sqlite3_column_text(stmt, 0));
  }
  db_reset(db, stmt);
  return out;
} /* db_meta_get */

/*
 * db_meta_set: see include/library/db.h.
 */
int
db_meta_set(Db * db, const char * key, const char * value)
{
  sqlite3_stmt * stmt = db->meta_set;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  db_reset(db, stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
} /* db_meta_set */

/*
 * db_track_unchanged: see include/library/db.h.
 */
int
db_track_unchanged(Db * db, const char * path, int64_t mtime, int64_t size)
{
  sqlite3_stmt * stmt = db->track_unchanged;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, mtime);
  sqlite3_bind_int64(stmt, 3, size);

  int rc = sqlite3_step(stmt);
  db_reset(db, stmt);
  if (rc == SQLITE_ROW) {
    return 1;
  }
  if (rc == SQLITE_DONE) {
    return 0;
  }
  return -1;
} /* db_track_unchanged */

/*
 * db_track_touch: see include/library/db.h.
 */
int
db_track_touch(Db * db, const char * path, int64_t gen)
{
  sqlite3_stmt * stmt = db->track_touch;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_int64(stmt, 1, gen);
  sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);

  int rc = sqlite3_step(stmt);
  db_reset(db, stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
} /* db_track_touch */

/*
 * db_track_upsert: see include/library/db.h.
 */
int
db_track_upsert(Db * db, const DbTrackWrite * t)
{
  sqlite3_stmt * stmt = db->track_upsert;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);

  sqlite3_bind_text(stmt, 1, t->path, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, t->parent_dir, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, t->mtime);
  sqlite3_bind_int64(stmt, 4, t->size);
  bind_null_or_text(stmt, 5, t->title);
  bind_null_or_text(stmt, 6, t->artist);
  bind_null_or_text(stmt, 7, t->album);
  bind_null_or_text(stmt, 8, t->album_artist);
  bind_null_or_text(stmt, 9, t->genre);
  bind_null_or_int64(stmt, 10, t->year);
  bind_null_or_int64(stmt, 11, t->track_no);
  bind_null_or_int64(stmt, 12, t->disc_no);
  bind_null_or_int64(stmt, 13, t->duration_ms);
  bind_null_or_int64(stmt, 14, t->bitrate);
  bind_null_or_int64(stmt, 15, t->sample_rate);
  bind_null_or_int64(stmt, 16, t->channels);
  bind_null_or_text(stmt, 17, t->codec);
  sqlite3_bind_int64(stmt, 18, t->scan_gen);

  int rc = sqlite3_step(stmt);
  db_reset(db, stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
} /* db_track_upsert */

/*
 * db_tracks_sweep: see include/library/db.h.
 */
int
db_tracks_sweep(Db * db, int64_t gen)
{
  sqlite3_stmt * stmt = db->tracks_sweep;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_int64(stmt, 1, gen);

  int rc = sqlite3_step(stmt);
  db_reset(db, stmt);
  return (rc == SQLITE_DONE) ? 0 : -1;
} /* db_tracks_sweep */

/*
 * row_from_stmt: fill *row from the current row of a "SELECT *" against
 * `tracks` (column order == SQL_SCHEMA's declaration order). Owned
 * strings are allocated fresh; the caller frees with
 * db_track_row_free_fields.
 */
static void
row_from_stmt(sqlite3_stmt * stmt, DbTrackRow * row)
{
  memset(row, 0, sizeof(*row));

  row->id = sqlite3_column_int64(stmt, 0);
  row->path = strdup_or_null((const char *)sqlite3_column_text(stmt, 1));
  row->parent_dir = strdup_or_null((const char *)sqlite3_column_text(stmt, 2));
  row->mtime = sqlite3_column_int64(stmt, 3);
  row->size = sqlite3_column_int64(stmt, 4);
  row->title = strdup_or_null((const char *)sqlite3_column_text(stmt, 5));
  row->artist = strdup_or_null((const char *)sqlite3_column_text(stmt, 6));
  row->album = strdup_or_null((const char *)sqlite3_column_text(stmt, 7));
  row->album_artist = strdup_or_null((const char *)sqlite3_column_text(stmt, 8));
  row->genre = strdup_or_null((const char *)sqlite3_column_text(stmt, 9));

  if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
    row->has_year = 1;
    row->year = sqlite3_column_int64(stmt, 10);
  }
  if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
    row->has_track_no = 1;
    row->track_no = sqlite3_column_int64(stmt, 11);
  }
  if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
    row->has_disc_no = 1;
    row->disc_no = sqlite3_column_int64(stmt, 12);
  }
  if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) {
    row->has_duration_ms = 1;
    row->duration_ms = sqlite3_column_int64(stmt, 13);
  }
  if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
    row->has_bitrate = 1;
    row->bitrate = sqlite3_column_int64(stmt, 14);
  }
  if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) {
    row->has_sample_rate = 1;
    row->sample_rate = sqlite3_column_int64(stmt, 15);
  }
  if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) {
    row->has_channels = 1;
    row->channels = sqlite3_column_int64(stmt, 16);
  }
  row->codec = strdup_or_null((const char *)sqlite3_column_text(stmt, 17));
  row->scan_gen = sqlite3_column_int64(stmt, 18);
} /* row_from_stmt */

/*
 * rows_from_stmt: drain an already-bound, already-reset statement into
 * a growable DbTrackRow array. A mid-scan SQLITE_ERROR (a malformed FTS5
 * MATCH string is the one every caller here can hit) is reported as
 * zero rows rather than a return of -1, matching DESIGN.md V.6's "never
 * treated as fatal" for search and, by the same reasoning, every other
 * query in this file (DESIGN.md XI.7 -- a query path degrades, it does
 * not abort). An allocation failure while growing the array stops the
 * scan and returns whatever was collected so far, per the same policy.
 * Always resets `stmt` before returning. Always returns 0.
 */
static int
rows_from_stmt(Db * db, sqlite3_stmt * stmt, DbTrackRow ** out_rows, size_t * out_n)
{
  DbTrackRow * rows = NULL;
  size_t       n = 0, cap = 0;
  int          rc;

  db->truncated = 0;

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (n == cap) {
      size_t       new_cap = (cap == 0) ? 16 : cap * 2;
      DbTrackRow * grown = realloc(rows, new_cap * sizeof(*rows));
      if (grown == NULL) {
        db->truncated = 1;
        break;
      }
      rows = grown;
      cap = new_cap;
    }
    row_from_stmt(stmt, &rows[n]);
    n++;
  }
  (void)rc;

  db_reset(db, stmt);
  *out_rows = rows;
  *out_n = n;
  return 0;
} /* rows_from_stmt */

/*
 * names_from_stmt: drain an already-bound, already-reset single-column
 * statement into a growable array of owned strings. Same degrade
 * policy as rows_from_stmt. Always returns 0.
 */
static int
names_from_stmt(Db * db, sqlite3_stmt * stmt, char *** out_names, size_t * out_n)
{
  char ** names = NULL;
  size_t  n = 0, cap = 0;

  db->truncated = 0;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char * txt = (const char *)sqlite3_column_text(stmt, 0);
    if (txt == NULL) {
      continue;
    }
    if (n == cap) {
      size_t new_cap = (cap == 0) ? 16 : cap * 2;
      char ** grown = realloc(names, new_cap * sizeof(*names));
      if (grown == NULL) {
        db->truncated = 1;
        break;
      }
      names = grown;
      cap = new_cap;
    }
    char * copy = strdup(txt);
    if (copy == NULL) {
      db->truncated = 1;
      break;
    }
    names[n++] = copy;
  }

  db_reset(db, stmt);
  *out_names = names;
  *out_n = n;
  return 0;
} /* names_from_stmt */

/*
 * db_query_dirs: see include/library/db.h.
 */
int
db_query_dirs(Db * db, const char * parent, char *** out_names, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_dirs;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, parent, -1, SQLITE_TRANSIENT);
  return names_from_stmt(db, stmt, out_names, out_n);
} /* db_query_dirs */

/*
 * db_query_artists: see include/library/db.h.
 */
int
db_query_artists(Db * db, char *** out_names, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_artists;
  db_reset(db, stmt);
  return names_from_stmt(db, stmt, out_names, out_n);
} /* db_query_artists */

/*
 * db_query_album_artists: see include/library/db.h.
 */
int
db_query_album_artists(Db * db, char *** out_names, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_album_artists;
  db_reset(db, stmt);
  return names_from_stmt(db, stmt, out_names, out_n);
} /* db_query_album_artists */

/*
 * db_query_albums_for: see include/library/db.h.
 */
int
db_query_albums_for(Db * db, const char * artist, char *** out_names, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_albums_for;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, artist, -1, SQLITE_TRANSIENT);
  return names_from_stmt(db, stmt, out_names, out_n);
} /* db_query_albums_for */

/*
 * db_query_albums_for_album_artist: see include/library/db.h.
 */
int
db_query_albums_for_album_artist(Db * db, const char * album_artist,
                                  char *** out_names, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_albums_for_album_artist;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, album_artist, -1, SQLITE_TRANSIENT);
  return names_from_stmt(db, stmt, out_names, out_n);
} /* db_query_albums_for_album_artist */

/*
 * db_query_tracks_for_album: see include/library/db.h.
 */
int
db_query_tracks_for_album(Db * db, const char * album_artist, const char * album,
                           DbTrackRow ** out_rows, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_tracks_for_album;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, album_artist, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, album, -1, SQLITE_TRANSIENT);
  return rows_from_stmt(db, stmt, out_rows, out_n);
} /* db_query_tracks_for_album */

/*
 * db_query_tracks_for_artist_album: see include/library/db.h.
 */
int
db_query_tracks_for_artist_album(Db * db, const char * artist, const char * album,
                                  DbTrackRow ** out_rows, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_tracks_for_artist_album;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, artist, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, album, -1, SQLITE_TRANSIENT);
  return rows_from_stmt(db, stmt, out_rows, out_n);
} /* db_query_tracks_for_artist_album */

/*
 * db_query_tracks_in_dir: see include/library/db.h.
 */
int
db_query_tracks_in_dir(Db * db, const char * dir, DbTrackRow ** out_rows, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_tracks_in_dir;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, dir, -1, SQLITE_TRANSIENT);
  return rows_from_stmt(db, stmt, out_rows, out_n);
} /* db_query_tracks_in_dir */

/*
 * db_query_tracks_under_dir: see include/library/db.h.
 */
int
db_query_tracks_under_dir(Db * db, const char * dir, DbTrackRow ** out_rows, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_tracks_under_dir;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, dir, -1, SQLITE_TRANSIENT);
  return rows_from_stmt(db, stmt, out_rows, out_n);
} /* db_query_tracks_under_dir */

/*
 * db_query_track_by_id: see include/library/db.h.
 */
int
db_query_track_by_id(Db * db, int64_t id, DbTrackRow * out_row)
{
  sqlite3_stmt * stmt = db->query_track_by_id;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_int64(stmt, 1, id);

  int rc = sqlite3_step(stmt);
  int found = 0;
  if (rc == SQLITE_ROW) {
    row_from_stmt(stmt, out_row);
    found = 1;
  }
  db_reset(db, stmt);
  return found;
} /* db_query_track_by_id */

/*
 * db_query_search: see include/library/db.h.
 */
int
db_query_search(Db * db, const char * match_string, DbTrackRow ** out_rows, size_t * out_n)
{
  sqlite3_stmt * stmt = db->query_search;
  db_reset(db, stmt);
  sqlite3_clear_bindings(stmt);
  sqlite3_bind_text(stmt, 1, match_string, -1, SQLITE_TRANSIENT);
  return rows_from_stmt(db, stmt, out_rows, out_n);
} /* db_query_search */

/*
 * db_names_free: see include/library/db.h.
 */
void
db_names_free(char ** names, size_t n)
{
  if (names == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    free(names[i]);
  }
  free(names);
} /* db_names_free */

/*
 * db_track_row_free_fields: see include/library/db.h.
 */
void
db_track_row_free_fields(DbTrackRow * row)
{
  if (row == NULL) {
    return;
  }
  free(row->path);
  free(row->parent_dir);
  free(row->title);
  free(row->artist);
  free(row->album);
  free(row->album_artist);
  free(row->genre);
  free(row->codec);
} /* db_track_row_free_fields */

/*
 * db_track_rows_free: see include/library/db.h.
 */
void
db_track_rows_free(DbTrackRow * rows, size_t n)
{
  if (rows == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    db_track_row_free_fields(&rows[i]);
  }
  free(rows);
} /* db_track_rows_free */
