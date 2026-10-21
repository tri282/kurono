/* SPDX-License-Identifier: GPL-3.0-or-later */

/* tools/playlist_test.c -- standalone test binary for src/playlist.c
   (v2.1.0 design VI.1, T1-T14). Links only playlist.o and libc. Runs in
   a mkdtemp directory under build/ that it removes itself; alarm(10)
   turns a blocking open into a failure instead of a hang. */

#include "../include/playlist.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WATCHDOG_SECS 10
#define CAP_BYTES     (32u << 20)
#define FAT_REASON    "name cannot contain \" * : < > ? \\ |"

static int g_failures = 0;
static int g_checks = 0;
static int g_root_fd = -1;

#define CHECK(cond, ...)                                                    \
  do {                                                                      \
    g_checks++;                                                             \
    if (!(cond)) {                                                          \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                  \
      fprintf(stderr, __VA_ARGS__);                                         \
      fprintf(stderr, "\n");                                                \
      g_failures++;                                                         \
    }                                                                       \
  } while (0)

#define S(x) ((x) != NULL ? (x) : "(null)")

/* str_eq: 1 when both are non-NULL and equal. */
static int
str_eq(const char * a, const char * b)
{
  return a != NULL && b != NULL && strcmp(a, b) == 0;
} /* str_eq */

/*
 * rm_children: unlinks everything inside dfd, recursing into
 * subdirectories. Only ever called on the test's own scratch directory.
 */
static void
rm_children(int dfd)
{
  int             nfd = dup(dfd);
  DIR *           d = nfd >= 0 ? fdopendir(nfd) : NULL;
  struct dirent * e;
  struct stat     st;

  if (d == NULL) {
    if (nfd >= 0) {
      close(nfd);
    }
    return;
  }
  rewinddir(d);
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode)) {
      int sub = openat(dfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);

      if (sub >= 0) {
        fchmod(sub, 0755);
        rm_children(sub);
        close(sub);
      }
      unlinkat(dfd, e->d_name, AT_REMOVEDIR);
    } else {
      unlinkat(dfd, e->d_name, 0);
    }
  }
  closedir(d);
} /* rm_children */

/* sub_dir: creates and opens a fresh subdirectory of the scratch root. */
static int
sub_dir(const char * name)
{
  int fd;

  if (mkdirat(g_root_fd, name, 0755) < 0) {
    perror("mkdirat");
    exit(1);
  }
  fd = openat(g_root_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    perror("openat");
    exit(1);
  }
  return fd;
} /* sub_dir */

/* count_prefixed: entries of dfd whose name starts with prefix ("" = all). */
static int
count_prefixed(int dfd, const char * prefix)
{
  int             nfd = dup(dfd);
  DIR *           d = fdopendir(nfd);
  struct dirent * e;
  int             n = 0;

  rewinddir(d);
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) {
      n++;
    }
  }
  closedir(d);
  return n;
} /* count_prefixed */

/* put_file: creates an empty regular file name in dfd. */
static void
put_file(int dfd, const char * name)
{
  int fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL, 0644);

  if (fd >= 0) {
    close(fd);
  }
} /* put_file */

/* slurp: reads dfd/name into buf (NUL-terminated); returns length or -1. */
static long
slurp(int dfd, const char * name, char * buf, size_t sz)
{
  int     fd = openat(dfd, name, O_RDONLY);
  ssize_t r;

  if (fd < 0) {
    return -1;
  }
  r = read(fd, buf, sz - 1);
  close(fd);
  if (r < 0) {
    return -1;
  }
  buf[r] = '\0';
  return (long) r;
} /* slurp */

/* mode_of: permission bits of dfd/name, or -1. */
static int
mode_of(int dfd, const char * name)
{
  struct stat st;

  return fstatat(dfd, name, &st, 0) == 0 ? (int) (st.st_mode & 0777) : -1;
} /* mode_of */

/* write_simple: playlist_write of paths under dir "/m/pl", mode 0644. */
static int
write_simple(int dfd, const char * name, const char * const * paths, size_t n,
             int relative, long cursor, size_t * skipped)
{
  return playlist_write(dfd, "/m/pl", name, paths, n, relative, cursor, 0644, skipped);
} /* write_simple */

