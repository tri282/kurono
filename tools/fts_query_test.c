/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/fts_query_test.c -- standalone test binary for
   src/library/query.c (DESIGN.md XII.2 testable tier,
   phase-15-query.md I.5).

   Both functions under test are pure and take no database handle
   (plans/kurono/02-decide/amendment-08-query-layer-scope.md), so the
   whole phase is testable without one, and the directory collapse rides
   along in the same binary as the MATCH builder at no extra cost.

   The second mode does need SQLite, and it is the point of the
   transform rather than a bonus: it feeds every generated MATCH string
   to a real FTS5 table and asserts the query does not return
   SQLITE_ERROR. That is the property query_build_match exists to
   guarantee, and nothing but a real FTS5 parser can check it. The
   database is :memory:, so the mode leaves nothing behind and needs no
   scratch path -- it runs by default, and --pure skips it. */

#include "../include/library/query.h"

#include "../include/library/db.h"

#include "../include/ui/browser.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

/*
 * kurono_die: test double for main.c's fatal path. Takes a printf
 * format and its arguments, never returns.
 *
 * db.o is linked here for test_prefix_range_precondition below, and
 * kurono_die is its only undefined symbol outside libc and sqlite3.
 * The real one lives in main.c, which would pull the entire program --
 * termbox2, the engine, ffmpeg -- into a test binary that needs none of
 * it. This is the first test double for a program symbol in this tree.
 *
 * What it does not cover: the real kurono_die restores the terminal
 * before printing, and nothing here claims a terminal, so the two
 * differ exactly where that difference cannot matter. No check below
 * reaches a db_open failure path -- an in-memory database is never
 * corrupt, never a newer schema version, and never a root mismatch --
 * so reaching this function at all means a check is wrong, and the
 * non-zero exit says so.
 */
_Noreturn void
kurono_die(const char * fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  fprintf(stderr, "kurono_die (test stub): ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
} /* kurono_die */

static int g_failures = 0;

#define CHECK(cond, ...)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                        \
      fprintf(stderr, "\n");                               \
      g_failures++;                                        \
    }                                                      \
  } while (0)

/* Every raw input the pure tests build a MATCH string from. Reused by
 * the SQLite mode, which is what makes "no input in this file can
 * produce a query error" a property of the whole set rather than of the
 * four documented pairs. */
static const char * const g_raw_inputs[] = {
  "bjork hom", "AND", "a\"b", "*",
  "", "   ", "\t\n ", "\"", "\"\"\"", "**", "* *",
  "OR", "NOT", "NEAR", "and or not near",
  "AND OR NOT NEAR bjork",
  "-x", "^x", "x:y", "(x)", "{x}", "x+y", "x,y",
  "Bj\xc3\xb6rk", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
  "\xc3\xa9\xc3\xa8 \xc3\xaa", "caf\xc3\xa9*", "\"quoted phrase\"",
  "NEAR(a b, 2)", "a AND b", "column:value", "rowid", "rank",
};
#define N_RAW_INPUTS (sizeof(g_raw_inputs) / sizeof(g_raw_inputs[0]))

/*
 * check_match: asserts query_build_match(raw) equals `want`. Takes the
 * raw input and the expected MATCH string; returns nothing, recording a
 * failure through CHECK. Frees what it built.
 */
static void check_match(const char * raw, const char * want)
{
  char * got = query_build_match(raw);

  if (got == NULL) {
    CHECK(0, "query_build_match(\"%s\") returned NULL", raw);
    return;
  }
  CHECK(strcmp(got, want) == 0,
        "query_build_match(\"%s\") = \"%s\", want \"%s\"", raw, got, want);
  free(got);
} /* check_match */

/*
 * test_match_documented: the four input/output pairs written into
 * phase-15-query.md I.2 and DESIGN.md V.6 step 3, exactly as documented.
 * Takes nothing, returns nothing.
 */
static void test_match_documented(void)
{
  check_match("bjork hom", "\"bjork\" \"hom\"*");
  check_match("AND",       "\"AND\"*");
  check_match("a\"b",      "\"a\"\"b\"*");
  check_match("*",         "\"*\"*");
} /* test_match_documented */

/*
 * test_match_edges: empty, whitespace-only, quote runs, bare keywords,
 * separator handling and NULL. Takes nothing, returns nothing.
 *
 * The tokenless cases return an owned empty string by contract
 * (query.h): FTS5 has no spelling of the empty query, so the caller
 * skips the search rather than being handed something that only looks
 * like one.
 */
static void test_match_edges(void)
{
  char * got;

  check_match("", "");
  check_match("   ", "");
  check_match("\t\n ", "");

  check_match("\"",    "\"\"\"\"*");
  check_match("\"\"\"", "\"\"\"\"\"\"\"\"*");

  check_match("OR",   "\"OR\"*");
  check_match("NOT",  "\"NOT\"*");
  check_match("NEAR", "\"NEAR\"*");

  /* Only the final token takes the prefix operator. */
  check_match("a b c", "\"a\" \"b\" \"c\"*");
  /* Runs of separators collapse; leading and trailing ones vanish. */
  check_match("  a \t b  ", "\"a\" \"b\"*");

  got = query_build_match(NULL);
  CHECK(got != NULL && got[0] == '\0', "NULL input must give an empty string");
  free(got);
} /* test_match_edges */

/*
 * test_match_big: a 4 KiB input of nothing but quote characters -- the
 * case where every byte doubles, so it is simultaneously the buffer
 * bound and the "no truncation mid-escape" check. Takes nothing,
 * returns nothing.
 */
static void test_match_big(void)
{
#define BIG 4096
  char * raw = malloc(BIG + 1);
  char * got;
  size_t i;

  if (raw == NULL) {
    CHECK(0, "out of memory building the 4 KiB input");
    return;
  }
  memset(raw, '"', BIG);
  raw[BIG] = '\0';

  got = query_build_match(raw);
  if (got == NULL) {
    CHECK(0, "query_build_match on 4 KiB returned NULL");
    free(raw);
    return;
  }

  /* One token: an opening quote, BIG doubled quotes, a closing quote,
   * then the prefix operator. Every quote in the body must be paired,
   * which is what a truncation mid-escape would break. */
  CHECK(strlen(got) == (size_t) (2 * BIG + 3),
        "4 KiB of quotes gave %zu bytes, want %d", strlen(got), 2 * BIG + 3);
  CHECK(got[0] == '"' && got[strlen(got) - 1] == '*',
        "4 KiB output is not a quoted token with a prefix operator");
  for (i = 1; i + 2 < strlen(got); i += 2) {
    if (got[i] != '"' || got[i + 1] != '"') {
      CHECK(0, "unpaired quote at offset %zu", i);
      break;
    }
  }

  free(got);
  free(raw);
#undef BIG
} /* test_match_big */

/*
 * test_match_utf8: multibyte input must never be split inside a
 * codepoint, and its bytes must survive unchanged. Takes nothing,
 * returns nothing.
 */
static void test_match_utf8(void)
{
  check_match("Bj\xc3\xb6rk", "\"Bj\xc3\xb6rk\"*");
  check_match("\xe6\x97\xa5\xe6\x9c\xac \xe8\xaa\x9e",
              "\"\xe6\x97\xa5\xe6\x9c\xac\" \"\xe8\xaa\x9e\"*");
  /* U+00A0 NO-BREAK SPACE is not an ASCII separator and must stay
   * inside the token: splitting on it would cut a codepoint in half. */
  check_match("a\xc2\xa0" "b", "\"a\xc2\xa0" "b\"*");
} /* test_match_utf8 */

/*
 * check_collapse: runs query_dirs_collapse over `paths` and asserts the
 * result equals the `want` list. Takes the parent, the input paths and
 * count, and the expected names and count; returns nothing.
 */
static void check_collapse(const char * parent,
                           const char * const * paths, size_t n,
                           const char * const * want, size_t want_n)
{
  char ** got = NULL;
  size_t  got_n = 0;
  size_t  i;
  int     rc = query_dirs_collapse(parent, (char * const *) paths, n,
                                   &got, &got_n);

  CHECK(rc == 0, "collapse(%s) returned %d, want 0", parent, rc);
  CHECK(got_n == want_n, "collapse(%s) gave %zu names, want %zu",
        parent, got_n, want_n);

  for (i = 0; i < got_n && i < want_n; i++) {
    CHECK(strcmp(got[i], want[i]) == 0,
          "collapse(%s)[%zu] = \"%s\", want \"%s\"", parent, i, got[i], want[i]);
  }

  for (i = 0; i < got_n; i++) {
    free(got[i]);
  }
  free(got);
} /* check_collapse */

/*
 * test_collapse: the seven cases phase-15-query.md I.1 names, plus the
 * order-independence pair. Takes nothing, returns nothing.
 */
static void test_collapse(void)
{
  static const char * const deep[] = {
    "/m/plain", "/m/plain/sub", "/m/plain/sub/deeper", "/m/plain/other",
  };
  static const char * const deep_want[] = { "other", "sub" };

  static const char * const shuffled[] = {
    "/m/plain/sub/deeper", "/m/plain/other", "/m/plain", "/m/plain/sub",
  };

  static const char * const prefix_trap[] = {
    "/m/plain", "/m/plainer", "/m/plain/sub",
  };
  static const char * const prefix_want[] = { "sub" };

  static const char * const at_root[] = { "/", "/m", "/m/plain", "/opt" };
  static const char * const at_root_want[] = { "m", "opt" };

  static const char * const unicode[] = {
    "/m/\xe6\x97\xa5/x", "/m/\xe6\x97\xa5",
  };
  static const char * const unicode_want[] = { "\xe6\x97\xa5" };

  static const char * const only_self[] = { "/m/plain" };

  char ** got = NULL;
  size_t  got_n = 0;

  /* Descendants at any depth contribute only their first segment, and
   * two descendants sharing that segment yield one name. */
  check_collapse("/m/plain", deep, 4, deep_want, 2);
  /* A trailing slash on the parent must not change the answer. */
  check_collapse("/m/plain/", deep, 4, deep_want, 2);
  /* Unsorted input gives the same sorted output. */
  check_collapse("/m/plain", shuffled, 4, deep_want, 2);
  /* "/m/plainer" merely shares a prefix and is not a descendant. */
  check_collapse("/m/plain", prefix_trap, 3, prefix_want, 1);
  /* The parent may be a root, and "/" is all separator. */
  check_collapse("/", at_root, 4, at_root_want, 2);
  /* Multibyte directory names survive whole. */
  check_collapse("/m", unicode, 2, unicode_want, 1);
  /* The parent itself contributes nothing... */
  check_collapse("/m/plain", only_self, 1, NULL, 0);
  /* ...and neither does an empty input. */
  check_collapse("/m/plain", NULL, 0, NULL, 0);

  /* An empty result hands back NULL rather than a zero-length
   * allocation, so a caller freeing unconditionally is safe. */
  query_dirs_collapse("/m/plain", (char * const *) only_self, 1, &got, &got_n);
  CHECK(got == NULL && got_n == 0, "an empty collapse must yield NULL");
} /* test_collapse */

