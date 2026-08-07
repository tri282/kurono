#include "../include/main.h"
#include "../include/config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Set by tui_init, cleared by tui_shutdown (phase 11). Always 0 before
 * then, which is what makes kurono_die a plain vfprintf+exit until the
 * TUI exists.
 */
static int g_tb_active = 0;

/*
 * kurono_die: see include/main.h for the contract. Deliberately does
 * not install itself as a signal handler and is not called from one --
 * DESIGN.md IV.9.c requires SIGSEGV/SIGABRT to leave the terminal raw
 * because tb_shutdown is not async-signal-safe.
 */
_Noreturn void
kurono_die(const char * fmt, ...)
{
  if (g_tb_active) {
    /* tb_shutdown(); -- wired in phase 11; unreachable until then since
       g_tb_active never leaves 0 before tui_init exists. */
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);

  exit(1);
} /* kurono_die */

/*
 * print_paths: implements --paths (plan.md VI.8, phase-3-config.md
 * I.6). Prints every resolved Config field with the environment
 * variable that overrides it, and whether each music root currently
 * exists. Takes an already-initialised Config, returns nothing.
 */
static void
print_paths(const Config * c)
{
  for (size_t i = 0; i < c->nroots; i++) {
    printf("music_dir[%zu]  %-28s (KURONO_MUSIC_DIR)   %s\n",
           i, c->roots[i].path, c->roots[i].ok ? "ok" : "MISSING");
  }

  printf("ignore_dirs   ");
  for (size_t i = 0; i < c->nignore; i++) {
    printf("%s%s", c->ignore[i], (i + 1 < c->nignore) ? ", " : "");
  }
  printf("  (KURONO_IGNORE_DIRS)\n");

  printf("playlist_dir  %s  (KURONO_PLAYLIST_DIR)\n", c->playlist_dir);
  printf("cache_dir     %s  (XDG_CACHE_HOME)     ok\n", c->cache_dir);
  printf("db_path       %s\n", c->db_path);
  printf("config_path   %s  (unused in v1)\n", c->config_path);
  printf("no_scan       %d  (KURONO_NO_SCAN)\n", c->no_scan);
} /* print_paths */

/*
 * Entry point. Handles --version by printing "kurono <version>" to
 * stdout and returning 0, and --paths by resolving the Config and
 * printing it (see print_paths). Any other invocation (including no
 * arguments) prints a one-line "not implemented yet" notice to stderr
 * and returns 1 -- startup, wiring, and teardown land in later phases.
 * Takes the process argc/argv, returns the process exit status.
 */
int
main(int argc, char * argv[])
{
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("kurono %s\n", KURONO_VERSION);
    return 0;
  }

  if (argc == 2 && strcmp(argv[1], "--paths") == 0) {
    Config cfg;
    if (config_init(&cfg) != 0) {
      kurono_die("failed to initialize configuration");
    }
    print_paths(&cfg);
    config_free(&cfg);
    return 0;
  }

  fprintf(stderr, "kurono: not implemented yet\n");
  return 1;
} /* main */
