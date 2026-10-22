/* SPDX-License-Identifier: GPL-3.0-or-later */

/* The m3u8 format and contained playlist directory I/O; libc only
   (v2.1.0 design P2, II.3-II.5, III.2). */

#include "../include/playlist.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLAYLIST_EXT        ".m3u8"
#define PLAYLIST_EXT_LEN    5
#define PLAYLIST_NAME_CHARS 64          /* code points, create path [user 2026-10-21] */
#define PLAYLIST_NAME_MAX   256         /* bytes; the safety check also enforces 250 */
#define PLAYLIST_SAFE_BYTES 250         /* NAME_MAX minus PLAYLIST_EXT */
#define PLAYLIST_MAX_BYTES  (32u << 20)
#define PLAYLIST_LINE_MAX   PATH_MAX
#define TEMP_TRIES          16
#define TEMP_PREFIX         ".kurono-"
#define TEMP_SUFFIX         ".tmp"
#define CURSOR_TAG          "#KURONO-CURSOR:"
#define FILE_BUF            (PLAYLIST_SAFE_BYTES + PLAYLIST_EXT_LEN + 1)
#define LIST_INITIAL_CAP    16

_Static_assert(PLAYLIST_NAME_BUF == PLAYLIST_NAME_MAX + 1,
               "PLAYLIST_NAME_BUF sizes a PLAYLIST_NAME_MAX name plus its NUL");

/*
 * utf8_next: decodes one UTF-8 sequence from s[0..len), len >= 1. Returns
 * its length and stores the code point in *cp, or returns 0 for a stray
 * continuation, a truncated or overlong sequence, a surrogate, or a value
 * above U+10FFFF.
 */
static size_t
utf8_next(const unsigned char * s, size_t len, uint32_t * cp)
{
  size_t   n;
  uint32_t c;
  uint32_t min;

  if (s[0] < 0x80) {
    *cp = s[0];
    return 1;
  }
  if (s[0] >= 0xC2 && s[0] <= 0xDF) {
    n = 2; c = s[0] & 0x1F; min = 0x80;
  } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
    n = 3; c = s[0] & 0x0F; min = 0x800;
  } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
    n = 4; c = s[0] & 0x07; min = 0x10000;
  } else {
    return 0;
  }
  if (len < n) {
    return 0;
  }
  for (size_t i = 1; i < n; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      return 0;
    }
    c = (c << 6) | (s[i] & 0x3F);
  }
  if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
    return 0;
  }
  *cp = c;
  return n;
} /* utf8_next */

/*
 * file_name: formats "<name>.m3u8" into buf (FILE_BUF bytes). name must
 * already have passed playlist_name_safe.
 */
static void
file_name(const char * name, char * buf)
{
  snprintf(buf, FILE_BUF, "%s" PLAYLIST_EXT, name);
} /* file_name */

/* playlist_name_safe: see include/playlist.h. */
const char *
playlist_name_safe(const char * name)
{
  const unsigned char * s = (const unsigned char *) name;
  size_t                len = strlen(name);

  if (len == 0) {
    return "name is empty";
  }
  if (s[0] == '.') {
    return "name cannot start with .";
  }
  if (strchr(name, '/') != NULL) {
    return "name cannot contain /";
  }
  for (size_t i = 0; i < len;) {
    uint32_t cp;
    size_t   n;

    if (s[i] < 0x20 || s[i] == 0x7F) {
      return "name cannot contain control characters";
    }
    n = utf8_next(s + i, len - i, &cp);
    if (n == 0) {
      return "name is not valid UTF-8";
    }
    if (cp >= 0x80 && cp <= 0x9F) {
      return "name cannot contain control characters";
    }
    i += n;
  }
  if (len > PLAYLIST_SAFE_BYTES) {
    return "name is too long";
  }
  return NULL;
} /* playlist_name_safe */

