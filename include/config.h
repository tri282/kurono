#ifndef KURONO_CONFIG_H
#define KURONO_CONFIG_H

#include <stddef.h>

/*
 * A single resolved music root. `ok` records whether it existed and was
 * a directory at resolution time -- a missing root is kept, not
 * dropped, so --paths and the status bar can name it (amendment-01).
 */
typedef struct {
  char * path;
  int    ok;             /* exists and is a directory                     */
} MusicRoot;

/*
 * Every field is owned (strdup'd, or built with malloc/snprintf) and
 * freed by config_free, including both arrays and each element inside
 * them. See amendment-01-multi-root.md for the roots/ignore shape.
 */
typedef struct {
  MusicRoot * roots;     /* KURONO_MUSIC_DIR (':'-separated), else the    */
  size_t      nroots;    /*   XDG/~ fallback -- nroots >= 1 always        */
  char **     ignore;    /* KURONO_IGNORE_DIRS (','-separated basenames)  */
  size_t      nignore;
  char * playlist_dir;   /* KURONO_PLAYLIST_DIR, else roots[0]/playlists  */
  char * cache_dir;      /* $XDG_CACHE_HOME/kurono, else ~/.cache/kurono  */
  char * db_path;        /* cache_dir/library.db                          */
  char * config_path;    /* KURONO_CONFIG -- resolved, unused in v1       */
  int    no_scan;        /* KURONO_NO_SCAN set to anything                */
} Config;

/*
 * config_init: resolve every runtime path from the environment into
 * *c and create the cache directory (mode 0700). Returns 0 on success,
 * -1 on allocation failure -- on -1, *c has already been passed through
 * config_free internally and is safe to pass to config_free again (a
 * no-op) or to discard. Cache-directory creation failure is not
 * reported through the return value: it is unconditionally fatal and
 * exits the process via kurono_die before this function can return.
 * *c must not be read before a successful call.
 */
int  config_init(Config * c);

/*
 * config_free: release every owned allocation in *c and zero the
 * struct. Safe to call on a zeroed Config (e.g. one that was declared
 * with = {0} and never initialised) and safe to call twice on the same
 * Config.
 */
void config_free(Config * c);

#endif // KURONO_CONFIG_H