/*
 * test_collapse_wide: 10k descendants under one parent -- the
 * O(descendants) cost phase 13 accepted when it put the collapse here.
 * Asserts the distinct first segments come back, and nothing about the
 * clock: the timing that phase-15-query.md III asks for is read off the
 * verify run, not asserted here. Takes nothing, returns nothing.
 */
static void test_collapse_wide(void)
{
#define WIDE 10000
#define WIDE_DISTINCT 100
  char ** paths = malloc(WIDE * sizeof(*paths));
  char ** got = NULL;
  size_t  got_n = 0;
  size_t  i;
  int     rc;

  if (paths == NULL) {
    CHECK(0, "out of memory building the wide input");
    return;
  }

  /* Each of WIDE_DISTINCT children gets WIDE/WIDE_DISTINCT descendants,
   * so the collapse has real de-duplication to do. */
  for (i = 0; i < WIDE; i++) {
    char buf[128];
    snprintf(buf, sizeof(buf), "/m/plain/child%03zu/n%05zu",
             i % WIDE_DISTINCT, i);
    paths[i] = strdup(buf);
    if (paths[i] == NULL) {
      CHECK(0, "out of memory at path %zu", i);
      while (i-- > 0) {
        free(paths[i]);
      }
      free(paths);
      return;
    }
  }

  rc = query_dirs_collapse("/m/plain", paths, WIDE, &got, &got_n);
  CHECK(rc == 0, "wide collapse returned %d, want 0", rc);
  CHECK(got_n == WIDE_DISTINCT, "wide collapse gave %zu names, want %d",
        got_n, WIDE_DISTINCT);
  if (got_n == WIDE_DISTINCT) {
    CHECK(strcmp(got[0], "child000") == 0 &&
          strcmp(got[WIDE_DISTINCT - 1], "child099") == 0,
          "wide collapse is not sorted over the distinct children");
  }

  for (i = 0; i < got_n; i++) {
    free(got[i]);
  }
  free(got);
  for (i = 0; i < WIDE; i++) {
    free(paths[i]);
  }
  free(paths);
#undef WIDE
#undef WIDE_DISTINCT
} /* test_collapse_wide */

/*
 * test_against_fts5: the property the transform exists for. Takes
 * nothing, returns nothing. Builds an in-memory FTS5 table with the
 * same tokenizer options the real schema uses, then runs every string
 * query_build_match produces from g_raw_inputs against it and asserts
 * the step does not fail. A raw input that is not transformed is
 * expected to fail, and one is checked that way so a transform that
 * silently became the identity function would be caught.
 */
static void test_against_fts5(void)
{
  sqlite3 * db = NULL;
  size_t    i;
  int       rc;
  char *    err = NULL;

  rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) {
    CHECK(0, "sqlite3_open(:memory:) failed: %s", sqlite3_errmsg(db));
    sqlite3_close(db);
    return;
  }

  rc = sqlite3_exec(db,
                    "CREATE VIRTUAL TABLE t USING fts5("
                    "  title, artist, album,"
                    "  tokenize = 'unicode61 remove_diacritics 2');"
                    "INSERT INTO t VALUES('Homogenic', 'Bj" "\xc3\xb6" "rk', 'Homogenic');",
                    NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    CHECK(0, "FTS5 table setup failed: %s", err ? err : "(no message)");
    sqlite3_free(err);
    sqlite3_close(db);
    return;
  }

  for (i = 0; i < N_RAW_INPUTS; i++) {
    sqlite3_stmt * st = NULL;
    char *         match = query_build_match(g_raw_inputs[i]);

    if (match == NULL) {
      CHECK(0, "query_build_match returned NULL for input %zu", i);
      continue;
    }
    /* The tokenless inputs are contractually not queries; the caller
     * skips them rather than binding them (query.h). */
    if (match[0] == '\0') {
      free(match);
      continue;
    }

    rc = sqlite3_prepare_v2(db, "SELECT rowid FROM t WHERE t MATCH ?1 ORDER BY rank",
                            -1, &st, NULL);
    if (rc != SQLITE_OK) {
      CHECK(0, "prepare failed: %s", sqlite3_errmsg(db));
      free(match);
      continue;
    }
    sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(st);
    CHECK(rc == SQLITE_ROW || rc == SQLITE_DONE,
          "MATCH %s (from \"%s\") failed: %s",
          match, g_raw_inputs[i], sqlite3_errmsg(db));

    sqlite3_finalize(st);
    free(match);
  }

  /* Diacritic folding, which is what makes a search for "bjork" find
   * "Bjork" with an umlaut -- the remove_diacritics 2 row of the verify
   * table, checked here rather than only against the real library. */
  {
    sqlite3_stmt * st = NULL;
    char *         match = query_build_match("bjork hom");

    if (match != NULL &&
        sqlite3_prepare_v2(db, "SELECT rowid FROM t WHERE t MATCH ?1",
                           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
      CHECK(sqlite3_step(st) == SQLITE_ROW,
            "\"bjork hom\" did not match Bj\xc3\xb6rk/Homogenic");
      sqlite3_finalize(st);
    }
    free(match);
  }

  /* The negative control: raw, untransformed input really is a query
   * error, so the pass above is the transform's doing. */
  {
    sqlite3_stmt * st = NULL;

    if (sqlite3_prepare_v2(db, "SELECT rowid FROM t WHERE t MATCH ?1",
                           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, "AND", -1, SQLITE_STATIC);
      CHECK(sqlite3_step(st) == SQLITE_ERROR,
            "raw \"AND\" was expected to be an FTS5 syntax error");
      sqlite3_finalize(st);
    }
  }

  sqlite3_close(db);
} /* test_against_fts5 */

/*
 * cmp_name_qsort: qsort(3) adapter over query_name_cmp for the listing
 * check below. Takes two `const char *` elements by address.
 */
static int cmp_name_qsort(const void * a, const void * b)
{
  const char * const * pa = a;
  const char * const * pb = b;
  return query_name_cmp(*pa, *pb);
} /* cmp_name_qsort */

/*
 * cmp_id_qsort: qsort(3) adapter ordering two `int64_t` track ids by
 * value. Takes two elements by address, returns the usual -1/0/1.
 */
static int cmp_id_qsort(const void * a, const void * b)
{
  int64_t x = *(const int64_t *) a;
  int64_t y = *(const int64_t *) b;
  return (x < y) ? -1 : ((x > y) ? 1 : 0);
} /* cmp_id_qsort */

/*
 * test_name_cmp: query_name_cmp is the display ordering for every list
 * of names the user reads, and two properties of it are load-bearing.
 *
 * The first is that case does not partition the list: plain strcmp put
 * every A-Z name above every a-z one, which is the bug this function
 * was introduced to fix (2026-08-21).
 *
 * The second is that it is still a *total* order. strcasecmp alone
 * calls "abc" and "ABC" equal; qsort(3) is not stable, so dropping the
 * strcmp tiebreak would let two names differing only in case swap
 * between runs over an unchanged directory. That failure is invisible
 * in a single run, which is why it is asserted here rather than left to
 * inspection. Takes nothing, returns nothing, reports through CHECK.
 */
static void test_name_cmp(void)
{
  /* Case must not partition: the interleaved order is the whole point. */
  CHECK(query_name_cmp("abba", "Zeppelin") < 0,
        "a lowercase name must sort before a later uppercase one");
  CHECK(query_name_cmp("Zeppelin", "abba") > 0,
        "query_name_cmp must be antisymmetric on that pair");
  CHECK(query_name_cmp("Beatles", "abba") > 0,
        "an uppercase B must sort after a lowercase a");

  /* Equal ignoring case, so the tiebreak decides -- and must decide
     consistently rather than returning 0. Deleting the tiebreak makes
     both of these fail. */
  CHECK(query_name_cmp("abc", "ABC") != 0,
        "names differing only in case must be totally ordered, not equal");
  CHECK((query_name_cmp("abc", "ABC") > 0) == (strcmp("abc", "ABC") > 0),
        "the tiebreak must be strcmp, so the order is deterministic");

  /* Identity, and the ordinary cases still hold. */
  CHECK(query_name_cmp("same", "same") == 0, "a name must equal itself");
  CHECK(query_name_cmp("a", "b") < 0, "plain lowercase ordering must hold");
  CHECK(query_name_cmp("A", "B") < 0, "plain uppercase ordering must hold");
  CHECK(query_name_cmp("abc", "abcd") < 0, "a prefix must sort before what extends it");

  /* Non-ASCII is untouched: strcasecmp in the C locale folds A-Z and
     nothing else, so UTF-8 stays in codepoint order. */
  CHECK(query_name_cmp("\xc3\xa9tude", "Zzz") > 0,
        "a leading UTF-8 byte must keep its codepoint order");

  /* The ordering an actual listing gets, sorted through the real
     comparator rather than asserted pairwise. */
  const char * want[] = { "abba", "Beatles", "Chopin", "dvorak", "Elgar" };
  const char * got[]  = { "Elgar", "abba", "Chopin", "Beatles", "dvorak" };
  size_t       n      = sizeof got / sizeof got[0];

  qsort((void *) got, n, sizeof got[0], cmp_name_qsort);
  for (size_t i = 0; i < n; i++) {
    CHECK(strcmp(got[i], want[i]) == 0,
          "sorted listing position %zu is %s, want %s", i, got[i], want[i]);
  }
} /* test_name_cmp */

/*
 * test_upsert_preserves_id: assertion for S1 (design.md 4A.1). A
 * rescanned track's `id` must survive an mtime/size-only update and a
 * tags-changed update alike -- otherwise anything holding the id (the
 * queue, jump-to-playing) orphans on the next rescan. Reads the id back
 * through db_query_tracks_in_dir/db_query_track_by_id rather than a
 * dedicated accessor. Takes nothing, returns nothing.
 */
static void test_upsert_preserves_id(void)
{
  const char * roots[] = { "/music" };
  Db *         db = NULL;
  DbTrackRow * rows = NULL;
  size_t       n = 0;
  DbTrackWrite t;
  int64_t      id = 0;

  db = db_open(":memory:", roots, 1, NULL);
  if (db == NULL) {
    CHECK(0, "db_open(:memory:) returned NULL, which its contract forbids");
    return;
  }

  memset(&t, 0, sizeof(t));
  t.path       = "/music/a.flac";
  t.parent_dir = "/music";
  t.mtime      = 1;
  t.size       = 100;
  t.title      = "Original Title";
  t.artist     = "Artist";
  t.scan_gen   = 1;
  CHECK(db_track_upsert(db, &t) == 0, "first upsert failed");

  CHECK(db_query_tracks_in_dir(db, "/music", &rows, &n) == 0,
        "db_query_tracks_in_dir returned non-zero");
  CHECK(n == 1, "expected 1 track after the first upsert, got %zu", n);
  if (n == 1) {
    id = rows[0].id;
  }
  db_track_rows_free(rows, n);
  rows = NULL;
  n    = 0;

  /* mtime/size change, tags identical -- must keep the id. */
  t.mtime = 2;
  t.size  = 200;
  CHECK(db_track_upsert(db, &t) == 0, "second upsert failed");

  DbTrackRow row2 = { 0 };
  CHECK(db_query_track_by_id(db, id, &row2) == 1,
        "id did not survive an mtime/size-only rescan");
  CHECK(row2.mtime == 2, "mtime was not updated by the second upsert");
  db_track_row_free_fields(&row2);

  /* Title changes too -- rules out "preserves id by refusing to update". */
  t.title = "New Title";
  CHECK(db_track_upsert(db, &t) == 0, "third upsert failed");

  DbTrackRow row3 = { 0 };
  CHECK(db_query_track_by_id(db, id, &row3) == 1,
        "id did not survive a tags-changed rescan");
  CHECK(row3.title != NULL && strcmp(row3.title, "New Title") == 0,
        "title was not updated by the third upsert");
  db_track_row_free_fields(&row3);

  /* F11 (decide D18): FTS must reflect the update too, not just the row
     -- a trigger this change stopped firing would show up as a stale
     filter index. Read through the All+FTS list statement. */
  char *       match = query_build_match("New Title");
  DbTrackRow * srows = NULL;
  size_t       sn = 0;
  CHECK(match != NULL, "query_build_match(\"New Title\") returned NULL");
  if (match != NULL) {
    CHECK(db_query_tracks_list(db, NULL, match, &srows, &sn) == 0,
          "db_query_tracks_list returned non-zero");
    CHECK(sn == 1 && srows != NULL && srows[0].id == id,
          "FTS index was not updated to the new title");
    db_track_rows_free(srows, sn);
    free(match);
  }

  db_close(db);
} /* test_upsert_preserves_id */