/* playlist_name_check: see include/playlist.h. */
const char *
playlist_name_check(const char * name)
{
  const char * r = playlist_name_safe(name);
  size_t       len;
  size_t       chars = 0;

  if (r != NULL) {
    return r;
  }
  if (strpbrk(name, "\"*:<>?\\|") != NULL) {
    return "name cannot contain \" * : < > ? \\ |";
  }
  len = strlen(name);
  if (name[0] == ' ' || name[len - 1] == ' ') {
    return "name cannot start or end with a space";
  }
  for (size_t i = 0; i < len; i++) {
    if (((unsigned char) name[i] & 0xC0) != 0x80) {
      chars++;
    }
  }
  if (chars > PLAYLIST_NAME_CHARS) {
    return "name is too long";
  }
  return NULL;
} /* playlist_name_check */

/* playlist_path_normalize: see include/playlist.h. */
char *
playlist_path_normalize(const char * path)
{
  char *       out;
  size_t       o = 0;
  const char * p;

  if (path == NULL || path[0] != '/') {
    return NULL;
  }
  out = malloc(strlen(path) + 2);
  if (out == NULL) {
    return NULL;
  }
  p = path;
  while (*p != '\0') {
    const char * s;
    size_t       cl;

    while (*p == '/') {
      p++;
    }
    s = p;
    while (*p != '\0' && *p != '/') {
      p++;
    }
    cl = (size_t) (p - s);
    if (cl == 0 || (cl == 1 && s[0] == '.')) {
      continue;
    }
    if (cl == 2 && s[0] == '.' && s[1] == '.') {
      while (o > 0 && out[o - 1] != '/') {
        o--;
      }
      if (o > 0) {
        o--;
      }
      continue;
    }
    out[o++] = '/';
    memcpy(out + o, s, cl);
    o += cl;
  }
  if (o == 0) {
    out[o++] = '/';
  }
  out[o] = '\0';
  return out;
} /* playlist_path_normalize */

/*
 * hex_val: value of one hex digit, either case, or -1.
 */
static int
hex_val(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
} /* hex_val */

/*
 * percent_decode: owned copy of s with %XX escapes decoded; NULL on a
 * malformed escape, a decoded NUL, or allocation failure.
 */
static char *
percent_decode(const char * s)
{
  char * out = malloc(strlen(s) + 1);
  size_t o = 0;

  if (out == NULL) {
    return NULL;
  }
  for (; *s != '\0'; s++) {
    if (*s == '%') {
      int hi = hex_val(s[1]);
      int lo = hi < 0 ? -1 : hex_val(s[2]);

      if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
        free(out);
        return NULL;
      }
      out[o++] = (char) (hi * 16 + lo);
      s += 2;
    } else {
      out[o++] = *s;
    }
  }
  out[o] = '\0';
  return out;
} /* percent_decode */

/*
 * has_scheme: 1 when line starts ^[A-Za-z][A-Za-z0-9+.-]*://.
 */
static int
has_scheme(const char * line)
{
  size_t i = 1;

  if (!((line[0] >= 'A' && line[0] <= 'Z') || (line[0] >= 'a' && line[0] <= 'z'))) {
    return 0;
  }
  while ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') ||
         (line[i] >= '0' && line[i] <= '9') || line[i] == '+' || line[i] == '.' ||
         line[i] == '-') {
    i++;
  }
  return strncmp(line + i, "://", 3) == 0;
} /* has_scheme */

/* playlist_path_resolve: see include/playlist.h. */
char *
playlist_path_resolve(const char * dir, const char * line)
{
  char * joined;
  char * res;

  if (dir == NULL || dir[0] != '/' || line == NULL) {
    return NULL;
  }
  if (strncasecmp(line, "file:", 5) == 0) {
    const char * rest = line + 5;
    char *       dec;

    if (strncmp(rest, "///", 3) == 0) {
      rest += 2;
    } else if (strncasecmp(rest, "//localhost/", 12) == 0) {
      rest += 11;
    } else {
      return NULL;
    }
    dec = percent_decode(rest);
    if (dec == NULL) {
      return NULL;
    }
    res = playlist_path_normalize(dec);
    free(dec);
    return res;
  }
  if (has_scheme(line)) {
    return NULL;
  }
  if (line[0] == '/') {
    return playlist_path_normalize(line);
  }
  joined = malloc(strlen(dir) + strlen(line) + 2);
  if (joined == NULL) {
    return NULL;
  }
  sprintf(joined, "%s/%s", dir, line);
  res = playlist_path_normalize(joined);
  free(joined);
  return res;
} /* playlist_path_resolve */