/* T1: name acceptance, create cap in code points, safety accepts foreign. */
static void
test_t1(void)
{
  static const char * const ok[] = {"Late Night", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
                                    "a.b", "x-1", "Mix.m3u8"};
  static const char * const safe_only[] = {"a:b", " x", "x "};
  char                      a64[65], a65[66], cp64[64 * 3 + 1], b250[251];

  memset(a64, 'a', 64);
  a64[64] = '\0';
  memset(a65, 'a', 65);
  a65[65] = '\0';
  for (int i = 0; i < 64; i++) {
    memcpy(cp64 + i * 3, "\xE6\x97\xA5", 3);
  }
  cp64[64 * 3] = '\0';
  memset(b250, 'a', 247);
  memcpy(b250 + 247, "\xE6\x97\xA5", 3);
  b250[250] = '\0';

  for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
    CHECK(playlist_name_check(ok[i]) == NULL, "check rejected '%s': %s", ok[i],
          S(playlist_name_check(ok[i])));
    CHECK(playlist_name_safe(ok[i]) == NULL, "safe rejected '%s'", ok[i]);
  }
  CHECK(playlist_name_check(a64) == NULL, "check rejected 64 ASCII");
  CHECK(playlist_name_check(cp64) == NULL, "check rejected 64 three-byte code points");
  for (size_t i = 0; i < sizeof(safe_only) / sizeof(safe_only[0]); i++) {
    CHECK(playlist_name_safe(safe_only[i]) == NULL, "safe rejected '%s'", safe_only[i]);
  }
  CHECK(playlist_name_safe(a65) == NULL, "safe rejected 65 ASCII");
  CHECK(playlist_name_safe(b250) == NULL, "safe rejected 250 bytes ending in a multibyte char");
} /* test_t1 */

/* T2: both reject unsafe names with the same reason; check alone rejects the create rows. */
static void
test_t2(void)
{
  static const struct {
    const char * name;
    const char * reason;
  } unsafe[] = {
    {"", "name is empty"},
    {".", "name cannot start with ."},
    {"..", "name cannot start with ."},
    {"...", "name cannot start with ."},
    {".hidden", "name cannot start with ."},
    {"../x", "name cannot start with ."},
    {"a/b", "name cannot contain /"},
    {"/etc/x", "name cannot contain /"},
    {"x\n", "name cannot contain control characters"},
    {"\x7f", "name cannot contain control characters"},
    {"\xc2\x85", "name cannot contain control characters"},
    {"\xff", "name is not valid UTF-8"},
    {"\xc0\xaf", "name is not valid UTF-8"},
    {"\x80", "name is not valid UTF-8"},
    {"\xe6\x97", "name is not valid UTF-8"},
    {"\xe0\x80\x80", "name is not valid UTF-8"},
    {"\xed\xa0\x80", "name is not valid UTF-8"},
    {"\xf4\x90\x80\x80", "name is not valid UTF-8"},
  };
  static const struct {
    const char * name;
    const char * reason;
  } create_only[] = {
    {" x", "name cannot start or end with a space"},
    {"x ", "name cannot start or end with a space"},
    {"a\"b", FAT_REASON}, {"a*b", FAT_REASON}, {"a:b", FAT_REASON}, {"a<b", FAT_REASON},
    {"a>b", FAT_REASON},  {"a?b", FAT_REASON}, {"a\\b", FAT_REASON}, {"a|b", FAT_REASON},
  };
  char b251[252], a65[66], two65[65 * 2 + 1];

  memset(b251, 'a', 251);
  b251[251] = '\0';
  memset(a65, 'a', 65);
  a65[65] = '\0';
  for (int i = 0; i < 65; i++) {
    memcpy(two65 + i * 2, "\xC3\xA9", 2);
  }
  two65[130] = '\0';

  for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
    const char * s = playlist_name_safe(unsafe[i].name);
    const char * c = playlist_name_check(unsafe[i].name);

    CHECK(str_eq(s, unsafe[i].reason), "safe[%zu]: got '%s' want '%s'", i, S(s), unsafe[i].reason);
    CHECK(str_eq(c, unsafe[i].reason), "check[%zu]: got '%s' want '%s'", i, S(c), unsafe[i].reason);
  }
  CHECK(str_eq(playlist_name_safe(b251), "name is too long"), "safe 251 bytes");
  CHECK(str_eq(playlist_name_check(b251), "name is too long"), "check 251 bytes");
  for (size_t i = 0; i < sizeof(create_only) / sizeof(create_only[0]); i++) {
    const char * c = playlist_name_check(create_only[i].name);

    CHECK(playlist_name_safe(create_only[i].name) == NULL, "safe rejected create-only '%s'",
          create_only[i].name);
    CHECK(str_eq(c, create_only[i].reason), "check '%s': got '%s' want '%s'", create_only[i].name,
          S(c), create_only[i].reason);
  }
  CHECK(playlist_name_safe(a65) == NULL && str_eq(playlist_name_check(a65), "name is too long"),
        "65 ASCII");
  CHECK(playlist_name_safe(two65) == NULL && str_eq(playlist_name_check(two65), "name is too long"),
        "65 two-byte code points");
} /* test_t2 */

