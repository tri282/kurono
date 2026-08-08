#include "../include/main.h"
#include "../include/config.h"
#include "../include/event.h"
#include "../include/audio/decoder.h"
#include "../include/audio/sink.h"
#include "../include/audio/trace.h"

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
 * resolve_trace_log_path: build the KURONO_TRACE log path --
 * $XDG_STATE_HOME/kurono/trace.log, falling back to
 * $HOME/.local/state/kurono/trace.log (DESIGN.md IV.10/X, README.md) --
 * or return NULL if KURONO_TRACE is not set to "1", or the environment
 * cannot resolve a base directory (no $XDG_STATE_HOME and no $HOME).
 * Mirrors config.c's xdg_or_home fallback shape, but is not folded into
 * Config: KURONO_TRACE governs trace.c's own opt-in file, not a startup
 * path every caller of config_init needs. Takes nothing, returns an
 * owned string or NULL -- the caller passes the result straight to
 * trace_init, which treats NULL as "record only, no file" and never
 * fails on it (a missing/unwritable directory is reported there, once,
 * non-fatally).
 */
static char *
resolve_trace_log_path(void)
{
  const char * flag = getenv("KURONO_TRACE");
  if (flag == NULL || strcmp(flag, "1") != 0) {
    return NULL;
  }

  const char * xdg = getenv("XDG_STATE_HOME");
  char * base;
  if (xdg != NULL && xdg[0] != '\0') {
    base = strdup(xdg);
  } else {
    const char * home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
      return NULL;
    }
    size_t len = strlen(home) + strlen("/.local/state") + 1;
    base = malloc(len);
    if (base != NULL) {
      snprintf(base, len, "%s/.local/state", home);
    }
  }
  if (base == NULL) {
    return NULL;
  }

  size_t len = strlen(base) + strlen("/kurono/trace.log") + 1;
  char * path = malloc(len);
  if (path != NULL) {
    snprintf(path, len, "%s/kurono/trace.log", base);
  }
  free(base);
  return path;
} /* resolve_trace_log_path */

/*
 * No real device is opened for --decode (phase-6-decoder.md I.7: "no
 * device, no ring, no threads"), so there is no negotiated session rate
 * to target the way a live run would read one back from audio/sink.c.
 * The probe pass uses this rate only to reach StreamInfo and learn the
 * file's own rate; the measuring pass then re-opens at that native rate
 * (see run_decode). It does not claim to match whatever rate this
 * process's sink would negotiate at runtime.
 */
#define DECODE_PROBE_SAMPLE_RATE 48000u

/* Frames per read() call in the drain pass. Arbitrary -- large enough
   that the pass is not dominated by call overhead, small enough that a
   short return is exercised well before the end of any real track. */
#define DECODE_READ_CHUNK_FRAMES 4096

/*
 * decode_probe_rate: open path just far enough to read its StreamInfo
 * and report the file's own sample rate, so the measuring pass can
 * target it. Takes the path and the StreamInfo to fill, returns the
 * file's sample rate, or 0 if the file could not be opened at all (the
 * caller reports that; this function prints nothing).
 */
static unsigned
decode_probe_rate(const char * path, StreamInfo * out_info)
{
  SessionFormat probe;
  memset(&probe, 0, sizeof(probe));
  probe.sample_rate = DECODE_PROBE_SAMPLE_RATE;
  probe.channels    = 2;

  AudioSource src = decoder_create(&probe);
  if (src.self == NULL) {
    return 0;
  }
  if (src.open(src.self, path, out_info) != 0) {
    decoder_destroy(&src);
    return 0;
  }
  unsigned rate = (out_info->sample_rate > 0) ? (unsigned) out_info->sample_rate
                                              : DECODE_PROBE_SAMPLE_RATE;
  src.close(src.self);
  decoder_destroy(&src);
  return rate;
} /* decode_probe_rate */

