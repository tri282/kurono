#include "../include/main.h"
#include "../include/config.h"
#include "../include/event.h"
#include "../include/audio/sink.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bounded by EVENT_MAX_EPOLL_EVENTS in event.c; a local constant here
   keeps main.c ignorant of that internal. */
#define HEADLESS_MAX_FIRED 8
/* redraw_timerfd rate for the phase-4 smoke-test loop -- DESIGN.md
   VIII.3's ~30 Hz redraw policy. Phase 11 replaces this loop's body,
   not its structure, and inherits the same rate. */
#define HEADLESS_REDRAW_HZ 30

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
 * evsrc_name: the diagnostic label for an EventSource, for the
 * headless loop's one-line-per-wakeup stderr output. Takes the tag,
 * returns a static string -- never NULL, an unrecognised value prints
 * as "EVSRC_?".
 */
static const char *
evsrc_name(EventSource src)
{
  switch (src) {
    case EVSRC_TTY:       return "EVSRC_TTY";
    case EVSRC_RESIZE:    return "EVSRC_RESIZE";
    case EVSRC_SIGNAL:    return "EVSRC_SIGNAL";
    case EVSRC_PIPE:      return "EVSRC_PIPE";
    case EVSRC_REDRAW:    return "EVSRC_REDRAW";
    case EVSRC_RECONNECT: return "EVSRC_RECONNECT";
    case EVSRC_TTY_HUP:   return "EVSRC_TTY_HUP";
    case EVSRC_NONE:
    case EVSRC_COUNT:
    default:              return "EVSRC_?";
  }
} /* evsrc_name */

/*
 * run_headless: implements --headless (plan.md VI.8, phase-4-eventloop.md
 * I.6). Temporary scaffolding until phase 11 gives kurono a TUI: builds
 * an EventLoop, arms the redraw timer at HEADLESS_REDRAW_HZ, registers
 * stdin as the tty source, and prints one stderr line per source that
 * fires on every wakeup. A self-pipe wakeup is drained in full, per
 * event.h's contract, before the loop continues; a tty wakeup is
 * drained because epoll is level-triggered. Exits the loop -- not the
 * process -- on EVSRC_SIGNAL, on tty EOF, or on EVSRC_TTY_HUP; all are
 * EVSRC_TTY_HUP; both are unconditional-shutdown sources per DESIGN.md
 * VIII.2. Takes nothing, returns the process exit status.
 */
static int
run_headless(void)
{
  EventLoop * el = event_create();
  if (el == NULL) {
    fprintf(stderr, "kurono: event_create failed\n");
    return 1;
  }

  if (event_arm_redraw(el, HEADLESS_REDRAW_HZ) != 0) {
    fprintf(stderr, "kurono: event_arm_redraw failed\n");
    event_destroy(el);
    return 1;
  }

  /* Register stdin as the tty source. Phase 11 registers termbox2's
     ttyfd here instead; until then this is what makes EVSRC_TTY_HUP
     reachable, which is the whole point of the reclassification in
     event_wait -- an unreachable branch is an untested one.
     Non-fatal: epoll_ctl rejects a regular file or /dev/null with
     EPERM, so `--headless </dev/null` must still run the timer loop
     rather than abort. */
  if (event_add(el, EVSRC_TTY, STDIN_FILENO) != 0) {
    fprintf(stderr, "kurono: stdin not pollable, tty source disabled\n");
  }

  /* phase-4-eventloop.md III: the write end must be O_NONBLOCK, because
     phase 5 writes to it from the audio thread. Reported rather than
     asserted -- assert is compiled out under -DNDEBUG, and this is a
     property worth seeing in the smoke test's own output. */
  int wflags = fcntl(event_pipe_write_fd(el), F_GETFL);
  fprintf(stderr, "pipe_write O_NONBLOCK=%d\n",
          (wflags >= 0 && (wflags & O_NONBLOCK) != 0) ? 1 : 0);

  int running = 1;
  int status = 0;
  while (running) {
    EventSource fired[HEADLESS_MAX_FIRED];
    int n = event_wait(el, fired, HEADLESS_MAX_FIRED, -1);
    if (n < 0) {
      fprintf(stderr, "kurono: event_wait failed\n");
      status = 1;
      break;
    }

    for (int i = 0; i < n; i++) {
      fprintf(stderr, "%s\n", evsrc_name(fired[i]));

      if (fired[i] == EVSRC_PIPE) {
        /* Drain every available byte before doing anything else --
           event.h's contract for the self-pipe. There is no event
           queue yet (that lands with audio/engine.c in phase 7), so
           draining the bytes is the whole of this branch for now. */
        char    buf[64];
        ssize_t r;
        while ((r = read(event_pipe_read_fd(el), buf, sizeof(buf))) > 0) {
          /* discard */
        }
      } else if (fired[i] == EVSRC_TTY) {
        /* epoll is level-triggered, so an undrained tty spins at 100%
           CPU. read() returning 0 is EOF -- a closed pty that did not
           also deliver SIGHUP, which is the case DESIGN.md VIII.2 warns
           about -- and is treated as a hangup. Phase 11 replaces this
           with tb_peek_event. */
        char    buf[256];
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r == 0) {
          fprintf(stderr, "%s\n", evsrc_name(EVSRC_TTY_HUP));
          running = 0;
        }
      } else if (fired[i] == EVSRC_SIGNAL || fired[i] == EVSRC_TTY_HUP) {
        running = 0;
      }
    }
  }

  event_destroy(el);
  return status;
} /* run_headless */

/*
 * Entry point. Handles --version by printing "kurono <version>" to
 * stdout and returning 0, --paths by resolving the Config and printing
 * it (see print_paths), --headless by running the phase-4 event-loop
 * smoke test (see run_headless) until SIGINT/SIGTERM/SIGHUP or a tty
 * hangup, and --tone by running the phase-5 sink/ring/callback smoke
 * test (sink_run_tone, plan.md VI.8) with no event loop and no audio
 * file. Any other invocation (including no arguments) prints a one-line
 * "not implemented yet" notice to stderr and returns 1 -- startup,
 * wiring, and teardown land in later phases. Takes the process
 * argc/argv, returns the process exit status.
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

  if (argc == 2 && strcmp(argv[1], "--headless") == 0) {
    /* Temporary, plan.md VI.8 diagnostic scaffolding -- removed in
       phase 11 when ui/tui.c gives kurono a real event loop body. */
    return run_headless();
  }

  if (argc == 2 && strcmp(argv[1], "--tone") == 0) {
    /* plan.md VI.8 diagnostic scaffolding -- exercises audio/sink.c's
       device, ring, and callback with no ffmpeg and no audio file. */
    return (sink_run_tone() == 0) ? 0 : 1;
  }

  fprintf(stderr, "kurono: not implemented yet\n");
  return 1;
} /* main */
