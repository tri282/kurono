/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * library/query.c -- the two transforms between db.c's rows and what a
 * view can show. Stateless, takes no database handle and includes no
 * SQLite header -- which is what the verify grep checks. See query.h and
 * plans/kurono/02-decide/amendment-08-query-layer-scope.md.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../include/library/query.h"

/*
 * Worst case for query_build_match's output: every input byte is a
 * double quote and so doubles, every token costs two quotes plus one
 * separating space, and the whole string costs one trailing '*' and one
 * NUL. 4n + 8 covers that with room to spare (the tight bound is
 * 3.5n + 4, since a token boundary consumes an input byte of its own).
 */
#define MATCH_GROWTH 4
#define MATCH_SLACK  8

/*
 * query_name_cmp: see query.h.
 */
int query_name_cmp(const char * a, const char * b)
{
  int c = strcasecmp(a, b);
  return (c != 0) ? c : strcmp(a, b);
} /* query_name_cmp */

/*
 * cmp_str: qsort comparator over char * elements, ordering by
 * query_name_cmp so the collapsed names agree with the COLLATE NOCASE
 * order db.c produces.
 */
static int cmp_str(const void * a, const void * b)
{
  const char * const * pa = a;
  const char * const * pb = b;
  return query_name_cmp(*pa, *pb);
} /* cmp_str */

/*
 * query_dirs_collapse: see query.h. Takes `parent`, the borrowed
 * descendant paths and their count; fills *out_names / *out_n with an
 * owned, sorted, de-duplicated array of immediate child names. Returns
 * 0, or 1 if an allocation failed and the result is partial.
 */
int query_dirs_collapse(const char * parent, char * const * paths, size_t n,
                        char *** out_names, size_t * out_n)
{
  char ** names = NULL;
  size_t  count = 0;
  size_t  plen;
  size_t  i;
  int     truncated = 0;

  *out_names = NULL;
  *out_n = 0;

  if (parent == NULL || n == 0) {
    return 0;
  }

  /* A trailing slash on `parent` must not become part of the prefix,
   * but "/" is all slash and still has to match at length 1. */
  plen = strlen(parent);
  while (plen > 1 && parent[plen - 1] == '/') {
    plen--;
  }

  names = malloc(n * sizeof(*names));
  if (names == NULL) {
    return 1;
  }

  for (i = 0; i < n; i++) {
    const char * rest;
    size_t       seglen;
    char *       name;

    if (paths[i] == NULL || strncmp(paths[i], parent, plen) != 0) {
      continue;
    }

    rest = paths[i] + plen;

    /* `parent` itself contributes no child. A path that merely shares
     * the prefix ("/m/plainer" under "/m/plain") is not under it at all
     * -- only a '/' right at the boundary makes it a descendant. The
     * one exception is parent == "/", which ends in the separator. */
    if (*rest == '/') {
      rest++;
    } else if (!(plen == 1 && parent[0] == '/')) {
      continue;
    }

    seglen = strcspn(rest, "/");
    if (seglen == 0) {
      continue;
    }

    name = malloc(seglen + 1);
    if (name == NULL) {
      truncated = 1;
      break;
    }
    memcpy(name, rest, seglen);
    name[seglen] = '\0';
    names[count++] = name;
  }

  if (count > 1) {
    size_t w = 1;

    qsort(names, count, sizeof(*names), cmp_str);
    for (i = 1; i < count; i++) {
      if (strcmp(names[i], names[w - 1]) == 0) {
        free(names[i]);
      } else {
        names[w++] = names[i];
      }
    }
    count = w;
  }

  if (count == 0) {
    free(names);
    names = NULL;
  }

  *out_names = names;
  *out_n = count;
  return truncated;
} /* query_dirs_collapse */

/*
 * query_build_match: see query.h. Takes the user's raw input; returns
 * an owned FTS5 MATCH string, an owned empty string if the input holds
 * no tokens, or NULL on allocation failure.
 */
char * query_build_match(const char * raw)
{
  const unsigned char * in;
  char *                out;
  size_t                len;
  size_t                w = 0;
  int                   any_token = 0;

  if (raw == NULL) {
    raw = "";
  }
  len = strlen(raw);
  if (len > (SIZE_MAX - MATCH_SLACK) / MATCH_GROWTH) {
    return NULL;
  }

  out = malloc(MATCH_GROWTH * len + MATCH_SLACK);
  if (out == NULL) {
    return NULL;
  }

  in = (const unsigned char *) raw;
  while (*in != '\0') {
    /* isspace is only ever handed an unsigned char, and no byte of a
     * UTF-8 continuation sequence (>= 0x80) can match it, so a token is
     * never split inside a codepoint. */
    if (isspace(*in)) {
      in++;
      continue;
    }

    if (any_token) {
      out[w++] = ' ';
    }
    any_token = 1;

    out[w++] = '"';
    while (*in != '\0' && !isspace(*in)) {
      if (*in == '"') {
        out[w++] = '"';   /* an embedded quote is escaped by doubling */
      }
      out[w++] = (char) *in;
      in++;
    }
    out[w++] = '"';
  }

  /* The final token is the word still being typed, so it -- and only it
   * -- gets the prefix operator. The loop always exits just past a
   * closing quote, so that is exactly here. */
  if (any_token) {
    out[w++] = '*';
  }

  out[w] = '\0';
  return out;
} /* query_build_match */

/*
 * fold_ascii: lower-case 'A'-'Z' and leave every other byte alone. Not
 * tolower(3), which is locale-dependent above 0x7f.
 */
static unsigned char
fold_ascii(unsigned char c)
{
  return (c >= 'A' && c <= 'Z') ? (unsigned char) (c + ('a' - 'A')) : c;
} /* fold_ascii */

/*
 * query_name_contains: see query.h.
 */
int query_name_contains(const char * hay, const char * needle)
{
  if (needle[0] == '\0') {
    return 1;
  }
  if (hay == NULL) {
    return 0;
  }

  /* ponytail: O(n*m) scan, artist names are short; Boyer-Moore if a
     facet ever lists names of kilobytes. */
  for (; *hay != '\0'; hay++) {
    const unsigned char * h = (const unsigned char *) hay;
    const unsigned char * n = (const unsigned char *) needle;

    while (*n != '\0' && *h != '\0' && fold_ascii(*h) == fold_ascii(*n)) {
      h++;
      n++;
    }
    if (*n == '\0') {
      return 1;
    }
  }
  return 0;
} /* query_name_contains */