/*
 * test_prefix_range_precondition: the documented precondition on the
 * two prefix-range statements (v1.0.1 #9). Takes nothing, returns
 * nothing.
 *
 * db_query_dirs and db_query_tracks_under_dir both select with
 * `parent_dir = ?1 OR (parent_dir > ?1 || '/' AND parent_dir < ?1 ||
 * '0')`. That range is correct for a parent written without a trailing
 * slash and is silently empty for two spellings nothing in the tree
 * produces: a trailing slash ("/m/"), because the comparison then
 * builds "/m//" and no stored path has the doubled separator; and bare
 * "/", because it builds "//" and "/0" and every absolute path sorts
 * below the first.
 *
 * Zero rows, not an error -- which is why this is a check and not a
 * comment. The never-fatal-query policy (DESIGN.md XI.7) means a
 * caller cannot tell the empty answer from the wrong question, so the
 * only thing that can hold the line is an assertion that the two
 * spellings really do behave as documented. If someone "fixes" the
 * statement to accept them, these rows flip and say so.
 *
 * This asserts the precondition, it does not endorse it: the fix ruled
 * for v1.0.1 is documentation plus this check, not normalisation
 * inside db.c, which would cost an allocation per query for a spelling
 * no caller produces (SCOPE.md #9).
 */
static void test_prefix_range_precondition(void)
{
  const char * roots[] = { "/m" };
  Db *         db = NULL;
  char **      names = NULL;
  size_t       n = 0;
  DbTrackRow * rows = NULL;
  size_t       nrows = 0;
  DbTrackWrite t;

  db = db_open(":memory:", roots, 1, NULL);
  if (db == NULL) {
    CHECK(0, "db_open(:memory:) returned NULL, which its contract forbids");
    return;
  }

  /* Two rows, one directly under the root and one nested, so a correct
     query returns both parent_dir values and a broken one cannot pass
     by returning the single row it happened to match. */
  memset(&t, 0, sizeof(t));
  t.path       = "/m/a.flac";
  t.parent_dir = "/m";
  t.scan_gen   = 1;
  CHECK(db_track_upsert(db, &t) == 0, "upsert /m/a.flac failed");

  memset(&t, 0, sizeof(t));
  t.path       = "/m/sub/b.flac";
  t.parent_dir = "/m/sub";
  t.scan_gen   = 1;
  CHECK(db_track_upsert(db, &t) == 0, "upsert /m/sub/b.flac failed");

  /* The spelling every caller produces: rows come back. */
  CHECK(db_query_dirs(db, "/m", &names, &n) == 0, "db_query_dirs(\"/m\") returned non-zero");
  CHECK(n == 2, "db_query_dirs(\"/m\"): expected 2 parent_dir values, got %zu", n);
  db_names_free(names, n);
  names = NULL;
  n     = 0;

  /* Trailing slash: silently empty. */
  CHECK(db_query_dirs(db, "/m/", &names, &n) == 0, "db_query_dirs(\"/m/\") returned non-zero");
  CHECK(n == 0,
        "db_query_dirs(\"/m/\"): precondition says zero rows, got %zu -- if the "
        "statement was changed to accept a trailing slash, update db.h and "
        "delete this check rather than loosening it",
        n);
  db_names_free(names, n);
  names = NULL;
  n     = 0;

  /* Bare root: silently empty. */
  CHECK(db_query_dirs(db, "/", &names, &n) == 0, "db_query_dirs(\"/\") returned non-zero");
  CHECK(n == 0, "db_query_dirs(\"/\"): precondition says zero rows, got %zu", n);
  db_names_free(names, n);
  names = NULL;
  n     = 0;

  /* The sibling statement carries the byte-identical range shape, and
     so the identical precondition. Checked here because a fix applied
     to one and not the other is the failure this pair exists to catch. */
  CHECK(db_query_tracks_under_dir(db, "/m", &rows, &nrows) == 0,
        "db_query_tracks_under_dir(\"/m\") returned non-zero");
  CHECK(nrows == 2, "db_query_tracks_under_dir(\"/m\"): expected 2 tracks, got %zu", nrows);
  db_track_rows_free(rows, nrows);
  rows  = NULL;
  nrows = 0;

  CHECK(db_query_tracks_under_dir(db, "/m/", &rows, &nrows) == 0,
        "db_query_tracks_under_dir(\"/m/\") returned non-zero");
  CHECK(nrows == 0,
        "db_query_tracks_under_dir(\"/m/\"): precondition says zero rows, got %zu",
        nrows);
  db_track_rows_free(rows, nrows);
  rows  = NULL;
  nrows = 0;

  CHECK(db_query_tracks_under_dir(db, "/", &rows, &nrows) == 0,
        "db_query_tracks_under_dir(\"/\") returned non-zero");
  CHECK(nrows == 0,
        "db_query_tracks_under_dir(\"/\"): precondition says zero rows, got %zu",
        nrows);
  db_track_rows_free(rows, nrows);

  db_close(db);
} /* test_prefix_range_precondition */

/* Assertions for #10 (design.md 4.4, decide R5-R7): the inner-LIMIT
   subquery, the fts5 prefix index, and the schema_version 1 -> 2
   migration. The fixture is 20k rows with varying field lengths so a
   one-character prefix matches most rows and a two-token query still
   produces distinct bm25 scores (R7). */

#define SEARCH_FIXTURE_N 20000

static const char * const g_fixture_words[] = {
  "a", "an", "aria", "alto", "amber", "anthem", "arpeggio", "autumnal",
  "album", "azure", "the", "song", "sound", "silence", "symphony",
  "stanza", "blue", "black", "bright", "night", "day", "dream", "echo",
  "elegy", "ember", "one", "two", "three", "four", "love", "hope",
  "wind", "storm",
};
#define FIXTURE_NWORDS (sizeof(g_fixture_words) / sizeof(g_fixture_words[0]))

/* A handful of rare two-word combinations for F3 (test_list_compose) to
   search among for one matching fewer than 200 of the fixture's rows. */
static const char * const g_selective_candidates[] = {
  "storm hope", "wind love", "ember dream", "elegy stanza",
  "arpeggio autumnal", "azure amber", "aria alto", "anthem symphony",
  "silence echo", "four three",
};

/*
 * fixture_word: a deterministic, triangularly-skewed pick from
 * g_fixture_words for row `i`, field slot `slot` -- rank 0 ("a") is
 * FIXTURE_NWORDS times as likely as the last rank, giving both a broad
 * one-character prefix match and a long tail of rare terms.
 */
static const char *
fixture_word(size_t i, unsigned slot)
{
  unsigned n = (unsigned) FIXTURE_NWORDS;
  unsigned sum = n * (n + 1) / 2;
  unsigned h = ((unsigned) i * 2654435761u) ^ (slot * 40503u + 7u);
  unsigned pool = h % sum;
  unsigned rank, acc = 0;

  for (rank = 0; rank < n; rank++) {
    acc += n - rank;
    if (pool < acc) {
      return g_fixture_words[rank];
    }
  }
  return g_fixture_words[n - 1];
} /* fixture_word */

/*
 * fixture_field: join `nwords` words from fixture_word(i, slot..) with
 * spaces into `buf`. Varying `nwords` per call is what gives the
 * fixture's fields varying lengths.
 */
static void
fixture_field(char * buf, size_t bufsize, size_t i, unsigned slot, unsigned nwords)
{
  unsigned w;

  buf[0] = '\0';
  for (w = 0; w < nwords; w++) {
    if (w > 0) {
      strncat(buf, " ", bufsize - strlen(buf) - 1);
    }
    strncat(buf, fixture_word(i, slot + w), bufsize - strlen(buf) - 1);
  }
} /* fixture_field */

/*
 * build_raw_fixture: an in-memory tracks/tracks_fts pair, schema
 * identical to the current SQL_TRACKS_FTS_DDL (prefix included), with
 * `n` fixture rows inserted through the tracks_ai trigger. Used where a
 * raw sqlite3 * is needed for a statement db.h has no accessor for.
 * Returns 0 and sets *out_conn on success, -1 on error.
 */
