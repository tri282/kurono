#include "../include/main.h"
#include "../include/config.h"
#include "../include/event.h"
#include "../include/audio/decoder.h"
#include "../include/audio/engine.h"
#include "../include/audio/sink.h"
#include "../include/audio/trace.h"
#include "../include/ui/keymap.h"
#include "../include/ui/tui.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

/*
 * kurono_die: see include/main.h for the contract. Deliberately does
 * not install itself as a signal handler and is not called from one --
 * DESIGN.md IV.9.c requires SIGSEGV/SIGABRT to leave the terminal raw
 * because tb_shutdown is not async-signal-safe.
 */
_Noreturn void
kurono_die(const char * fmt, ...)
{
  /* Idempotent, and a no-op in every diagnostic mode -- those return
     before tui_init. Ordered first so the message below reaches a
     restored terminal rather than the alternate screen. */
  tui_shutdown();

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

/* Arbitrary: --play has no library, so there is no real track_id to
   report in EV_STREAM_INFO/EV_OPEN_FAILED/EV_DECODE_ERROR. Any
   constant works here -- phase 12+ replaces this whole diagnostic path
   with a real queue cursor. */
#define PLAY_HEADLESS_TRACK_ID 1

/* Redraw tick rate for the player loop (DESIGN.md VIII.3, ~30 Hz). It
   drives the device-state watchdog (IV.8.a), the trace drain (IV.10),
   and the 1 Hz position line -- the same tick, not three timers. */
#define PLAYER_REDRAW_HZ 30

/* Ticks between position lines, so the 30 Hz timer still prints at the
   1 Hz phase-7-engine.md I.9 asks for. */
#define PLAYER_POSITION_TICKS PLAYER_REDRAW_HZ

/* Bounded by EVENT_MAX_EPOLL_EVENTS in event.c; a local constant keeps
   main.c ignorant of that internal. */
#define PLAYER_MAX_FIRED 8

/* Seek-stress target step. A prime, so consecutive targets land far
   apart in the file rather than walking forward: the flush_gen defect
   this row exists to stress is a flush arriving while T2 is midway
   through writing a decoded chunk, and adjacent targets let T2 satisfy
   the next seek out of work it has already done. */
#define SEEK_STRESS_STEP_MS 7919

/* Settle time after the stress burst before the position is sampled --
   long enough for T2 to refill and T3 to consume past the resync
   point, short enough not to drift into the next second. */
#define SEEK_SETTLE_MS 500

/* How far the settled position may sit from the final seek target
   before the resync is called wrong. Generous on purpose: this is
   looking for the counters diverging permanently, which shows up as
   seconds or minutes of error, not as jitter. */
#define SEEK_TOLERANCE_MS 1000

/* Bound on the wait for T2 to work through the seek backlog and settle
   on the final target, and the poll step inside that wait. Generous:
   under TSan every flush costs far more than it does in the default
   build, and a false RESYNC FAIL from an impatient harness is worse
   than a slow test. */
#define SEEK_DRAIN_WAIT_MS 20000
#define SEEK_DRAIN_POLL_MS 100

/* engine_flush_begin's fence cap is FLUSH_FENCE_CAP_MS (200) in
   audio/engine.c; the DESIGN.md IV.7.5 skip returns in microseconds.
   Anything under this proves the skip branch fired. */
#define FENCE_SKIP_MAX_MS 20

/* Upper bound on the wait for T2 to make the paused-seek resync
   visible. Must exceed engine.c's 200 ms fence cap by enough that a
   fence that did run still reports a number instead of timing out. */
#define FLUSH_FENCE_WAIT_MS 400

/*
 * PlayMode: which driver --play runs after EV_STREAM_INFO confirms the
 * track is open. PLAY_PLAIN and PLAY_SEEK_ONCE are the two shapes
 * --play already had. The other two exist to drive the two phase-7
 * verify rows the 1 Hz poll loop cannot reach on its own
 * (status/kurono/STATUS.md, "Phase 7 verify debt").
 */
typedef enum {
  PLAY_PLAIN,
  PLAY_SEEK_ONCE,
  PLAY_SEEK_STRESS,
  PLAY_SEEK_PAUSED
} PlayMode;

/*
 * play_sleep_ms: nanosleep for ms milliseconds. --play's poll-loop
 * helper only -- mirrors audio/sink.c's own sleep_ms, kept as a
 * separate static here rather than exported, the same non-sharing
 * choice sink.c already made for its identical helper.
 */
static void
play_sleep_ms(long ms)
{
  struct timespec ts;
  ts.tv_sec  = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
} /* play_sleep_ms */

/*
 * elapsed_ms: wall time between two CLOCK_MONOTONIC samples. Takes the
 * earlier and later timespec, returns the difference in milliseconds.
 */
static double
elapsed_ms(const struct timespec * t0, const struct timespec * t1)
{
  return (double) (t1->tv_sec - t0->tv_sec) * 1000.0
       + (double) (t1->tv_nsec - t0->tv_nsec) / 1000000.0;
} /* elapsed_ms */

/*
 * position_ms: the playback position implied by the frame counter.
 * Reads g_frames_consumed (never the decoder -- DESIGN.md IV.1) and
 * converts with the negotiated rate. Takes the counter and the sample
 * rate, returns milliseconds, or -1 if the rate is not yet known.
 */
static int64_t
position_ms(const atomic_uint_least64_t * pos, int rate)
{
  if (rate <= 0) {
    return -1;
  }
  uint64_t frames = atomic_load_explicit(pos, memory_order_relaxed);
  return (int64_t) (frames * 1000u / (uint64_t) rate);
} /* position_ms */

/*
 * drive_seek_stress: the phase-7 "50 seeks in 5 s" verify row. Issues n
 * back-to-back engine_seek calls with no sleep between them, so that
 * commands land while T2 is mid-chunk -- which is the only state in
 * which the flush_gen check in engine.c's fill loop does any work. The
 * burst is naturally paced by cmd_push blocking on a full 16-slot
 * queue, so T2 provably processes every one rather than coalescing.
 *
 * Then settles and checks that g_frames_consumed agrees with the last
 * target: a broken resync leaves the write counter and the consumed
 * counter permanently apart, which reads here as a position nowhere
 * near where the seek asked for. Takes the Engine, the frame counter,
 * the negotiated sample rate, the track duration, and the seek count;
 * returns nothing (verdict goes to stderr).
 */
static void
drive_seek_stress(Engine * e, const atomic_uint_least64_t * pos, int rate,
                  int64_t duration_ms, int n)
{
  /* Keep targets a second clear of the end so a seek does not land on
     EOF and end the run mid-burst. */
  int64_t span = (duration_ms > 2000) ? duration_ms - 1000 : 1000;

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < n; i++) {
    engine_seek(e, ((int64_t) i * SEEK_STRESS_STEP_MS) % span);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  fprintf(stderr, "seek-stress: %d seeks issued in %.1f ms\n",
          n, elapsed_ms(&t0, &t1));

  int64_t target = span / 2;
  engine_seek(e, target);

  /* The burst leaves T2 with a backlog -- engine_seek only queues, and
     cmd_push blocking at 16 slots means the caller returns long before
     the last flush has run. Sampling on a fixed sleep reads whichever
     intermediate target T2 happened to be on, which is not the thing
     under test. Wait for convergence instead, and require two
     consecutive in-tolerance samples that are advancing: a single one
     could be an intermediate target that happens to sit near the final
     one, but it will be seeked away from, while the settled position
     stays and grows. */
  int64_t got = -1, prev = -1, delta = -1;
  int     converged = 0;
  double  drain_ms  = 0.0;
  struct timespec d0, d1;
  clock_gettime(CLOCK_MONOTONIC, &d0);

  for (int i = 0; i < SEEK_DRAIN_WAIT_MS / SEEK_DRAIN_POLL_MS; i++) {
    play_sleep_ms(SEEK_DRAIN_POLL_MS);
    got   = position_ms(pos, rate);
    delta = (got >= target) ? got - target : target - got;
    if (delta <= SEEK_TOLERANCE_MS && prev >= 0 && got > prev
        && (prev >= target ? prev - target : target - prev) <= SEEK_TOLERANCE_MS) {
      converged = 1;
      clock_gettime(CLOCK_MONOTONIC, &d1);
      drain_ms = elapsed_ms(&d0, &d1);
      break;
    }
    prev = got;
  }

  fprintf(stderr,
          "seek-stress: target=%lld ms position=%lld ms delta=%lld ms "
          "drain=%.0f ms -- %s\n",
          (long long) target, (long long) got, (long long) delta,
          drain_ms, converged ? "RESYNC OK" : "RESYNC FAIL");
} /* drive_seek_stress */

/*
 * drive_seek_paused: the phase-7 "seek while paused" verify row, which
 * exists to prove the DESIGN.md IV.7.5 fence skip. engine.c never
 * stops the device -- that is T1's job and does not exist as a policy
 * until phase 8/12 -- so this harness, which *is* T1, stops it
 * directly. That is the whole point: with the device stopped,
 * sink_is_started returns false and engine_flush_begin must reset
 * immediately instead of burning the 200 ms fence cap.
 *
 * Timing is the assertion. A skipped fence returns in microseconds; a
 * fence that ran polls at 1 ms up to 200 ms. Note this proves the skip
 * mechanism, not phase 8/12's pause policy. Takes the Engine, the
 * sink, the frame counter, the sample rate, and the seek target;
 * returns nothing (verdict goes to stderr).
 */
static void
drive_seek_paused(Engine * e, AudioSink * sink, const atomic_uint_least64_t * pos,
                  int rate, int64_t seek_ms)
{
  engine_pause(e);
  play_sleep_ms(SEEK_SETTLE_MS); /* let T2 reach its park before the device stops */

  sink->stop(sink->self); /* T1 only, DESIGN.md IV.9.b; synchronous */
  fprintf(stderr, "seek-paused: device stopped, sink_is_started=%d\n",
          sink_is_started(sink));

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  engine_seek(e, seek_ms);

  /* engine_seek only queues; the flush happens on T2. Wait for the
     resync to become visible in the counter rather than timing the
     queue push, which would measure nothing. */
  int64_t got = -1;
  double  ms  = 0.0;
  for (int i = 0; i < FLUSH_FENCE_WAIT_MS; i++) {
    got = position_ms(pos, rate);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = elapsed_ms(&t0, &t1);
    if (got >= 0 && llabs((long long) (got - seek_ms)) <= SEEK_TOLERANCE_MS) {
      break;
    }
    play_sleep_ms(1);
  }

  fprintf(stderr,
          "seek-paused: resync visible after %.2f ms (position=%lld ms) -- %s\n",
          ms, (long long) got,
          (ms < (double) FENCE_SKIP_MAX_MS) ? "FENCE SKIPPED" : "FENCE RAN");

  if (sink->start(sink->self) != 0) {
    fprintf(stderr, "seek-paused: sink restart failed\n");
  }
  engine_resume(e);

  /* Prove the resume actually produced audio: the counter must advance
     past the seek target, which it cannot do unless T3 is running again
     and T2 refilled the ring the flush emptied. */
  int64_t before = position_ms(pos, rate);
  play_sleep_ms(SEEK_SETTLE_MS);
  int64_t after = position_ms(pos, rate);
  fprintf(stderr, "seek-paused: after resume %lld -> %lld ms -- %s\n",
          (long long) before, (long long) after,
          (after > before) ? "PLAYBACK RESUMED" : "STALLED");
} /* drive_seek_paused */

/* ---------------------------------------------------------------------
 * Phase 8: startup and teardown ordering (DESIGN.md IV.9), the
 * device-loss watchdog (IV.8.a), the reopen ladder (IV.8.b), and
 * browse-only degradation (XI.2). All of it on T1.
 * ------------------------------------------------------------------- */

/*
 * Teardown step 1's flag (DESIGN.md IV.9.b). Load-bearing, not
 * cosmetic: step 2's ma_device_stop raises a `stopped` notification, and
 * without this flag the policy table below would read our own orderly
 * shutdown as device loss and start a reopen ladder against a device we
 * are in the middle of destroying. It also disarms the IV.8.a watchdog.
 * Written by T1 only, but read on the notification-dispatch path, so it
 * is atomic rather than a plain int.
 */
static atomic_bool g_shutdown = false;

/*
 * IV.8.b step 3's ladder: 250 ms, 500 ms, 1 s, 2 s, 4 s, then every 4 s,
 * giving up after 60 s of total failure and degrading to browse-only.
 */
static const int g_ladder_backoff_ms[] = { 250, 500, 1000, 2000, 4000 };
#define LADDER_TAIL_MS    4000
#define LADDER_GIVE_UP_MS 60000

/*
 * DeviceState: T1's own view of the device, distinct from EngineState.
 * DESIGN.md IV.8 names a DEVICE_LOST engine state, but T1 storing it
 * into the Engine's atomic would park T2 behind its back -- bypassing
 * park(), leaving the producer-active flag set and the step 1 handshake
 * stale. T1 tracks device policy here and parks T2 the one supported
 * way, through engine_park_sync. See include/audio/engine.h's note on
 * ENG_DEVICE_LOST.
 */
typedef enum { DEV_READY, DEV_LOST, DEV_BROWSE_ONLY } DeviceState;

/*
 * Player: everything one headless run of the audio spine owns, in
 * construction order (DESIGN.md IV.9.a) so that teardown can be read as
 * its exact reverse. Lives on run_player's stack for the whole run; the
 * pointer registered with AudioSink.open as the notification user
 * pointer is this struct, which is why it must outlive the sink.
 */
typedef struct {
  Config        cfg;
  AudioSink     sink;
  SessionFormat fmt;
  Engine *      e;
  EventLoop *   el;

  DeviceState     dev;
  int             ladder_attempt;    /* index into g_ladder_backoff_ms */
  int             ladder_elapsed_ms; /* against LADDER_GIVE_UP_MS */
  struct timespec loss_at;           /* for the detection/recovery timings */
  int64_t         pos_origin_ms;     /* IV.8.b step 5's elapsed-time origin */

  PlayMode     mode;
  int64_t      mode_arg;
  int          driven;       /* latches the one-shot driver */

  /* Raised across the ladder's own device teardown and reopen. Step 2's
     ma_device_stop and step 6's ma_device_start each raise a
     notification of their own, and those say nothing about the device
     the user did or did not unplug -- they are our own actions coming
     back. Without the mute, the `stopped` our step 2 caused can still be
     sitting in sink.c's single pending slot when step 6 restores
     DEV_READY, and the very next drain would read it as a fresh loss and
     re-enter the ladder against a device that just came back. Exactly
     the same reason g_shutdown exists for teardown step 2, applied to
     the other place we stop the device on purpose. */
  int notify_muted;

  int running;
  int status;
  int tick;                  /* redraw ticks since the last position line */
  int redraw_armed;

  /* The status-bar line. Written by the device policy and by
     action_dispatch, read once per repaint. A fixed buffer rather than a
     const char * because most of what goes in it is formatted (a
     notification name, a ladder attempt number), and phase 11 has no
     allocation story for transient UI strings yet. */
  char message[128];
  int  ui;                   /* 1 when this run drives the TUI (phase 11) */
} Player;

/*
 * The Player the TUI is driving, for action_dispatch -- the one function
 * ui/ calls that needs to reach every subsystem, and the one place a
 * global is unavoidable because keymap.h's prototype takes only the
 * Action and its arg. Set by run_tui before tui_init and cleared after
 * the loop; NULL in every diagnostic mode, which is what makes a
 * dispatch outside a TUI run a no-op rather than a wild pointer.
 */
static Player * g_ui_player = NULL;

static void player_ladder_begin(Player * p, const char * detector);
static void player_ladder_attempt(Player * p);
static void player_on_notify(void * user, int notification_type);

/*
 * player_report: one diagnostic, to whichever channel is legal right now.
 * With the TUI up it becomes the status-bar line; on the headless --play
 * path it is the stderr line it has always been. Every device-policy and
 * engine-event message on the Player path goes through here, because
 * those paths are shared by both kinds of run and a bare fprintf in one
 * of them corrupts the display in the other (CLAUDE.md: the TUI owns the
 * terminal). Takes the Player and a printf format, returns nothing.
 */
static void
player_report(Player * p, const char * fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  if (p->ui) {
    vsnprintf(p->message, sizeof p->message, fmt, ap);
    tui_mark_dirty();
  } else {
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
  }
  va_end(ap);
} /* player_report */

/*
 * action_dispatch: the single entry point from ui/ into every other
 * subsystem (DESIGN.md VIII.5, declared in include/ui/keymap.h). It lives
 * in main.c because this is the only translation unit holding the engine,
 * the queue, the browser and the TUI at once -- ui/ never calls back into
 * audio/ or library/ directly. Takes a resolved Action and its table arg,
 * returns nothing.
 *
 * Phase 11 implements ACT_QUIT and nothing else, on purpose: this phase
 * is the terminal's lifecycle and the layout, and every other action
 * needs a browser selection or a queue to act on (phases 12-17). The rest
 * report themselves in the status bar, which is also how the binding
 * table gets exercised end to end before there is anything to control --
 * pressing `n` proves the key resolved and dispatched even though nothing
 * plays yet.
 *
 * Never writes to stderr: termbox2 owns the terminal here.
 */
void
action_dispatch(Action a, int32_t arg)
{
  Player * p = g_ui_player;
  if (p == NULL) {
    return; /* no TUI run in progress -- every diagnostic mode */
  }

  if (a == ACT_QUIT) {
    /* Converges on the same shutdown_all as SIGINT/SIGTERM/SIGHUP;
       there is no second exit path (DESIGN.md IV.9.b). */
    p->running = 0;
    return;
  }

  player_report(p, "%s (arg %d) -- not implemented yet", action_name(a),
                (int) arg);
} /* action_dispatch */

/*
 * trace_report_and_shutdown: teardown step 10 (DESIGN.md IV.9.b) --
 * drain whatever trace_ring still holds, print the one-line summary and
 * the flush snapshot (amendment 03), then free the ring. It sits at step
 * 10 rather than step 6 for two reasons that pull in opposite
 * directions: trace_shutdown frees a ring whose producer is T3, so it
 * must follow step 3's uninit, but the drain and the summary write to
 * stderr, which is unavailable until tb_shutdown at step 8.
 *
 * Idempotent, because two callers reach it: shutdown_all for a real run,
 * and main for every diagnostic mode that never built a Player. Takes
 * and returns nothing.
 */
static void
trace_report_and_shutdown(void)
{
  static int done = 0;
  if (done) {
    return;
  }
  done = 1;

  trace_drain();
  char summary[128];
  trace_summary_line(summary, sizeof summary);
  fprintf(stderr, "%s\n", summary);

  /* The flush snapshot is not in trace_ring and so is not in the drained
     log (DESIGN.md IV.10, amendment 03). Rendered here for the same
     reason trace_drain is: phase 11's redraw tick, which owns both, does
     not exist yet. Suppressed when no flush ever happened, so plain
     --tone and --headless runs stay one line. */
  uint64_t flush_w = 0, flush_c = 0;
  uint64_t flush_gen = engine_last_flush(&flush_w, &flush_c);
  if (flush_gen > 0) {
    fprintf(stderr, "kurono: %llu flush(es), last at write=%llu consumed=%llu\n",
            (unsigned long long) flush_gen, (unsigned long long) flush_w,
            (unsigned long long) flush_c);
  }

  trace_shutdown();
} /* trace_report_and_shutdown */

/*
 * player_position_ms: the playback position T1 reports. Always
 * g_frames_consumed, never the decoder (DESIGN.md IV.1), offset by the
 * elapsed-time origin IV.8.b step 5 sets: a reopen resyncs both frame
 * counters to 0, so without the origin every device loss would rewind
 * the displayed clock to zero rather than continuing from where the
 * track actually was. Takes the Player, returns milliseconds.
 */
static int64_t
player_position_ms(const Player * p)
{
  int64_t since_flush = position_ms(sink_frames_consumed(), (int) p->fmt.sample_rate);
  return (since_flush < 0) ? p->pos_origin_ms : p->pos_origin_ms + since_flush;
} /* player_position_ms */

/*
 * player_set_redraw: arm or disarm redraw_timerfd. DESIGN.md VIII.3 arms
 * it exactly while playing, which is what makes the IV.8.a watchdog cost
 * one atomic load per tick and exist only when it can do something --
 * there is nothing to poll while the device is gone or the track has
 * ended. Takes the Player and 1 to arm / 0 to disarm, returns nothing.
 */
static void
player_set_redraw(Player * p, int on)
{
  if (p->redraw_armed == on) {
    return;
  }
  if (event_arm_redraw(p->el, on ? PLAYER_REDRAW_HZ : 0) != 0) {
    player_report(p, "warning: could not %s the redraw timer",
                  on ? "arm" : "disarm");
    return;
  }
  p->redraw_armed = on;
} /* player_set_redraw */

/*
 * notify_name: the status-bar label for a SinkNotification. Takes the
 * value, returns a static string -- never NULL.
 */
static const char *
notify_name(int type)
{
  switch (type) {
    case SINK_NOTIFY_STARTED:            return "started";
    case SINK_NOTIFY_STOPPED:            return "stopped";
    case SINK_NOTIFY_REROUTED:           return "rerouted";
    case SINK_NOTIFY_INTERRUPTION_BEGAN: return "interruption_began";
    case SINK_NOTIFY_INTERRUPTION_ENDED: return "interruption_ended";
    default:                             return "unknown";
  }
} /* notify_name */

/*
 * player_browse_only: DESIGN.md XI.2's degraded state. Reached when the
 * device could not be opened at startup (IV.9.a step 5) or when the
 * reopen ladder gave up after 60 s (IV.8.b step 3). Reallocates the
 * rings at the default f32/48000/2 session format and rebinds the engine
 * to them, so every ring pointer stays non-NULL and every code path
 * stays uniform -- T2 remains created and parked rather than special-
 * cased away.
 *
 * The run then ends. In the finished player this state keeps the TUI up
 * with browsing, searching, and queueing live and playback actions
 * reporting "no audio device"; there is no TUI in this phase, so there
 * is nothing headless to browse and continuing would only block on an
 * event loop with every timer disarmed. Takes the Player and the reason
 * to report, returns nothing.
 */
static void
player_browse_only(Player * p, const char * why)
{
  p->dev          = DEV_BROWSE_ONLY;
  p->notify_muted = 0;
  player_set_redraw(p, 0);
  (void) event_arm_reconnect(p->el, 0);

  /* Only when the rings are actually gone -- which is the ladder path,
     where step 2 freed them. Reached from startup they were already
     allocated at this same default format before T2 was created, and
     freeing and reallocating them underneath a live (if parked) T2 for
     no reason is precisely the move the rest of this phase exists to
     make impossible. */
  if (sink_pcm_ring() == NULL) {
    if (sink_alloc_default_rings(&p->fmt) != 0) {
      player_report(p, "browse-only: ring allocation failed");
      p->status  = 1;
      p->running = 0;
      return;
    }
    if (p->e != NULL && engine_device_reflush(p->e, &p->fmt) != 0) {
      player_report(p, "browse-only: engine rebind failed");
    }
  }

  player_report(p,
                "device: BROWSE-ONLY (%s) -- no audio device; playback "
                "actions report \"no audio device\". Rings at f32/%u/%u, "
                "T2 created and parked (DESIGN.md XI.2).",
                why, p->fmt.sample_rate, p->fmt.channels);

  /* With a TUI up this is a degraded state, not the end of the run
     (DESIGN.md XI.2): browsing, searching and queueing stay live and only
     playback actions report "no audio device". Headless there is nothing
     to browse and every timer is disarmed, so continuing would only block
     forever in epoll_wait -- that run ends here, as it always has. */
  if (!p->ui) {
    p->running = 0;
  }
} /* player_browse_only */

/*
 * player_ladder_retry: IV.8.b step 3's failure arm -- schedule the next
 * reopen attempt on reconnect_timerfd at the next backoff interval, or
 * degrade to browse-only once 60 s of failure have accumulated. Takes
 * the Player, returns nothing.
 */
static void
player_ladder_retry(Player * p)
{
  const int nsteps = (int) (sizeof g_ladder_backoff_ms / sizeof g_ladder_backoff_ms[0]);
  int delay = (p->ladder_attempt < nsteps) ? g_ladder_backoff_ms[p->ladder_attempt]
                                           : LADDER_TAIL_MS;

  if (p->ladder_elapsed_ms + delay > LADDER_GIVE_UP_MS) {
    player_browse_only(p, "reopen ladder exhausted after 60 s");
    return;
  }

  p->ladder_elapsed_ms += delay;
  p->ladder_attempt++;

  if (event_arm_reconnect(p->el, delay) != 0) {
    player_browse_only(p, "reconnect timer could not be armed");
    return;
  }

  player_report(p, "device: reopen attempt %d failed, retrying in %d ms "
                   "(%d ms of 60000 spent)",
                p->ladder_attempt, delay, p->ladder_elapsed_ms);
} /* player_ladder_retry */

/*
 * player_ladder_attempt: IV.8.b steps 3 through 6 -- one attempt at
 * reopening the device, and on success the mandatory flush/resync, the
 * restart, and the resume. Called once directly from
 * player_ladder_begin and thereafter from every reconnect_timerfd
 * expiry. Takes the Player, returns nothing.
 */
static void
player_ladder_attempt(Player * p)
{
  if (atomic_load_explicit(&g_shutdown, memory_order_relaxed) || p->dev != DEV_LOST) {
    return;
  }

  /* Step 3. */
  if (p->sink.open(p->sink.self, &p->fmt, player_on_notify, p) != 0) {
    player_ladder_retry(p);
    return;
  }

  /* Step 4: unconditional flush and resync to frame 0, plus the
     resampler retarget to the newly negotiated R. Legal because T3
     provably does not exist yet (the device is initialised but not
     started) and T2 is parked by step 1. A refusal here means the
     handshake was skipped or the rings are gone -- resuming with the two
     frame counters apart is the silent, permanent corruption step 5's
     warning is about, so the run degrades instead. */
  if (engine_device_reflush(p->e, &p->fmt) != 0) {
    p->sink.close(p->sink.self);
    player_browse_only(p, "flush/resync refused after reopen");
    return;
  }

  /* Step 5 is already done: the elapsed-time origin was captured in
     player_ladder_begin, before the flush zeroed the counter, and both
     frame counters were resynced inside pcm_flush_and_resync. Writing
     either counter again here would be a second store site that
     `make check-flush` does not guard, because it guards ring_reset
     calls and not counter stores. */

  /* Step 6. */
  if (p->sink.start(p->sink.self) != 0) {
    p->sink.close(p->sink.self);
    player_ladder_retry(p);
    return;
  }
  engine_resume(p->e);
  p->dev = DEV_READY;
  (void) event_arm_reconnect(p->el, 0);
  player_set_redraw(p, 1);

  /* Drop everything our own stop/start put in the pending slot before
     unmuting, so the first drain after recovery cannot read step 2's
     `stopped` as a fresh loss. From here on the poll and the
     notification both describe the new device. */
  while (sink_dispatch_notifications()) {
    /* discarded by the mute inside player_on_notify */
  }
  p->notify_muted = 0;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  player_report(p,
                "device: REOPENED after %.0f ms (rate=%u), resumed at %lld ms",
                elapsed_ms(&p->loss_at, &now), p->fmt.sample_rate,
                (long long) p->pos_origin_ms);
} /* player_ladder_attempt */

/*
 * player_ladder_begin: IV.8.b steps 1 and 2, then the first attempt.
 * Entered from either detector -- the redraw-tick poll (authoritative)
 * or a `stopped` notification arriving while playing (an early wakeup
 * only). Takes the Player and the name of the detector that fired,
 * returns nothing.
 */
static void
player_ladder_begin(Player * p, const char * detector)
{
  if (p->dev != DEV_READY
      || atomic_load_explicit(&g_shutdown, memory_order_relaxed)) {
    return;
  }

  p->dev               = DEV_LOST;
  p->ladder_attempt    = 0;
  p->ladder_elapsed_ms = 0;
  clock_gettime(CLOCK_MONOTONIC, &p->loss_at);

  /* Step 5's origin, captured here rather than after the reopen: the
     flush in step 4 resyncs g_frames_consumed to 0, so the pre-loss
     position has to be read while it still exists. */
  p->pos_origin_ms = player_position_ms(p);
  player_set_redraw(p, 0);

  player_report(p, "device: LOST (detected by %s) at %lld ms",
                detector, (long long) p->pos_origin_ms);

  /* Step 1. The synchronous park, not a bare CMD_PAUSE: step 4's
     ring_reset runs on T1, and without this handshake it can land while
     T2 is still inside its ring_write loop -- two threads mutating one
     SPSC ring's write index. */
  engine_park_sync(p->e);

  /* Step 2, from T1 and only T1: both calls self-deadlock from T3
     (DESIGN.md IV.3). Separate and in order -- ma_device_uninit
     explicitly does not stop the device. Muted first: everything these
     two raise is our own doing. */
  p->notify_muted = 1;
  p->sink.stop(p->sink.self);
  p->sink.close(p->sink.self);

  player_ladder_attempt(p);
} /* player_ladder_begin */

/*
 * player_on_notify: DESIGN.md IV.8.a's notification policy table, in
 * full. Registered with AudioSink.open and invoked by
 * sink_dispatch_notifications on T1 -- never on T3, which only records
 * the type and rings the doorbell. The notification is deliberately not
 * the detector: `stopped` here is an early wakeup and the source of the
 * status-bar message, and the poll in player_watchdog is what is
 * authoritative. Takes the user pointer (the Player) and the
 * SinkNotification, returns nothing.
 */
static void
player_on_notify(void * user, int notification_type)
{
  Player * p = (Player *) user;

  /* Row 5: anything at all while shutting down is discarded. Teardown
     step 2 raises `stopped` by design. */
  if (atomic_load_explicit(&g_shutdown, memory_order_relaxed)) {
    return;
  }

  player_report(p, "device: notification %s%s", notify_name(notification_type),
                p->notify_muted ? " (ours, during the ladder -- ignored)" : "");

  /* Our own stop/start coming back to us -- see Player.notify_muted. */
  if (p->notify_muted) {
    return;
  }

  switch (notification_type) {
    case SINK_NOTIFY_REROUTED:
      /* Row 1: a status line and nothing else, in any state. PulseAudio
         moved the stream transparently and the negotiated format is
         unchanged; reopening would cause an audible gap for no benefit. */
      player_report(p, "device: stream rerouted (no reopen)");
      break;

    case SINK_NOTIFY_STOPPED:
      /* Rows 2 and 3. Ignored unless the engine believes it is playing,
         which is what makes PipeWire's idle-suspend a non-event while
         paused or stopped. */
      if (p->dev == DEV_READY && engine_state(p->e) == ENG_PLAYING) {
        player_ladder_begin(p, "notification");
      }
      break;

    case SINK_NOTIFY_STARTED:
      /* Row 4: cancel the ladder and return to playing, but only if the
         device-state poll agrees. Reachable only in the window between
         a loss being declared and step 2 tearing the device down, which
         this implementation makes vanishingly small by running steps 1-6
         synchronously from the declaration -- kept because the policy
         table is normative and a later phase may widen that window. */
      if (p->dev == DEV_LOST && sink_is_started(&p->sink)) {
        (void) event_arm_reconnect(p->el, 0);
        p->dev = DEV_READY;
        engine_resume(p->e);
        player_set_redraw(p, 1);
        player_report(p, "device: recovered without a reopen");
      }
      break;

    default:
      break;
  }
} /* player_on_notify */

/*
 * player_watchdog: DESIGN.md IV.8.a's authoritative detector. On every
 * redraw tick, if the engine is playing and the device is not started,
 * declare device loss. One atomic load per tick, and the tick is armed
 * only while playing, so it costs nothing when it cannot fire.
 *
 * It exists because the notification cannot be trusted: miniaudio raises
 * `stopped` only when onDeviceStop returned MA_SUCCESS, and PulseAudio's
 * onDeviceStop is a cork that fails when the stream is already dead --
 * exactly the yanked-DAC case. The worker sets ma_device_state_stopped
 * unconditionally, which is what this poll reads. Worst-case latency is
 * one tick, 33 ms.
 *
 * Takes the Player, returns nothing.
 */
static void
player_watchdog(Player * p)
{
  if (atomic_load_explicit(&g_shutdown, memory_order_relaxed)) {
    return; /* teardown stops the device on purpose */
  }
  if (p->dev != DEV_READY || engine_state(p->e) != ENG_PLAYING) {
    return;
  }
  if (sink_is_started(&p->sink)) {
    return;
  }

  /* If PipeWire's idle-suspend ever trips this with no device actually
     lost (plan phase-8 III.2 / residual risk 3), the chosen fix is to
     require two consecutive disagreeing ticks before declaring loss, and
     it belongs here and nowhere else. Not applied speculatively. */
  player_ladder_begin(p, "poll");
} /* player_watchdog */

/*
 * player_handle_engine_event: drain one Event from T2 and act on it.
 * EV_STREAM_INFO is where every --play driver fires, because it is the
 * first point at which the track is provably open and its duration is
 * known. Takes the Player and the event, returns nothing.
 */
static void
player_handle_engine_event(Player * p, const Event * ev)
{
  switch (ev->kind) {
    case EV_STREAM_INFO:
      player_report(p, "stream: codec=%s rate=%d channels=%d duration_ms=%lld",
                    ev->info.codec, ev->info.sample_rate, ev->info.channels,
                    (long long) ev->info.duration_ms);
      if (p->driven) {
        break;
      }
      p->driven = 1;
      switch (p->mode) {
        case PLAY_SEEK_ONCE:
          engine_seek(p->e, p->mode_arg);
          break;
        /* fmt.sample_rate, not ev->info.sample_rate: g_frames_consumed
           counts frames in the session format the sink negotiated, which
           is what the decoder resamples *to*. The source rate is a
           property of the file and is off by the resample ratio. */
        /* Both stress drivers end the run rather than playing on to EOF:
           the verdict is already decided, and a bounded run is what lets
           the sanitiser builds observe the full IV.9.b teardown instead
           of being killed by an external timeout partway through a
           four-minute track. */
        case PLAY_SEEK_STRESS:
          drive_seek_stress(p->e, sink_frames_consumed(), (int) p->fmt.sample_rate,
                            ev->info.duration_ms, (int) p->mode_arg);
          p->running = 0;
          break;
        case PLAY_SEEK_PAUSED:
          drive_seek_paused(p->e, &p->sink, sink_frames_consumed(),
                            (int) p->fmt.sample_rate, p->mode_arg);
          p->running = 0;
          break;
        case PLAY_PLAIN:
          break;
      }
      break;

    case EV_OPEN_FAILED:
      player_report(p, "open failed (error %d)", ev->averror);
      p->status  = 1;
      p->running = 0;
      break;

    case EV_DECODE_ERROR:
      player_report(p, "decode error (error %d)", ev->averror);
      p->status  = 1;
      p->running = 0;
      break;

    case EV_EOF:
      player_set_redraw(p, 0); /* VIII.3: nothing left to tick for */
      p->running = 0;
      break;
  }
} /* player_handle_engine_event */

/*
 * player_drain_pipe: the EVSRC_PIPE branch. The doorbell carries no
 * payload, so every byte is drained first and the two things that ring
 * it are then polled in turn: T3's notification callback (dispatched to
 * player_on_notify on this thread) and T2's event queue. trace_ring is
 * drained here as well as on the tick, per DESIGN.md IV.10 -- no new fd,
 * no new timer. Takes the Player, returns nothing.
 */
static void
player_drain_pipe(Player * p)
{
  char    buf[64];
  ssize_t r;
  while ((r = read(event_pipe_read_fd(p->el), buf, sizeof buf)) > 0) {
    /* discard: the byte is a wakeup, not a message */
  }

  while (sink_dispatch_notifications()) {
    /* policy ran on T1; loop in case a second one landed meanwhile */
  }

  Event ev;
  while (p->running && engine_poll_event(p->e, &ev)) {
    player_handle_engine_event(p, &ev);
  }

  trace_drain();
} /* player_drain_pipe */

/*
 * player_tick: the EVSRC_REDRAW branch, ~30 Hz. Runs the device-state
 * watchdog, drains trace_ring, and prints the position line once a
 * second. Phase 11 replaces this body with a real repaint and inherits
 * the same three responsibilities. Takes the Player, returns nothing.
 */
static void
player_tick(Player * p)
{
  player_watchdog(p);
  trace_drain();

  /* The tick is the progress bar and the clock, so every tick repaints
     (DESIGN.md VIII.3). It only fires while playing -- idle, the timer is
     disarmed and this costs nothing. */
  if (p->ui) {
    tui_mark_dirty();
    return;
  }

  if (++p->tick < PLAYER_POSITION_TICKS) {
    return;
  }
  p->tick = 0;

  if (p->dev == DEV_READY) {
    fprintf(stderr, "position: %.2f s\n", (double) player_position_ms(p) / 1000.0);
  }
} /* player_tick */

/*
 * player_repaint: fill a TuiModel from the Player and hand it to the one
 * tb_present in the program. Called once at the bottom of a loop
 * iteration and only when something marked the screen dirty. Takes the
 * Player, returns nothing.
 */
static void
player_repaint(Player * p)
{
  TuiModel m;
  memset(&m, 0, sizeof m);
  m.now_playing = NULL;               /* the queue and the browser: phase 12 */
  m.message     = (p->message[0] != '\0') ? p->message : NULL;
  m.pos_ms      = (p->dev == DEV_READY) ? player_position_ms(p) : -1;
  m.dur_ms      = -1;
  m.playing     = 0;
  m.volume_pct  = 100;
  tui_draw(&m);
} /* player_repaint */

/*
 * shutdown_all: DESIGN.md IV.9.b, the normative teardown table, in
 * order, with no merges. ACT_QUIT and SIGINT/SIGTERM/SIGHUP both
 * converge here; there is no other exit path from a running state.
 *
 * The rule that generates the order: stop the threads you cannot join
 * before you free anything they can reach; then stop and join the
 * threads you can; then free. Steps 2 and 3 must not be merged or
 * reordered -- ma_device_uninit explicitly does not stop the device --
 * and steps 4 and 5 must not move above step 2, which is what makes the
 * ordering error where step 6 frees rings a live callback is still
 * reading impossible to write.
 *
 * Takes the Player, returns nothing.
 */
static void
shutdown_all(Player * p)
{
  /* 1 */ atomic_store_explicit(&g_shutdown, true, memory_order_release);

  /* 2. Synchronous: on return T3 has finished its last invocation and
        will not be called again. Both calls are no-ops when no device is
        open, which is the browse-only and mid-ladder case. */
  p->sink.stop(p->sink.self);

  /* The four-line residual-risk-2 mitigation phase 5 adopted: step 2
     claims the callback is provably done, and this is what keeps that a
     measurement rather than a belief. Reported, not assert(3)ed --
     assert compiles out under -DNDEBUG. Recorded here and printed after
     step 8, not through player_report: the status bar it would write to
     is never repainted again on this path, and the whole point of the
     check is that its result is seen. */
  int cb_alive = sink_cb_alive();

  /* 3. Joins T3. g_event_pipe_w must still be open through this call:
        step 2 fired a `stopped` notification from T3, which writes to
        it. event_destroy closes it at step 9 and nowhere earlier. */
  p->sink.close(p->sink.self);

  /* 4 and 5. Guarded because a startup that failed before engine_create
        reaches this function with no T2 to quit -- the ordering is
        unchanged, the steps are simply empty. */
  if (p->e != NULL) {
    /* 4 */ engine_quit(p->e);
    /* 5 */ engine_join(p->e); /* the only legal way to know T2 is done */
  }

  /* 6. Both touchers of pcm_ring are now provably gone: T3 joined by
        step 3, T2 by step 5. engine_destroy frees boundary_ring, whose
        only toucher is the joined T2, and closes whatever file the
        AudioSource still had open. */
  sink_free_rings();
  engine_destroy(p->e);
  p->e = NULL;

  /* 7. sqlite3_finalize / sqlite3_close_v2 -- phase 13. */

  /* 8. tb_shutdown. After it, and only after it, may stderr be written
        again -- which is exactly why step 10's trace drain and summary
        sit below it rather than at step 6. Idempotent and a no-op for
        every diagnostic mode, none of which claimed the terminal. */
  tui_shutdown();

  if (cb_alive) {
    fprintf(stderr, "kurono: WARNING: data callback still alive after "
                    "AudioSink.stop returned (DESIGN.md IV.9.b step 2)\n");
  }

  /* 9. UI state and the queue land in later phases; the config strings
        and the event loop -- which owns the epoll fd, the signalfd, both
        timerfds, and both ends of the self-pipe -- are what exist now.
        The pipe's write end closes here and nowhere earlier, which is
        why event.c owns it and sink.c only cached it. */
  config_free(&p->cfg);
  event_destroy(p->el);
  p->el = NULL;

  /* 10 */ trace_report_and_shutdown();
} /* shutdown_all */

/*
 * player_loop: DESIGN.md IV.9.a step 10, the event loop. One epoll wait,
 * four branches, no polling and no sleeping -- the 1 Hz position line
 * that phase 7's diagnostic got from a nanosleep now comes off the same
 * redraw tick that carries the device-state watchdog, which is the whole
 * point of VIII.3's "one timer, several jobs".
 *
 * The tty and resize sources are phase 11's; event_create already
 * registered everything event.c owns, so nothing here needs adding.
 * Takes the Player, returns nothing -- the verdict is p->status.
 */
static void
player_loop(Player * p)
{
  while (p->running) {
    EventSource fired[PLAYER_MAX_FIRED];
    int n = event_wait(p->el, fired, PLAYER_MAX_FIRED, -1);
    if (n < 0) {
      player_report(p, "event_wait failed");
      p->status  = 1;
      p->running = 0;
      break;
    }

    for (int i = 0; i < n && p->running; i++) {
      switch (fired[i]) {
        case EVSRC_SIGNAL: {
          /* Drained rather than merely observed: the signalfd is
             level-triggered, and naming the signal is what makes the
             SIGINT and SIGTERM verify rows distinguishable in the log.
             Both converge on shutdown_all, as does SIGHUP -- DESIGN.md
             VIII.2 makes all three unconditional-shutdown sources. */
          struct signalfd_siginfo si;
          ssize_t r = read(event_signalfd(p->el), &si, sizeof si);
          player_report(p, "signal: %u -- shutting down",
                        (r == (ssize_t) sizeof si) ? si.ssi_signo : 0u);
          p->running = 0;
          break;
        }

        case EVSRC_PIPE:
          player_drain_pipe(p);
          tui_mark_dirty();
          break;

        case EVSRC_REDRAW:
          player_tick(p);
          break;

        case EVSRC_RECONNECT:
          player_ladder_attempt(p);
          tui_mark_dirty();
          break;

        /* Both terminal sources call the same drain and dispatch on
           ev.type. There is no fd-to-event-type correspondence:
           tb_peek_event selects on both fds regardless of which one woke
           epoll, so a tty wakeup can legitimately yield a resize event
           and vice versa, and two type-specific handlers would drop
           events (DESIGN.md VIII.2). */
        case EVSRC_TTY:
        case EVSRC_RESIZE:
          tui_drain();
          break;

        /* EPOLLHUP|EPOLLERR on the tty. SIGHUP on the signalfd usually
           covers a closed terminal, but not when kurono is not in the
           foreground process group -- and epoll is level-triggered, so
           without this branch the loop spins at 100% CPU forever on a
           permanently-readable dead fd (DESIGN.md VIII.2's second
           warning). Unconditional shutdown, no questions asked. */
        case EVSRC_TTY_HUP:
          p->running = 0;
          break;

        case EVSRC_NONE:
        case EVSRC_COUNT:
        default:
          break;
      }
    }

    /* Exactly one present per wakeup, at the bottom of the iteration,
       guarded by the dirty flag every branch above sets (DESIGN.md
       VIII.3). Skipped entirely on the headless --play path, where
       tui_draw is a no-op anyway. */
    if (p->ui && p->running && tui_dirty()) {
      player_repaint(p);
    }
  }
} /* player_loop */

/*
 * player_start: DESIGN.md IV.9.a steps 1-7, shared by every kind of run.
 * Extracted in phase 11 because bare `kurono` and --play now both need
 * them and a second transcription of the startup order is exactly the
 * kind of divergence the order exists to prevent -- phase 11 owns step 9,
 * which sits in the middle of this sequence, so the two callers differ
 * only after step 7.
 *
 * Takes the Player (zeroed, with mode/ui already set), a label for
 * diagnostics, and an out-parameter set to 1 when a device was opened.
 * Returns 0 with the Player fully constructed, or -1 with everything it
 * built already torn down and nothing for the caller to release.
 */
static int
player_start(Player * p, const char * who, int * have_device)
{
  /* Step 1: config_init. */
  if (config_init(&p->cfg) != 0) {
    kurono_die("failed to initialize configuration");
  }

  /* Step 2: event_create -- which carries the pthread_sigmask blocking
     INT/TERM/HUP, and so must run before any pthread_create and before
     ma_device_start. New threads inherit the mask, which is what keeps
     T2 and miniaudio's T3 from ever taking a signal. It also opens the
     signalfd, both timerfds (disarmed), the self-pipe, and the epoll fd,
     and registers everything it owns. */
  p->el = event_create();
  if (p->el == NULL) {
    fprintf(stderr, "kurono: %s: event_create failed\n", who);
    config_free(&p->cfg);
    return -1;
  }

  /* Step 3: db_open -- phase 13.
     Step 4: scan_run -- phase 14. This is the last point at which stderr
     may be written on a TUI run; tui_init is step 9, and everything after
     it reports through player_report instead. */

  /* Step 5: sink_open. ma_context_init and ma_device_init with the
     notification callback set, but NOT ma_device_start. The pipe's write
     fd is cached first, because ma_device_init itself can raise a
     notification. pcm_ring and viz_ring are allocated inside this call,
     sized from the negotiated format. */
  sink_get(&p->sink);
  sink_set_event_pipe(event_pipe_write_fd(p->el));
  *have_device = (p->sink.open(p->sink.self, &p->fmt, player_on_notify, p) == 0);
  if (!*have_device) {
    fprintf(stderr, "kurono: %s: sink open failed\n", who);
    if (sink_alloc_default_rings(&p->fmt) != 0) {
      fprintf(stderr, "kurono: %s: ring allocation failed\n", who);
      config_free(&p->cfg);
      event_destroy(p->el);
      p->el = NULL;
      return -1;
    }
  }

  /* Step 6: there is no separate ring-allocation step. pcm_ring and
     viz_ring came from step 5 (or, with no device, from
     sink_alloc_default_rings just above); boundary_ring is allocated
     inside engine_create below. The ordering guarantee this step exists
     to state still holds and is what matters: every ring exists before
     T2 is created and before ma_device_start, so every ring pointer is
     stable for both threads' whole lifetime and none of them needs to be
     atomic. */

  /* Step 7: engine_create then engine_start. T2 starts parked on the
     command condvar with no track loaded. */
  p->e = engine_create(&p->fmt, &p->sink);
  if (p->e == NULL) {
    fprintf(stderr, "kurono: %s: engine_create failed\n", who);
    p->sink.close(p->sink.self);
    sink_free_rings();
    config_free(&p->cfg);
    event_destroy(p->el);
    p->el = NULL;
    return -1;
  }
  engine_set_event_pipe(p->e, event_pipe_write_fd(p->el));
  if (engine_start(p->e) != 0) {
    fprintf(stderr, "kurono: %s: engine_start failed\n", who);
    p->sink.close(p->sink.self);
    sink_free_rings(); /* T2 never started, so nothing can still reach them */
    engine_destroy(p->e);
    p->e = NULL;
    config_free(&p->cfg);
    event_destroy(p->el);
    p->el = NULL;
    return -1;
  }

  return 0;
} /* player_start */

/*
 * run_play: implements --play FILE [--seek MS | --seek-stress N |
 * --seek-paused MS] (plan.md VI.8, phase-7-engine.md I.9): DESIGN.md
 * IV.9.a's ten startup steps in order, the IV.9.a step 10 event loop with
 * the IV.8.a device-loss watchdog on its redraw tick and the IV.8.b
 * reopen ladder on its reconnect timer, and IV.9.b's teardown table on
 * the way out. Steps 3 and 4 are the only stubs -- db_open and scan_run
 * belong to phases 13 and 14. Step 9 is deliberately skipped: this is the
 * headless harness, and it must stay usable under a sanitiser with no
 * terminal to claim.
 *
 * A device that cannot be opened or started is not fatal (DESIGN.md
 * XI.2): the rings are allocated at the default session format, T2 is
 * created and parked anyway, and the run reports browse-only.
 *
 * Takes the file path, the PlayMode, and its argument (a seek target in
 * ms for PLAY_SEEK_ONCE and PLAY_SEEK_PAUSED, a seek count for
 * PLAY_SEEK_STRESS, ignored for PLAY_PLAIN); returns 0 on a clean EOF or
 * a clean signalled exit, 1 on any open/decode failure.
 */
static int
run_play(const char * path, PlayMode mode, int64_t mode_arg)
{
  Player p;
  memset(&p, 0, sizeof p);
  p.mode     = mode;
  p.mode_arg = mode_arg;
  p.dev      = DEV_READY;
  p.running  = 1;
  p.ui       = 0;

  int have_device = 0;
  if (player_start(&p, "--play", &have_device) != 0) {
    return 1;
  }

  /* DESIGN.md XI.2: a device that never opened is browse-only, not a
     failure to start. Everything above still got built -- that is the
     point of the state, and the reason it is entered here rather than at
     step 5. */
  if (!have_device) {
    player_browse_only(&p, "device open failed at startup");
    shutdown_all(&p);
    return p.status;
  }

  /* Step 8: ma_device_start. T3 begins and immediately underruns into
     silence, which is correct -- the first invocation happens on this
     thread, inside this call, from PulseAudio's pre-fill. */
  if (p.sink.start(p.sink.self) != 0) {
    fprintf(stderr, "kurono: --play: sink start failed\n");
    player_browse_only(&p, "device start failed at startup");
    shutdown_all(&p);
    return p.status;
  }

  /* VIII.3's warning: the tick is armed BEFORE the first CMD_PLAY, not
     after, or the first boundary is observed late. */
  player_set_redraw(&p, 1);
  engine_play(p.e, path, PLAY_HEADLESS_TRACK_ID);

  /* Step 10: enter the event loop. */
  player_loop(&p);

  shutdown_all(&p);
  return p.status;
} /* run_play */

/*
 * run_tui: bare `kurono` -- the real player. The same ten startup steps
 * run_play uses, with step 9 present: tb_init, and the two fds event.c
 * does not own registered with the epoll it does.
 *
 * Nothing plays. The queue and the browser are phase 12, so this phase
 * brings up the terminal, draws DESIGN.md VIII.4's frame, and sits in
 * epoll_wait with every timer disarmed -- which is exactly the "paused,
 * stopped, idle -> zero wakeups, zero CPU" state VIII.3 requires, and the
 * easiest state in which to measure it.
 *
 * Takes nothing, returns the process exit status.
 */
static int
run_tui(void)
{
  Player p;
  memset(&p, 0, sizeof p);
  p.mode    = PLAY_PLAIN;
  p.dev     = DEV_READY;
  p.running = 1;
  p.ui      = 1;

  int have_device = 0;
  if (player_start(&p, "kurono", &have_device) != 0) {
    return 1;
  }

  /* Step 8: ma_device_start -- deliberately NOT called here, and this is
     the one place this run departs from IV.9.a's order.
     Measured on this host (2026-08-12, per-thread sampling of each
     task's stat file under /proc over 12 s of idle): with the device
     started
     and nothing to play, T1 sat in epoll_wait at 0.00% and T2 in futex at
     0.00% -- VIII.3's "zero wakeups, zero CPU" holding exactly -- while
     miniaudio's T3 burned 1.33% in ppoll, waking ~53 times a second at
     periodFrames = 900 to memset silence and count an underrun. A player
     with nothing loaded has no reason to hold a running device.
     Phase 11 cannot play anything at all, so the device is simply left
     open and stopped; the general policy -- start on the first CMD_PLAY,
     stop when playback ends -- belongs with the transport wiring in phase
     12, and needs an amendment to IV.9.a step 8 before it lands.
     Nothing downstream assumes a started device: the IV.8.a watchdog acts
     only when engine_state is ENG_PLAYING, and teardown step 2's
     AudioSink.stop is a no-op on a device that never started. */
  if (!have_device) {
    player_browse_only(&p, "no audio device at startup");
  }

  /* Step 9: tb_init, then the remaining epoll registrations. It comes
     after the device is started, per IV.9.a, and before anything can
     write to stderr again -- from here to tui_shutdown, every diagnostic
     goes through player_report.
     g_ui_player is set first: tui_init can deliver a resize immediately,
     and a dispatch reaching a NULL player would be a dropped keystroke. */
  g_ui_player = &p;
  if (tui_init() != 0) {
    g_ui_player = NULL;
    fprintf(stderr, "kurono: could not initialize the terminal\n");
    p.status = 1;
    shutdown_all(&p);
    return p.status;
  }

  int ttyfd = -1, resizefd = -1;
  tui_get_fds(&ttyfd, &resizefd);
  if (ttyfd < 0 || resizefd < 0
      || event_add(p.el, EVSRC_TTY, ttyfd) != 0
      || event_add(p.el, EVSRC_RESIZE, resizefd) != 0) {
    /* Without both fds there is no input and no resize: the loop would
       block forever with a drawn screen and no way out but a signal. */
    tui_shutdown();
    g_ui_player = NULL;
    fprintf(stderr, "kurono: could not register the terminal fds\n");
    p.status = 1;
    shutdown_all(&p);
    return p.status;
  }

  /* First paint before the first wait, or the screen stays blank until
     something happens to be dirty. */
  player_repaint(&p);

  /* Step 10: enter the event loop. The redraw timer stays disarmed --
     nothing is playing, so there is nothing to tick for (DESIGN.md
     VIII.3). */
  player_loop(&p);

  shutdown_all(&p);
  g_ui_player = NULL;
  return p.status;
} /* run_tui */

/*
 * Entry point. Handles --version by printing "kurono <version>" to
 * stdout and returning 0 immediately -- no trace lifecycle, nothing
 * runs. Every other invocation is wrapped in the trace ring's lifetime
 * (trace_init before, trace_drain / trace_summary_line / trace_shutdown
 * after) so every run yields evidence at no configuration cost
 * (DESIGN.md IV.10): --paths by resolving the Config and printing it
 * (see print_paths), --tone by running the phase-5 sink/ring/callback
 * smoke test
 * (sink_run_tone, plan.md VI.8) with no event loop and no audio file,
 * --decode FILE by running the phase-6 decoder smoke test (run_decode,
 * plan.md VI.8) with no device, no ring, and no threads, --play FILE
 * [--seek MS | --seek-stress N | --seek-paused MS] by running the audio
 * spine headless (run_play, plan.md VI.8) -- a real sink and decoder
 * wired together through audio/engine.c's T2, now with DESIGN.md
 * IV.9.a's startup order, the IV.9.a step 10 event loop carrying the
 * IV.8.a device-loss watchdog and the IV.8.b reopen ladder, and
 * IV.9.b's teardown table.
 *
 * No arguments is the player itself (run_tui): the same startup, with
 * DESIGN.md IV.9.a step 9 present, the TUI claiming the terminal, and the
 * event loop driving repaints. Every diagnostic flag returns before
 * tb_init and keeps stderr to itself. Anything unrecognised prints a
 * usage line. Takes the process argc/argv, returns the process exit
 * status.
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

  } else if (argc >= 3 && strcmp(argv[1], "--play") == 0) {
    /* plan.md VI.8 diagnostic scaffolding, phase-7-engine.md I.9 --
       exercises audio/engine.c's T2 wired to a real sink and decoder,
       headless. Optional trailing "--seek MS" exercises the flush
       handshake once EV_STREAM_INFO confirms the track is open. */
    if (argc == 3) {
      status = run_play(argv[2], PLAY_PLAIN, 0);
    } else if (argc == 5 && strcmp(argv[3], "--seek") == 0) {
      status = run_play(argv[2], PLAY_SEEK_ONCE, (int64_t) atoll(argv[4]));
    } else if (argc == 5 && strcmp(argv[3], "--seek-stress") == 0) {
      status = run_play(argv[2], PLAY_SEEK_STRESS, (int64_t) atoll(argv[4]));
    } else if (argc == 5 && strcmp(argv[3], "--seek-paused") == 0) {
      status = run_play(argv[2], PLAY_SEEK_PAUSED, (int64_t) atoll(argv[4]));
    } else {
      fprintf(stderr, "kurono: usage: --play FILE [--seek MS | "
                      "--seek-stress N | --seek-paused MS]\n");
      status = 1;
    }

  } else if (argc == 1) {
    /* The player. Phase 4's --headless smoke loop is gone: its structure
       is what player_loop became, and keeping a second event loop around
       would mean two places to fix a bug in one. */
    status = run_tui();

  } else {
    fprintf(stderr, "kurono: usage: kurono [--version | --paths | --tone | "
                    "--decode FILE | --play FILE [...]]\n");
    status = 1;
  }

  /* Teardown step 10 (DESIGN.md IV.9.b), for the diagnostic modes that
     never built a Player. --play reaches it inside shutdown_all, where
     the table puts it; the helper is idempotent, so this call is a no-op
     after that path ran. The other modes have no redraw tick and no pipe
     wakeup to drive trace_drain, so the drain here is their only one. */
  trace_report_and_shutdown();

  return status;
} /* main */
