/*
 * db_probe: a scratch driver for phase 13's verify table. main.c does
 * not call db_open yet (that wiring belongs to phase 14), so every row
 * of the table that reads "run kurono" has no way to reach db.c. This
 * exercises the handle directly instead: open with the roots given on
 * the command line, report what db_open decided, optionally write and
 * read a row, then close.
 *
 * Not part of the build. Compile ad hoc:
 *   gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Iinclude \
 *       -o /tmp/db_probe tools/db_probe.c src/library/db.c -lsqlite3
 *
 * usage: db_probe <db_path> [--insert] [--dirs PARENT] [root ...]
 */
#include "../include/library/db.h"
#include "../include/main.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * kurono_die: the probe's own copy. The real one lives in main.c and
 * restores the terminal first; linking main.o here would drag in the
 * whole binary, so this stands in with the same contract -- print to
 * stderr, exit 1, never return.
 */
_Noreturn void
kurono_die(const char * fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  fputs("kurono: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
} /* kurono_die */

int
main(int argc, char ** argv)
{
  if (argc < 2) {
    fprintf(stderr, "usage: %s <db_path> [--insert] [--dirs PARENT] [root ...]\n", argv[0]);
    return 2;
  }

  const char * db_path = argv[1];
  const char * dirs_parent = NULL;
  int          do_insert = 0;
  int          argi = 2;

  if (argi < argc && strcmp(argv[argi], "--insert") == 0) {
    do_insert = 1;
    argi++;
  }
  if (argi + 1 < argc && strcmp(argv[argi], "--dirs") == 0) {
    dirs_parent = argv[argi + 1];
    argi += 2;
  }

  const char ** roots = (const char **)&argv[argi];
  size_t        nroots = (size_t)(argc - argi);
  int           rescanned = -1;

  Db * db = db_open(db_path, roots, nroots, &rescanned);
  printf("open ok, rescanned=%d, corrupt=%d\n", rescanned, db_is_corrupt(db));

  if (do_insert) {
    /* One row through the real upsert path, so the tracks_ai trigger
       populates tracks_fts and the FTS row below has something to hit. */
    int64_t      year = 2001;
    DbTrackWrite t;
    memset(&t, 0, sizeof(t));
    t.path = "/music/a/b.flac";
    t.parent_dir = "/music/a";
    t.mtime = 1;
    t.size = 2;
    t.title = "Björk Test";
    t.artist = "Björk";
    t.album = "Vespertine";
    t.album_artist = "Björk";
    t.year = &year;
    t.scan_gen = 1;

    printf("upsert=%d\n", db_track_upsert(db, &t));

    DbTrackRow * rows = NULL;
    size_t       n = 0;
    db_query_search(db, "bjork", &rows, &n);
    printf("search 'bjork' -> %zu row(s), corrupt=%d\n", n, db_is_corrupt(db));
    if (n > 0) {
      printf("  title=%s artist=%s\n", rows[0].title, rows[0].artist);
    }
    db_track_rows_free(rows, n);
  }

  DbTrackRow * rows = NULL;
  size_t       n = 0;
  db_query_tracks_in_dir(db, "/music/a", &rows, &n);
  printf("tracks_in_dir /music/a -> %zu row(s), corrupt=%d\n", n, db_is_corrupt(db));
  db_track_rows_free(rows, n);

  /* A full scan of `tracks`, so a corrupt page anywhere in the table is
     actually read -- the targeted query above only touches one dir. */
  char ** names = NULL;
  size_t  nn = 0;
  db_query_artists(db, &names, &nn);
  printf("artists -> %zu name(s), corrupt=%d\n", nn, db_is_corrupt(db));
  db_names_free(names, nn);

  /* db_query_dirs against whatever parent the caller named, so the
     Directory view's query can be exercised on paths containing GLOB
     metacharacters without a TUI. */
  if (dirs_parent != NULL) {
    char ** dirs = NULL;
    size_t  nd = 0;
    db_query_dirs(db, dirs_parent, &dirs, &nd);
    printf("dirs under \"%s\" -> %zu:\n", dirs_parent, nd);
    for (size_t i = 0; i < nd; i++) {
      printf("  %s\n", dirs[i]);
    }
    db_names_free(dirs, nd);
  }

  db_close(db);
  return 0;
} /* main */
