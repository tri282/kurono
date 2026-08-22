/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_LIBRARY_SCAN_H
#define KURONO_LIBRARY_SCAN_H

#include <stddef.h>

#include "db.h"

/*
 * ScanProgressFn: how a scan reports where it is, and how it reports a
 * per-file failure. library/scan.c must not call into ui/ -- the
 * dependency graph is acyclic (CLAUDE.md; DESIGN.md III.2) -- so the
 * caller supplies the sink and scan.c only calls downward.
 *
 * v1 has two callers with genuinely different sinks: main.c passes a
 * stderr printer for the startup scan, which is legal only because it
 * runs before tb_init claims the terminal, and a status-bar repaint for
 * ACT_RESCAN (phase 17), where a printf would corrupt the display. It
 * is also the seam a post-v1 background scan thread needs -- that
 * thread would supply a printer posting EV_SCAN_PROGRESS onto the
 * existing T2->T1 queue, and scan.c would not change (DESIGN.md V.5).
 *
 * `done` and `total` span every root, not the current one. `total` is
 * an estimate that may be revised upward as roots are walked; a caller
 * rendering a percentage must tolerate total growing. `path` is the
 * file just handled, borrowed for the duration of the call only.
 */
typedef void (* ScanProgressFn)(void * ctx, int done, int total, const char * path);

/*
 * ScanOpts: everything scan_run needs, as a struct rather than
 * positional parameters so post-v1 additions -- a cancellation flag, a
 * follow-symlinks toggle -- do not change the signature again
 * (DESIGN.md V.5).
 *
 * `roots` are the resolved music roots in the order config.c produced
 * them; scan_run does not read KURONO_MUSIC_DIR itself, so scan.c
 * carries no configuration knowledge. A root that does not exist is
 * skipped, not an error -- config.c deliberately keeps unusable roots
 * in the set so they can be named.
 *
 * `ignore_names` are basenames never descended into, compared exactly
 * and case-sensitively against a directory's own name.
 *
 * on_progress may be NULL, in which case the scan is silent.
 */
typedef struct {
  const char * const * roots;
  size_t               nroots;
  const char * const * ignore_names;   /* basenames, never descended into */
  size_t               nignore;
  ScanProgressFn       on_progress;
  void *               ctx;
} ScanOpts;

/*
 * scan_run: walk every root, index what it finds, and remove what is no
 * longer there -- the seven steps of DESIGN.md V.5, on T1, inside a
 * single transaction.
 *
 * One call, one transaction, one scan_gen bump, every root walked in
 * order, and exactly one sweep after the last root (amendment 01). The
 * sweep is what forces this shape: DELETE FROM tracks WHERE
 * scan_gen < G inside the per-root loop would have the second root's
 * sweep delete every track the first root just inserted, and the
 * symptom is a library that only ever contains the last root. A
 * post-v1 partial rescan of one subtree needs a scoped sweep
 * (AND path LIKE ?), not a second call to this.
 *
 * Per-file failures are never fatal (DESIGN.md XI.1): an unreadable
 * file, a corrupt tag, an unsupported codec, or a zero-length file is
 * counted and skipped, and the walk continues. One bad file must never
 * abort a scan of five thousand.
 *
 * Takes the open database and the options; returns the number of files
 * skipped (0 on a clean scan), or -1 if the transaction did not commit
 * -- a database-level failure, after which the caller should treat the
 * index as unchanged, since the rollback leaves it exactly as it was.
 *
 * The count is the return value rather than a per-file message because
 * DESIGN.md V.5 fixes ScanProgressFn's signature and it carries no
 * error channel, while scan.c may not write to stderr itself: that is
 * fine at startup but corrupts the display on the ACT_RESCAN path,
 * where the terminal is already claimed. Reporting an aggregate lets
 * each caller render it in its own medium, which is the same reasoning
 * that made progress a callback in the first place.
 */
int scan_run(Db * db, const ScanOpts * opts);

#endif // KURONO_LIBRARY_SCAN_H