/* T3: lexical normalisation. */
static void
test_t3(void)
{
  static const char * const tab[][2] = {
    {"/a/./b//c/../d", "/a/b/d"}, {"/..", "/"},       {"/../../x", "/x"},
    {"/a/b/", "/a/b"},            {"/", "/"},         {"//", "/"},
    {"/a/..", "/"},
  };

  for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
    char * r = playlist_path_normalize(tab[i][0]);

    CHECK(str_eq(r, tab[i][1]), "normalize '%s': got '%s' want '%s'", tab[i][0], S(r), tab[i][1]);
    free(r);
  }
  CHECK(playlist_path_normalize("a/b") == NULL, "relative input must give NULL");
} /* test_t3 */

/* T4: resolve against /m/pl. */
static void
test_t4(void)
{
  static const char * const ok[][2] = {
    {"x.flac", "/m/pl/x.flac"},
    {"../A/x.flac", "/m/A/x.flac"},
    {"/abs/x.flac", "/abs/x.flac"},
    {"file:///m/a%20b.flac", "/m/a b.flac"},
    {"FILE://localhost/m/x", "/m/x"},
    {"file:///m/a%2fb", "/m/a/b"},
    {"a%20b.flac", "/m/pl/a%20b.flac"},
    {"../../../../../../etc/x", "/etc/x"},
  };
  static const char * const bad[] = {"file://host/x",    "http://h/y.mp3", "smb://h/x",
                                     "file:///a%00b",    "file:///a%zz",   "file:///a%4",
                                     "file:relative"};

  for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
    char * r = playlist_path_resolve("/m/pl", ok[i][0]);

    CHECK(str_eq(r, ok[i][1]), "resolve '%s': got '%s' want '%s'", ok[i][0], S(r), ok[i][1]);
    CHECK(r != NULL && r[0] == '/', "resolve '%s' not absolute", ok[i][0]);
    free(r);
  }
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char * r = playlist_path_resolve("/m/pl", bad[i]);

    CHECK(r == NULL, "resolve '%s' should be rejected, got '%s'", bad[i], S(r));
    free(r);
  }
  CHECK(playlist_path_resolve("m/pl", "x.flac") == NULL, "non-absolute dir must give NULL");
} /* test_t4 */

/* T5: relative paths and the resolve(relative()) round trip. */
static void
test_t5(void)
{
  static const char * const tab[][3] = {
    {"/m/pl", "/m/A/x.flac", "../A/x.flac"},
    {"/m/pl", "/m/pl/x.flac", "x.flac"},
    {"/m/pl", "/other/x.flac", "../../other/x.flac"},
    {"/", "/x", "x"},
    {"/m/pl", "/m/pl/#1.flac", "./#1.flac"},
  };
  static const char * const rt[][2] = {
    {"/m/pl", "/m//A/../A/x.flac"}, {"/m/pl/", "/m/pl/./y"}, {"/", "/x"},
    {"/m/pl", "/m/pl/#1.flac"},     {"/a/b/c", "/a/x"},      {"/m/pl", "/m"},
    {"/m/pl", "/"},                 {"/m/pl", "/m/p/x"},     {"/m/pl", "/m/pl2/x"},
  };

  for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
    char * r = playlist_path_relative(tab[i][0], tab[i][1]);

    CHECK(str_eq(r, tab[i][2]), "relative(%s, %s): got '%s' want '%s'", tab[i][0], tab[i][1], S(r),
          tab[i][2]);
    free(r);
  }
  for (size_t i = 0; i < sizeof(rt) / sizeof(rt[0]); i++) {
    char * rel = playlist_path_relative(rt[i][0], rt[i][1]);
    char * back = rel != NULL ? playlist_path_resolve(rt[i][0], rel) : NULL;
    char * want = playlist_path_normalize(rt[i][1]);

    CHECK(str_eq(back, want), "round trip (%s, %s) via '%s': got '%s' want '%s'", rt[i][0],
          rt[i][1], S(rel), S(back), S(want));
    free(rel);
    free(back);
    free(want);
  }
  CHECK(playlist_path_relative("m/pl", "/x") == NULL, "non-absolute dir");
  CHECK(playlist_path_relative("/m/pl", "x") == NULL, "non-absolute abs");
} /* test_t5 */