/*
 * comp_len: length of the path component starting at s (up to '/' or NUL).
 */
static size_t
comp_len(const char * s)
{
  return strcspn(s, "/");
} /* comp_len */

/* playlist_path_relative: see include/playlist.h. */
char *
playlist_path_relative(const char * dir, const char * abs)
{
  char *       nd = playlist_path_normalize(dir);
  char *       na = playlist_path_normalize(abs);
  char *       out = NULL;
  const char * d;
  const char * t;
  size_t       ups = 0;

  if (nd == NULL || na == NULL) {
    goto done;
  }
  /* "/" has no components: start both cursors at the terminating NUL. */
  d = nd[1] == '\0' ? nd + 1 : nd;
  t = na[1] == '\0' ? na + 1 : na;
  while (*d == '/' && *t == '/') {
    size_t dl = comp_len(d + 1);
    size_t tl = comp_len(t + 1);

    if (dl != tl || memcmp(d + 1, t + 1, dl) != 0) {
      break;
    }
    d += 1 + dl;
    t += 1 + tl;
  }
  for (const char * q = d; *q != '\0'; q++) {
    if (*q == '/') {
      ups++;
    }
  }
  if (*t == '/') {
    t++;
  }
  out = malloc(ups * 3 + strlen(t) + 3);
  if (out == NULL) {
    goto done;
  }
  out[0] = '\0';
  if (ups == 0 && t[0] == '#') {
    strcpy(out, "./");
  }
  for (size_t i = 0; i < ups; i++) {
    strcat(out, "../");
  }
  strcat(out, t);
done:
  free(nd);
  free(na);
  return out;
} /* playlist_path_relative */

/*
 * parse_cursor: reads the decimal after CURSOR_TAG in line[0..ll) into
 * *cursor. Anything but 1-18 digits is left alone.
 */
static void
parse_cursor(const char * line, size_t ll, long * cursor)
{
  char   digits[24];
  size_t n = ll - (sizeof(CURSOR_TAG) - 1);

  if (n == 0 || n > 18) {
    return;
  }
  memcpy(digits, line + sizeof(CURSOR_TAG) - 1, n);
  digits[n] = '\0';
  if (strspn(digits, "0123456789") != n) {
    return;
  }
  *cursor = strtol(digits, NULL, 10);
} /* parse_cursor */

/* playlist_parse: see include/playlist.h. */
int
playlist_parse(const char * dir, const char * buf, size_t len, Playlist * out)
{
  size_t cap = 0;
  size_t i = 0;

  memset(out, 0, sizeof(*out));
  out->cursor = -1;
  if (len >= 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0) {
    buf += 3;
    len -= 3;
  }
  while (i < len) {
    const char * ls = buf + i;
    const char * nl = memchr(ls, '\n', len - i);
    size_t       ll = nl != NULL ? (size_t) (nl - ls) : len - i;
    char *       line;
    char *       path;

    i += ll + (nl != NULL ? 1 : 0);
    if (ll > 0 && ls[ll - 1] == '\r') {
      ll--;
    }
    if (ll == 0) {
      continue;
    }
    if (ll > PLAYLIST_LINE_MAX || memchr(ls, '\0', ll) != NULL) {
      out->bad++;
      continue;
    }
    if (ls[0] == '#') {
      if (ll > sizeof(CURSOR_TAG) - 1 && memcmp(ls, CURSOR_TAG, sizeof(CURSOR_TAG) - 1) == 0) {
        parse_cursor(ls, ll, &out->cursor);
      }
      continue;
    }
    line = strndup(ls, ll);
    if (line == NULL) {
      playlist_free(out);
      out->cursor = -1;
      return -1;
    }
    path = playlist_path_resolve(dir, line);
    free(line);
    if (path == NULL) {
      out->bad++;
      continue;
    }
    if (out->n == cap) {
      size_t   ncap = cap != 0 ? cap * 2 : LIST_INITIAL_CAP;
      char **  np = realloc(out->paths, ncap * sizeof(*np));

      if (np == NULL) {
        free(path);
        playlist_free(out);
        out->cursor = -1;
        return -1;
      }
      out->paths = np;
      cap = ncap;
    }
    out->paths[out->n++] = path;
  }
  return 0;
} /* playlist_parse */

