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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

  /* FTS must reflect the update too, not just the row -- a trigger this
     change stopped firing would show up as a stale search index. */
  char *       match = query_build_match("New Title");
  DbTrackRow * srows = NULL;
  size_t       sn = 0;
  CHECK(match != NULL, "query_build_match(\"New Title\") returned NULL");
  if (match != NULL) {
    CHECK(db_query_search(db, match, &srows, &sn) == 0, "db_query_search returned non-zero");
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

/* A handful of rare two-word combinations for test_search_order_preserved
   to search among for one matching fewer than 200 of the fixture's rows. */
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
      "title TEXT, artist TEXT, album TEXT, album_artist TEXT);"
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
 * path, exercised so db_query_search is checked as the caller actually
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
 * test_search_order_preserved: assertion 1 (design.md 5.3, decide R7).
 * Finds a query among g_selective_candidates matching fewer than 200 of
 * the fixture's rows, runs it through db_query_search (the real Db, the
 * real production statement) and through the pre-phase-6 reference
 * shape ("ORDER BY rank, f.rowid LIMIT 200", the tie-break R7 requires)
 * against the raw fixture, and asserts identical ids in identical
 * order. Also asserts the reference has at least two distinct bm25
 * scores -- the precondition that keeps the DESC-sign mutant from being
 * invisible on a tied fixture (R7).
 */
static void
test_search_order_preserved(void)
{
  sqlite3 *    raw = search_fixture_raw();
  Db *         db = search_fixture_db();
  char *       match = NULL;
  size_t       ci;
  int64_t      ref_ids[200];
  double       ref_ranks[200];
  size_t       ref_n = 0;

  if (raw == NULL || db == NULL) {
    CHECK(0, "search fixture unavailable for order-preservation test");
    return;
  }

  for (ci = 0; ci < sizeof(g_selective_candidates) / sizeof(g_selective_candidates[0]); ci++) {
    sqlite3_stmt * probe = NULL;
    int64_t        total = -1;

    free(match);
    match = query_build_match(g_selective_candidates[ci]);
    if (match == NULL) {
      continue;
    }
    if (sqlite3_prepare_v2(raw, "SELECT count(*) FROM tracks_fts WHERE tracks_fts MATCH ?1",
                            -1, &probe, NULL) == SQLITE_OK) {
      sqlite3_bind_text(probe, 1, match, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(probe) == SQLITE_ROW) {
        total = sqlite3_column_int64(probe, 0);
      }
      sqlite3_finalize(probe);
    }
    if (total > 0 && total < 200) {
      break;
    }
  }
  CHECK(match != NULL, "no candidate query built");
  if (match == NULL) {
    return;
  }

  sqlite3_stmt * ref = NULL;
  CHECK(sqlite3_prepare_v2(raw,
      "SELECT t.rowid, rank FROM tracks_fts f JOIN tracks t ON t.id = f.rowid "
      "WHERE tracks_fts MATCH ?1 ORDER BY rank, f.rowid LIMIT 200",
      -1, &ref, NULL) == SQLITE_OK, "failed to prepare the reference statement");
  if (ref != NULL) {
    double distinct[200];
    size_t ndistinct = 0;

    sqlite3_bind_text(ref, 1, match, -1, SQLITE_TRANSIENT);
    while (ref_n < 200 && sqlite3_step(ref) == SQLITE_ROW) {
      size_t k;
      int    seen = 0;

      ref_ids[ref_n] = sqlite3_column_int64(ref, 0);
      ref_ranks[ref_n] = sqlite3_column_double(ref, 1);
      for (k = 0; k < ndistinct; k++) {
        if (distinct[k] == ref_ranks[ref_n]) {
          seen = 1;
          break;
        }
      }
      if (!seen && ndistinct < 200) {
        distinct[ndistinct++] = ref_ranks[ref_n];
      }
      ref_n++;
    }
    sqlite3_finalize(ref);

    CHECK(ref_n > 0 && ref_n < 200,
          "candidate query matched %zu rows, want 1..199 -- fixture drifted", ref_n);
    CHECK(ndistinct >= 2,
          "reference query has only %zu distinct bm25 score(s); a sign-error "
          "mutant is invisible on a tied fixture (decide R7)", ndistinct);
  }

  DbTrackRow * srows = NULL;
  size_t       sn = 0;
  CHECK(db_query_search(db, match, &srows, &sn) == 0, "db_query_search returned non-zero");
  CHECK(sn == ref_n, "db_query_search returned %zu rows, reference returned %zu", sn, ref_n);
  if (srows != NULL) {
    size_t k;
    for (k = 0; k < sn && k < ref_n; k++) {
      CHECK(srows[k].id == ref_ids[k],
            "row %zu: db_query_search id %lld, reference id %lld",
            k, (long long) srows[k].id, (long long) ref_ids[k]);
    }
  }
  db_track_rows_free(srows, sn);
  free(match);
} /* test_search_order_preserved */