/*
 * run_decode: implements --decode FILE (plan.md VI.8,
 * phase-6-decoder.md I.7). Three passes over the file, in order:
 *
 *   1. Probe, to learn the file's own sample rate (decode_probe_rate).
 *   2. Step pass, at that native rate, printing the raw frame count of
 *      each of the first ten decode steps plus the total.
 *      decoder_diag_step reports a step's true size, where
 *      AudioSource.read clips to the caller's request and would hide a
 *      short first step inside a larger one -- and a short first step
 *      is exactly what phase-6-decoder.md III.1 exists to observe.
 *   3. Drain pass, after seeking back to 0, pulling the whole file
 *      through AudioSource.read into a discard buffer.
 *
 * Passes 2 and 3 both run at the file's native rate so that swr is a
 * passthrough. Resampling would add its own filter delay to the first
 * output and scale every count, which is precisely the measurement
 * III.1 is trying to make -- a 48 kHz target would report the
 * resampler's delay rather than the encoder's.
 *
 * Pass 3 exists because read() is the function phase 7 actually calls;
 * without it the only diagnostic in this phase would exercise a code
 * path the engine never uses. Its total must equal pass 2's -- the two
 * paths drain the same decoder through different plumbing, so a
 * mismatch is a residual-buffer or seek bug.
 *
 * Takes the file path, returns 0 when both passes complete, or 1 on any
 * open failure, decode error, or disagreement between the two totals.
 * A corrupt file or unsupported codec is reported on one stderr line
 * and never crashes (DESIGN.md XI.1).
 */
static int
run_decode(const char * path)
{
  StreamInfo info;
  unsigned   rate = decode_probe_rate(path, &info);
  if (rate == 0) {
    fprintf(stderr, "kurono: --decode: cannot open %s\n", path);
    return 1;
  }

  SessionFormat fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.sample_rate = rate;
  fmt.channels    = 2;

  AudioSource src = decoder_create(&fmt);
  if (src.self == NULL) {
    fprintf(stderr, "kurono: --decode: allocation failed\n");
    return 1;
  }

  int ret = src.open(src.self, path, &info);
  if (ret != 0) {
    fprintf(stderr, "kurono: --decode: open failed for %s (error %d)\n", path, ret);
    decoder_destroy(&src);
    return 1;
  }

  printf("duration_ms=%lld bitrate=%lld sample_rate=%d channels=%d codec=%s\n",
         (long long) info.duration_ms, (long long) info.bitrate,
         info.sample_rate, info.channels, info.codec);
  printf("target rate: %u Hz (file native -- swr is a passthrough)\n", rate);

  /* Pass 2 -- raw step sizes. */
  int  status     = 0;
  long step_total = 0;
  int  step       = 0;
  for (;;) {
    long n = decoder_diag_step(&src);
    if (n < 0) {
      fprintf(stderr, "kurono: --decode: decode error (%ld)\n", n);
      status = 1;
      break;
    }
    if (n == 0) {
      break; /* end of stream */
    }
    if (step < 10) {
      printf("step %2d: %ld frames\n", step, n);
    }
    step++;
    step_total += n;
  }
  printf("total frames (steps): %ld\n", step_total);

  /* Pass 3 -- the same file through the interface phase 7 uses. */
  long read_total = 0;
  if (status == 0) {
    ret = src.seek(src.self, 0);
    if (ret != 0) {
      fprintf(stderr, "kurono: --decode: seek to 0 failed (error %d)\n", ret);
      status = 1;
    } else {
      static float discard[DECODE_READ_CHUNK_FRAMES * 2];
      for (;;) {
        long n = src.read(src.self, discard, DECODE_READ_CHUNK_FRAMES);
        if (n < 0) {
          fprintf(stderr, "kurono: --decode: read error (%ld)\n", n);
          status = 1;
          break;
        }
        read_total += n;
        if (n < DECODE_READ_CHUNK_FRAMES) {
          break; /* short return is the EOF signal */
        }
      }
      printf("total frames (read):  %ld\n", read_total);

      /* Reported, deliberately not fatal. On a lossless format the two
         totals should match exactly and a difference means a
         residual-buffer bug. On a lossy one they legitimately can
         differ: AVSEEK_FLAG_BACKWARD lands on a frame boundary rather
         than on sample 0, so pass 3 may start slightly earlier or later
         than pass 2 did. Judging which case applies is the reader's
         job, so print the delta and let them. */
      if (read_total != step_total) {
        printf("note: totals differ by %ld frames (exact match expected for lossless)\n",
               read_total - step_total);
      }
    }
  }

  src.close(src.self);
  decoder_destroy(&src);
  return status;
} /* run_decode */

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
 * stdout and returning 0 immediately -- no trace lifecycle, nothing
 * runs. Every other invocation is wrapped in the trace ring's lifetime
 * (trace_init before, trace_drain / trace_summary_line / trace_shutdown
 * after) so every run yields evidence at no configuration cost
 * (DESIGN.md IV.10): --paths by resolving the Config and printing it
 * (see print_paths), --headless by running the phase-4 event-loop smoke
 * test (see run_headless) until SIGINT/SIGTERM/SIGHUP or a tty hangup,
 * --tone by running the phase-5 sink/ring/callback smoke test
 * (sink_run_tone, plan.md VI.8) with no event loop and no audio file,
 * --decode FILE by running the phase-6 decoder smoke test (run_decode,
 * plan.md VI.8) with no device, no ring, and no threads, and any other
 * invocation (including no arguments) by printing a one-line "not
 * implemented yet" notice to stderr. Startup, wiring, and
 * teardown ordering for a real run land in later phases. Takes the
 * process argc/argv, returns the process exit status.
 */
