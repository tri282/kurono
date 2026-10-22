/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_PLAYLIST_H
#define KURONO_PLAYLIST_H

#include <stddef.h>
#include <sys/stat.h>

/* Bytes a buffer needs for any valid name plus its NUL: 64 code points
   of up to 4 bytes [user 2026-10-21]. */
#define PLAYLIST_NAME_BUF 257

/* playlist_write: saved, but the directory fsync after the rename failed. */
#define PLAYLIST_UNSYNCED 1

/*
 * One parsed playlist. paths[] are absolute and lexically normalised,
 * owned. bad counts lines that were not usable entries; cursor is the
 * #KURONO-CURSOR value or -1; is_link is set by playlist_read only.
 */
typedef struct {
  char ** paths;
  size_t  n;
  size_t  bad;
  long    cursor;
  int     is_link;
} Playlist;

/*
 * playlist_name_safe: NULL when name is a stem safe to combine with a
 * dirfd (empty, leading '.', '/', control characters, invalid UTF-8 and
 * more than 250 bytes are refused), else a static reason string. Called by
 * every playlist_* function that takes a name.
 */
const char * playlist_name_safe(const char * name);

/*
 * playlist_name_check: playlist_name_safe plus the portability rules
 * (FAT-illegal characters, leading or trailing space, more than 64 code
 * points). main.c only, on the create path; NULL or a static reason.
 */
const char * playlist_name_check(const char * name);

/*
 * playlist_path_normalize: owned lexical normalisation of an absolute
 * path; NULL for a non-absolute path or on allocation failure.
 */
char * playlist_path_normalize(const char * path);

/*
 * playlist_path_resolve: one non-comment, non-empty line to an owned
 * absolute normalised path, resolved against the absolute dir; NULL when
 * the line is rejected (non-file URI, bad escape, decoded NUL,
 * non-absolute dir) or on allocation failure.
 */
char * playlist_path_resolve(const char * dir, const char * line);

/*
 * playlist_path_relative: owned relative path from absolute dir to
 * absolute abs, both normalised first, a result starting '#' prefixed
 * "./"; NULL on a non-absolute input or allocation failure.
 */
char * playlist_path_relative(const char * dir, const char * abs);

/*
 * playlist_parse: fills *out from a buffer of len bytes, resolving
 * entries against dir. Returns 0, or -1 on allocation failure with *out
 * empty. Never fails on content.
 */
int playlist_parse(const char * dir, const char * buf, size_t len, Playlist * out);

/*
 * playlist_dedupe: removes later duplicates (byte-exact) from paths[0..n)
 * in place, freeing them, keeping first-occurrence order; returns the new
 * count. O(n log n) via a sorted index copy. On allocation failure
 * nothing is removed and n is returned.
 */
size_t playlist_dedupe(char ** paths, size_t n);

/*
 * playlist_dir_open: a directory fd (O_CLOEXEC) or -errno; create makes
 * one missing level. A relative dir is -EINVAL.
 */
int playlist_dir_open(const char * dir, int create);

/*
 * playlist_list: owned stems of the safe *.m3u8 names in dfd, in
 * directory order; 0 or -errno.
 */
int playlist_list(int dfd, char *** out_names, size_t * out_n);

/*
 * playlist_exists: 1 when <name>.m3u8 exists (a symlink is not followed),
 * 0 otherwise; 0 for an unsafe name.
 */
int playlist_exists(int dfd, const char * name);

/*
 * playlist_read: opens <name>.m3u8 in dfd, reads it whole and parses it
 * against dir; 0 or -errno (-EINVAL for an unsafe name or a non-regular
 * file, -EFBIG over 32 MiB). Sets out->is_link.
 */
int playlist_read(int dfd, const char * dir, const char * name, Playlist * out);

/*
 * playlist_write: atomically replaces <name>.m3u8 in dfd (temp file,
 * fsync, renameat, directory fsync); 0, PLAYLIST_UNSYNCED, or -errno,
 * *out_skipped set either
 * way. relative selects the dir-relative form, cursor < 0 omits the
 * cursor line, mode is the openat mode (umask applies). Sweeps dead-pid
 * temp files first [decide V3, V4]. A failure leaves the previous file. A
 * directory fsync failure after the rename, other than EINVAL, returns
 * PLAYLIST_UNSYNCED: the new file is in place but not confirmed on disk.
 */
int playlist_write(int dfd, const char * dir, const char * name,
                   const char * const * paths, size_t n, int relative,
                   long cursor, mode_t mode, size_t * out_skipped);

/*
 * playlist_delete: validates name and unlinks <name>.m3u8 in dfd (a
 * symlink is removed, not its target); 0 or -errno.
 */
int playlist_delete(int dfd, const char * name);

/* playlist_free: frees a Playlist's paths and zeroes it; NULL-safe. */
void playlist_free(Playlist * pl);

/* playlist_names_free: frees names[0..n) and the array; NULL-safe. */
void playlist_names_free(char ** names, size_t n);

#endif // KURONO_PLAYLIST_H
