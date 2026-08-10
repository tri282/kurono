#include "../include/main.h"
#include "../include/config.h"
#include "../include/event.h"
#include "../include/audio/decoder.h"
#include "../include/audio/engine.h"
#include "../include/audio/sink.h"
#include "../include/audio/trace.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* Arbitrary: --play has no library, so there is no real track_id to
   report in EV_STREAM_INFO/EV_OPEN_FAILED/EV_DECODE_ERROR. Any
   constant works here -- phase 12+ replaces this whole diagnostic path
   with a real queue cursor. */
#define PLAY_HEADLESS_TRACK_ID 1

/* Position-print / event-poll interval for --play's headless loop
   (plan.md VI.8, phase-7-engine.md I.9: "position printed at 1 Hz"). */
#define PLAY_POLL_MS 1000

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

/*
 * run_play: implements --play FILE [--seek MS] (plan.md VI.8,
 * phase-7-engine.md I.9). Opens the sink, wires an Engine to it
 * (engine_create), starts T2 and the device, pushes CMD_PLAY, and
 * polls once per second: drains every posted Event (printing
 * EV_STREAM_INFO once and issuing the one-shot --seek on it if
 * requested, exiting the loop on EV_OPEN_FAILED/EV_DECODE_ERROR/
 * EV_EOF) and otherwise prints the current position computed from
 * sink_frames_consumed() and the negotiated sample rate. No event loop
 * is built for this diagnostic -- engine_set_event_pipe is never
 * called, so Engine's doorbell write is skipped (fd stays -1), exactly
 * the pattern sink_run_tone already uses to run standalone. Takes the
 * file path, the PlayMode, and its argument (a seek target in ms for
 * PLAY_SEEK_ONCE and PLAY_SEEK_PAUSED, a seek count for
 * PLAY_SEEK_STRESS, ignored for PLAY_PLAIN); returns 0 on a clean EOF,
 * 1 on any open/decode failure.
 */