/* T6: parser. */
static void
test_t6(void)
{
  static const char text[] =
    "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:1,x\r\n\r\n#KURONO-CURSOR:3\r\n../A/x.flac\r\n\r\n"
    "/abs/y.flac\nlast.flac";
  static const char noent[] = "#EXTM3U\n";
  static const char junk[] = "#KURONO-CURSOR:x1\nhttp://h/y.mp3\n";
  Playlist          pl;
  char              big[6100];
  size_t            bl = 0;

  CHECK(playlist_parse("/m/pl", text, sizeof(text) - 1, &pl) == 0, "parse failed");
  CHECK(pl.n == 3 && pl.bad == 0 && pl.cursor == 3, "n=%zu bad=%zu cursor=%ld", pl.n, pl.bad,
        pl.cursor);
  if (pl.n == 3) {
    CHECK(str_eq(pl.paths[0], "/m/A/x.flac"), "entry 0 '%s'", S(pl.paths[0]));
    CHECK(str_eq(pl.paths[1], "/abs/y.flac"), "entry 1 '%s'", S(pl.paths[1]));
    CHECK(str_eq(pl.paths[2], "/m/pl/last.flac"), "entry 2 '%s'", S(pl.paths[2]));
  }
  playlist_free(&pl);

  memcpy(big, "a.flac\nb", 8);
  bl = 8;
  big[bl++] = '\0';
  memcpy(big + bl, "c\n", 2);
  bl += 2;
  memset(big + bl, 'x', 5000);
  bl += 5000;
  memcpy(big + bl, "\nc.flac\n", 8);
  bl += 8;
  CHECK(playlist_parse("/m/pl", big, bl, &pl) == 0, "parse failed");
  CHECK(pl.n == 2 && pl.bad == 2, "NUL and long line: n=%zu bad=%zu", pl.n, pl.bad);
  playlist_free(&pl);

  CHECK(playlist_parse("/m/pl", noent, sizeof(noent) - 1, &pl) == 0, "parse failed");
  CHECK(pl.n == 0 && pl.bad == 0 && pl.cursor == -1, "header only: n=%zu bad=%zu", pl.n, pl.bad);
  playlist_free(&pl);

  CHECK(playlist_parse("/m/pl", junk, sizeof(junk) - 1, &pl) == 0, "parse failed");
  CHECK(pl.n == 0 && pl.bad == 1 && pl.cursor == -1, "junk: n=%zu bad=%zu cursor=%ld", pl.n,
        pl.bad, pl.cursor);
  playlist_free(&pl);
  CHECK(pl.paths == NULL && pl.n == 0, "free must zero");
} /* test_t6 */