/* One dedupe sort key: the string and its original index. */
typedef struct {
  const char * s;
  size_t       i;
} DedupeKey;

/*
 * dedupe_cmp: qsort comparator, strcmp then original index, so the first
 * occurrence of a string sorts first.
 */
static int
dedupe_cmp(const void * a, const void * b)
{
  const DedupeKey * x = a;
  const DedupeKey * y = b;
  int               c = strcmp(x->s, y->s);

  if (c != 0) {
    return c;
  }
  return (x->i > y->i) - (x->i < y->i);
} /* dedupe_cmp */

/* playlist_dedupe: see include/playlist.h. */
size_t
playlist_dedupe(char ** paths, size_t n)
{
  DedupeKey *     keys;
  unsigned char * drop;
  size_t          out = 0;

  if (n < 2) {
    return n;
  }
  keys = malloc(n * sizeof(*keys));
  drop = calloc(n, 1);
  if (keys == NULL || drop == NULL) {
    free(keys);
    free(drop);
    return n;
  }
  for (size_t i = 0; i < n; i++) {
    keys[i].s = paths[i];
    keys[i].i = i;
  }
  qsort(keys, n, sizeof(*keys), dedupe_cmp);
  for (size_t k = 1; k < n; k++) {
    if (strcmp(keys[k].s, keys[k - 1].s) == 0) {
      drop[keys[k].i] = 1;
    }
  }
  for (size_t i = 0; i < n; i++) {
    if (drop[i]) {
      free(paths[i]);
    } else {
      paths[out++] = paths[i];
    }
  }
  free(keys);
  free(drop);
  return out;
} /* playlist_dedupe */

/* playlist_free: see include/playlist.h. */
void
playlist_free(Playlist * pl)
{
  if (pl == NULL) {
    return;
  }
  for (size_t i = 0; i < pl->n; i++) {
    free(pl->paths[i]);
  }
  free(pl->paths);
  memset(pl, 0, sizeof(*pl));
} /* playlist_free */

/* playlist_names_free: see include/playlist.h. */
void
playlist_names_free(char ** names, size_t n)
{
  if (names == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    free(names[i]);
  }
  free(names);
} /* playlist_names_free */

/* playlist_dir_open: see include/playlist.h. */
int
playlist_dir_open(const char * dir, int create)
{
  int fd;

  /* Entries resolve against dir, so it must be absolute (v2.1.0 phase 6). */
  if (dir == NULL || dir[0] != '/') {
    return -EINVAL;
  }
  fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  if (fd < 0 && errno == ENOENT && create) {
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
      return -errno;
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  }
  return fd < 0 ? -errno : fd;
} /* playlist_dir_open */

/*
 * dir_stream: DIR over a dup of dfd, rewound (the dup shares dfd's
 * offset). Returns NULL with errno set.
 */
static DIR *
dir_stream(int dfd)
{
  int   nfd = dup(dfd);
  DIR * d;

  if (nfd < 0) {
    return NULL;
  }
  d = fdopendir(nfd);
  if (d == NULL) {
    int saved = errno;

    close(nfd);
    errno = saved;
    return NULL;
  }
  rewinddir(d);
  return d;
} /* dir_stream */