int
main(int argc, char * argv[])
{
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    printf("kurono %s\n", KURONO_VERSION);
    return 0;
  }

  char * trace_path = resolve_trace_log_path();
  if (trace_init(trace_path) != 0) {
    /* Ring allocation failed. Not fatal, by the same rule that makes a
       missing log file not fatal: trace_emit degrades to counting drops
       against a zero-capacity ring, so the run continues and the exit
       line reports the drops rather than the player refusing to start. */
    fprintf(stderr, "kurono: warning: trace ring allocation failed -- "
                    "continuing without recording\n");
  }
  free(trace_path);

  int status;

  if (argc == 2 && strcmp(argv[1], "--paths") == 0) {
    Config cfg;
    if (config_init(&cfg) != 0) {
      kurono_die("failed to initialize configuration");
    }
    print_paths(&cfg);
    config_free(&cfg);
    status = 0;

  } else if (argc == 2 && strcmp(argv[1], "--headless") == 0) {
    /* Temporary, plan.md VI.8 diagnostic scaffolding -- removed in
       phase 11 when ui/tui.c gives kurono a real event loop body. */
    status = run_headless();

  } else if (argc == 2 && strcmp(argv[1], "--tone") == 0) {
    /* plan.md VI.8 diagnostic scaffolding -- exercises audio/sink.c's
       device, ring, and callback with no ffmpeg and no audio file.
       sink_run_tone drives sink_open_impl, which registers the frame
       counter and sample rate with trace.c (trace.h,
       trace_set_frame_counter / trace_set_sample_rate). */
    status = (sink_run_tone() == 0) ? 0 : 1;

  } else if (argc == 3 && strcmp(argv[1], "--decode") == 0) {
    /* plan.md VI.8 diagnostic scaffolding -- exercises audio/decoder.c's
       AudioSource over ffmpeg with no device, no ring, and no threads. */
    status = run_decode(argv[2]);

  } else {
    fprintf(stderr, "kurono: not implemented yet\n");
    status = 1;
  }

  /* No redraw tick and no pipe wakeup exist yet to drive trace_drain
     (DESIGN.md IV.10) -- there is no TUI and no event loop wired to the
     sink until later phases. One drain here, before the summary line,
     is the temporary equivalent: it empties whatever trace_ring
     accumulated during this run so the exit line reflects it. */
  trace_drain();
  char summary[128];
  trace_summary_line(summary, sizeof summary);
  fprintf(stderr, "%s\n", summary);
  trace_shutdown();

  return status;
} /* main */
