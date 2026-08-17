#ifndef KURONO_LIBRARY_QUERY_H
#define KURONO_LIBRARY_QUERY_H

#include <stddef.h>

/*
 * The two transforms the query layer owns, and the whole of it.
 *
 * library/db.c already exposes every named query the views need
 * (db_query_dirs, db_query_artists, ... db_query_search), each backed
 * by a statement prepared once at db_open. ui/ calls those directly;
 * this file deliberately holds no pass-through wrapper over them
 * (plans/kurono/02-decide/amendment-08-query-layer-scope.md).
 *
 * What is left is the work db.c will not do: db.c does not parse paths
 * (see db_query_dirs), and the user's raw search text must never reach
 * SQL unescaped (DESIGN.md V.6 step 3). Neither function below takes a
 * Db *, opens a database, or includes a SQLite header -- that is what
 * puts both of them in DESIGN.md XII.2's testable tier, so
 * tools/fts_query_test.c can cover them with no database at all.
 */

/*
 * query_dirs_collapse: reduce the all-depths descendant list that
 * db_query_dirs returns to the distinct next-segment names directly
 * under `parent` -- the Directory view's column 1 (DESIGN.md VIII.4).
 *
 * db_query_dirs returns every parent_dir equal to `parent` or nested
 * under it at any depth, so browsing "/m/plain" yields "/m/plain",
 * "/m/plain/sub" and "/m/plain/sub/deeper" alike. This turns that into
 * { "sub" }: one entry per distinct immediate child, names only, not
 * paths -- the caller rebuilds a path as parent + "/" + name. O(n) plus
 * the sort, which is the O(descendants of parent) cost phase 13
 * accepted when it chose this split over teaching db.c segmentation.
 *
 * `parent` may or may not carry a trailing slash. `paths` are borrowed
 * for the call only and need not be sorted; the output is sorted by
 * strcmp (matching SQLite's BINARY collation, so it agrees with the
 * order db.c would have produced) and de-duplicated. `parent` itself is
 * excluded, and so is any input path not actually under `parent`.
 *
 * On success returns 0 with an owned array of owned strings in
 * *out_names / *out_n -- free it with db_names_free, which is the same
 * shape and saves a second free function. On allocation failure it
 * returns 1 and still hands back the partial array collected so far,
 * which the UI path shows as a truncated result rather than aborting
 * (DESIGN.md XI.7). It does not touch db_result_truncated: that flag
 * belongs to db.c and is cleared at every query entry.
 */
int query_dirs_collapse(const char * parent, char * const * paths, size_t n,
                        char *** out_names, size_t * out_n);

/*
 * query_build_match: turn raw user input into an FTS5 MATCH string that
 * always parses (DESIGN.md V.6 step 3).
 *
 * Splits on whitespace, wraps every token in double quotes with any
 * embedded " doubled, and appends * to the final token so the word
 * being typed is a prefix match:
 *
 *   bjork hom  ->  "bjork" "hom"*
 *   AND        ->  "AND"*
 *   a"b        ->  "a""b"*
 *   *          ->  "*"*
 *
 * Verified against SQLite 3.51.2: unquoted, each of those last three
 * inputs is a query error ("unterminated string", "syntax error",
 * "unknown special query"); quoted, all of them parse and return zero
 * or more rows. The result is bound as a parameter by db_query_search
 * and is never concatenated into SQL.
 *
 * Input with no tokens at all (empty or whitespace-only) returns an
 * owned empty string, and the caller must treat that as "no search" and
 * not call db_query_search with it -- FTS5 has no spelling of the empty
 * query, so there is nothing honest to build. Returns NULL only on
 * allocation failure. Caller frees with free().
 */
char * query_build_match(const char * raw);

#endif // KURONO_LIBRARY_QUERY_H