/* playlist_list: see include/playlist.h. */
int
playlist_list(int dfd, char *** out_names, size_t * out_n)
{
  DIR *           d = dir_stream(dfd);
  struct dirent * e;
  char **         names = NULL;
  size_t          n = 0;
  size_t          cap = 0;

  *out_names = NULL;
  *out_n = 0;
  if (d == NULL) {
    return -errno;
  }
  while ((e = readdir(d)) != NULL) {
    size_t nl = strlen(e->d_name);
    char * stem;

    if (nl <= PLAYLIST_EXT_LEN || memcmp(e->d_name + nl - PLAYLIST_EXT_LEN, PLAYLIST_EXT,
                                         PLAYLIST_EXT_LEN) != 0) {
      continue;
    }
    stem = strndup(e->d_name, nl - PLAYLIST_EXT_LEN);
    if (stem == NULL) {
      goto oom;
    }
    if (playlist_name_safe(stem) != NULL) {
      free(stem);
      continue;
    }
    if (n == cap) {
      size_t  ncap = cap != 0 ? cap * 2 : LIST_INITIAL_CAP;
      char ** nn = realloc(names, ncap * sizeof(*nn));

      if (nn == NULL) {
        free(stem);
        goto oom;
      }
      names = nn;
      cap = ncap;
    }
    names[n++] = stem;
  }
  closedir(d);
  *out_names = names;
  *out_n = n;
  return 0;
oom:
  closedir(d);
  playlist_names_free(names, n);
  return -ENOMEM;
} /* playlist_list */

/* playlist_exists: see include/playlist.h. */
int
playlist_exists(int dfd, const char * name)
{
  char        fn[FILE_BUF];
  struct stat st;

  if (playlist_name_safe(name) != NULL) {
    return 0;
  }
  file_name(name, fn);
  return fstatat(dfd, fn, &st, AT_SYMLINK_NOFOLLOW) == 0;
} /* playlist_exists */

/* playlist_read: see include/playlist.h. */
int
playlist_read(int dfd, const char * dir, const char * name, Playlist * out)
{
  char        fn[FILE_BUF];
  struct stat st;
  char *      buf;
  size_t      sz;
  size_t      got = 0;
  int         fd;
  int         rc;

  memset(out, 0, sizeof(*out));
  out->cursor = -1;
  if (playlist_name_safe(name) != NULL) {
    return -EINVAL;
  }
  file_name(name, fn);
  fd = openat(dfd, fn, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
  if (fd < 0) {
    return -errno;
  }
  if (fstat(fd, &st) < 0) {
    rc = -errno;
    close(fd);
    return rc;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    return -EINVAL;
  }
  if ((unsigned long long) st.st_size > PLAYLIST_MAX_BYTES) {
    close(fd);
    return -EFBIG;
  }
  sz = (size_t) st.st_size;
  buf = malloc(sz + 1);
  if (buf == NULL) {
    close(fd);
    return -ENOMEM;
  }
  while (got < sz) {
    ssize_t r = read(fd, buf + got, sz - got);

    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      rc = -errno;
      close(fd);
      free(buf);
      return rc;
    }
    if (r == 0) {
      break;
    }
    got += (size_t) r;
  }
  close(fd);
  rc = playlist_parse(dir, buf, got, out);
  free(buf);
  if (rc < 0) {
    return -ENOMEM;
  }
  out->is_link = fstatat(dfd, fn, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode);
  return 0;
} /* playlist_read */

/* playlist_delete: see include/playlist.h. */
int
playlist_delete(int dfd, const char * name)
{
  char fn[FILE_BUF];

  if (playlist_name_safe(name) != NULL) {
    return -EINVAL;
  }
  file_name(name, fn);
  return unlinkat(dfd, fn, 0) < 0 ? -errno : 0;
} /* playlist_delete */