/* T7: write then read round trip, CR/LF and non-absolute skip. */
static void
test_t7(void)
{
  static const char * const in[] = {"/m/A/x.flac", "/other/y.flac", "/m/pl/z.flac"};
  static const char * const evil[] = {"/m/a\nb.flac", "rel/x.flac", "/m/ok.flac", "/m/c\rd.flac"};
  int                       dfd = sub_dir("t7");
  Playlist                  pl;
  size_t                    skipped = 99;
  char                      raw[512];

  CHECK(write_simple(dfd, "rel", in, 3, 1, -1, &skipped) == 0 && skipped == 0, "relative write");
  CHECK(slurp(dfd, "rel.m3u8", raw, sizeof(raw)) > 0 &&
          strcmp(raw, "#EXTM3U\n../A/x.flac\n../../other/y.flac\nz.flac\n") == 0,
        "relative text '%s'", raw);
  CHECK(playlist_read(dfd, "/m/pl", "rel", &pl) == 0 && pl.n == 3 && pl.cursor == -1 && !pl.is_link,
        "relative read");
  for (size_t i = 0; i < pl.n && i < 3; i++) {
    CHECK(str_eq(pl.paths[i], in[i]), "relative entry %zu '%s'", i, S(pl.paths[i]));
  }
  playlist_free(&pl);

  CHECK(write_simple(dfd, "abs", in, 3, 0, 2, &skipped) == 0 && skipped == 0, "absolute write");
  CHECK(playlist_read(dfd, "/m/pl", "abs", &pl) == 0 && pl.n == 3 && pl.cursor == 2,
        "absolute read: n=%zu cursor=%ld", pl.n, pl.cursor);
  for (size_t i = 0; i < pl.n && i < 3; i++) {
    CHECK(str_eq(pl.paths[i], in[i]), "absolute entry %zu '%s'", i, S(pl.paths[i]));
  }
  playlist_free(&pl);

  CHECK(write_simple(dfd, "evil", evil, 4, 1, -1, &skipped) == 0 && skipped == 3, "skipped=%zu",
        skipped);
  CHECK(playlist_read(dfd, "/m/pl", "evil", &pl) == 0 && pl.n == 1 && pl.bad == 0,
        "evil read: n=%zu bad=%zu", pl.n, pl.bad);
  playlist_free(&pl);
  rm_children(dfd);
  close(dfd);
} /* test_t7 */

/* T8: atomic replace keeps an open fd on the old content. */
static void
test_t8(void)
{
  static const char * const v1[] = {"/m/v1.flac"};
  static const char * const v2[] = {"/m/v2.flac"};
  int                       dfd = sub_dir("t8");
  size_t                    skipped;
  struct stat               a, b;
  char                      old[256];
  int                       fd;
  ssize_t                   r;

  CHECK(write_simple(dfd, "x", v1, 1, 0, -1, &skipped) == 0, "write v1");
  fd = openat(dfd, "x.m3u8", O_RDONLY);
  CHECK(fd >= 0 && fstat(fd, &a) == 0, "open v1");
  CHECK(write_simple(dfd, "x", v2, 1, 0, -1, &skipped) == 0, "write v2");
  CHECK(fstatat(dfd, "x.m3u8", &b, 0) == 0 && b.st_ino != a.st_ino, "inode did not change");
  r = fd >= 0 ? read(fd, old, sizeof(old) - 1) : -1;
  old[r > 0 ? r : 0] = '\0';
  CHECK(strstr(old, "v1.flac") != NULL && strstr(old, "v2.flac") == NULL, "old fd reads '%s'", old);
  if (fd >= 0) {
    close(fd);
  }
  rm_children(dfd);
  close(dfd);
} /* test_t8 */

