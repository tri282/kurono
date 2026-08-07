#include "../include/config.h"
#include "../include/main.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * split_owned: split `s` on single-character `delim` into an array of
 * strdup'd tokens, skipping empty tokens ("a::b" on ':' yields two
 * tokens, not three). Neither side is trimmed -- a token may legitimately
 * start or end with whitespace. `s` may be NULL or empty, in which case
 * *out is set to NULL and *out_n to 0 -- this is not an error.
 *
 * Returns 0 on success (including the zero-token case) or -1 on
 * allocation failure, in which case *out and *out_n are left at
 * NULL/0 and anything partially built has already been freed.
 */
static int
split_owned(const char * s, char delim, char *** out, size_t * out_n)
{
  *out = NULL;
  *out_n = 0;
  if (s == NULL || s[0] == '\0') {
    return 0;
  }

  /* first pass: count non-empty tokens so the array can be sized exactly */
  size_t n = 0;
  const char * p = s;
  for (;;) {
    const char * q = strchr(p, delim);
    size_t       len = q ? (size_t)(q - p) : strlen(p);
    if (len > 0) {
      n++;
    }
    if (q == NULL) {
      break;
    }
    p = q + 1;
  }
  if (n == 0) {
    return 0;
  }

  char ** arr = malloc(n * sizeof(char *));
  if (arr == NULL) {
    return -1;
  }

  /* second pass: strndup each non-empty token into the array */
  size_t idx = 0;
  p = s;
  for (;;) {
    const char * q = strchr(p, delim);
    size_t       len = q ? (size_t)(q - p) : strlen(p);
    if (len > 0) {
      char * tok = strndup(p, len);
      if (tok == NULL) {
        for (size_t i = 0; i < idx; i++) {
          free(arr[i]);
        }
        free(arr);
        return -1;
      }
      arr[idx++] = tok;
    }
    if (q == NULL) {
      break;
    }
    p = q + 1;
  }

  *out = arr;
  *out_n = n;
  return 0;
} /* split_owned */

/*
 * path_join: allocate and return "a" concatenated with "b" (caller
 * supplies any separator as part of `b`, e.g. path_join(home, "/Music")).
 * Returns NULL on allocation failure.
 */
static char *
path_join(const char * a, const char * b)
{
  size_t len = strlen(a) + strlen(b) + 1;
  char * out = malloc(len);
  if (out != NULL) {
    snprintf(out, len, "%s%s", a, b);
  }
  return out;
} /* path_join */

/*
 * xdg_or_home: resolve an XDG base-directory variable, falling back to
 * $HOME + home_suffix (e.g. "/.cache") when it is unset or empty.
 * Returns an owned string, or NULL on allocation failure or a missing
 * $HOME.
 */
static char *
xdg_or_home(const char * xdg_var, const char * home_suffix)
{
  const char * xdg = getenv(xdg_var);
  if (xdg != NULL && xdg[0] != '\0') {
    return strdup(xdg);
  }
  const char * home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return NULL;
  }
  return path_join(home, home_suffix);
} /* xdg_or_home */

/*
 * mkdir_one: mkdir `path` at `mode`, treating an existing *directory* as
 * success. Returns 0 on success, -1 with errno set otherwise. EEXIST is
 * only forgiven when the existing entry is a directory -- forgiving it
 * unconditionally would let a regular file at the cache path report
 * success here and surface later as an unopenable database.
 */
static int
mkdir_one(const char * path, mode_t mode)
{
  if (mkdir(path, mode) == 0) {
    return 0;
  }
  if (errno != EEXIST) {
    return -1;
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  return 0;
} /* mkdir_one */

/*
 * mkdir_p: create `path` and any missing parent directories, all at
 * `mode`. Returns 0 on success (including "already exists as a
 * directory"), -1 on any other failure with errno set by the failing
 * mkdir(2)/stat(2). errno is saved across the free() on the failure
 * paths: free() is not required to preserve it, and the caller reports
 * it via strerror().
 */
static int
mkdir_p(const char * path, mode_t mode)
{
  char * copy = strdup(path);
  if (copy == NULL) {
    return -1;
  }

  char * p = copy;
  if (*p == '/') {
    p++;
  }
  for (; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir_one(copy, mode) != 0) {
        int saved = errno;
        free(copy);
        errno = saved;
        return -1;
      }
      *p = '/';
    }
  }
  if (mkdir_one(copy, mode) != 0) {
    int saved = errno;
    free(copy);
    errno = saved;
    return -1;
  }

  free(copy);
  return 0;
} /* mkdir_p */