/*
 * temp_sweep: unlink every TEMP_PREFIX<pid>-<k>TEMP_SUFFIX in dfd whose
 * pid is provably dead (kill(pid, 0) fails with ESRCH) [decide V3].
 * Takes the directory fd; returns nothing. Names that do not match the
 * exact <digits>-<digits> shape, and live or unknowable pids, are left.
 */
static void
temp_sweep(int dfd)
{
  DIR *           d = dir_stream(dfd);
  struct dirent * e;

  if (d == NULL) {
    return;
  }
  while ((e = readdir(d)) != NULL) {
    const char * p;
    const char * q;
    size_t       d1;
    size_t       d2;
    long         pid;

    if (strncmp(e->d_name, TEMP_PREFIX, strlen(TEMP_PREFIX)) != 0) {
      continue;
    }
    p = e->d_name + strlen(TEMP_PREFIX);
    d1 = strspn(p, "0123456789");
    if (d1 == 0 || d1 > 9 || p[d1] != '-') {
      continue;
    }
    q = p + d1 + 1;
    d2 = strspn(q, "0123456789");
    if (d2 == 0 || strcmp(q + d2, TEMP_SUFFIX) != 0) {
      continue;
    }
    pid = strtol(p, NULL, 10);
    if (pid > 0 && kill((pid_t) pid, 0) == -1 && errno == ESRCH) {
      unlinkat(dfd, e->d_name, 0);
    }
  }
  closedir(d);
} /* temp_sweep */

/* playlist_write: see include/playlist.h. */
int
playlist_write(int dfd, const char * dir, const char * name,
               const char * const * paths, size_t n, int relative,
               long cursor, mode_t mode, size_t * out_skipped)
{
  char   fn[FILE_BUF];
  char   tmp[64];
  FILE * f;
  int    fd = -1;
  int    err;

  *out_skipped = 0;
  if (playlist_name_safe(name) != NULL) {
    return -EINVAL;
  }
  file_name(name, fn);
  temp_sweep(dfd);
  for (int k = 0; k < TEMP_TRIES; k++) {
    snprintf(tmp, sizeof(tmp), TEMP_PREFIX "%ld-%d" TEMP_SUFFIX, (long) getpid(), k);
    fd = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd >= 0) {
      break;
    }
    if (errno != EEXIST) {
      return -errno;
    }
  }
  if (fd < 0) {
    return -EEXIST;
  }
  f = fdopen(fd, "w");
  if (f == NULL) {
    err = errno;
    close(fd);
    goto fail;
  }
  if (fputs("#EXTM3U\n", f) == EOF) {
    goto ioerr;
  }
  if (cursor >= 0 && fprintf(f, CURSOR_TAG "%ld\n", cursor) < 0) {
    goto ioerr;
  }
  for (size_t i = 0; i < n; i++) {
    const char * line = paths[i];
    char *       rel = NULL;
    int          bad;

    if (line[0] != '/' || strpbrk(line, "\r\n") != NULL) {
      (*out_skipped)++;
      continue;
    }
    if (relative) {
      rel = playlist_path_relative(dir, line);
      if (rel == NULL) {
        (*out_skipped)++;
        continue;
      }
      line = rel;
    }
    bad = fputs(line, f) == EOF || fputc('\n', f) == EOF;
    free(rel);
    if (bad) {
      goto ioerr;
    }
  }
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
    goto ioerr;
  }
  if (fclose(f) != 0) {
    err = errno != 0 ? errno : EIO;
    goto fail;
  }
  if (renameat(dfd, tmp, dfd, fn) < 0) {
    err = errno;
    goto fail;
  }
  /* The rename is done, so a directory fsync failure is not a failed save.
     EINVAL is vfat/exfat, which cannot sync a directory [decide V21]; any
     other errno leaves the rename unconfirmed on disk. */
  if (fsync(dfd) < 0 && errno != EINVAL) {
    return PLAYLIST_UNSYNCED;
  }
  return 0;
ioerr:
  err = errno != 0 ? errno : EIO;
  fclose(f);
fail:
  unlinkat(dfd, tmp, 0);
  return -err;
} /* playlist_write */