/* T9: temp hygiene, sweep, failure path, mode. */
static void
test_t9(void)
{
  static const char * const v1[] = {"/m/v1.flac"};
  static const char * const v2[] = {"/m/v2.flac"};
  int                       dfd = sub_dir("t9");
  size_t                    skipped;
  char                      name[64], before[256], after[256];
  pid_t                     dead;
  mode_t                    um;

  CHECK(write_simple(dfd, "x", v1, 1, 0, -1, &skipped) == 0, "write");
  CHECK(count_prefixed(dfd, ".kurono-") == 0, "temp left after success");

  /* Sweep: dead pid removed, live pid and odd shapes kept [decide V3]. */
  for (dead = 999999; dead < 1099999; dead++) {
    if (kill(dead, 0) == -1 && errno == ESRCH) {
      break;
    }
  }
  snprintf(name, sizeof(name), ".kurono-%ld-0.tmp", (long) dead);
  put_file(dfd, name);
  put_file(dfd, ".kurono-x.tmp");
  {
    char live[64];

    snprintf(live, sizeof(live), ".kurono-%ld-7.tmp", (long) getpid());
    put_file(dfd, live);
    /* pid 1 answers kill(1, 0) with EPERM unless run as root: unknowable, kept. */
    put_file(dfd, ".kurono-1-0.tmp");
    CHECK(write_simple(dfd, "x", v2, 1, 0, -1, &skipped) == 0, "write with planted temps");
    CHECK(mode_of(dfd, name) < 0, "dead-pid temp not swept");
    CHECK(mode_of(dfd, live) >= 0, "live-pid temp swept");
    CHECK(mode_of(dfd, ".kurono-1-0.tmp") >= 0, "pid-1 temp swept");
    CHECK(mode_of(dfd, ".kurono-x.tmp") >= 0, "odd-shape temp swept");
    unlinkat(dfd, live, 0);
    unlinkat(dfd, ".kurono-1-0.tmp", 0);
    unlinkat(dfd, ".kurono-x.tmp", 0);
  }

  /* Failure path: renameat onto a directory fails with EISDIR, no litter. */
  mkdirat(dfd, "d.m3u8", 0755);
  CHECK(write_simple(dfd, "d", v1, 1, 0, -1, &skipped) == -EISDIR, "expected -EISDIR");
  CHECK(count_prefixed(dfd, ".kurono-") == 0, "temp left after failed write");

  /* Read-only directory: -EACCES, previous file byte-identical. */
  if (geteuid() == 0) {
    printf("playlist_test: root, skipping the 0555 case\n");
  } else {
    int rc;

    slurp(dfd, "x.m3u8", before, sizeof(before));
    fchmod(dfd, 0555);
    rc = write_simple(dfd, "x", v1, 1, 0, -1, &skipped);
    fchmod(dfd, 0755);
    CHECK(rc == -EACCES, "0555 directory: got %d", rc);
    slurp(dfd, "x.m3u8", after, sizeof(after));
    CHECK(strcmp(before, after) == 0, "previous file changed");
  }

  /* Mode honours the argument and the umask [decide V4]. */
  um = umask(022);
  CHECK(playlist_write(dfd, "/m/pl", "m", v1, 1, 0, -1, 0644, &skipped) == 0, "write 0644");
  CHECK(mode_of(dfd, "m.m3u8") == 0644, "mode %o want 644", mode_of(dfd, "m.m3u8"));
  CHECK(playlist_write(dfd, "/m/pl", "m", v1, 1, 0, -1, 0600, &skipped) == 0, "write 0600");
  CHECK(mode_of(dfd, "m.m3u8") == 0600, "mode %o want 600", mode_of(dfd, "m.m3u8"));
  umask(um);
  rm_children(dfd);
  close(dfd);
} /* test_t9 */

/* T10: unsafe names refused without touching the directory; foreign names accepted. */
static void
test_t10(void)
{
  static const char * const bad[] = {"../x", ".h", "a/b", "x\n"};
  static const char * const p[] = {"/m/a.flac"};
  int                       dfd = sub_dir("t10");
  size_t                    skipped;
  Playlist                  pl;
  int                       n0;

  put_file(dfd, "keep.m3u8");
  n0 = count_prefixed(dfd, "");
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    CHECK(write_simple(dfd, bad[i], p, 1, 0, -1, &skipped) == -EINVAL, "write '%s'", bad[i]);
    CHECK(playlist_read(dfd, "/m/pl", bad[i], &pl) == -EINVAL, "read '%s'", bad[i]);
    CHECK(playlist_delete(dfd, bad[i]) == -EINVAL, "delete '%s'", bad[i]);
    CHECK(playlist_exists(dfd, bad[i]) == 0, "exists '%s'", bad[i]);
  }
  CHECK(count_prefixed(dfd, "") == n0, "directory changed by refused operations");

  CHECK(write_simple(dfd, "a:b", p, 1, 0, -1, &skipped) == 0, "write foreign name");
  CHECK(playlist_exists(dfd, "a:b") == 1, "exists foreign name");
  CHECK(playlist_read(dfd, "/m/pl", "a:b", &pl) == 0 && pl.n == 1, "read foreign name");
  playlist_free(&pl);
  CHECK(playlist_delete(dfd, "a:b") == 0 && playlist_exists(dfd, "a:b") == 0, "delete foreign name");
  rm_children(dfd);
  close(dfd);
} /* test_t10 */

/* T11: listing filter. */
static void
test_t11(void)
{
  static const char * const files[] = {"a.m3u8", "b.m3u8", "x:y.m3u8", ".hidden.m3u8",
                                       "c.m3u",  "d.M3U8", ".kurono-1-0.tmp", "bad\x01.m3u8"};
  static const char * const want[] = {"a", "b", "x:y"};
  int                       dfd = sub_dir("t11");
  char **                   names;
  size_t                    n = 0;
  int                       seen[3] = {0, 0, 0};

  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    put_file(dfd, files[i]);
  }
  CHECK(playlist_list(dfd, &names, &n) == 0, "list failed");
  CHECK(n == 3, "listed %zu names", n);
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < 3; j++) {
      if (strcmp(names[i], want[j]) == 0) {
        seen[j]++;
      }
    }
  }
  CHECK(seen[0] == 1 && seen[1] == 1 && seen[2] == 1, "unexpected set");
  playlist_names_free(names, n);
  rm_children(dfd);
  close(dfd);
} /* test_t11 */