/*
 * resolve_roots: build c->roots/c->nroots from KURONO_MUSIC_DIR (a
 * ':'-separated list, PATH convention) or, if that yields zero
 * non-empty entries, from the XDG_MUSIC_DIR -> $HOME/Music fallback
 * chain. nroots >= 1 always holds on success. Each root is stat'd here
 * so `ok` is populated once, not re-derived by every caller. Returns 0
 * on success, -1 on allocation failure or a missing $HOME with no
 * XDG_MUSIC_DIR (an unrecoverable environment, folded into the same
 * -1/kurono_die path as a real allocation failure -- not otherwise
 * distinguished by the plan).
 */
static int
resolve_roots(Config * c)
{
  const char * env = getenv("KURONO_MUSIC_DIR");
  char **      paths = NULL;
  size_t       n = 0;

  if (env != NULL && env[0] != '\0') {
    if (split_owned(env, ':', &paths, &n) != 0) {
      return -1;
    }
  }

  if (n == 0) {
    /* KURONO_MUSIC_DIR unset, empty, or all-empty entries: fall through */
    const char * xdg_music = getenv("XDG_MUSIC_DIR");
    char *       single;

    if (xdg_music != NULL && xdg_music[0] != '\0') {
      single = strdup(xdg_music);
    } else {
      const char * home = getenv("HOME");
      if (home == NULL || home[0] == '\0') {
        return -1;
      }
      single = path_join(home, "/Music");
    }
    if (single == NULL) {
      return -1;
    }

    paths = malloc(sizeof(char *));
    if (paths == NULL) {
      free(single);
      return -1;
    }
    paths[0] = single;
    n = 1;
  }

  MusicRoot * roots = malloc(n * sizeof(MusicRoot));
  if (roots == NULL) {
    for (size_t i = 0; i < n; i++) {
      free(paths[i]);
    }
    free(paths);
    return -1;
  }

  for (size_t i = 0; i < n; i++) {
    struct stat st;
    roots[i].path = paths[i];
    roots[i].ok = (stat(paths[i], &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
  }
  free(paths); /* pointer array only -- strings are now owned by roots[] */

  c->roots = roots;
  c->nroots = n;
  return 0;
} /* resolve_roots */

/*
 * resolve_ignore: build c->ignore/c->nignore from KURONO_IGNORE_DIRS (a
 * ','-separated list of basenames). Unset or empty leaves ignore == NULL,
 * nignore == 0, which is not an error. Returns 0 on success, -1 on
 * allocation failure.
 */
static int
resolve_ignore(Config * c)
{
  const char * env = getenv("KURONO_IGNORE_DIRS");
  if (env == NULL || env[0] == '\0') {
    c->ignore = NULL;
    c->nignore = 0;
    return 0;
  }
  return split_owned(env, ',', &c->ignore, &c->nignore);
} /* resolve_ignore */

/*
 * resolve_playlist_dir: KURONO_PLAYLIST_DIR if set and non-empty, else
 * <roots[0].path>/playlists. Must run after resolve_roots. Returns 0 on
 * success, -1 on allocation failure.
 */
static int
resolve_playlist_dir(Config * c)
{
  const char * env = getenv("KURONO_PLAYLIST_DIR");
  if (env != NULL && env[0] != '\0') {
    c->playlist_dir = strdup(env);
  } else {
    c->playlist_dir = path_join(c->roots[0].path, "/playlists");
  }
  return c->playlist_dir != NULL ? 0 : -1;
} /* resolve_playlist_dir */

/*
 * resolve_cache_dir: $XDG_CACHE_HOME/kurono, else $HOME/.cache/kurono.
 * Returns 0 on success, -1 on allocation failure.
 */
static int
resolve_cache_dir(Config * c)
{
  char * base = xdg_or_home("XDG_CACHE_HOME", "/.cache");
  if (base == NULL) {
    return -1;
  }
  c->cache_dir = path_join(base, "/kurono");
  free(base);
  return c->cache_dir != NULL ? 0 : -1;
} /* resolve_cache_dir */

/*
 * resolve_db_path: <cache_dir>/library.db. Must run after
 * resolve_cache_dir. Returns 0 on success, -1 on allocation failure.
 */
static int
resolve_db_path(Config * c)
{
  c->db_path = path_join(c->cache_dir, "/library.db");
  return c->db_path != NULL ? 0 : -1;
} /* resolve_db_path */

/*
 * resolve_config_path: KURONO_CONFIG if set and non-empty, else
 * $XDG_CONFIG_HOME/kurono/config.toml, else
 * $HOME/.config/kurono/config.toml. Resolved but unread in v1 (DESIGN.md
 * X) -- see phase-3-config.md II.1. Returns 0 on success, -1 on
 * allocation failure.
 */
static int
resolve_config_path(Config * c)
{
  const char * env = getenv("KURONO_CONFIG");
  if (env != NULL && env[0] != '\0') {
    c->config_path = strdup(env);
    return c->config_path != NULL ? 0 : -1;
  }

  char * base = xdg_or_home("XDG_CONFIG_HOME", "/.config");
  if (base == NULL) {
    return -1;
  }
  c->config_path = path_join(base, "/kurono/config.toml");
  free(base);
  return c->config_path != NULL ? 0 : -1;
} /* resolve_config_path */

/*
 * config_init: see include/config.h for the contract. Ownership
 * discipline: *c is zeroed first, then built up field by field; on any
 * allocation failure this function calls config_free(c) on itself
 * before returning -1, so the caller always receives either a fully
 * populated Config or a zeroed one -- never a half-built struct. Cache
 * directory creation failure is unconditionally fatal and calls
 * kurono_die directly (DESIGN.md XI.4) -- there is no recoverable
 * degraded mode, so it is not folded into the -1 return.
 */
int
config_init(Config * c)
{
  memset(c, 0, sizeof(*c));

  if (resolve_roots(c) != 0) {
    goto fail;
  }
  if (resolve_ignore(c) != 0) {
    goto fail;
  }
  if (resolve_playlist_dir(c) != 0) {
    goto fail;
  }
  if (resolve_cache_dir(c) != 0) {
    goto fail;
  }
  if (resolve_db_path(c) != 0) {
    goto fail;
  }
  if (resolve_config_path(c) != 0) {
    goto fail;
  }
  c->no_scan = (getenv("KURONO_NO_SCAN") != NULL) ? 1 : 0;

  if (mkdir_p(c->cache_dir, 0700) != 0) {
    kurono_die("failed to create cache directory %s: %s", c->cache_dir, strerror(errno));
  }

  return 0;

fail:
  config_free(c);
  return -1;
} /* config_init */

/*
 * config_free: see include/config.h for the contract.
 */
void
config_free(Config * c)
{
  if (c == NULL) {
    return;
  }

  if (c->roots != NULL) {
    for (size_t i = 0; i < c->nroots; i++) {
      free(c->roots[i].path);
    }
    free(c->roots);
  }
  c->roots = NULL;
  c->nroots = 0;

  if (c->ignore != NULL) {
    for (size_t i = 0; i < c->nignore; i++) {
      free(c->ignore[i]);
    }
    free(c->ignore);
  }
  c->ignore = NULL;
  c->nignore = 0;

  free(c->playlist_dir);
  c->playlist_dir = NULL;
  free(c->cache_dir);
  c->cache_dir = NULL;
  free(c->db_path);
  c->db_path = NULL;
  free(c->config_path);
  c->config_path = NULL;
} /* config_free */