static int
build_raw_fixture(sqlite3 ** out_conn, size_t n)
{
  sqlite3 *      conn = NULL;
  sqlite3_stmt * ins = NULL;
  char *         err = NULL;
  size_t         i;

  if (sqlite3_open(":memory:", &conn) != SQLITE_OK) {
    if (conn != NULL) {
      sqlite3_close(conn);
    }
    return -1;
  }

  if (sqlite3_exec(conn,
      "CREATE TABLE tracks (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, "
      "title TEXT, artist TEXT, album TEXT, album_artist TEXT, "
      "year INTEGER, disc_no INTEGER, track_no INTEGER);"
      "CREATE VIRTUAL TABLE tracks_fts USING fts5("
      "  title, artist, album, album_artist,"
      "  content='tracks', content_rowid='id',"
      "  tokenize='unicode61 remove_diacritics 2',"
      "  prefix='1 2 3');"
      "CREATE TRIGGER tracks_ai AFTER INSERT ON tracks BEGIN"
      "  INSERT INTO tracks_fts(rowid, title, artist, album, album_artist)"
      "    VALUES (new.id, new.title, new.artist, new.album, new.album_artist);"
      "END;",
      NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "build_raw_fixture: schema failed: %s\n", err != NULL ? err : "?");
    sqlite3_free(err);
    sqlite3_close(conn);
    return -1;
  }

  if (sqlite3_exec(conn, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_close(conn);
    return -1;
  }
  if (sqlite3_prepare_v2(conn,
      "INSERT INTO tracks(path, title, artist, album, album_artist) VALUES (?1,?2,?3,?4,?5)",
      -1, &ins, NULL) != SQLITE_OK) {
    sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_close(conn);
    return -1;
  }

  for (i = 0; i < n; i++) {
    char path[64], title[128], artist[64], album[64], album_artist[64];

    snprintf(path, sizeof(path), "/music/track%05zu.flac", i);
    fixture_field(title, sizeof(title), i, 0, 1 + (unsigned) (i % 4));
    fixture_field(artist, sizeof(artist), i, 10, 1 + (unsigned) ((i / 4) % 3));
    fixture_field(album, sizeof(album), i, 20, 1 + (unsigned) ((i / 7) % 3));
    fixture_field(album_artist, sizeof(album_artist), i, 30, 1 + (unsigned) ((i / 11) % 2));

    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    sqlite3_bind_text(ins, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 5, album_artist, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins) != SQLITE_DONE) {
      sqlite3_finalize(ins);
      sqlite3_exec(conn, "ROLLBACK;", NULL, NULL, NULL);
      sqlite3_close(conn);
      return -1;
    }
  }
  sqlite3_finalize(ins);

  if (sqlite3_exec(conn, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_close(conn);
    return -1;
  }

  *out_conn = conn;
  return 0;
} /* build_raw_fixture */

/*
 * build_db_fixture: the same `n` rows as build_raw_fixture, inserted
 * through db_track_upsert on a real Db handle -- the production code
 * path, exercised so db_query_tracks_list is checked as the caller actually
 * calls it, not just as raw SQL. Same path/field generation as
 * build_raw_fixture, so a fresh sequential id in one lines up with the
 * other's rowid. Returns 0 and sets *out_db on success, -1 on error.
 */
static int
build_db_fixture(Db ** out_db, size_t n)
{
  const char * roots[] = { "/music" };
  Db *         db = db_open(":memory:", roots, 1, NULL);
  size_t       i;

  if (db == NULL) {
    return -1;
  }
  if (db_begin(db) != 0) {
    db_close(db);
    return -1;
  }

  for (i = 0; i < n; i++) {
    char         path[64], title[128], artist[64], album[64], album_artist[64];
    DbTrackWrite t;

    snprintf(path, sizeof(path), "/music/track%05zu.flac", i);
    fixture_field(title, sizeof(title), i, 0, 1 + (unsigned) (i % 4));
    fixture_field(artist, sizeof(artist), i, 10, 1 + (unsigned) ((i / 4) % 3));
    fixture_field(album, sizeof(album), i, 20, 1 + (unsigned) ((i / 7) % 3));
    fixture_field(album_artist, sizeof(album_artist), i, 30, 1 + (unsigned) ((i / 11) % 2));

    memset(&t, 0, sizeof(t));
    t.path = path;
    t.parent_dir = "/music";
    t.mtime = (int64_t) i;
    t.size = 1000 + (int64_t) i;
    t.title = title;
    t.artist = artist;
    t.album = album;
    t.album_artist = album_artist;
    t.scan_gen = 1;

    if (db_track_upsert(db, &t) != 0) {
      db_rollback(db);
      db_close(db);
      return -1;
    }
  }

  if (db_commit(db) != 0) {
    db_close(db);
    return -1;
  }

  *out_db = db;
  return 0;
} /* build_db_fixture */

static sqlite3 * g_search_fixture_raw = NULL;
static Db *      g_search_fixture_db = NULL;

/*
 * search_fixture_raw / search_fixture_db: lazily build, then reuse, the
 * shared 20k-row fixture across the #10 assertions below -- each is
 * read-only against it. Return NULL on build failure.
 */
static sqlite3 *
search_fixture_raw(void)
{
  if (g_search_fixture_raw == NULL) {
    build_raw_fixture(&g_search_fixture_raw, SEARCH_FIXTURE_N);
  }
  return g_search_fixture_raw;
} /* search_fixture_raw */

static Db *
search_fixture_db(void)
{
  if (g_search_fixture_db == NULL) {
    build_db_fixture(&g_search_fixture_db, SEARCH_FIXTURE_N);
  }
  return g_search_fixture_db;
} /* search_fixture_db */

/*
 * search_fixture_cleanup: release the shared fixture. Called once from
 * main after every #10 assertion has run.
 */
static void
search_fixture_cleanup(void)
{
  if (g_search_fixture_raw != NULL) {
    sqlite3_close(g_search_fixture_raw);
    g_search_fixture_raw = NULL;
  }
  if (g_search_fixture_db != NULL) {
    db_close(g_search_fixture_db);
    g_search_fixture_db = NULL;
  }
} /* search_fixture_cleanup */

/*
 * test_fts_prefix_index: assertion 4 (decide R7). A fresh database's
 * tracks_fts carries prefix='1 2 3' -- checked by reopening the file
 * db_open just created with a raw connection, since db.h exposes no
 * generic SQL accessor and none is added for a test's convenience.
 */
static void
test_fts_prefix_index(void)
{
  char         path[] = "/tmp/kurono_fts_prefix_XXXXXX";
  int          fd = mkstemp(path);
  const char * roots[] = { "/music" };
  Db *         db;
  sqlite3 *    raw = NULL;

  if (fd < 0) {
    CHECK(0, "mkstemp failed for the prefix-index fixture");
    return;
  }
  close(fd);
  unlink(path); /* db_open must create the file itself */

  db = db_open(path, roots, 1, NULL);
  CHECK(db != NULL, "db_open(%s) returned NULL", path);
  if (db != NULL) {
    db_close(db);
  }

  CHECK(sqlite3_open_v2(path, &raw, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK,
        "failed to reopen %s read-only", path);
  if (raw != NULL) {
    sqlite3_stmt * st = NULL;

    if (sqlite3_prepare_v2(raw, "SELECT sql FROM sqlite_master WHERE name = 'tracks_fts'",
                            -1, &st, NULL) == SQLITE_OK) {
      CHECK(sqlite3_step(st) == SQLITE_ROW, "tracks_fts not found in sqlite_master");
      if (st != NULL) {
        const char * ddl = (const char *) sqlite3_column_text(st, 0);
        CHECK(ddl != NULL && strstr(ddl, "prefix='1 2 3'") != NULL,
              "tracks_fts DDL missing prefix='1 2 3': %s", ddl != NULL ? ddl : "(null)");
      }
      sqlite3_finalize(st);
    } else {
      CHECK(0, "failed to prepare the sqlite_master query");
    }
    sqlite3_close(raw);
  }

  unlink(path);
} /* test_fts_prefix_index */

/*
 * test_schema_migrate: assertion 5 (decide R7/R8). A version-1 database
 * (tracks_fts recreated by hand without prefix, schema_version forced
 * back to '1') is opened through the real db_open/migrate path and must
 * come out at schema_version 2, with prefix='1 2 3' in the DDL and the
 * original track's id intact -- the id check is what tells a migration
 * apart from an unlink-and-rebuild, which would also produce correct
 * DDL but a fresh id. Runs on a real temp file (mkstemp), the one test
 * in this binary that cannot be :memory:.
 */
static void
test_schema_migrate(void)
{
  char         path[] = "/tmp/kurono_schema_migrate_XXXXXX";
  int          fd = mkstemp(path);
  const char * roots[] = { "/music" };
  int64_t      id = 0;
  Db *         db;
  DbTrackWrite t;
  DbTrackRow * rows = NULL;
  size_t       n = 0;

  if (fd < 0) {
    CHECK(0, "mkstemp failed for the migration fixture");
    return;
  }
  close(fd);
  unlink(path);

  /* A fresh (version-2) database, then downgraded in place to look like
     a version-1 database migrate has never seen. */
  db = db_open(path, roots, 1, NULL);
  CHECK(db != NULL, "db_open(%s) returned NULL", path);
  if (db == NULL) {
    unlink(path);
    return;
  }

  memset(&t, 0, sizeof(t));
  t.path = "/music/a.flac";
  t.parent_dir = "/music";
  t.mtime = 1;
  t.size = 100;
  t.title = "Migrate Me";
  t.scan_gen = 1;
  CHECK(db_track_upsert(db, &t) == 0, "upsert into the fixture database failed");

  CHECK(db_query_tracks_in_dir(db, "/music", &rows, &n) == 0, "db_query_tracks_in_dir failed");
  CHECK(n == 1, "expected 1 track before downgrade, got %zu", n);
  if (n == 1) {
    id = rows[0].id;
  }
  db_track_rows_free(rows, n);
  db_close(db);

  /* Downgrade: recreate tracks_fts without prefix (undoing phase 6's
     schema) and force schema_version back to '1' -- what a real
     v1.0.1-era file looks like. */
  {
    sqlite3 * raw = NULL;

    CHECK(sqlite3_open(path, &raw) == SQLITE_OK, "failed to reopen %s for downgrade", path);
    if (raw != NULL) {
      char * err = NULL;
      int    rc = sqlite3_exec(raw,
          "DROP TABLE tracks_fts;"
          "CREATE VIRTUAL TABLE tracks_fts USING fts5("
          "  title, artist, album, album_artist,"
          "  content='tracks', content_rowid='id',"
          "  tokenize='unicode61 remove_diacritics 2');"
          "INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild');"
          "UPDATE meta SET value = '1' WHERE key = 'schema_version';",
          NULL, NULL, &err);
      CHECK(rc == SQLITE_OK, "downgrade failed: %s", err != NULL ? err : "?");
      sqlite3_free(err);
      sqlite3_close(raw);
    }
  }

  /* The real path: db_open must run migrate and land on schema_version 2. */
  db = db_open(path, roots, 1, NULL);
  CHECK(db != NULL, "db_open(%s) returned NULL after downgrade", path);
  if (db != NULL) {
    char *      version = db_meta_get(db, "schema_version");
    DbTrackRow  row = { 0 };

    CHECK(version != NULL && strcmp(version, "2") == 0,
          "schema_version after migration is %s, want 2", version != NULL ? version : "(null)");
    free(version);

    CHECK(db_query_track_by_id(db, id, &row) == 1,
          "track id %lld did not survive the migration -- a rebuild, not a migration",
          (long long) id);
    db_track_row_free_fields(&row);

    db_close(db);
  }

  {
    sqlite3 * raw2 = NULL;

    CHECK(sqlite3_open_v2(path, &raw2, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK,
          "failed to reopen %s to check the DDL", path);
    if (raw2 != NULL) {
      sqlite3_stmt * st = NULL;

      if (sqlite3_prepare_v2(raw2, "SELECT sql FROM sqlite_master WHERE name = 'tracks_fts'",
                              -1, &st, NULL) == SQLITE_OK) {
        CHECK(sqlite3_step(st) == SQLITE_ROW, "tracks_fts not found after migration");
        if (st != NULL) {
          const char * ddl = (const char *) sqlite3_column_text(st, 0);
          CHECK(ddl != NULL && strstr(ddl, "prefix='1 2 3'") != NULL,
                "post-migration tracks_fts DDL missing prefix='1 2 3': %s",
                ddl != NULL ? ddl : "(null)");
        }
        sqlite3_finalize(st);
      }
      sqlite3_close(raw2);
    }
  }

  unlink(path);
} /* test_schema_migrate */

/* ---------------------------------------------------------------------
 * Library listing (design IV.2-IV.3, XI.2 F1-F8, decisions Q2).
 * ------------------------------------------------------------------- */

/* The order key <K> as db.c's LIST_ORDER spells it; F5 builds raw
   statements from it. design IV.2. */
#define T_LIST_ORDER \
  " ORDER BY artist COLLATE NOCASE, artist, year, album COLLATE NOCASE, " \
  "album, disc_no, track_no, path"

/* Header text pieces (design IV.3), spelled as bytes so the file stays ASCII. */
#define T_SEP_ARTIST " \xe2\x80\x94 "
#define T_SEP_YEAR   " \xc2\xb7 "

/*
 * cmp_nocase_null: ASCII-NOCASE compare, NULL first (SQLite ascending).
 * Takes two nullable strings, returns <0, 0 or >0.
 */
static int
cmp_nocase_null(const char * a, const char * b)
{
  if (a == NULL || b == NULL) {
    return (a == b) ? 0 : (a == NULL ? -1 : 1);
  }
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    int y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (x != y) {
      return (x < y) ? -1 : 1;
    }
  }
  return (*a == *b) ? 0 : ((*a == '\0') ? -1 : 1);
} /* cmp_nocase_null */

/*
 * cmp_bin_null: strcmp with NULL first. Takes two nullable strings.
 */
static int
cmp_bin_null(const char * a, const char * b)
{
  if (a == NULL || b == NULL) {
    return (a == b) ? 0 : (a == NULL ? -1 : 1);
  }
  return strcmp(a, b);
} /* cmp_bin_null */

/*
 * cmp_int_null: numeric compare, NULL (has == 0) first.
 */
static int
cmp_int_null(int ha, int64_t a, int hb, int64_t b)
{
  if (!ha || !hb) {
    return (ha == hb) ? 0 : (!ha ? -1 : 1);
  }
  return (a < b) ? -1 : ((a > b) ? 1 : 0);
} /* cmp_int_null */

/*
 * list_cmp: design IV.2 <K>; the SQL order in db.c is the other half.
 * Per name term NULL first, then ASCII-NOCASE, then strcmp; integers
 * NULL first then numeric; path strcmp. List rows carry a path only when
 * untitled (Q3), so the path term is skipped (0) when either is NULL.
 * Takes two rows, returns <0, 0, >0.
 */
static int
list_cmp(const DbTrackRow * a, const DbTrackRow * b)
{
  int c;

  if ((c = cmp_nocase_null(a->artist, b->artist)) != 0) return c;
  if ((c = cmp_bin_null(a->artist, b->artist)) != 0) return c;
  if ((c = cmp_int_null(a->has_year, a->year, b->has_year, b->year)) != 0) return c;
  if ((c = cmp_nocase_null(a->album, b->album)) != 0) return c;
  if ((c = cmp_bin_null(a->album, b->album)) != 0) return c;
  if ((c = cmp_int_null(a->has_disc_no, a->disc_no, b->has_disc_no, b->disc_no)) != 0) return c;
  if ((c = cmp_int_null(a->has_track_no, a->track_no, b->has_track_no, b->track_no)) != 0) return c;
  if (a->path == NULL || b->path == NULL) return 0;
  return strcmp(a->path, b->path);
} /* list_cmp */

/*
 * check_list_sorted: assert `rows` is strictly increasing under
 * list_cmp (equal is also a failure, except where both rows are titled
 * and so carry no path to break the tie, Q3). Takes a label
 * for the message and the rows.
 */
static void
check_list_sorted(const char * what, const DbTrackRow * rows, size_t n)
{
  for (size_t i = 1; i < n; i++) {
    int c = list_cmp(&rows[i - 1], &rows[i]);
    if (c > 0 || (c == 0 && (rows[i].path != NULL || rows[i - 1].path != NULL))) {
      CHECK(0, "%s: row %zu (id %" PRId64 ") does not follow row %zu (id %" PRId64 ") under <K>",
            what, i, rows[i].id, i - 1, rows[i - 1].id);
      return;
    }
  }
} /* check_list_sorted */

/*
 * gf_add: upsert one group-fixture row. Takes the Db, the path, parent
 * directory, artist, album and title (NULL for an absent tag), year
 * (-1 absent) and disc and track numbers (-1 absent); returns 0 or -1.
 */
static int
gf_add(Db * db, const char * path, const char * parent, const char * artist,
       const char * album, const char * title, int year, int disc, int track)
{
  static int64_t seq = 0;
  int64_t        y = year, d = disc, t = track;
  DbTrackWrite   w;

  memset(&w, 0, sizeof w);
  w.path = path;
  w.parent_dir = parent;
  w.mtime = ++seq;
  w.size = 1000 + seq;
  w.title = title;
  w.artist = artist;
  w.album = album;
  w.year = (year >= 0) ? &y : NULL;
  w.disc_no = (disc >= 0) ? &d : NULL;
  w.track_no = (track >= 0) ? &t : NULL;
  w.scan_gen = 1;
  return db_track_upsert(db, &w);
} /* gf_add */

/*
 * build_group_fixture: a small in-memory Db for the grouping checks: one
 * artist with two albums whose year order differs from their name order,
 * a two-disc album, an album-less track, case variants of one artist
 * (abc, ABC, Abc) sharing an album name and inserted so that path order
 * interleaves them, and one row with every tag NULL
 * (/music/untagged/x.flac). Returns 0 and sets *out_db, or -1.
 */
static int
build_group_fixture(Db ** out_db)
{
  const char * roots[] = { "/music" };
  Db *         db = db_open(":memory:", roots, 1, NULL);
  int          rc = 0;

  if (db == NULL) {
    return -1;
  }

  rc |= gf_add(db, "/music/beta/s1.flac", "/music/beta", "Beta", "Second", "S One", 2001, 1, 1);
  rc |= gf_add(db, "/music/beta/s2.flac", "/music/beta", "Beta", "Second", "S Two", 2001, 1, 2);
  rc |= gf_add(db, "/music/beta/f1.flac", "/music/beta", "Beta", "First", "F One", 1999, 1, 1);
  rc |= gf_add(db, "/music/beta/f2.flac", "/music/beta", "Beta", "First", "F Two", 1999, 1, 2);
  rc |= gf_add(db, "/music/beta/f3.flac", "/music/beta", "Beta", "First", "F Three", 1999, 1, 3);
  rc |= gf_add(db, "/music/beta/loose.flac", "/music/beta", "Beta", NULL, "Loose", -1, -1, -1);
  rc |= gf_add(db, "/music/gamma/d1t1.flac", "/music/gamma", "Gamma", "Two Discs", "D1 T1", 2005, 1, 1);
  rc |= gf_add(db, "/music/gamma/d1t2.flac", "/music/gamma", "Gamma", "Two Discs", "D1 T2", 2005, 1, 2);
  rc |= gf_add(db, "/music/gamma/d2t1.flac", "/music/gamma", "Gamma", "Two Discs", "D2 T1", 2005, 2, 1);
  rc |= gf_add(db, "/music/gamma/d2t2.flac", "/music/gamma", "Gamma", "Two Discs", "D2 T2", 2005, 2, 2);
  rc |= gf_add(db, "/music/alpha/a1.flac", "/music/alpha", "Alpha", "Aardvark", "A One", 1990, 1, 1);
  /* Same track number and year across the variants: only the BINARY
     tiebreak on `artist` keeps each variant's rows together. */
  rc |= gf_add(db, "/music/case/t1-a.flac", "/music/case", "abc", "Same", "Same 1", 2000, 1, 1);
  rc |= gf_add(db, "/music/case/t1-b.flac", "/music/case", "ABC", "Same", "Same 1", 2000, 1, 1);
  rc |= gf_add(db, "/music/case/t1-c.flac", "/music/case", "Abc", "Same", "Same 1", 2000, 1, 1);
  rc |= gf_add(db, "/music/case/t2-a.flac", "/music/case", "abc", "Same", "Same 2", 2000, 1, 2);
  rc |= gf_add(db, "/music/case/t2-b.flac", "/music/case", "ABC", "Same", "Same 2", 2000, 1, 2);
  rc |= gf_add(db, "/music/case/t2-c.flac", "/music/case", "Abc", "Same", "Same 2", 2000, 1, 2);
  rc |= gf_add(db, "/music/untagged/x.flac", "/music/untagged", NULL, NULL, NULL, -1, -1, -1);

  if (rc != 0) {
    db_close(db);
    return -1;
  }
  *out_db = db;
  return 0;
} /* build_group_fixture */

static Db * g_group_fixture_db = NULL;

/*
 * group_fixture_db: lazily build, then reuse, the group fixture.
 * Returns NULL on build failure.
 */
static Db *
group_fixture_db(void)
{
  if (g_group_fixture_db == NULL) {
    build_group_fixture(&g_group_fixture_db);
  }
  return g_group_fixture_db;
} /* group_fixture_db */

/*
 * ids_sorted: the ids of `rows`, ascending, in a fresh array. Takes the
 * rows; returns the array (NULL when n == 0 or on allocation failure).
 */
static int64_t *
ids_sorted(const DbTrackRow * rows, size_t n)
{
  int64_t * ids = (n > 0) ? malloc(n * sizeof *ids) : NULL;

  for (size_t i = 0; ids != NULL && i < n; i++) {
    ids[i] = rows[i].id;
  }
  if (ids != NULL) {
    qsort(ids, n, sizeof *ids, cmp_id_qsort);
  }
  return ids;
} /* ids_sorted */

/*
 * raw_fts_ids: every rowid matching `match` in the raw fixture, ascending,
 * uncapped. Takes the connection, the match and an out-param for a fresh
 * array; returns the count.
 */
static size_t
raw_fts_ids(sqlite3 * raw, const char * match, int64_t ** out)
{
  sqlite3_stmt * st = NULL;
  int64_t *      ids = NULL;
  size_t         n = 0, cap = 0;

  if (sqlite3_prepare_v2(raw,
        "SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?1 ORDER BY rowid",
        -1, &st, NULL) != SQLITE_OK) {
    *out = NULL;
    return 0;
  }
  sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (n == cap) {
      int64_t * grown = realloc(ids, (cap ? cap * 2 : 64) * sizeof *ids);
      if (grown == NULL) {
        break;
      }
      ids = grown;
      cap = cap ? cap * 2 : 64;
    }
    ids[n++] = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  *out = ids;
  return n;
} /* raw_fts_ids */

/*
 * raw_artist_of: strdup of the artist of track `id` in the raw fixture,
 * or NULL.
 */
static char *
raw_artist_of(sqlite3 * raw, int64_t id)
{
  sqlite3_stmt * st = NULL;
  char *         out = NULL;

  if (sqlite3_prepare_v2(raw, "SELECT artist FROM tracks WHERE id = ?1", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL) {
      out = strdup((const char *) sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
  }
  return out;
} /* raw_artist_of */

/*
 * test_list_order: F1. Each of the four list statements over the 20k
 * fixture, and All (plus one case-variant artist) over the group
 * fixture, is strictly increasing under list_cmp.
 */
static void
test_list_order(void)
{
  Db *         db = search_fixture_db();
  Db *         gdb = group_fixture_db();
  char **      names = NULL;
  size_t       nn = 0;
  char *       match = query_build_match("a");
  DbTrackRow * rows = NULL;
  size_t       n = 0;

  if (db == NULL || gdb == NULL || match == NULL) {
    CHECK(0, "fixtures unavailable for F1");
    free(match);
    return;
  }

  db_query_artists(db, &names, &nn);
  CHECK(nn > 0, "20k fixture has no artists");
  if (nn > 0) {
    const char * artist = names[0];

    db_query_tracks_list(db, NULL, NULL, &rows, &n);
    CHECK(n == SEARCH_FIXTURE_N, "All listing has %zu rows, want %d", n, SEARCH_FIXTURE_N);
    check_list_sorted("F1 All", rows, n);
    db_track_rows_free(rows, n);

    db_query_tracks_list(db, artist, NULL, &rows, &n);
    CHECK(n > 0, "F1 artist listing is empty");
    check_list_sorted("F1 artist", rows, n);
    db_track_rows_free(rows, n);

    db_query_tracks_list(db, NULL, match, &rows, &n);
    CHECK(n > 0, "F1 All+FTS listing is empty");
    check_list_sorted("F1 All+FTS", rows, n);
    db_track_rows_free(rows, n);

    db_query_tracks_list(db, artist, match, &rows, &n);
    CHECK(n > 0, "F1 artist+FTS listing is empty");
    check_list_sorted("F1 artist+FTS", rows, n);
    db_track_rows_free(rows, n);
  }
  db_names_free(names, nn);

  db_query_tracks_list(gdb, NULL, NULL, &rows, &n);
  CHECK(n == 18, "group fixture All listing has %zu rows, want 18", n);
  check_list_sorted("F1 group All", rows, n);
  db_track_rows_free(rows, n);

  db_query_tracks_list(gdb, "Abc", NULL, &rows, &n);
  CHECK(n == 2, "group fixture artist \"Abc\" has %zu rows, want 2", n);
  check_list_sorted("F1 group artist", rows, n);
  db_track_rows_free(rows, n);

  free(match);
} /* test_list_order */

/*
 * count_artist_in_all: rows of `db`'s All listing whose artist equals
 * `artist` byte for byte; the independent oracle for the group fixture.
 */
static size_t
count_artist_in_all(Db * db, const char * artist)
{
  DbTrackRow * rows = NULL;
  size_t       n = 0, hits = 0;

  db_query_tracks_list(db, NULL, NULL, &rows, &n);
  for (size_t i = 0; i < n; i++) {
    if (rows[i].artist != NULL && strcmp(rows[i].artist, artist) == 0) {
      hits++;
    }
  }
  db_track_rows_free(rows, n);
  return hits;
} /* count_artist_in_all */

/*
 * test_list_artist: F2. The artist-only count equals a raw
 * SELECT count(*) ... WHERE artist = ? for three 20k-fixture artists,
 * and equals an exact-match count for the three case variants in the
 * group fixture (the 20k fixture is lower-case, so a case-insensitive
 * match could not fail there).
 */
static void
test_list_artist(void)
{
  Db *           db = search_fixture_db();
  Db *           gdb = group_fixture_db();
  sqlite3 *      raw = search_fixture_raw();
  char **        names = NULL;
  size_t         nn = 0;
  const char *   variants[] = { "abc", "ABC", "Abc" };

  if (db == NULL || gdb == NULL || raw == NULL) {
    CHECK(0, "fixtures unavailable for F2");
    return;
  }

  db_query_artists(db, &names, &nn);
  CHECK(nn >= 3, "20k fixture has %zu artists, want >= 3", nn);
  for (size_t k = 0; k < 3 && nn >= 3; k++) {
    const char *   artist = names[(k * (nn - 1)) / 2];
    sqlite3_stmt * st = NULL;
    int64_t        want = -1;
    DbTrackRow *   rows = NULL;
    size_t         n = 0;

    if (sqlite3_prepare_v2(raw, "SELECT count(*) FROM tracks WHERE artist = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, artist, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) {
        want = sqlite3_column_int64(st, 0);
      }
      sqlite3_finalize(st);
    }
    db_query_tracks_list(db, artist, NULL, &rows, &n);
    CHECK((int64_t) n == want && want > 0,
          "artist \"%s\": list returned %zu rows, raw count %lld", artist, n, (long long) want);
    db_track_rows_free(rows, n);
  }
  db_names_free(names, nn);

  for (size_t k = 0; k < 3; k++) {
    DbTrackRow * rows = NULL;
    size_t       n = 0;
    size_t       want = count_artist_in_all(gdb, variants[k]);

    db_query_tracks_list(gdb, variants[k], NULL, &rows, &n);
    CHECK(want == 2 && n == want, "artist \"%s\": list returned %zu rows, want %zu (2)",
          variants[k], n, want);
    db_track_rows_free(rows, n);
  }
} /* test_list_artist */

/*
 * test_list_compose: F3. For an artist and a selective query, the
 * artist+FTS id set equals the artist-only ids intersected with the raw,
 * uncapped FTS ids. The artist is taken from the first match, and the
 * candidate is accepted only if the match set spans more than that
 * artist, so dropping the artist term cannot go unseen.
 */
static void
test_list_compose(void)
{
  Db *      db = search_fixture_db();
  sqlite3 * raw = search_fixture_raw();
  int       done = 0;

  if (db == NULL || raw == NULL) {
    CHECK(0, "fixtures unavailable for F3");
    return;
  }

  for (size_t ci = 0; ci < sizeof g_selective_candidates / sizeof g_selective_candidates[0] && !done; ci++) {
    char *       match = query_build_match(g_selective_candidates[ci]);
    int64_t *    raw_ids = NULL;
    size_t       nraw = (match != NULL) ? raw_fts_ids(raw, match, &raw_ids) : 0;
    char *       artist = (nraw > 0) ? raw_artist_of(raw, raw_ids[0]) : NULL;
    DbTrackRow * arows = NULL, * grows = NULL;
    size_t       an = 0, gn = 0;
    int64_t *    a_ids = NULL, * g_ids = NULL, * inter = NULL;
    size_t       ninter = 0;

    if (artist != NULL) {
      db_query_tracks_list(db, artist, NULL, &arows, &an);
      a_ids = ids_sorted(arows, an);
      inter = malloc(nraw * sizeof *inter);
      for (size_t i = 0, j = 0; inter != NULL && a_ids != NULL && i < nraw && j < an; ) {
        if (raw_ids[i] == a_ids[j]) {
          inter[ninter++] = raw_ids[i];
          i++;
          j++;
        } else if (raw_ids[i] < a_ids[j]) {
          i++;
        } else {
          j++;
        }
      }
      if (inter != NULL && ninter > 0 && ninter < nraw && ninter < DB_MATCH_CAP) {
        done = 1;
        db_query_tracks_list(db, artist, match, &grows, &gn);
        g_ids = ids_sorted(grows, gn);
        CHECK(gn == ninter, "artist \"%s\" + \"%s\": %zu rows, want %zu",
              artist, g_selective_candidates[ci], gn, ninter);
        for (size_t i = 0; g_ids != NULL && i < gn && i < ninter; i++) {
          CHECK(g_ids[i] == inter[i], "compose id %zu: got %lld, want %lld",
                i, (long long) g_ids[i], (long long) inter[i]);
        }
      }
    }
    db_track_rows_free(arows, an);
    db_track_rows_free(grows, gn);
    free(a_ids);
    free(g_ids);
    free(inter);
    free(artist);
    free(raw_ids);
    free(match);
  }
  CHECK(done, "no selective candidate gave a usable artist/FTS intersection");
} /* test_list_compose */

/*
 * test_list_cap: F4. FTS-filtered listings return at most DB_MATCH_CAP
 * rows; All+FTS "a" returns exactly DB_MATCH_CAP, and they are the
 * lowest-rowid matches (port of test_search_cap's "which 200").
 */
static void
test_list_cap(void)
{
  Db *         db = search_fixture_db();
  sqlite3 *    raw = search_fixture_raw();
  char *       match = query_build_match("a");
  char **      names = NULL;
  size_t       nn = 0;
  DbTrackRow * rows = NULL;
  size_t       n = 0;

  if (db == NULL || raw == NULL || match == NULL) {
    CHECK(0, "fixtures unavailable for F4");
    free(match);
    return;
  }

  db_query_artists(db, &names, &nn);
  if (nn > 0) {
    db_query_tracks_list(db, names[0], match, &rows, &n);
    CHECK(n <= DB_MATCH_CAP, "artist+FTS returned %zu rows, cap %d", n, DB_MATCH_CAP);
    db_track_rows_free(rows, n);
  }
  db_names_free(names, nn);

  db_query_tracks_list(db, NULL, match, &rows, &n);
  CHECK(n == DB_MATCH_CAP, "All+FTS \"a\" returned %zu rows, want %d", n, DB_MATCH_CAP);
  if (n == DB_MATCH_CAP) {
    int64_t * got = ids_sorted(rows, n);
    int64_t * all = NULL;
    size_t    nall = raw_fts_ids(raw, match, &all);

    CHECK(nall > DB_MATCH_CAP, "query \"a\"* matched %zu rows, want > %d -- fixture drifted",
          nall, DB_MATCH_CAP);
    for (size_t i = 0; got != NULL && all != NULL && i < n && i < nall; i++) {
      CHECK(got[i] == all[i],
            "cap row %zu: got id %lld, want %lld -- the rows are not the lowest-rowid "
            "matches, so the LIMIT is not inside the subquery",
            i, (long long) got[i], (long long) all[i]);
    }
    free(got);
    free(all);
  }
  db_track_rows_free(rows, n);
  free(match);
} /* test_list_cap */

/*
 * time_stmt_best: best-of-five wall time of draining `sql` with ?1 bound
 * to `match`. Returns seconds, or -1 on a prepare failure.
 */
static double
time_stmt_best(sqlite3 * conn, const char * sql, const char * match)
{
  sqlite3_stmt * st = NULL;
  double         best = -1.0;

  if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) {
    return -1.0;
  }
  for (int rep = 0; rep < 5; rep++) {
    struct timespec t0, t1;
    double          dt;

    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, match, -1, SQLITE_TRANSIENT);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (sqlite3_step(st) == SQLITE_ROW) { }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dt = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (best < 0.0 || dt < best) {
      best = dt;
    }
  }
  sqlite3_finalize(st);
  return best;
} /* time_stmt_best */

/*
 * time_list_best: best-of-five wall time of db_query_tracks_list for one
 * shape on the Db fixture. Returns seconds.
 */
static double
time_list_best(Db * db, const char * artist, const char * match)
{
  double best = -1.0;

  for (int rep = 0; rep < 5; rep++) {
    struct timespec t0, t1;
    DbTrackRow *    rows = NULL;
    size_t          n = 0;
    double          dt;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    db_query_tracks_list(db, artist, match, &rows, &n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    db_track_rows_free(rows, n);
    dt = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (best < 0.0 || dt < best) {
      best = dt;
    }
  }
  return best;
} /* time_list_best */

/*
 * test_list_latency: F5. On the raw fixture, the All+FTS text with the
 * LIMIT inside the subquery against the same text with the LIMIT moved
 * outside; best of five, ratio >= 2.0 (port of test_search_latency).
 * Also prints best-of-five times for the four list shapes on the Db
 * fixture.
 */
static void
test_list_latency(void)
{
  sqlite3 *    raw = search_fixture_raw();
  Db *         db = search_fixture_db();
  char *       match = query_build_match("a");
  char **      names = NULL;
  size_t       nn = 0;
  double       fast, slow, ratio;

  if (raw == NULL || db == NULL || match == NULL) {
    CHECK(0, "fixtures unavailable for F5");
    free(match);
    return;
  }

  fast = time_stmt_best(raw,
      "SELECT t.* FROM tracks t JOIN (SELECT rowid AS rid FROM tracks_fts "
      "WHERE tracks_fts MATCH ?1 LIMIT 200) x ON t.id = x.rid" T_LIST_ORDER, match);
  slow = time_stmt_best(raw,
      "SELECT t.* FROM tracks t JOIN (SELECT rowid AS rid FROM tracks_fts "
      "WHERE tracks_fts MATCH ?1) x ON t.id = x.rid" T_LIST_ORDER " LIMIT 200", match);
  CHECK(fast >= 0.0 && slow >= 0.0, "failed to prepare the F5 statements");

  ratio = (fast > 0.0) ? slow / fast : 0.0;
  printf("fts_query_test: list latency outer-limit=%.3fms inner-limit=%.3fms ratio=%.2fx\n",
         slow * 1000.0, fast * 1000.0, ratio);

  /* If this fails on CI with no db.c change, demote to a printed figure
     -- do not re-tune the gate upward (decide R7). */
  CHECK(ratio >= 2.0, "list latency ratio %.2fx, want >= 2.0x (outer=%.3fms inner=%.3fms)",
        ratio, slow * 1000.0, fast * 1000.0);

  db_query_artists(db, &names, &nn);
  if (nn > 0) {
    printf("fts_query_test: list shapes @%d rows: all=%.3fms artist=%.3fms "
           "all+fts=%.3fms artist+fts=%.3fms\n", SEARCH_FIXTURE_N,
           time_list_best(db, NULL, NULL) * 1000.0,
           time_list_best(db, names[0], NULL) * 1000.0,
           time_list_best(db, NULL, match) * 1000.0,
           time_list_best(db, names[0], match) * 1000.0);
  }
  db_names_free(names, nn);
  free(match);
} /* test_list_latency */

/*
 * test_list_path_tiebreak: <K>'s last term is the real path even though
 * a titled list row carries none (Q3). Two titled rows equal on every
 * other key, inserted so that rowid order is the reverse of path order;
 * paths come from db_query_track_by_id, since the list rows lack them.
 */
static void
test_list_path_tiebreak(void)
{
  const char * roots[] = { "/music" };
  Db *         db = db_open(":memory:", roots, 1, NULL);
  DbTrackRow * rows = NULL;
  size_t       n = 0;

  CHECK(db != NULL, "tiebreak: db_open failed");
  if (db == NULL) {
    return;
  }
  CHECK(gf_add(db, "/music/dup/z.flac", "/music/dup", "Dup", "Twin", "Same", 2000, 1, 1) == 0 &&
        gf_add(db, "/music/dup/a.flac", "/music/dup", "Dup", "Twin", "Same", 2000, 1, 1) == 0,
        "tiebreak: fixture insert failed");

  db_query_tracks_list(db, NULL, NULL, &rows, &n);
  CHECK(n == 2, "tiebreak: All has %zu rows, want 2", n);
  if (n == 2) {
    DbTrackRow first, second;
    int        ok = db_query_track_by_id(db, rows[0].id, &first) == 1 &&
                    db_query_track_by_id(db, rows[1].id, &second) == 1;
    CHECK(ok, "tiebreak: track lookup by id failed");
    if (ok) {
      CHECK(strcmp(first.path, "/music/dup/a.flac") == 0 && strcmp(second.path, "/music/dup/z.flac") == 0,
            "tiebreak: rows tied on every other key are not in path order (%s, %s)",
            first.path, second.path);
      db_track_row_free_fields(&first);
      db_track_row_free_fields(&second);
    }
  }
  db_track_rows_free(rows, n);
  db_close(db);
} /* test_list_path_tiebreak */

/*
 * plan_mentions: 1 if EXPLAIN QUERY PLAN of `sql` on `raw` has a detail
 * line containing `needle`, else 0.
 */
static int
plan_mentions(sqlite3 * raw, const char * sql, const char * needle)
{
  sqlite3_stmt * stmt = NULL;
  char           q[512];
  int            found = 0;

  snprintf(q, sizeof q, "EXPLAIN QUERY PLAN %s", sql);
  if (sqlite3_prepare_v2(raw, q, -1, &stmt, NULL) != SQLITE_OK) {
    return 0;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char * detail = (const char *) sqlite3_column_text(stmt, 3);
    if (detail != NULL && strstr(detail, needle) != NULL) {
      found = 1;
    }
  }
  sqlite3_finalize(stmt);
  return found;
} /* plan_mentions */

/*
 * test_list_index: Q3 rung 3. db_open creates idx_tracks_list on an
 * existing database that lacks it, and the All order walks it with no
 * temporary sort.
 */
static void
test_list_index(void)
{
  char         path[] = "/tmp/kurono_list_index_XXXXXX";
  int          fd = mkstemp(path);
  const char * roots[] = { "/music" };
  const char * all = "SELECT * FROM tracks" T_LIST_ORDER;
  sqlite3 *    raw = NULL;
  Db *         db;

  if (fd < 0) {
    CHECK(0, "mkstemp failed for the list index fixture");
    return;
  }
  close(fd);
  unlink(path);

  db = db_open(path, roots, 1, NULL);
  CHECK(db != NULL, "db_open(%s) returned NULL", path);
  if (db == NULL) {
    unlink(path);
    return;
  }
  db_close(db);

  /* Drop it, as a cache written before v2.0.0 would lack it. */
  if (sqlite3_open(path, &raw) == SQLITE_OK) {
    CHECK(sqlite3_exec(raw, "DROP INDEX idx_tracks_list;", NULL, NULL, NULL) == SQLITE_OK,
          "a fresh database has no idx_tracks_list");
  }
  sqlite3_close(raw);
  raw = NULL;

  db = db_open(path, roots, 1, NULL);
  CHECK(db != NULL, "reopen of %s returned NULL", path);
  if (db != NULL) {
    db_close(db);
  }

  if (sqlite3_open(path, &raw) == SQLITE_OK) {
    CHECK(plan_mentions(raw, all, "idx_tracks_list"), "the All order does not use idx_tracks_list after reopen");
    CHECK(!plan_mentions(raw, all, "TEMP B-TREE"), "the All order still sorts in a temporary B-tree");
  }
  sqlite3_close(raw);
  unlink(path);
} /* test_list_index */

/*
 * test_list_untagged: F6 and Q2. The all-NULL row is in the All listing
 * and in no artist's listing, and it is the first row of the All listing
 * (decisions Q2: NULL sorts first).
 */
static void
test_list_untagged(void)
{
  Db *         gdb = group_fixture_db();
  DbTrackRow * rows = NULL;
  size_t       n = 0;
  int64_t      id = 0;
  char **      names = NULL;
  size_t       nn = 0;

  if (gdb == NULL) {
    CHECK(0, "group fixture unavailable for F6");
    return;
  }

  db_query_tracks_list(gdb, NULL, NULL, &rows, &n);
  for (size_t i = 0; i < n; i++) {
    if (rows[i].path != NULL && strcmp(rows[i].path, "/music/untagged/x.flac") == 0) {
      id = rows[i].id;
    }
  }
  CHECK(id != 0, "F6: the untagged row is missing from the All listing");
  CHECK(n > 0 && rows[0].id == id,
        "Q2: the first row of All is id %" PRId64 ", want the untagged row (id %" PRId64 ")",
        n > 0 ? rows[0].id : 0, id);
  /* Q3 projection contract: path only for untitled rows, other fields unloaded. */
  for (size_t i = 0; i < n; i++) {
    CHECK((rows[i].title == NULL) == (rows[i].path != NULL),
          "Q3: list row id %" PRId64 " title %s but path %s", rows[i].id,
          rows[i].title ? "set" : "NULL", rows[i].path ? "set" : "NULL");
    CHECK(rows[i].parent_dir == NULL && rows[i].album_artist == NULL
          && rows[i].genre == NULL && rows[i].codec == NULL,
          "Q3: list row id %" PRId64 " carries a non-projected text field", rows[i].id);
  }
  db_track_rows_free(rows, n);

  db_query_artists(gdb, &names, &nn);
  CHECK(nn > 0, "group fixture has no artists");
  for (size_t a = 0; a < nn; a++) {
    db_query_tracks_list(gdb, names[a], NULL, &rows, &n);
    for (size_t i = 0; i < n; i++) {
      CHECK(rows[i].id != id, "F6: the untagged row is listed under artist \"%s\"", names[a]);
    }
    db_track_rows_free(rows, n);
  }
  db_names_free(names, nn);
} /* test_list_untagged */

/*
 * test_name_contains: F7. Pure. ASCII letters fold, bytes >= 0x80 match
 * exactly, empty needle matches all, NULL hay matches only an empty needle.
 */
static void
test_name_contains(void)
{
  CHECK(query_name_contains("Aphex Twin", "twin") == 1, "Aphex Twin / twin");
  CHECK(query_name_contains("AC/DC", "c/d") == 1, "AC/DC / c/d");
  CHECK(query_name_contains("Bj\xc3\xb6rk", "BJ\xc3\x96RK") == 0, "Bj\xc3\xb6rk / BJ\xc3\x96RK folded a non-ASCII letter");
  CHECK(query_name_contains("Bj\xc3\xb6rk", "Bj\xc3\xb6") == 1, "Bj\xc3\xb6rk / Bj\xc3\xb6");
  CHECK(query_name_contains("x", "") == 1, "x / empty");
  CHECK(query_name_contains(NULL, "") == 1, "NULL / empty");
  CHECK(query_name_contains(NULL, "a") == 0, "NULL / a");
  CHECK(query_name_contains("abc", "abcd") == 0, "abc / abcd");
} /* test_name_contains */

/*
 * header_text: the label design IV.3 gives the header of a run starting
 * at `t`, spelled independently of browser.c. Takes the row, whether the
 * listing is artist-scoped and an out buffer.
 */
static void
header_text(const DbTrackRow * t, int scoped, char * buf, size_t cap)
{
  size_t w = 0;

  buf[0] = '\0';
  if (!scoped) {
    w += (size_t) snprintf(buf + w, cap - w, "%s" T_SEP_ARTIST,
                           t->artist != NULL ? t->artist : "(unknown artist)");
  }
  w += (size_t) snprintf(buf + w, cap - w, "%s", t->album != NULL ? t->album : "(no album)");
  if (t->has_year) {
    snprintf(buf + w, cap - w, T_SEP_YEAR "%lld", (long long) t->year);
  }
} /* header_text */

/*
 * check_listing: assert the grouping invariants of one LIB_KEY_LIST
 * listing: row 0 is a header, a header precedes each (artist, album) run
 * and only there, is_dir is set on headers and on nothing else, every
 * header is followed by a track, and each header reads as header_text
 * says. Takes the Db, the source, the rows, whether the listing is
 * artist-scoped and a label; returns the header count.
 */
static size_t
check_listing(Db * db, const BrowserSource * src, const BrowserRow * rows, size_t n,
              int scoped, const char * what)
{
  DbTrackRow prev;
  int        have_prev = 0;
  size_t     headers = 0;

  memset(&prev, 0, sizeof prev);
  CHECK(n > 0 && rows[0].is_dir, "%s: row 0 is not a header", what);

  for (size_t i = 0; i < n; i++) {
    int is_header = (rows[i].key[0] == 'H');

    CHECK(rows[i].is_dir == is_header, "%s: row %zu (%s) is_dir=%d, header=%d",
          what, i, rows[i].name, rows[i].is_dir, is_header);
    if (is_header) {
      headers++;
      CHECK(i + 1 < n && rows[i + 1].key[0] == 'I', "%s: header %zu is not followed by a track", what, i);
      continue;
    }

    DbTrackRow cur;
    char       want[1200];
    int        after_header = (i > 0 && rows[i - 1].key[0] == 'H');

    if (db_query_track_by_id(db, browser_library_track_id(src, rows[i].key), &cur) != 1) {
      CHECK(0, "%s: row %zu names no track", what, i);
      continue;
    }
    int boundary = !have_prev || cmp_bin_null(prev.artist, cur.artist) != 0
                              || cmp_bin_null(prev.album, cur.album) != 0;
    CHECK(boundary == after_header,
          "%s: row %zu (%s): group boundary=%d but header before it=%d",
          what, i, rows[i].name, boundary, after_header);
    if (after_header) {
      header_text(&cur, scoped, want, sizeof want);
      CHECK(strcmp(rows[i - 1].name, want) == 0, "%s: header \"%s\", want \"%s\"",
            what, rows[i - 1].name, want);
    }
    if (have_prev) {
      db_track_row_free_fields(&prev);
    }
    prev = cur;
    have_prev = 1;
  }
  if (have_prev) {
    db_track_row_free_fields(&prev);
  }
  return headers;
} /* check_listing */

/*
 * has_ci: 1 when `needle` occurs in `hay` ignoring ASCII case; an
 * independent oracle for the facet filter (strncasecmp, not
 * query_name_contains).
 */
static int
has_ci(const char * hay, const char * needle)
{
  size_t m = strlen(needle);

  if (m == 0) {
    return 1;
  }
  for (; *hay != '\0'; hay++) {
    if (strncasecmp(hay, needle, m) == 0) {
      return 1;
    }
  }
  return 0;
} /* has_ci */

/*
 * check_facet: load the facet root for `filter` and assert row 0 is
 * "All (N)" with N == db_count_tracks and the All list key, and that the
 * remaining rows are exactly the artists containing `filter` (oracle
 * has_ci), in db_query_artists' order.
 */
static void
check_facet(Db * db, const BrowserSource * src, const char * filter)
{
  BrowserRow * rows = NULL;
  size_t       n = 0;
  char **      names = NULL;
  size_t       nn = 0, want = 0;
  char         label[64];
  char *       fk = browser_library_facet_key(filter);
  char *       allkey = browser_library_list_key(NULL, "");

  CHECK(fk != NULL && allkey != NULL && src->load(src->self, BROWSER_COL_MAIN, fk, &rows, &n) == 0,
        "facet load failed for filter \"%s\"", filter);
  db_query_artists(db, &names, &nn);

  snprintf(label, sizeof label, "All (%lld)", (long long) db_count_tracks(db));
  CHECK(n > 0 && strcmp(rows[0].name, label) == 0, "facet \"%s\": row 0 is \"%s\", want \"%s\"",
        filter, n > 0 ? rows[0].name : "(none)", label);
  CHECK(n > 0 && allkey != NULL && strcmp(rows[0].key, allkey) == 0 && rows[0].is_dir,
        "facet \"%s\": the All row key or is_dir is wrong", filter);

  for (size_t i = 0; i < nn; i++) {
    if (has_ci(names[i], filter)) {
      CHECK(n > 1 + want && strcmp(rows[1 + want].name, names[i]) == 0,
            "facet \"%s\": artist row %zu is \"%s\", want \"%s\"", filter, want,
            n > 1 + want ? rows[1 + want].name : "(none)", names[i]);
      want++;
    }
  }
  CHECK(n == 1 + want, "facet \"%s\": %zu rows, want %zu", filter, n, 1 + want);

  db_names_free(names, nn);
  src->free_rows(src->self, rows, n);
  free(fk);
  free(allkey);
} /* check_facet */

/*
 * test_list_grouping: F8. Group-fixture listings through the library
 * source: the All listing and an artist-scoped one satisfy
 * check_listing; the untagged header heads All (Q2); a filtered key
 * carries its query; list and group keys load nothing outside the main
 * column; the facet root lists All (N) first; browser_reroot swaps the
 * root and leaves the browser alone on a failed load.
 */
static void
test_list_grouping(void)
{
  Db *               gdb = group_fixture_db();
  const char *       roots[] = { "/music" };
  BrowserSource      src;
  BrowserRow *       rows = NULL;
  size_t             n = 0;
  char *             key;

  if (gdb == NULL) {
    CHECK(0, "group fixture unavailable for F8");
    return;
  }
  browser_library_source(&src, BROWSER_SRC_LIBRARY, gdb, roots, 1);

  key = browser_library_list_key(NULL, "");
  CHECK(key != NULL && src.load(src.self, BROWSER_COL_MAIN, key, &rows, &n) == 0, "All list load failed");
  if (key != NULL && rows != NULL) {
    size_t headers = check_listing(gdb, &src, rows, n, 0, "F8 All");
    CHECK(headers == 9, "F8 All: %zu headers, want 9", headers);
    CHECK(strcmp(rows[0].name, "(unknown artist)" T_SEP_ARTIST "(no album)") == 0,
          "Q2: row 0 of All is \"%s\", want the untagged header", rows[0].name);

    BrowserRow * none = NULL;
    size_t       nn = 99;
    CHECK(src.load(src.self, BROWSER_COL_MAIN, rows[0].key, &none, &nn) == 0 && nn == 0,
          "a header key must load zero rows");
    src.free_rows(src.self, none, nn);
    none = NULL;
    nn = 99;
    CHECK(src.load(src.self, BROWSER_COL_PREVIEW, key, &none, &nn) == 0 && nn == 0,
          "a list key in the preview column must load zero rows");
    src.free_rows(src.self, none, nn);
  }
  src.free_rows(src.self, rows, n);
  rows = NULL;
  free(key);

  check_facet(gdb, &src, "");
  check_facet(gdb, &src, "ab");
  check_facet(gdb, &src, "AB");
  check_facet(gdb, &src, "zzz");

  /* Artist scope: the key of the "Beta" row of the facet root. */
  {
    char * fk = browser_library_facet_key("");
    char * beta = NULL;

    CHECK(fk != NULL && src.load(src.self, BROWSER_COL_MAIN, fk, &rows, &n) == 0, "facet load failed");
    for (size_t i = 0; i < n; i++) {
      if (strcmp(rows[i].name, "Beta") == 0) {
        beta = strdup(rows[i].key);
      }
    }
    src.free_rows(src.self, rows, n);
    rows = NULL;
    free(fk);
    CHECK(beta != NULL, "no Beta row in the facet root");

    if (beta != NULL) {
      key = browser_library_list_key(beta, "");
      CHECK(key != NULL && src.load(src.self, BROWSER_COL_MAIN, key, &rows, &n) == 0, "Beta list load failed");
      if (rows != NULL) {
        size_t headers = check_listing(gdb, &src, rows, n, 1, "F8 Beta");
        CHECK(headers == 3 && n == 3 + 6, "F8 Beta: %zu headers, %zu rows, want 3, 9", headers, n);
      }
      src.free_rows(src.self, rows, n);
      rows = NULL;
      free(key);

      key = browser_library_list_key(beta, "First");
      CHECK(key != NULL && src.load(src.self, BROWSER_COL_MAIN, key, &rows, &n) == 0, "Beta+First load failed");
      CHECK(n == 4, "F8 Beta+\"First\": %zu rows, want 4 (header and 3 tracks)", n);
      if (rows != NULL) {
        check_listing(gdb, &src, rows, n, 1, "F8 Beta+First");
      }
      src.free_rows(src.self, rows, n);
      rows = NULL;
      free(key);
    }
    free(beta);
  }

  /* browser_reroot: the listing and the root change together; a failed
     load leaves both alone. */
  {
    char *    fk = browser_library_facet_key("");
    Browser * b = (fk != NULL) ? browser_create(&src, fk) : NULL;

    key = browser_library_list_key(NULL, "");
    CHECK(b != NULL && key != NULL, "browser_create failed");
    if (b != NULL && key != NULL) {
      browser_cursor_move(b, 1);
      CHECK(browser_cursor(b) == 1, "facet browser cursor did not move");
      CHECK(browser_reroot(b, key) == 0, "browser_reroot failed");
      CHECK(strcmp(browser_cwd(b), key) == 0, "browser_reroot: cwd is not the new key");
      CHECK(browser_count(b) == 18 + 9, "browser_reroot: %zu rows, want 27", browser_count(b));
      CHECK(browser_cursor(b) == 0, "browser_reroot: cursor not at 0");
      CHECK(browser_ascend(b) == 0, "browser_reroot: the new key is not the ceiling");

      CHECK(browser_reroot(b, "?") == -1, "browser_reroot accepted a key the source refuses");
      CHECK(strcmp(browser_cwd(b), key) == 0 && browser_count(b) == 27,
            "a failed browser_reroot changed the browser");
    }
    browser_destroy(b);
    free(key);
    free(fk);
  }
} /* test_list_grouping */

/*
 * group_fixture_cleanup: release the group fixture. Called once from main.
 */
static void
group_fixture_cleanup(void)
{
  if (g_group_fixture_db != NULL) {
    db_close(g_group_fixture_db);
    g_group_fixture_db = NULL;
  }
} /* group_fixture_cleanup */

/*
 * main: runs the pure tests always and the FTS5 mode unless --pure is
 * given -- the FTS5 mode now also covers the prefix-range precondition,
 * which likewise needs a real engine. Takes the usual argc/argv;
 * returns 0 if every check passed, 1 otherwise.
 */
int main(int argc, char ** argv)
{
  int pure_only = (argc > 1 && strcmp(argv[1], "--pure") == 0);

  test_match_documented();
  test_match_edges();
  test_match_big();
  test_match_utf8();
  test_name_cmp();
  test_name_contains();
  test_collapse();
  test_collapse_wide();

  if (!pure_only) {
    test_against_fts5();
    test_prefix_range_precondition();
    test_upsert_preserves_id();
    test_list_order();
    test_list_artist();
    test_list_compose();
    test_list_cap();
    test_list_latency();
    test_list_untagged();
    test_list_path_tiebreak();
    test_list_index();
    test_list_grouping();
    test_fts_prefix_index();
    test_schema_migrate();
    search_fixture_cleanup();
    group_fixture_cleanup();
  }

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("fts_query_test: all checks passed%s\n",
         pure_only ? " (pure mode, FTS5 checks skipped)" : "");
  return 0;
} /* main */