/* T12: FIFO read returns promptly; over-cap file refused. */
static void
test_t12(void)
{
  int      dfd = sub_dir("t12");
  Playlist pl;
  int      fd;

  CHECK(mkfifoat(dfd, "f.m3u8", 0600) == 0, "mkfifoat");
  CHECK(playlist_read(dfd, "/m/pl", "f", &pl) == -EINVAL, "FIFO read");
  fd = openat(dfd, "big.m3u8", O_WRONLY | O_CREAT, 0600);
  CHECK(fd >= 0 && ftruncate(fd, (off_t) CAP_BYTES + 1) == 0, "sparse file");
  if (fd >= 0) {
    close(fd);
  }
  CHECK(playlist_read(dfd, "/m/pl", "big", &pl) == -EFBIG, "over-cap read");
  rm_children(dfd);
  close(dfd);
} /* test_t12 */

/* T13: delete removes a symlinked playlist and leaves its target. */
static void
test_t13(void)
{
  static const char * const p[] = {"/m/t.flac"};
  int                       dfd = sub_dir("t13");
  size_t                    skipped;
  Playlist                  pl;

  CHECK(write_simple(dfd, "target", p, 1, 0, -1, &skipped) == 0, "write target");
  CHECK(symlinkat("target.m3u8", dfd, "l.m3u8") == 0, "symlinkat");
  CHECK(playlist_read(dfd, "/m/pl", "l", &pl) == 0 && pl.n == 1 && pl.is_link, "read link");
  playlist_free(&pl);
  CHECK(playlist_delete(dfd, "l") == 0, "delete link");
  CHECK(playlist_exists(dfd, "l") == 0 && playlist_exists(dfd, "target") == 1,
        "link gone, target kept");
  rm_children(dfd);
  close(dfd);
} /* test_t13 */

/* dedupe_case: dedupes the words in `in` and compares with `want` (space separated). */
static void
dedupe_case(const char * const * in, size_t n, const char * const * want, size_t wn)
{
  char * v[8];
  size_t got;

  for (size_t i = 0; i < n; i++) {
    v[i] = strdup(in[i]);
  }
  got = playlist_dedupe(v, n);
  CHECK(got == wn, "dedupe count %zu want %zu", got, wn);
  for (size_t i = 0; i < got && i < wn; i++) {
    CHECK(str_eq(v[i], want[i]), "dedupe entry %zu '%s' want '%s'", i, S(v[i]), want[i]);
  }
  for (size_t i = 0; i < got; i++) {
    free(v[i]);
  }
} /* dedupe_case */

/* T14: dedupe keeps first occurrences in order. */
static void
test_t14(void)
{
  static const char * const dup[] = {"a", "b", "a", "c", "b"};
  static const char * const abc[] = {"a", "b", "c"};
  static const char * const one[] = {"a"};

  dedupe_case(dup, 5, abc, 3);
  dedupe_case(abc, 3, abc, 3);
  dedupe_case(one, 1, one, 1);
  dedupe_case(one, 0, one, 0);
} /* test_t14 */

/* main: runs T1-T14 in a scratch directory under build/ and removes it. */
int
main(void)
{
  char root[] = "build/playlist_test.XXXXXX";

  alarm(WATCHDOG_SECS);
  if (mkdtemp(root) == NULL) {
    perror("playlist_test: mkdtemp build/playlist_test.XXXXXX");
    return 1;
  }
  g_root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (g_root_fd < 0) {
    perror("playlist_test: open scratch directory");
    rmdir(root);
    return 1;
  }

  test_t1();
  test_t2();
  test_t3();
  test_t4();
  test_t5();
  test_t6();
  test_t7();
  test_t8();
  test_t9();
  test_t10();
  test_t11();
  test_t12();
  test_t13();
  test_t14();

  rm_children(g_root_fd);
  close(g_root_fd);
  rmdir(root);
  if (g_failures == 0) {
    printf("playlist_test: ok (%d checks)\n", g_checks);
    return 0;
  }
  fprintf(stderr, "playlist_test: FAIL (%d of %d checks)\n", g_failures, g_checks);
  return 1;
} /* main */
