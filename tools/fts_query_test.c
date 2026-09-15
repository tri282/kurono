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
  }

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("fts_query_test: all checks passed%s\n",
         pure_only ? " (pure mode, FTS5 checks skipped)" : "");
  return 0;
} /* main */