/*
 * test_search_cap: assertion 2 (design.md 4.5/5.3). A query matching
 * more than 200 of the fixture's rows still returns exactly 200 through
 * db_query_search.
 */
static void
test_search_cap(void)
{
  Db *           db = search_fixture_db();
  sqlite3 *      raw = search_fixture_raw();
  char *         match;
  sqlite3_stmt * count_stmt = NULL;
  int64_t        total = -1;
  DbTrackRow *   rows = NULL;
  size_t         n = 0;

  if (db == NULL || raw == NULL) {
    CHECK(0, "search fixture unavailable for the >200 cap test");
    return;
  }

  match = query_build_match("a");
  CHECK(match != NULL, "query_build_match(\"a\") returned NULL");
  if (match == NULL) {
    return;
  }

  if (sqlite3_prepare_v2(raw, "SELECT count(*) FROM tracks_fts WHERE tracks_fts MATCH ?1",
                          -1, &count_stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(count_stmt, 1, match, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
      total = sqlite3_column_int64(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);
  }
  CHECK(total > 200, "query \"a\"* matched %lld of %d fixture rows, want > 200 "
        "-- fixture drifted", (long long) total, SEARCH_FIXTURE_N);

  CHECK(db_query_search(db, match, &rows, &n) == 0, "db_query_search returned non-zero");
  CHECK(n == 200, "db_query_search returned %zu rows for a >200-match query, want 200", n);

  /* Which 200, not just how many: the shipped statement caps inside the
     subquery, so a >200-match query returns the 200 lowest rowids ranked
     among themselves (design.md 4.5). Ranking before the cap would return
     the global top 200 instead -- same count, different rows, and 110-200x
     the cost at scale. Counting alone cannot tell the two apart. */
  if (n == 200) {
    sqlite3_stmt * want_stmt = NULL;
    int64_t        want[200];
    int64_t        got[200];
    size_t         i = 0;

    for (i = 0; i < n; i++) {
      got[i] = rows[i].id;
    }
    qsort(got, n, sizeof got[0], cmp_id_qsort);

    if (sqlite3_prepare_v2(raw,
          "SELECT rowid FROM tracks_fts WHERE tracks_fts MATCH ?1 ORDER BY rowid LIMIT 200",
          -1, &want_stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(want_stmt, 1, match, -1, SQLITE_TRANSIENT);
      for (i = 0; i < 200 && sqlite3_step(want_stmt) == SQLITE_ROW; i++) {
        want[i] = sqlite3_column_int64(want_stmt, 0);
      }
      sqlite3_finalize(want_stmt);
      CHECK(i == 200, "reference query returned %zu rowids, want 200", i);

      for (i = 0; i < 200; i++) {
        CHECK(got[i] == want[i],
              "cap row %zu: got id %lld, want %lld -- the 200 rows are not the "
              "lowest-rowid matches, so the LIMIT is not inside the subquery",
              i, (long long) got[i], (long long) want[i]);
      }
    } else {
      CHECK(0, "failed to prepare the lowest-rowid reference query");
    }
  }

  db_track_rows_free(rows, n);
  free(match);
} /* test_search_cap */

/*
 * test_search_latency: assertion 3 (design.md 4.4/5.3, decide R7). Times
 * the pre-phase-6 statement shape against the current PREP(query_search,
 * ...) text, both against the same connection and the same
 * prefix-indexed schema, so the ratio isolates the statement shape
 * rather than the index. Best of five. Prints both figures and their
 * ratio for CHANGELOG.md. Gate >= 2.0 against a healthy measured ~4.0x
 * at this fixture size (decide R5/R7).
 */
static void
test_search_latency(void)
{
  sqlite3 *      conn = search_fixture_raw();
  char *         match;
  sqlite3_stmt * old_stmt = NULL;
  sqlite3_stmt * new_stmt = NULL;
  double         old_best = -1.0, new_best = -1.0;
  double         ratio;
  int            rep;

  if (conn == NULL) {
    CHECK(0, "search fixture unavailable for the latency test");
    return;
  }

  match = query_build_match("a");
  CHECK(match != NULL, "query_build_match(\"a\") returned NULL");
  if (match == NULL) {
    return;
  }

  CHECK(sqlite3_prepare_v2(conn,
      "SELECT t.* FROM tracks_fts f JOIN tracks t ON t.id = f.rowid "
      "WHERE tracks_fts MATCH ?1 ORDER BY rank LIMIT 200",
      -1, &old_stmt, NULL) == SQLITE_OK, "failed to prepare the old-shape statement");
  CHECK(sqlite3_prepare_v2(conn,
      "SELECT t.* FROM tracks t "
      "JOIN (SELECT rowid AS rid, rank AS r FROM tracks_fts "
      "       WHERE tracks_fts MATCH ?1 LIMIT 200) x ON t.id = x.rid "
      "ORDER BY x.r, x.rid",
      -1, &new_stmt, NULL) == SQLITE_OK, "failed to prepare the new-shape statement");

  if (old_stmt == NULL || new_stmt == NULL) {
    if (old_stmt != NULL) {
      sqlite3_finalize(old_stmt);
    }
    if (new_stmt != NULL) {
      sqlite3_finalize(new_stmt);
    }
    free(match);
    return;
  }

  for (rep = 0; rep < 5; rep++) {
    struct timespec t0, t1;
    double          elapsed;

    sqlite3_reset(old_stmt);
    sqlite3_clear_bindings(old_stmt);
    sqlite3_bind_text(old_stmt, 1, match, -1, SQLITE_TRANSIENT);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (sqlite3_step(old_stmt) == SQLITE_ROW) { }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (old_best < 0.0 || elapsed < old_best) {
      old_best = elapsed;
    }

    sqlite3_reset(new_stmt);
    sqlite3_clear_bindings(new_stmt);
    sqlite3_bind_text(new_stmt, 1, match, -1, SQLITE_TRANSIENT);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (sqlite3_step(new_stmt) == SQLITE_ROW) { }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (new_best < 0.0 || elapsed < new_best) {
      new_best = elapsed;
    }
  }
  sqlite3_finalize(old_stmt);
  sqlite3_finalize(new_stmt);
  free(match);

  ratio = (new_best > 0.0) ? old_best / new_best : 0.0;
  printf("fts_query_test: search latency old=%.3fms new=%.3fms ratio=%.2fx\n",
         old_best * 1000.0, new_best * 1000.0, ratio);

  /* If this fails on CI with no db.c change, demote to a printed figure
     -- do not re-tune the gate upward (decide R7). */
  CHECK(ratio >= 2.0, "search latency ratio %.2fx, want >= 2.0x (old=%.3fms new=%.3fms)",
        ratio, old_best * 1000.0, new_best * 1000.0);
} /* test_search_latency */

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
  test_collapse();
  test_collapse_wide();

  if (!pure_only) {
    test_against_fts5();
    test_prefix_range_precondition();
    test_upsert_preserves_id();
    test_search_order_preserved();
    test_search_cap();
    test_search_latency();
    test_fts_prefix_index();
    test_schema_migrate();
    search_fixture_cleanup();
  }

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("fts_query_test: all checks passed%s\n",
         pure_only ? " (pure mode, FTS5 checks skipped)" : "");
  return 0;
} /* main */