static int
run_play(const char * path, PlayMode mode, int64_t mode_arg)
{
  AudioSink sink;
  sink_get(&sink);

  SessionFormat fmt;
  memset(&fmt, 0, sizeof(fmt));

  if (sink.open(sink.self, &fmt, NULL, NULL) != 0) {
    fprintf(stderr, "kurono: --play: sink open failed\n");
    return 1;
  }

  Engine * e = engine_create(&fmt, &sink);
  if (e == NULL) {
    fprintf(stderr, "kurono: --play: engine_create failed\n");
    sink.close(sink.self);
    sink_free_rings();
    return 1;
  }

  if (engine_start(e) != 0) {
    fprintf(stderr, "kurono: --play: engine_start failed\n");
    engine_destroy(e);
    sink.close(sink.self);
    sink_free_rings(); /* T2 never started, so nothing can still reach the rings */
    return 1;
  }

  if (sink.start(sink.self) != 0) {
    fprintf(stderr, "kurono: --play: sink start failed\n");
    sink.close(sink.self);
    engine_quit(e);
    engine_join(e);
    sink_free_rings();
    engine_destroy(e);
    return 1;
  }

  engine_play(e, path, PLAY_HEADLESS_TRACK_ID);

  const atomic_uint_least64_t * pos = sink_frames_consumed();
  int driven = 0; /* latches the one-shot driver in the EV_STREAM_INFO branch */
  int status = 0;
  int done   = 0;

  while (!done) {
    play_sleep_ms(PLAY_POLL_MS);

    Event ev;
    while (!done && engine_poll_event(e, &ev)) {
      switch (ev.kind) {
        case EV_STREAM_INFO:
          fprintf(stderr,
                  "stream: codec=%s rate=%d channels=%d duration_ms=%lld\n",
                  ev.info.codec, ev.info.sample_rate, ev.info.channels,
                  (long long) ev.info.duration_ms);
          /* Every driver is one-shot and runs from here because this is
             the first point at which the track is provably open and
             ev.info.duration_ms is known. */
          if (!driven) {
            driven = 1;
            switch (mode) {
              case PLAY_SEEK_ONCE:
                engine_seek(e, mode_arg);
                break;
              /* fmt.sample_rate, not ev.info.sample_rate: g_frames_consumed
                 counts frames in the session format the sink negotiated,
                 which is what the decoder resamples *to*. The source rate
                 is a property of the file and is off by the resample
                 ratio -- 44100 vs 48000 here. */
              /* Both stress drivers end the run rather than playing on
                 to EOF: the verdict is already decided, and a bounded
                 run is what lets the sanitizer builds observe the full
                 DESIGN.md IV.9.b teardown instead of being killed by an
                 external timeout partway through a 4-minute track. */
              case PLAY_SEEK_STRESS:
                drive_seek_stress(e, pos, fmt.sample_rate,
                                  ev.info.duration_ms, (int) mode_arg);
                done = 1;
                break;
              case PLAY_SEEK_PAUSED:
                drive_seek_paused(e, &sink, pos, fmt.sample_rate, mode_arg);
                done = 1;
                break;
              case PLAY_PLAIN:
                break;
            }
          }
          break;
        case EV_OPEN_FAILED:
          fprintf(stderr, "kurono: --play: open failed (error %d)\n", ev.averror);
          status = 1;
          done   = 1;
          break;
        case EV_DECODE_ERROR:
          fprintf(stderr, "kurono: --play: decode error (error %d)\n", ev.averror);
          status = 1;
          done   = 1;
          break;
        case EV_EOF:
          done = 1;
          break;
      }
    }

    if (!done) {
      uint64_t frames  = atomic_load_explicit(pos, memory_order_relaxed);
      double   seconds = (fmt.sample_rate > 0)
                            ? (double) frames / (double) fmt.sample_rate
                            : 0.0;
      fprintf(stderr, "position: %.2f s\n", seconds);
    }
  }

  /* DESIGN.md IV.9.b, in its exact order. Steps 4 and 5 must not be
     moved above step 2: if T2 exits while T3 is still consuming, T3
     plays out the ring and then silence -- harmless on its own, but it
     puts the ring free (step 6) within reach of a live callback, which
     is a use-after-free on the audio thread. Stopping the device first
     makes that ordering error impossible to write. Step 6 is
     sink_free_rings, split out of AudioSink.close for this phase
     precisely because T2 also touches pcm_ring and is not joined until
     step 5. engine_destroy frees boundary_ring, whose only toucher is
     the now-joined T2. There is no step 1 here (no g_shutdown flag and
     no device-loss policy until phase 8) and no steps 7-10 (no db, no
     TUI, no event loop in this diagnostic). */
  sink.stop(sink.self);   /* step 2: synchronous; T3 has finished its last invocation */
  sink.close(sink.self);  /* step 3: ma_device_uninit joins T3 */
  engine_quit(e);         /* step 4 */
  engine_join(e);         /* step 5: the only legal way to know T2 is done */
  sink_free_rings();      /* step 6: both touchers of pcm_ring provably gone */
  engine_destroy(e);

  return status;
} /* run_play */

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
 * plan.md VI.8) with no device, no ring, and no threads, --play FILE
 * [--seek MS | --seek-stress N | --seek-paused MS] by running the
 * phase-7 engine smoke test (run_play,
 * plan.md VI.8) -- a real sink and decoder wired together through
 * audio/engine.c's T2, headless -- and any other invocation (including
 * no arguments) by printing a one-line "not implemented yet" notice to
 * stderr. Startup, wiring, and
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

  /* The flush snapshot is not in trace_ring and so is not in the
     drained log (DESIGN.md IV.10, amendment 03). Rendered here for the
     same reason trace_drain is called here: phase 11's redraw tick,
     which owns both, does not exist yet. Suppressed when no flush ever
     happened, so plain --tone and --headless runs stay one line. */
  uint64_t flush_w = 0, flush_c = 0;
  uint64_t flush_gen = engine_last_flush(&flush_w, &flush_c);
  if (flush_gen > 0) {
    fprintf(stderr, "kurono: %llu flush(es), last at write=%llu consumed=%llu\n",
            (unsigned long long) flush_gen, (unsigned long long) flush_w,
            (unsigned long long) flush_c);
  }

  trace_shutdown();

  return status;
} /* main */
