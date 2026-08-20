#include "../include/main.h"
#include "../include/config.h"
#include "../include/event.h"
#include "../include/audio/decoder.h"
#include "../include/audio/engine.h"
#include "../include/audio/sink.h"
#include "../include/audio/trace.h"
#include "../include/library/db.h"
#include "../include/library/query.h"
#include "../include/library/scan.h"
#include "../include/queue.h"
#include "../include/ui/browser.h"
#include "../include/ui/keymap.h"
#include "../include/ui/queueview.h"
#include "../include/ui/tui.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
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

/* Amendment 06 rule 7: the EOF drain is bounded, because an unbounded
   wait turns a wedged T3 into a hung player -- strictly worse than a
   truncated track. PCM_LEAD_SECONDS (2, sink.c) plus a second of slack,
   counted in ticks rather than read off a wall clock. It is a duplicate
   of a sink.c-private constant on purpose: main.c has no business
   including sink.c's internals for a bound whose exactness does not
   matter. */
#define PLAYER_DRAIN_MAX_TICKS (3 * PLAYER_REDRAW_HZ)

/* Bounded by EVENT_MAX_EPOLL_EVENTS in event.c; a local constant keeps
   main.c ignorant of that internal. */
#define PLAYER_MAX_FIRED 8

/* How often the ACT_RESCAN progress line repaints, in files. The scan
   blocks T1, so this is the only repaint happening for its duration --
   every file would mean one tb_present per file. */
#define SCAN_UI_EVERY 32

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
/* Amendment 07: consecutive redraw ticks with no callback invocation
   before device loss is declared. Five ticks is ~165 ms, nine missed
   periods at the 900-frame period measured on this host. */
#define CB_STALL_TICKS    5
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
 * PlayState: T1's own authoritative view of whether the player is
 * playing. Deliberately NOT engine.h's EngineState, and the distinction
 * is load-bearing rather than pedantic.
 *
 * EngineState is T2's view of itself, stored and loaded
 * memory_order_relaxed, and engine_pause/engine_resume only *queue* a
 * command -- T2 adopts the new state whenever it next reaches a drain
 * point, which may be after a slow AudioSource.read. Amendment 05's
 * device lifecycle says "publish the state", and if the state published
 * were T2's, the publish would not have happened yet when T1 stops the
 * device one line later. IV.8.a's watchdog would then observe
 * ENG_PLAYING together with !sink_is_started -- its device-loss
 * predicate exactly -- and run the reopen ladder against a healthy
 * device on every pause.
 *
 * So T1 owns this one, sets it synchronously, and every piece of device
 * policy reads it. T2 keeps EngineState for its own purposes. The two
 * agree eventually and are never required to agree instantaneously.
 *
 * (Correction to amendment 05, 2026-08-13, made while implementing
 * phase 12 step 0. The amendment did not say whose state, and there are
 * two.)
 */
typedef enum { PLAY_STOPPED, PLAY_PLAYING, PLAY_PAUSED } PlayState;

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

  /* One source and one Browser per view (DESIGN.md VIII.4). `br` is the
     active view's widget and is what every navigation action and the
     draw code touch, so nothing below the view switch knows views
     exist. Each view keeps its own cursor by construction: switching
     away and back restores position because the widget was never torn
     down (phase-16-browser.md I.2). */
  /* Borrowed by every library source for the lifetime of the run: the
     source keeps the pointer rather than copying (browser.h), so this
     array must outlive the browsers, not the construction block that
     builds it. */
  const char **   rootv;
  BrowserSource   brsrc[BROWSER_VIEW_COUNT];
  Browser *       brv[BROWSER_VIEW_COUNT];
  int             view;              /* index into brv, the active view */
  Browser *       br;                /* == brv[view]; NULL on every headless run */
  Queue *         q;                 /* NULL on every headless run */

  /* The playing track, for the status bar. A copy of the basename
     rather than a pointer into the queue: QueueEntry storage moves
     under queue_remove and dies under queue_clear, and the status bar
     outlives both. `dur_ms` is -1 until EV_STREAM_INFO reports it. */
  char            now_name[192];
  int64_t         dur_ms;

  /* The duration of a track that T2 has opened but that is not sounding
     yet. amendment 09's prefetch means EV_STREAM_INFO for track N+1
     arrives at the decoder's EOF of track N -- up to a ring's worth of
     audio before the boundary that makes it current. Applying it to
     dur_ms there would name the wrong track's length, and clearing
     dur_ms at the boundary would throw the only copy away, so it is
     parked here under its own id until the boundary claims it. */
  int64_t         pend_dur_id;
  int64_t         pend_dur_ms;

  /* The playing track's full key, for ACT_JUMP_TO_PLAYING. Kept beside
     the basename rather than derived from it, and copied for the same
     reason: the QueueEntry it came from dies under queue_clear, which
     ACT_CLEAR_QUEUE reaches while a track is still named here. */
  char            now_path[PATH_MAX];
  /* The library id of what is playing, or 0 for a --play run with no
     library behind it. Carried beside the path because since phase 16 a
     track is addressed by id, not by path: ACT_JUMP_TO_PLAYING needs the
     row to look up its tags before it can name a position. */
  int64_t         now_id;

  /* Volume, and the whole of gap 4's ruling (2026-08-13). T1 holds the
     authoritative scalar because sink_set_gain and engine_set_gain are
     write-only -- there is no getter to read the current value back.
     `vol_pct` is what the status bar shows and is deliberately NOT
     touched by mute, so unmuting restores `gain` by assignment from
     `mute_saved` rather than recomputing it from a percentage. */
  float           gain;
  int             vol_pct;
  float           mute_saved;
  int             muted;

  /* DESIGN.md XI.1's five-consecutive-failures rule (plan.md VII gap 7,
     ruled 2026-08-13). Counts EV_OPEN_FAILED and EV_DECODE_ERROR alike;
     reset by evidence of playback *progress*, never by evidence of a
     successful open. See player_track_failed. */
  int             fail_count;

  /* Amendment 06's EOF drain. `drain_until` is T2's write-frame count at
     EOF and 0 means "not draining"; the state stays PLAY_PLAYING for the
     whole drain, because it is true -- the device is started and audio is
     still coming out of it. Cleared by completion, by the bound
     (`drain_ticks`), and by every path that flushes pcm_ring, since the
     value lives in the pre-flush frame domain and means nothing after a
     resync. Pause is deliberately not one of those paths: the counter
     stops with the device and the drain simply completes on resume. */
  uint64_t        drain_until;
  int             drain_ticks;

  /* Amendment 09's transition state, all three fields T1-only.
     `pend_b` is one boundary entry read out of boundary_ring but not yet
     due: ring.h has no peek, and the consumer side of a peek is exactly
     a one-slot hold. `next_pos` is the play-order position T1 last
     prefetched, which is what the boundary pop moves the cursor to --
     searching by track_id cannot work, because an entry may have
     track_id 0 and the same track may be queued twice. */
  BoundaryEntry   pend_b;
  int             pend_have;
  size_t          next_pos;
  int             next_valid;

  DeviceState     dev;
  PlayState       play;              /* T1's own; amendment 05 IV.9.a.i */
  int             ladder_attempt;    /* index into g_ladder_backoff_ms */
  int             ladder_elapsed_ms; /* against LADDER_GIVE_UP_MS */
  struct timespec loss_at;           /* for the detection/recovery timings */
  int64_t         pos_origin_ms;     /* IV.8.b step 5's elapsed-time origin */

  /* Amendment 07: the authoritative device-loss detector. g_cb_seq is
     T3's callback-invocation counter, and a device that has stopped
     asking for audio is the only observable a dead sound server leaves
     -- ma_device_get_state keeps reporting `started`, because miniaudio
     writes it in ma_device_start/stop and nowhere else. Sampled only in
     player_watchdog, which runs on the redraw tick and only while the
     device is ready and PLAY_PLAYING. */
  unsigned        cb_seq_last;
  int             cb_stall_ticks;

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

  /* The library index. Opened at startup step 3 and closed at teardown
     step 7, so it outlives every other subsystem that might query it. */
  Db * db;

  /* The current search's results, owned here and rebuilt on every
     keystroke by search_dispatch. Freed when the search ends and again
     at teardown, because a run can end with a search still open.
     Deliberately not inside the Browser: the browser's preview cache is
     what the search draws over, and keeping the two apart is what makes
     cancelling a search free rather than a restore. */
  BrowserRow * search_rows;
  size_t       search_n;
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

static void player_locate(Player * p, int64_t track_id, const char * label);
static int  player_play_entry(Player * p, const QueueEntry * ent);
static void player_queue_confirm(Player * p);
static int  player_queue_playing_index(const Player * p);
static void player_rescan(Player * p);
static void player_fill_model(Player * p, TuiModel * out);
static void player_queue_changed(Player * p);
static void player_prefetch_next(Player * p);
static void player_drain_cancel(Player * p);
static void player_ladder_begin(Player * p, const char * detector);
static void player_ladder_attempt(Player * p);
static void player_on_notify(void * user, int notification_type);

/* Every action that moves the player through a state -- transport, the
   queue, device recovery -- is defined below beside the state it moves
   through, and declared here because action_dispatch sits above all of
   it. Navigation is the exception and stays inline: it reaches nothing
   but browser.c. */
static void player_action(Player * p, Action a, int32_t arg);

/*
 * player_queue_find: play-order position of the first entry whose path
 * equals `path`. Takes the queue, the path and an out-param for the
 * position; returns 1 on a hit and 0 on a miss, leaving *out untouched
 * on a miss.
 *
 * A linear scan, and deliberately in main.c rather than queue.c: queue.c
 * depends on nothing (DESIGN.md III.2) and this is a UI policy question
 * -- what "already queued" means -- not a property of the order model.
 * Phase 16 answers it by track_id instead, once rows have one.
 */
static int
player_queue_find(const Queue * q, const char * path, size_t * out)
{
  size_t n = queue_count(q);
  for (size_t i = 0; i < n; i++) {
    const QueueEntry * e = queue_at(q, i);
    if (e != NULL && strcmp(e->path, path) == 0) {
      *out = i;
      return 1;
    }
  }
  return 0;
} /* player_queue_find */

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
 * It handles ACT_QUIT and the navigation actions itself -- the two
 * groups that reach nothing below ui/ -- and hands everything else to
 * player_action, which sits beside the state it moves. Actions with no
 * M1 implementation still report themselves in the status bar, which is
 * how the binding table stays exercised end to end.
 *
 * Never writes to stderr: termbox2 owns the terminal here.
 */
/*
 * queue_cursor_action: move the queue pane's cursor for one navigation
 * action. Takes the action, returns 1 when it moved the cursor and 0
 * when the action was not a cursor move.
 *
 * Separate from the browser's block in action_dispatch because the two
 * lists are never both focused: whichever is on top consumes the keys
 * outright.
 */
static int
queue_cursor_action(Action a)
{
  const TuiLayout * l = tui_layout();
  int page = (l != NULL && l->body_h > 1) ? l->body_h - 1 : 1;

  switch (a) {
    case ACT_CURSOR_UP:        queueview_cursor_move(-1);         return 1;
    case ACT_CURSOR_DOWN:      queueview_cursor_move(1);          return 1;
    case ACT_CURSOR_PAGE_UP:   queueview_cursor_move(-page);      return 1;
    case ACT_CURSOR_PAGE_DOWN: queueview_cursor_move(page);       return 1;
    case ACT_CURSOR_HOME:      queueview_cursor_set(0);           return 1;
    /* SIZE_MAX clamps to the last row inside queueview_cursor_set, which
       is the only place that knows the count. */
    case ACT_CURSOR_END:       queueview_cursor_set(SIZE_MAX);    return 1;
    default:                   return 0;
  }
} /* queue_cursor_action */

void
action_dispatch(Action a, int32_t arg)
{
  Player * p = g_ui_player;
  if (p == NULL) {
    return; /* no TUI run in progress -- every diagnostic mode */
  }

  if (a == ACT_QUIT) {
    /* Amendment 04 decision 5: quit closes what is on top of the mode
       stack first, and only ends the run once nothing is left to close.
       A dispatch rule rather than a binding, so q, Esc and Ctrl-C all
       inherit it by being bound to ACT_QUIT in each overlay's table. */
    if (ui_mode() != MODE_NORMAL) {
      ui_mode_pop();
      tui_mark_dirty();
      return;
    }
    /* Converges on the same shutdown_all as SIGINT/SIGTERM/SIGHUP;
       there is no second exit path (DESIGN.md IV.9.b). */
    p->running = 0;
    return;
  }

  if (a == ACT_HELP_TOGGLE) {
    if (ui_mode_toggle(MODE_HELP) < 0) {
      player_report(p, "terminal too small for the help overlay");
    }
    tui_mark_dirty();
    return;
  }

  if (a == ACT_QUEUE_TOGGLE) {
    /* Rebuilt on the way in, never per repaint: the pane's contents can
       only change through an action, and every action that changes them
       rebuilds. */
    if (ui_mode() == MODE_QUEUE) {
      ui_mode_pop();
    } else if (ui_mode_toggle(MODE_QUEUE) > 0) {
      queueview_rebuild(p->q, p->db);
      /* Open on what is playing rather than at the top: the pane is
         opened to see where you are far more often than to see entry 1. */
      if (p->q != NULL && queue_count(p->q) > 0) {
        queueview_cursor_set(queue_cursor(p->q));
      }
    } else {
      player_report(p, "terminal too small for the queue");
    }
    tui_mark_dirty();
    return;
  }

  if (a == ACT_MENU_TOGGLE) {
    /* Phase 17b owns the menu. The key is bound now so the keymap
       settles in one change; pushing MODE_MENU with nothing to draw
       would give a blank box and no way to tell it from a bug. */
    player_report(p, "settings menu: not implemented yet");
    tui_mark_dirty();
    return;
  }

  if (a == ACT_CONFIRM) {
    if (ui_mode() == MODE_QUEUE) {
      player_queue_confirm(p);
      return;
    }
    player_report(p, "confirm: nothing to confirm yet");
    tui_mark_dirty();
    return;
  }

  /* The queue pane owns the cursor keys while it is up. Checked above
     the browser's navigation block rather than inside it: the browser is
     still on screen underneath and must not move, and a pane whose
     arrows moved the list behind it would be the same class of silent
     wrongness as an unbound key. */
  if (ui_mode() == MODE_QUEUE && queue_cursor_action(a)) {
    tui_mark_dirty();
    return;
  }

  /* View switching sits above the browser guard on purpose: if one
     view failed to build, the others are still reachable, and being
     stranded in a dead view with no way out is worse than the failure
     that caused it. */
  switch (a) {
    case ACT_VIEW_NEXT:
    case ACT_VIEW_PREV:
    case ACT_VIEW_SET: {
      /* A view switch is an index change and nothing else: each view's
         widget keeps running, so its cursor, scroll and depth are
         exactly where they were left (phase-16-browser.md I.2). */
      int next;
      if (a == ACT_VIEW_SET) {
        next = arg;
      } else {
        int step = (a == ACT_VIEW_NEXT) ? 1 : BROWSER_VIEW_COUNT - 1;
        next = (p->view + step) % BROWSER_VIEW_COUNT;
      }

      if (next < 0 || next >= BROWSER_VIEW_COUNT || p->brv[next] == NULL) {
        player_report(p, "view unavailable");
      } else {
        p->view = next;
        p->br   = p->brv[next];
      }
      tui_mark_dirty();
      return;
    }
    default:
      break;
  }

  /* Navigation. Every one of these is a pure cursor move inside
     browser.c -- nothing here reaches the engine, and a browser that
     failed to build leaves them all no-ops rather than special cases. */
  if (p->br != NULL) {
    /* A page is one screenful minus a row of overlap, so the line the
       user was reading stays on screen across the jump. */
    const TuiLayout * l = tui_layout();
    int page = (l != NULL && l->body_h > 1) ? l->body_h - 1 : 1;

    switch (a) {
      case ACT_CURSOR_UP:
        browser_cursor_move(p->br, -1);
        tui_mark_dirty();
        return;
      case ACT_CURSOR_DOWN:
        browser_cursor_move(p->br, 1);
        tui_mark_dirty();
        return;
      case ACT_CURSOR_PAGE_UP:
        browser_cursor_move(p->br, -page);
        tui_mark_dirty();
        return;
      case ACT_CURSOR_PAGE_DOWN:
        browser_cursor_move(p->br, page);
        tui_mark_dirty();
        return;
      case ACT_CURSOR_HOME:
        browser_cursor_home(p->br);
        tui_mark_dirty();
        return;
      case ACT_CURSOR_END:
        browser_cursor_end(p->br);
        tui_mark_dirty();
        return;

      case ACT_COL_LEFT:
        if (browser_ascend(p->br) < 0) {
          player_report(p, "cannot read parent directory");
        }
        tui_mark_dirty();
        return;

      case ACT_COL_RIGHT: {
        /* A file is not an error here -- right on a track simply does
           nothing, and ACT_PLAY_NOW is what acts on one. */
        if (browser_descend(p->br) < 0) {
          /* Read the selection AFTER the failure, not before: a failed
             descend leaves the rows untouched, so it is still valid,
             and a successful one would have freed the array the pointer
             is into. Naming browser_cwd here instead would report the
             parent -- the directory that is readable. */
          const BrowserRow * sel = browser_selection(p->br);
          player_report(p, "cannot read %s",
                        (sel != NULL) ? sel->name : "directory");
        }
        tui_mark_dirty();
        return;
      }

      default:
        break;
    }
  }

  /* Everything else moves the player through a state and is therefore
     defined beside that state rather than here. */
  player_action(p, a, arg);
} /* action_dispatch */

/*
 * player_row_target: what a browser row means to the queue -- a playable
 * path and, when the library knows it, the id that goes with it. Takes
 * the Player, a row, and out-params for a freshly allocated path and the
 * id; returns 0, or -1 with neither set.
 *
 * It exists because a row key stopped being a path. In M1 the browser's
 * keys were filesystem paths and ACT_ENQUEUE handed row->key straight to
 * queue_append; since phase 16 a library track's key is a kind byte and
 * a decimal id, so the queue was being given "I42" as a filename. That
 * fails at open(2) and not at enqueue, so the report said "queued" and
 * the failure surfaced a track later.
 *
 * The id is asked of the source rather than parsed here, for the reason
 * browser_library_locate exists: the encoding has one owner.
 */
static int
player_row_target(Player * p, const BrowserRow * row,
                  char ** out_path, int64_t * out_id)
{
  if (row == NULL || row->key == NULL) {
    return -1;
  }

  int64_t id = browser_library_track_id(&p->brsrc[p->view], row->key);
  if (id <= 0) {
    /* Not a library row -- the filesystem source's keys are still paths,
       and so is anything a future source hands back that is not a track.
       Copying it is correct there and harmless here. */
    char * path = strdup(row->key);
    if (path == NULL) {
      return -1;
    }
    *out_path = path;
    *out_id   = 0;
    return 0;
  }

  DbTrackRow t;
  memset(&t, 0, sizeof t);
  if (p->db == NULL || db_query_track_by_id(p->db, id, &t) != 1 || t.path == NULL) {
    db_track_row_free_fields(&t);
    return -1;
  }

  char * path = strdup(t.path);
  db_track_row_free_fields(&t);
  if (path == NULL) {
    return -1;
  }

  *out_path = path;
  *out_id   = id;
  return 0;
} /* player_row_target */

/*
 * player_enqueue_rows: append every track in `rows` to the queue. Takes
 * the Player, the rows and their count, and an out-param for how many
 * landed; returns 0, or -1 if nothing could be queued at all.
 *
 * A row with no path, or one queue_append refuses, is skipped and
 * counted rather than abandoning the rest -- per-file failures are never
 * fatal (CLAUDE.md), and half an album queued beats none. The caller
 * still owns `rows`.
 */
static int
player_enqueue_rows(Player * p, const DbTrackRow * rows, size_t n, size_t * out_added)
{
  size_t added = 0;

  for (size_t i = 0; i < n; i++) {
    if (rows[i].path == NULL) {
      continue;
    }
    if (queue_append(p->q, rows[i].id, rows[i].path) == 0) {
      added++;
    }
  }

  *out_added = added;
  return (added > 0 || n == 0) ? 0 : -1;
} /* player_enqueue_rows */

/*
 * player_enqueue_row: ACT_ENQUEUE's whole body, for one browser row of
 * any kind. Takes the Player and the selected row, returns nothing --
 * every outcome is a status-bar line.
 *
 * A container row is expanded by the source (browser_library_expand),
 * never by parsing its key here: phase 16's ruling that the encoding has
 * exactly one owner. A row that expands to nothing and is not a track is
 * reported as empty rather than queued silently.
 */
static void
player_enqueue_row(Player * p, const BrowserRow * sel)
{
  DbTrackRow * rows = NULL;
  size_t       n = 0;
  size_t       added = 0;

  if (browser_library_expand(&p->brsrc[p->view], sel->key, &rows, &n) == 0 && n > 0) {
    int rc = player_enqueue_rows(p, rows, n, &added);
    db_track_rows_free(rows, n);
    if (rc != 0) {
      player_report(p, "could not queue %s", sel->name);
      return;
    }
    /* The skipped count is reported rather than swallowed: an album that
       queues 11 of 12 tracks has a reason, and silence here would make
       the missing one look like a scan bug later. */
    player_queue_changed(p);
    if (added < n) {
      player_report(p, "queued %zu of %zu tracks from %s (%zu in queue)",
                    added, n, sel->name, queue_count(p->q));
    } else {
      player_report(p, "queued %zu tracks from %s (%zu in queue)", added,
                    sel->name, queue_count(p->q));
    }
    tui_mark_dirty();
    return;
  }
  db_track_rows_free(rows, n);

  if (sel->is_dir) {
    /* A container the source could not expand, or one holding nothing:
       the synthetic root is the first, an empty directory the second.
       Neither is an error and neither queues anything. */
    player_report(p, "%s holds no tracks", sel->name);
    return;
  }

  /* A track. The id is 0 only for a source with no library row behind
     it; the display-metadata branch keys on track_id > 0, which is why
     the value is carried through the queue rather than recovered
     later. */
  char *  qpath = NULL;
  int64_t qid   = 0;
  if (player_row_target(p, sel, &qpath, &qid) != 0) {
    player_report(p, "could not queue %s", sel->name);
    return;
  }
  if (queue_append(p->q, qid, qpath) != 0) {
    free(qpath);
    player_report(p, "could not queue %s", sel->name);
    return;
  }
  free(qpath);
  player_queue_changed(p);
  player_report(p, "queued: %s (%zu in queue)", sel->name, queue_count(p->q));
  tui_mark_dirty();
} /* player_enqueue_row */

/*
 * player_play_container: ACT_PLAY_NOW on a directory, album or artist
 * row -- insert everything it holds after the cursor and start the first
 * of it. Takes the Player and the row, returns nothing.
 *
 * Inserting rather than appending, for the reason the track path
 * records: an append puts the tracks last in play order, so
 * queue_advance returns NULL the moment the first one ends and
 * everything already queued behind it never plays. There is deliberately
 * no dedupe here -- the track path dedupes because Enter on one row the
 * user already queued means "go there", and no such single entry exists
 * for a container.
 */
static void
player_play_container(Player * p, const BrowserRow * sel)
{
  DbTrackRow * rows = NULL;
  size_t       n = 0;
  size_t       added = 0;

  if (browser_library_expand(&p->brsrc[p->view], sel->key, &rows, &n) != 0 || n == 0) {
    db_track_rows_free(rows, n);
    player_report(p, "%s holds no tracks", sel->name);
    return;
  }

  size_t at = (queue_count(p->q) == 0) ? 0 : queue_cursor(p->q) + 1;
  for (size_t i = 0; i < n; i++) {
    if (rows[i].path == NULL) {
      continue;
    }
    if (queue_insert(p->q, at + added, rows[i].id, rows[i].path) == 0) {
      added++;
    }
  }
  db_track_rows_free(rows, n);

  if (added == 0) {
    player_report(p, "could not queue %s", sel->name);
    return;
  }

  /* Set the cursor explicitly: queue_insert adjusts it to keep referring
     to whatever was current *before* the insert, which is the opposite
     of what playing the first new entry needs. */
  if (queue_set_cursor(p->q, at) != 0) {
    player_report(p, "could not select %s", sel->name);
    return;
  }
  p->fail_count = 0; /* an explicit user action starts a fresh streak */
  player_queue_changed(p);
  player_report(p, "playing %s (%zu tracks)", sel->name, added);
  (void) player_play_entry(p, queue_at(p->q, at));
} /* player_play_container */

/*
 * player_search_free: drop the current result set. Takes the Player,
 * returns nothing. Idempotent, and a no-op when no search has run.
 */
static void
player_search_free(Player * p)
{
  for (size_t i = 0; i < p->search_n; i++) {
    free(p->search_rows[i].name);
    free(p->search_rows[i].key);
  }
  free(p->search_rows);
  p->search_rows = NULL;
  p->search_n    = 0;
} /* player_search_free */

/*
 * search_dispatch: see include/ui/keymap.h. Runs one query and keeps its
 * rows for the next repaint. Takes the raw query text or NULL, returns
 * nothing.
 *
 * Three inputs produce zero results and none of them is an error: NULL
 * (the search ended), a query with no tokens in it -- a bare quote, a
 * bare star, spaces -- and a query that matches nothing. The middle case
 * is why query_build_match returns an owned empty string rather than
 * NULL: the caller can tell "nothing to search for" from "allocation
 * failed" and skip the query instead of handing FTS5 a string it would
 * reject. DESIGN.md V.6 is explicit that a residual SQLITE_ERROR here
 * must never be fatal, and db_query_search already reports one as zero
 * rows.
 */
void
search_dispatch(const char * query)
{
  Player * p = g_ui_player;
  if (p == NULL) {
    return;
  }

  player_search_free(p);
  tui_mark_dirty();

  if (query == NULL || p->db == NULL) {
    return;
  }

  char * match = query_build_match(query);
  if (match == NULL) {
    player_report(p, "out of memory, search skipped");
    return;
  }
  if (match[0] == '\0') {
    free(match);
    return;  /* nothing to match on yet; not a query and not an error */
  }

  DbTrackRow * rows = NULL;
  size_t       n    = 0;
  (void) db_query_search(p->db, match, &rows, &n);
  free(match);

  /* DESIGN.md XI.7, read here rather than at the repaint because the
     flag is non-sticky: the next query overwrites it.

     Worded identically to the browser source's report on purpose. Both
     read the same Db-level flag, so one truncating search can be
     reported twice -- once here, and again when the next library load
     latches the flag this call left set. Two wordings for one event
     would read as two failures; one wording makes the duplicate
     invisible, and neither report can be dropped: this one is the only
     one guaranteed to run when no library load follows. */
  if (db_result_truncated(p->db)) {
    player_report(p, "out of memory, result truncated");
  }

  if (n == 0) {
    db_track_rows_free(rows, n);
    return;
  }

  p->search_rows = calloc(n, sizeof *p->search_rows);
  if (p->search_rows == NULL) {
    player_report(p, "out of memory, search skipped");
    db_track_rows_free(rows, n);
    return;
  }

  for (size_t i = 0; i < n; i++) {
    /* One line per hit, in the shape the columns already use: what it is
       called, and who by when that is known. A track with no tags at all
       still has a path, and showing the path beats showing a blank. */
    char line[512];
    const char * title = (rows[i].title != NULL) ? rows[i].title : rows[i].path;
    if (rows[i].artist != NULL) {
      snprintf(line, sizeof line, "%s - %s", rows[i].artist, title);
    } else {
      snprintf(line, sizeof line, "%s", (title != NULL) ? title : "");
    }

    /* The key is the library id, not the path: a track is addressed by
       id everywhere since phase 16, and confirming a hit hands this
       straight to player_locate. Decimal text rather than a parallel
       int64_t array, because BrowserRow already owns a string here and a
       second allocation per row would buy nothing. */
    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "%" PRId64, rows[i].id);

    p->search_rows[i].name   = strdup(line);
    p->search_rows[i].key    = strdup(idbuf);
    p->search_rows[i].is_dir = 0;

    if (p->search_rows[i].name == NULL || p->search_rows[i].key == NULL) {
      /* The same degrade every other result set applies: keep what was
         built, say so, and draw a short list rather than none. */
      player_report(p, "out of memory, result truncated");
      break;
    }
    p->search_n = i + 1;
  }

  db_track_rows_free(rows, n);
} /* search_dispatch */

/*
 * search_confirm: see include/ui/keymap.h. Move the browser to the
 * result at `index`. Takes the index, returns nothing.
 *
 * The second consumer of the locate path, and the reason that path is a
 * function rather than something inlined into the jump: finding a track
 * by searching for it and finding the one that is playing are the same
 * move, and the encoding they both need belongs to browser.c either way.
 */
void
search_confirm(size_t index)
{
  Player * p = g_ui_player;
  if (p == NULL || index >= p->search_n || p->search_rows[index].key == NULL) {
    return;
  }
  player_locate(p, (int64_t) strtoll(p->search_rows[index].key, NULL, 10),
                p->search_rows[index].name);
} /* search_confirm */

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

  /* Nothing is playing and nothing can. Set directly rather than
     through player_set_play_state: there is no device to stop (the
     ladder's step 2 already uninit'ed it, or it never opened), and
     routing through the lifecycle here would only re-derive that. */
  p->play = PLAY_STOPPED;

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

  /* Amendment 06 rule 5, the second flush path: step 4 resyncs both
     counters to 0, so a drain armed before the device vanished would
     never be reached and would sit until the bound expired. */
  player_drain_cancel(p);

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

  /* IV.8.b step 6's "state PLAYING", and under amendment 05 it is also
     what restores IV.9.a.i's invariant: the start above just happened,
     so the publish belongs immediately after it (entering PLAY_PLAYING
     is start-first, publish-second). It is unconditional because both
     ladder entry points are roads on which the user wants audio -- a
     loss is only ever declared from PLAY_PLAYING, and the other entry
     point is a failed play transition. Without this the device would be
     started while p->play still read PAUSED or STOPPED, which is the
     1.33%-forever state the amendment exists to prevent. */
  p->play = PLAY_PLAYING;

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
  /* Amendment 07 rule 4. Step 3 reopens with a fresh device whose
     counter starts at 0, and step 6 restarts playback; carrying a stall
     count across that would re-declare the loss on the first tick after
     a successful recovery. */
  p->cb_seq_last       = 0;
  p->cb_stall_ticks    = 0;
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

  /* Our own stop/start coming back to us -- see Player.notify_muted. */
  if (p->notify_muted) {
    player_report(p, "device: notification %s (ours, during the ladder -- ignored)",
                  notify_name(notification_type));
    return;
  }

  /* A `stopped` arriving while we are not playing is one we caused.
     Amendment 05 stops the device on every transition out of
     PLAY_PLAYING and publishes the state first, so by the time this runs
     p->play already says so -- which is exactly what puts this on
     IV.8.a's row 2 rather than row 3 and is why no ladder fires below.
     The line is suppressed rather than merely ignored because reporting
     it *overwrites* whatever the transition just put in the status bar:
     "end of queue" and "5 consecutive failures, stopped" both became
     "device: notification stopped" before this was added, and the user
     never saw why playback ended. Not news in the other direction
     either -- PipeWire's idle-suspend while stopped lands here too, and
     XI.2 and IV.8.a agree it is a non-event. */
  if (notification_type == SINK_NOTIFY_STOPPED && p->play != PLAY_PLAYING) {
    return;
  }

  player_report(p, "device: notification %s", notify_name(notification_type));

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
      /* p->play, not engine_state -- the same correction the watchdog
         carries, and for the same reason: our own sink.stop on a pause
         raises this notification, and T2 may not have adopted the pause
         yet. Publishing the state before the stop is what puts this on
         IV.8.a's row 2 rather than row 3. */
      if (p->dev == DEV_READY && p->play == PLAY_PLAYING) {
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
 * player_set_play_state: DESIGN.md IV.9.a.i (amendment 05), the device
 * lifecycle, and the ONLY place outside teardown and the IV.8.b ladder
 * that starts or stops the device. Everything else changes playback
 * state by calling this.
 *
 * The invariant: the device is started iff the state is PLAY_PLAYING.
 * The two edges are ordered OPPOSITELY and that is the whole of the
 * risk in this function:
 *
 *   entering PLAY_PLAYING -- start the device FIRST, publish SECOND
 *   leaving  PLAY_PLAYING -- publish FIRST, stop the device SECOND
 *
 * Both orders keep one combination unobservable: state PLAY_PLAYING
 * while the device is not started. That pair is verbatim
 * player_watchdog's device-loss predicate, so reversing either edge
 * makes every ordinary pause declare a loss and run the reopen ladder
 * against a device that is fine. It also puts the `stopped`
 * notification our own sink.stop raises onto IV.8.a's row 2 (ignored
 * while STOPPED or PAUSED) instead of row 3 (loss while playing) --
 * the rows differ by exactly the state this ordering guarantees has
 * already landed, which is why notify_muted is deliberately NOT used
 * here. A mute that hides a misordering is worse than no mute.
 *
 * Takes the Player and the state to move to. Returns 0, or -1 when
 * entering PLAY_PLAYING needed a device start that failed -- in which
 * case the state is NOT published, the ladder has been entered, and the
 * caller must not proceed with whatever it was starting.
 */
static int
player_set_play_state(Player * p, PlayState want)
{
  if (p->play == want) {
    return 0;
  }

  /* Browse-only and mid-ladder runs have no device to drive. The state
     still tracks intent so the status bar and the queue behave, and
     IV.8.b step 6 restores the device to match on the way back. */
  int have_device = (p->dev == DEV_READY);

  if (want == PLAY_PLAYING) {
    if (have_device && !sink_is_started(&p->sink)) {
      if (p->sink.start(p->sink.self) != 0) {
        /* Amendment 05 decision 5: not a startup condition and not
           browse-only. This is the most likely way a device that
           vanished while nothing was playing announces itself, so it
           goes to the ladder like any other loss. */
        player_report(p, "device: start failed, reopening");
        player_ladder_begin(p, "start failure");
        return -1;
      }
    }
    /* Amendment 07. The watchdog is inert while not PLAY_PLAYING, so a
       stall count from before the pause would otherwise survive it and
       declare a loss after one tick instead of CB_STALL_TICKS. Sampled
       rather than zeroed: the device may already have run callbacks
       between the start above and this line. */
    p->cb_seq_last    = atomic_load_explicit(sink_cb_seq(),
                                             memory_order_acquire);
    p->cb_stall_ticks = 0;

    p->play = want;             /* publish SECOND */
    return 0;
  }

  p->play = want;               /* publish FIRST */
  if (have_device && sink_is_started(&p->sink)) {
    p->sink.stop(p->sink.self); /* synchronous; T3 is done on return */
  }
  return 0;
} /* player_set_play_state */

/*
 * drive_seek_paused: the phase-7 "seek while paused" verify row, which
 * exists to prove the DESIGN.md IV.7.5 fence skip -- with the device
 * stopped, sink_is_started returns false and engine_flush_begin must
 * reset immediately instead of burning the 200 ms fence cap.
 *
 * Rewritten in phase 12 and moved down the file to sit below
 * player_set_play_state, which it now goes through. It used to call
 * engine_pause and sink->stop itself, on the stated grounds that T1 had
 * no pause policy yet and the harness *was* T1. Amendment 05 gave T1
 * that policy, and going around it here would now be actively wrong in
 * two ways: p->play would stay PLAY_PLAYING with the device stopped,
 * which is IV.8.a's device-loss predicate and would fire the reopen
 * ladder mid-measurement; and the row would go on measuring a harness
 * path rather than the product one. This closes the "verify the real
 * pause policy, not just the fence skip" item that has been open since
 * phase 7.
 *
 * Timing is the assertion. A skipped fence returns in microseconds; a
 * fence that ran polls at 1 ms up to 200 ms.
 *
 * Takes the Player, the frame counter, the sample rate, and the seek
 * target; returns nothing (the verdict goes to stderr, headless only).
 */
static void
drive_seek_paused(Player * p, const atomic_uint_least64_t * pos,
                  int rate, int64_t seek_ms)
{
  /* The real pause: publishes PLAY_PAUSED, then stops the device. T2
     adopts the queued CMD_PAUSE on its own schedule, which is precisely
     why the device policy reads p->play and not engine_state. */
  engine_pause(p->e);
  (void) player_set_play_state(p, PLAY_PAUSED);
  fprintf(stderr, "seek-paused: device stopped, sink_is_started=%d\n",
          sink_is_started(&p->sink));

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  engine_seek(p->e, seek_ms);

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

  if (player_set_play_state(p, PLAY_PLAYING) != 0) {
    fprintf(stderr, "seek-paused: sink restart failed\n");
    return; /* the ladder owns the device now */
  }
  engine_resume(p->e);

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
 * DESIGN.md XI.1: after this many consecutive failures T1 stops
 * advancing. A queue of 500 unreadable files would otherwise spin T1 and
 * T2 at full speed with nothing to show for it.
 */
#define PLAYER_MAX_FAILURES 5

/*
 * player_now_playing_set: remember the basename of `path` for the status
 * bar. Copies rather than borrowing, because the QueueEntry it came from
 * does not outlive queue_remove or queue_clear. Takes the Player, a path
 * and the library id (0 when there is no library row), returns nothing.
 */
static void
player_now_playing_set(Player * p, const char * path, int64_t track_id)
{
  const char * base = strrchr(path, '/');
  base = (base != NULL) ? base + 1 : path;
  snprintf(p->now_name, sizeof p->now_name, "%s", base);
  /* The full key as well, for ACT_JUMP_TO_PLAYING. A path too long to
     fit is truncated rather than rejected: the jump then finds no row
     and reports, which is the same outcome as any other stale key and
     costs less than a second allocation story on this path. */
  snprintf(p->now_path, sizeof p->now_path, "%s", path);
  p->now_id = track_id;
  p->dur_ms = -1;
} /* player_now_playing_set */

/*
 * player_stop: the transition to PLAY_STOPPED, shared by end of queue
 * (DESIGN.md V.4), the five-failures cutoff (XI.1) and ACT_STOP.
 *
 * The order is state, timer, device -- V.4 as amended by amendment 05.
 * Only the first pair of those is load-bearing and it is inside
 * player_set_play_state: publish before stopping, so that "PLAY_PLAYING
 * while the device is not started" is never observable to the watchdog.
 * The timer disarm is placed first here to match the way V.4 reads; it
 * could equally go last, because once the state says stopped the
 * watchdog is inert whether or not a tick fires. Takes the Player,
 * returns nothing.
 */
static void
player_stop(Player * p)
{
  engine_stop(p->e);
  player_drain_cancel(p);  /* amendment 06 rule 4 */
  player_set_redraw(p, 0); /* VIII.3: nothing left to tick for */
  (void) player_set_play_state(p, PLAY_STOPPED);
  /* After the state change, so it takes the PLAY_STOPPED branch and
     clears T2's prefetch slot: nothing follows a stopped player, and a
     slot left full would be consumed at the next track's EOF. */
  player_prefetch_next(p);
  p->now_name[0] = '\0';
  p->now_path[0] = '\0';
  p->now_id      = 0;
  p->dur_ms      = -1;
} /* player_stop */

/*
 * player_play_entry: the T1 half of DESIGN.md V.1's track start, for one
 * queue entry. Takes the Player and the entry, returns 0 on success and
 * -1 if there is no device or no entry.
 *
 * Two orderings matter here and neither is visible from the call sites.
 *
 * The redraw timer is armed *before* the command is pushed, not after
 * (VIII.3's warning): T2 can open, decode and post EV_STREAM_INFO before
 * T1 gets another instruction, and arming afterwards leaves a window in
 * which the position is advancing with nothing repainting it.
 *
 * engine_play comes before the play transition, so T2 has the command in
 * hand while T1 starts the device. The reverse also works but is worse:
 * PulseAudio's ma_device_start pre-fills by invoking the data callback
 * before it uncorks, and against a pcm_ring T2 has not been told to fill
 * yet that pre-fill is guaranteed to underrun. This does not eliminate
 * the floor amendment 05 documented in XI.6 -- T2 may still not have
 * decoded anything by the time the pre-fill runs -- it just stops
 * guaranteeing it.
 */
static int
player_play_entry(Player * p, const QueueEntry * ent)
{
  if (ent == NULL) {
    return -1;
  }
  if (p->dev != DEV_READY) {
    /* XI.2's browse-only, and the DEV_LOST window inside the ladder.
       Queueing and browsing keep working; this one action does not. */
    player_report(p, "no audio device -- cannot play %s", ent->path);
    return -1;
  }

  /* Amendment 06 rules 4 and 5: CMD_PLAY flushes, so any drain still in
     flight names frames that will not exist a moment from now. Cleared
     before the command goes out, not after, so no tick can see the old
     value against a rewound counter. */
  player_drain_cancel(p);

  player_set_redraw(p, 1);
  engine_play(p->e, ent->path, ent->track_id);
  player_now_playing_set(p, ent->path, ent->track_id);

  /* CMD_PLAY flushes and resyncs both counters to 0 (V.1 step 3), so the
     new track's position is g_frames_consumed alone. The origin survives
     from IV.8.b step 5 otherwise, and a track started after a device loss
     would display the previous track's elapsed time added to its own. */
  p->pos_origin_ms = 0;

  if (player_set_play_state(p, PLAY_PLAYING) != 0) {
    /* The start failed and player_set_play_state has already entered the
       IV.8.b ladder (amendment 05's new entry point 2). T2 keeps
       decoding into pcm_ring and the ladder's step 6 restores
       PLAY_PLAYING, so this is not an error to report over the top of
       the ladder's own message. */
    return -1;
  }
  tui_mark_dirty();
  return 0;
} /* player_play_entry */

/*
 * player_queue_changed: everything that has to happen after the queue is
 * mutated. Takes the Player, returns nothing.
 *
 * One function because the two halves must never drift apart: the pane
 * has to be rebuilt so it shows the new order, and T2 has to be told
 * again what follows, because queue.h only guarantees
 * queue_peek_advance matches the following queue_advance when nothing
 * mutated in between. A mutation site that rebuilt the pane and forgot
 * the re-push would look completely correct and play the wrong track at
 * the next boundary.
 */
static void
player_queue_changed(Player * p)
{
  queueview_rebuild(p->q, p->db);
  player_prefetch_next(p);
} /* player_queue_changed */

/*
 * player_prefetch_next: tell T2 what follows the current track
 * (amendment 09). Takes the Player, returns nothing.
 *
 * Called on EV_STREAM_INFO and after every queue mutation, because
 * queue.h guarantees queue_peek_advance predicts the following
 * queue_advance only while nothing mutates in between -- in the shuffled
 * REPEAT_ALL wrap it predicts a permutation from a copy of the rand_r
 * state, and any edit invalidates the prediction.
 *
 * Clearing the slot is as important as filling it: at the end of the
 * queue T2 must fall through to the EV_EOF path, and a stale prefetch
 * from before the last entry was removed would play a track that is no
 * longer queued.
 */
static void
player_prefetch_next(Player * p)
{
  p->next_valid = 0;

  if (p->e == NULL) {
    return;
  }
  if (p->q == NULL || p->play == PLAY_STOPPED) {
    engine_set_next(p->e, NULL, 0);
    return;
  }

  const QueueEntry * next = queue_peek_advance(p->q);
  if (next == NULL || next->path == NULL) {
    engine_set_next(p->e, NULL, 0);
    return;
  }

  /* The position, not just the entry: the boundary pop needs somewhere
     to put the cursor. A pointer scan rather than an index out of
     queue_peek_advance, which does not report one -- the entry stays
     valid until the next mutation, and every mutation re-runs this. */
  size_t n = queue_count(p->q);
  for (size_t i = 0; i < n; i++) {
    if (queue_at(p->q, i) == next) {
      p->next_pos   = i;
      p->next_valid = 1;
      break;
    }
  }

  engine_set_next(p->e, next->path, next->track_id);
} /* player_prefetch_next */

/*
 * player_on_boundary: one popped boundary_ring entry -- the track T2
 * moved to has now actually reached the speakers (DESIGN.md V.3 step 5).
 * Takes the Player and the entry, returns nothing.
 *
 * The cursor follows the recorded prefetch position and then verifies
 * it, rather than searching for the track_id: an entry may carry
 * track_id 0, and ACT_ENQUEUE never dedupes, so a search is ambiguous by
 * construction. The fallbacks exist for the one race amendment 09
 * leaves open -- the queue mutated after T2 consumed the slot -- and
 * degrade rather than guess.
 */
static void
player_on_boundary(Player * p, const BoundaryEntry * b)
{
  size_t n = (p->q != NULL) ? queue_count(p->q) : 0;
  int    placed = 0;

  if (n > 0 && p->next_valid && p->next_pos < n) {
    const QueueEntry * e = queue_at(p->q, p->next_pos);
    if (e != NULL && e->track_id == b->track_id) {
      (void) queue_set_cursor(p->q, p->next_pos);
      placed = 1;
    }
  }

  if (!placed && n > 0 && b->track_id > 0) {
    for (size_t i = 0; i < n; i++) {
      const QueueEntry * e = queue_at(p->q, i);
      if (e != NULL && e->track_id == b->track_id) {
        (void) queue_set_cursor(p->q, i);
        placed = 1;
        break;
      }
    }
  }

  /* Neither worked: the queue no longer contains what is sounding. The
     cursor stays where it is rather than moving somewhere arbitrary, and
     now-playing below still names the truth. */
  const QueueEntry * cur = (placed && p->q != NULL)
                               ? queue_at(p->q, queue_cursor(p->q))
                               : NULL;
  if (cur != NULL && cur->path != NULL) {
    player_now_playing_set(p, cur->path, cur->track_id);
  }

  /* The elapsed clock restarts at the boundary, in the same frame domain
     the boundary is keyed on. The new track's duration was parked by
     EV_STREAM_INFO when T2 opened it, a ring's worth of audio ago; the
     boundary is where it becomes current. Anything else -- a boundary
     for a track no stream info was seen for -- leaves the length
     unknown rather than showing the previous track's. */
  int rate = (int) p->fmt.sample_rate;
  p->pos_origin_ms = (rate > 0)
                         ? -(int64_t) (b->frame_index * 1000u / (uint64_t) rate)
                         : 0;
  p->dur_ms     = (p->pend_dur_id > 0 && p->pend_dur_id == b->track_id)
                      ? p->pend_dur_ms
                      : -1;
  p->fail_count = 0; /* a track that reached its boundary played */

  queueview_rebuild(p->q, p->db);
  player_prefetch_next(p);
  tui_mark_dirty();
} /* player_on_boundary */

/*
 * player_drain_boundaries: pop every boundary whose frame has been
 * consumed. Takes the Player and the current consumed count, returns
 * nothing.
 *
 * T1 is the only reader of boundary_ring, which is what keeps it SPSC.
 * ring.h has no peek, so an entry that is read but not yet due is held
 * in p->pend_b until it is -- the loop must not drop it and must not
 * read a second one over it.
 */
static void
player_drain_boundaries(Player * p, uint64_t consumed)
{
  Ring * br = engine_boundary_ring(p->e);
  if (br == NULL) {
    return;
  }

  for (;;) {
    if (!p->pend_have) {
      if (ring_read(br, &p->pend_b, sizeof p->pend_b) != sizeof p->pend_b) {
        return;
      }
      p->pend_have = 1;
    }
    if (p->pend_b.frame_index > consumed) {
      return; /* not audible yet; it stays held for a later tick */
    }
    p->pend_have = 0;
    player_on_boundary(p, &p->pend_b);
  }
} /* player_drain_boundaries */

/*
 * player_advance: DESIGN.md V.1 step 5 and V.4 -- move to whatever plays
 * next and start it, or stop at the end of the queue.
 *
 * Queue policy lives on T1 and only on T1: the engine never decides what
 * plays next, which is the reason queue.c has no dependency on audio/
 * (DESIGN.md III.2). Takes the Player, returns nothing.
 */
static void
player_advance(Player * p)
{
  const QueueEntry * next = (p->q != NULL) ? queue_advance(p->q) : NULL;
  if (next == NULL) {
    /* End of queue. V.4 as amended: stop the device rather than leaving
       T3 writing silence at ~53 wakeups a second forever. */
    player_stop(p);
    player_report(p, "end of queue");
    return;
  }
  (void) player_play_entry(p, next);
} /* player_advance */

/*
 * player_drain_cancel: forget an in-flight EOF drain. Takes the Player,
 * returns nothing.
 *
 * Called from the three places that flush pcm_ring behind T1's back --
 * player_play_entry (CMD_PLAY, V.1 step 3), player_seek_relative
 * (CMD_SEEK, IV.7) and player_ladder_attempt (IV.8.b step 4) -- plus
 * player_stop, which ends the track the drain belonged to. Those four
 * cover every action in amendment 06 rule 4 without a clear in each
 * action handler: ACT_NEXT and ACT_PREV reach player_play_entry,
 * ACT_STOP and ACT_CLEAR_QUEUE reach player_stop.
 */
static void
player_drain_cancel(Player * p)
{
  p->drain_until = 0;
  p->drain_ticks = 0;
  /* A held boundary entry goes with it, and for the identical reason:
     its frame_index is a value in the pre-flush frame domain, and
     pcm_flush_and_resync rewinds both counters and resets boundary_ring
     under it. Keeping it would fire the next transition immediately
     against a counter that just went back to zero. */
  p->pend_have  = 0;
  p->next_valid = 0;
} /* player_drain_cancel */

/*
 * player_drain_finish: the EOF drain is over -- run the queue policy that
 * EV_EOF used to run directly. Takes the Player, returns nothing.
 *
 * Reached from the tick when g_frames_consumed catches up with
 * drain_until, from the tick's bound when it does not, and immediately
 * from the EV_EOF arm when there was nothing queued to drain. The split
 * by run kind is the one EV_EOF always had: a headless --play is a
 * one-shot harness whose run ends here, the TUI has a queue to fall
 * through to.
 */
static void
player_drain_finish(Player * p)
{
  player_drain_cancel(p);

  if (!p->ui) {
    player_set_redraw(p, 0); /* VIII.3: nothing left to tick for */
    p->running = 0;
    return;
  }
  player_advance(p);
} /* player_drain_finish */

/*
 * player_track_failed: the shared arm of EV_OPEN_FAILED and
 * EV_DECODE_ERROR (DESIGN.md XI.1, plan.md VII gap 7 as ruled
 * 2026-08-13). Takes the Player, a short noun for the message, and the
 * averror; returns nothing.
 *
 * The rule is reset on evidence of playback progress, not on evidence of
 * opening. The plan originally proposed resetting on a successful
 * EV_STREAM_INFO, and that was rejected: a valid container header with a
 * corrupt first frame -- the shape of every truncated download -- opens
 * fine, posts EV_STREAM_INFO, and dies on the first read, so that reset
 * would spin forever in exactly the case this counter exists to stop.
 *
 * One second rather than "any frames at all" is deliberate too: a file
 * that yields 100 frames (~2 ms) and dies would reset the counter and
 * spin at very nearly full speed. The worst case is now bounded at five
 * tracks times one second.
 *
 * Two invariants this leans on, both verified rather than assumed.
 * g_frames_consumed is 0 when either event arrives for a track that
 * played nothing, because do_play calls pcm_flush_and_resync(e, 0)
 * before evt_push on both its paths (src/audio/engine.c:501) -- there is
 * no stale count from the previous track to misread. And an underrun
 * cannot inflate it, because IV.2 counts frames actually read out of
 * pcm_ring and never the device's requested frameCount.
 */
static void
player_track_failed(Player * p, const char * what, int averror)
{
  uint64_t consumed =
      atomic_load_explicit(sink_frames_consumed(), memory_order_acquire);
  int rate = (int) p->fmt.sample_rate;

  if (rate > 0 && consumed >= (uint64_t) rate) {
    p->fail_count = 1; /* this track played; it is the run's first failure */
  } else {
    p->fail_count++;
  }

  if (p->fail_count >= PLAYER_MAX_FAILURES) {
    player_stop(p);
    p->fail_count = 0;
    player_report(p, "%d consecutive failures, stopped", PLAYER_MAX_FAILURES);
    return;
  }

  player_report(p, "%s (error %d) -- skipping", what, averror);
  player_advance(p);
} /* player_track_failed */

/*
 * player_gain_push: hand T1's authoritative gain to the audio path.
 *
 * Through engine_set_gain rather than sink_set_gain directly, because
 * DESIGN.md IV's T1->T2 table lists set_gain(float) as a command and the
 * handler is a one-line sink_set_gain (src/audio/engine.c). Routing
 * around T2 would work -- the store is a relaxed atomic -- and would put
 * a second writer on a value the command table says belongs to one.
 * Nothing is lost by going the documented way: the gain is applied in the
 * callback rather than in the decoder (IV.2 step 7), so the change is
 * audible within one device period either way, not after the ring
 * drains. Takes the Player, returns nothing.
 */
static void
player_gain_push(Player * p)
{
  if (p->e != NULL) {
    engine_set_gain(p->e, p->gain);
  }
} /* player_gain_push */

/*
 * player_toggle_pause: ACT_PLAY_PAUSE, all three directions.
 *
 * The two live edges are ordered oppositely and player_set_play_state
 * owns that; what belongs here is the redraw timer, which is the other
 * half of the "paused costs nothing" claim. Disarming it is what takes T1
 * to zero wakeups; the device stop inside player_set_play_state is what
 * takes T3 there (VIII.3, amendment 05).
 *
 * Resuming arms the timer *before* the state moves, for VIII.3's
 * arm-before-push reason: the device starts consuming the ring the
 * instant it is uncorked, and arming afterwards leaves a window in which
 * the position advances with nothing repainting it.
 *
 * Takes the Player, returns nothing.
 */
static void
player_toggle_pause(Player * p)
{
  switch (p->play) {
    case PLAY_PLAYING:
      engine_pause(p->e);
      (void) player_set_play_state(p, PLAY_PAUSED);
      player_set_redraw(p, 0);
      player_report(p, "paused");
      break;

    case PLAY_PAUSED:
      if (p->dev != DEV_READY) {
        player_report(p, "no audio device -- cannot resume");
        return;
      }
      player_set_redraw(p, 1);
      if (player_set_play_state(p, PLAY_PLAYING) != 0) {
        /* The start failed; the ladder owns the device and has already
           reported. Leaving the timer armed is deliberate -- IV.8.b step
           6 restores PLAY_PLAYING and the tick is what drives the
           watchdog that observes the recovery. */
        return;
      }
      engine_resume(p->e);
      player_report(p, "playing: %s", p->now_name);
      break;

    case PLAY_STOPPED: {
      /* Nothing is loaded, so this is "start what the queue is pointing
         at" rather than a transport toggle. */
      const QueueEntry * cur =
          (p->q != NULL && queue_count(p->q) > 0) ? queue_at(p->q, queue_cursor(p->q))
                                                  : NULL;
      if (cur == NULL) {
        player_report(p, "nothing to play");
        return;
      }
      p->fail_count = 0; /* an explicit user action starts a fresh streak */
      (void) player_play_entry(p, cur);
      break;
    }
  }
  tui_mark_dirty();
} /* player_toggle_pause */

/*
 * player_skip: ACT_NEXT and ACT_PREV. Takes the Player and +1 or -1,
 * returns nothing.
 *
 * queue_skip_next/_prev, never queue_advance: advance applies repeat
 * policy and belongs to the EOF path, where "what plays next on its own"
 * is the question. This one asks "what did the user ask for", and repeat
 * one would otherwise make ACT_NEXT replay the same track forever.
 *
 * Running off either end reports and changes nothing -- in particular it
 * does not stop what is playing, because a mistyped skip should not
 * silence the player.
 */
static void
player_skip(Player * p, int dir)
{
  if (p->q == NULL || queue_count(p->q) == 0) {
    player_report(p, "queue is empty");
    return;
  }

  const QueueEntry * next = (dir > 0) ? queue_skip_next(p->q) : queue_skip_prev(p->q);
  if (next == NULL) {
    player_report(p, (dir > 0) ? "end of queue" : "start of queue");
    return;
  }

  p->fail_count = 0;
  (void) player_play_entry(p, next);
} /* player_skip */

/*
 * player_seek_relative: ACT_SEEK_FWD and ACT_SEEK_BACK, whose binding arg
 * is the step in milliseconds. Takes the Player and a signed delta,
 * returns nothing.
 *
 * Seeking while paused is legal and is the interesting case: with the
 * device stopped, IV.7.5's fence skip is what keeps the flush from
 * burning the full 200 ms cap, and amendment 05 made that path the
 * product path rather than a harness one.
 */
static void
player_seek_relative(Player * p, int32_t delta_ms)
{
  if (p->dev != DEV_READY) {
    player_report(p, "no audio device -- cannot seek");
    return;
  }
  if (p->play == PLAY_STOPPED) {
    player_report(p, "nothing playing");
    return;
  }

  int64_t target = player_position_ms(p) + (int64_t) delta_ms;
  if (target < 0) {
    target = 0;
  }
  if (p->dur_ms > 0 && target > p->dur_ms) {
    target = p->dur_ms;
  }

  engine_seek(p->e, target);

  /* Amendment 06 rule 5: the flush this seek causes rewinds both frame
     counters, so a drain armed against the pre-seek domain is
     meaningless. The decoder resumes from the target and posts a fresh
     EV_EOF with a new end_frame when it gets there. */
  player_drain_cancel(p);

  /* IV.7.6 sets both frame counters to target * R / 1000, so from here
     g_frames_consumed is absolute track time and the reopen origin must
     go with it. Without this, every seek after a device loss would show
     the target plus however far the track had got when the device
     vanished. */
  p->pos_origin_ms = 0;

  player_report(p, "seek to %lld:%02lld", (long long) (target / 60000),
                (long long) ((target / 1000) % 60));
  tui_mark_dirty();
} /* player_seek_relative */

/*
 * player_volume_step: ACT_VOL_UP and ACT_VOL_DOWN, whose binding arg is
 * the step in percent. Takes the Player and a signed step, returns
 * nothing.
 *
 * Adjusting the volume while muted unmutes (gap 4, ruled 2026-08-13):
 * changing a saved value behind a silent output makes the indicator lie
 * about what the next unmute will sound like, and that is precisely the
 * bug the ruling exists to forbid.
 *
 * Works with no device, deliberately. Volume is state, not playback, and
 * it must be right when the reopen ladder brings the device back.
 */
static void
player_volume_step(Player * p, int32_t step_pct)
{
  p->muted = 0;

  long want = (long) p->vol_pct + (long) step_pct;
  if (want < 0) {
    want = 0;
  }
  if (want > 100) {
    want = 100;
  }
  p->vol_pct = (int) want;
  p->gain    = (float) p->vol_pct / 100.0f;

  player_gain_push(p);
  player_report(p, "volume %d%%", p->vol_pct);
  tui_mark_dirty();
} /* player_volume_step */

/*
 * player_toggle_mute: ACT_MUTE, and the rest of gap 4's ruling. Takes the
 * Player, returns nothing.
 *
 * Unmuting restores `gain` by assignment from the saved float, not by
 * recomputing it from `vol_pct` -- which is why mute never touches
 * `vol_pct` at all. The two are kept apart on purpose: the percentage is
 * what the status bar shows and the float is what the callback
 * multiplies by, and only the second has to come back bit for bit.
 */
static void
player_toggle_mute(Player * p)
{
  if (p->muted) {
    p->gain  = p->mute_saved;
    p->muted = 0;
  } else {
    p->mute_saved = p->gain;
    p->gain       = 0.0f;
    p->muted      = 1;
  }

  player_gain_push(p);
  if (p->muted) {
    player_report(p, "muted");
  } else {
    player_report(p, "unmuted (%d%%)", p->vol_pct);
  }
  tui_mark_dirty();
} /* player_toggle_mute */

/*
 * player_clear_queue: ACT_CLEAR_QUEUE. Takes the Player, returns nothing.
 *
 * Stops first, then clears. Clearing under a live track would leave
 * queue_advance returning NULL at the next EOF anyway, so the track would
 * stop regardless -- just minutes later and for no reason the user could
 * connect to the key they pressed. now_name and now_path are copies, so
 * nothing dangles either way.
 */
static void
player_clear_queue(Player * p)
{
  if (p->q == NULL) {
    player_report(p, "no queue");
    return;
  }
  if (p->play != PLAY_STOPPED) {
    player_stop(p);
  }
  queue_clear(p->q);
  p->fail_count = 0;
  player_queue_changed(p);
  player_report(p, "queue cleared");
  tui_mark_dirty();
} /* player_clear_queue */

/*
 * player_queue_playing_index: the play-order position of the entry that
 * is sounding, or -1 when nothing is. Takes the Player.
 *
 * The queue cursor is the answer while playing: it is what player_advance
 * and player_play_entry move, and after phase 17e it is also what a track
 * boundary moves. Stopped, there is no playing entry and the pane must
 * not mark one -- a stale highlight on the last thing heard reads as
 * "this is playing" and is the one thing the marker exists to say.
 */
static int
player_queue_playing_index(const Player * p)
{
  if (p->q == NULL || p->play == PLAY_STOPPED || queue_count(p->q) == 0) {
    return -1;
  }
  return (int) queue_cursor(p->q);
} /* player_queue_playing_index */

/*
 * player_queue_remove: ACT_REMOVE against the queue pane's cursor. Takes
 * the Player, returns nothing.
 *
 * Removing the entry that is playing does not stop playback: the audio
 * already in flight belongs to a track that is open, and killing it
 * because its queue row went away would make "tidy the queue" a
 * transport action. What plays next comes from wherever the cursor lands,
 * which queue_remove has already decided.
 */
static void
player_queue_remove(Player * p)
{
  if (p->q == NULL || queue_count(p->q) == 0) {
    player_report(p, "queue is empty");
    return;
  }

  size_t at = queueview_cursor();
  if (at >= queue_count(p->q)) {
    at = queue_count(p->q) - 1;
  }
  if (queue_remove(p->q, at) != 0) {
    player_report(p, "could not remove entry %zu", at + 1);
    return;
  }

  player_queue_changed(p);
  queueview_cursor_set(at);
  player_report(p, "removed entry %zu (%zu left)", at + 1, queue_count(p->q));
  tui_mark_dirty();
} /* player_queue_remove */

/*
 * player_queue_move: ACT_MOVE_UP / ACT_MOVE_DOWN. Takes the Player and
 * the direction (-1 or 1), returns nothing.
 *
 * The cursor follows the entry rather than staying at the position, so
 * holding the key walks one entry up the list instead of swapping two
 * entries repeatedly.
 */
static void
player_queue_move(Player * p, int dir)
{
  if (p->q == NULL || queue_count(p->q) == 0) {
    player_report(p, "queue is empty");
    return;
  }

  size_t n    = queue_count(p->q);
  size_t from = queueview_cursor();
  if (from >= n) {
    from = n - 1;
  }
  if ((dir < 0 && from == 0) || (dir > 0 && from + 1 >= n)) {
    return; /* already at the end it is being moved toward */
  }

  size_t to = (dir < 0) ? from - 1 : from + 1;
  if (queue_move(p->q, from, to) != 0) {
    player_report(p, "could not move entry %zu", from + 1);
    return;
  }

  player_queue_changed(p);
  queueview_cursor_set(to);
  tui_mark_dirty();
} /* player_queue_move */

/*
 * player_queue_confirm: Enter in the queue pane -- play the entry under
 * the cursor. Takes the Player, returns nothing.
 *
 * An explicit user action, so it is a real CMD_PLAY and therefore
 * flushes (DESIGN.md V.1 step 3), unlike the natural transition phase
 * 17e adds.
 */
static void
player_queue_confirm(Player * p)
{
  if (p->q == NULL || queue_count(p->q) == 0) {
    player_report(p, "queue is empty");
    return;
  }

  size_t at = queueview_cursor();
  if (at >= queue_count(p->q)) {
    at = queue_count(p->q) - 1;
  }
  if (queue_set_cursor(p->q, at) != 0) {
    player_report(p, "could not select entry %zu", at + 1);
    return;
  }
  p->fail_count = 0; /* an explicit user action starts a fresh streak */
  (void) player_play_entry(p, queue_at(p->q, at));
} /* player_queue_confirm */

/*
 * player_locate: move the browser to the library track with this id, in
 * whichever view is showing. Takes the Player, the id and a name for the
 * message, returns nothing.
 *
 * Two callers: ACT_JUMP_TO_PLAYING and confirming a search hit. They are
 * the same operation with a different track, and are one function rather
 * than two because the part worth getting right -- the three-way "where
 * does this track sit in this view" -- is shared, and a second copy of
 * it would be a second thing to update when a fourth view lands.
 *
 * The keys come from browser.c and not from here. Until now this
 * function took strrchr of the path and passed the halves to
 * browser_goto, which was right when a key was a path and silently wrong
 * the moment it stopped being one: a library key is a kind byte and a
 * payload, so every comparison missed by one byte and the jump reported
 * "cannot reach" for a track that was on screen.
 */
static void
player_locate(Player * p, int64_t track_id, const char * label)
{
  if (p->br == NULL) {
    player_report(p, "nothing to browse");
    return;
  }
  if (p->db == NULL || track_id <= 0) {
    /* A --play run has no library row to find, which is less a failure
       of the jump than an absence of anywhere to jump to. */
    player_report(p, "no library entry for %s", label);
    return;
  }

  DbTrackRow t;
  memset(&t, 0, sizeof t);
  if (db_query_track_by_id(p->db, track_id, &t) != 1) {
    player_report(p, "no library entry for %s", label);
    return;
  }

  char * parent = NULL;
  char * row    = NULL;
  int    rc     = browser_library_locate(&p->brsrc[p->view], &t, &parent, &row);
  db_track_row_free_fields(&t);

  if (rc != 0) {
    /* The track exists but has no row in this view -- no artist tag in
       the Artists view, say. Saying which view is what makes the next
       keystroke obvious: the same track is reachable one view over. */
    player_report(p, "%s is not in this view", label);
    return;
  }

  rc = browser_goto(p->br, parent, row);
  free(parent);
  free(row);

  if (rc != 0) {
    player_report(p, "cannot reach %s", label);
    return;
  }
  tui_mark_dirty();
} /* player_locate */

/*
 * player_jump_to_playing: ACT_JUMP_TO_PLAYING -- move the browser to the
 * playing track. Takes the Player, returns nothing.
 */
static void
player_jump_to_playing(Player * p)
{
  if (p->br == NULL) {
    player_report(p, "nothing to browse");
    return;
  }
  if (p->now_path[0] == '\0') {
    player_report(p, "nothing playing");
    return;
  }

  player_locate(p, p->now_id, p->now_name);
} /* player_jump_to_playing */

/*
 * player_retry_device: ACT_RETRY_DEVICE -- the manual way out of
 * browse-only (DESIGN.md XI.2). Takes the Player, returns nothing.
 *
 * player_ladder_begin is the wrong entry point here: it returns early
 * unless the device is DEV_READY, which is exactly what browse-only is
 * not, and its steps 1 and 2 park T2 and tear down a device that does
 * not exist. This sets up the ladder's own state and enters at the
 * attempt, so the retry, the backoff and the 60 s give-up are all the
 * ones IV.8.b already specifies rather than a second implementation.
 *
 * The one correction it has to make afterwards: step 6 ends with
 * `p->play = PLAY_PLAYING` unconditionally, which is right for both of
 * the ladder's own entry points -- a loss is only ever declared from
 * PLAY_PLAYING, and the other is a failed play transition -- and wrong
 * for this third one, where nothing was ever loaded. Left alone it would
 * claim to be playing and hold a started device against a silent ring,
 * which is precisely the state amendment 05 exists to prevent. Correcting
 * it here rather than making step 6 conditional keeps the change out of
 * the verified ladder.
 */
static void
player_retry_device(Player * p)
{
  if (p->dev == DEV_READY) {
    player_report(p, "device: already open");
    return;
  }
  if (p->dev == DEV_LOST) {
    player_report(p, "device: reopen already in progress");
    return;
  }

  int was_loaded = (p->now_path[0] != '\0');

  p->dev               = DEV_LOST;
  p->ladder_attempt    = 0;
  p->ladder_elapsed_ms = 0;
  p->notify_muted      = 0;
  if (!was_loaded) {
    p->pos_origin_ms = 0; /* nothing to resume, so nothing to resume *at* */
  }
  clock_gettime(CLOCK_MONOTONIC, &p->loss_at);

  /* Step 1's park, kept even though this entry point skips the rest of
     steps 1 and 2. The attempt's sink.open frees and reallocates
     pcm_ring, and that is only legal with both touchers provably gone.
     T2 is already parked on both roads into browse-only -- startup never
     loaded a track, and ladder exhaustion parked it at step 1 and never
     resumed -- so this is a positive guarantee replacing an inferred one,
     and engine_park_sync is idempotent when T2 is already parked. */
  engine_park_sync(p->e);

  player_report(p, "device: retrying");
  player_ladder_attempt(p);

  if (p->dev == DEV_READY && !was_loaded) {
    /* Undo step 6's resume for the no-track case: engine_stop, the timer
       disarmed, and the state published before the device stops -- all of
       which player_stop already does in the right order. */
    player_stop(p);
    player_report(p, "device: reopened -- nothing playing");
  }
} /* player_retry_device */

/*
 * player_action: every action that moves the player through a state --
 * transport, the queue, device recovery. Declared above action_dispatch,
 * which handles ACT_QUIT and navigation itself and delegates the rest
 * here. Takes the Player, the action and its binding arg; returns
 * nothing.
 *
 * An unimplemented action still reports itself, which is what keeps the
 * binding table exercised end to end for the actions phases 13-17 own.
 */
static void
player_action(Player * p, Action a, int32_t arg)
{
  switch (a) {
    case ACT_PLAY_PAUSE:
      player_toggle_pause(p);
      return;

    case ACT_STOP:
      if (p->play == PLAY_STOPPED) {
        player_report(p, "nothing playing");
        return;
      }
      player_stop(p);
      player_report(p, "stopped");
      tui_mark_dirty();
      return;

    case ACT_NEXT:
      player_skip(p, 1);
      return;

    case ACT_PREV:
      player_skip(p, -1);
      return;

    /* The binding table carries the step as a positive magnitude in both
       directions, so the sign is the action's, not the arg's. */
    case ACT_SEEK_FWD:
      player_seek_relative(p, arg);
      return;

    case ACT_SEEK_BACK:
      player_seek_relative(p, -arg);
      return;

    case ACT_VOL_UP:
      player_volume_step(p, arg);
      return;

    case ACT_VOL_DOWN:
      player_volume_step(p, -arg);
      return;

    case ACT_MUTE:
      player_toggle_mute(p);
      return;

    case ACT_TOGGLE_SHUFFLE:
      if (p->q == NULL) {
        player_report(p, "no queue");
        return;
      }
      queue_set_shuffle(p->q, !queue_shuffle(p->q));
      /* Both toggles re-push the prefetch: neither changes the entries,
         but both change which one comes next, and T2 is holding the old
         answer (amendment 09 IV). */
      player_queue_changed(p);
      player_report(p, "shuffle %s", queue_shuffle(p->q) ? "on" : "off");
      tui_mark_dirty();
      return;

    case ACT_CYCLE_REPEAT: {
      if (p->q == NULL) {
        player_report(p, "no queue");
        return;
      }
      /* off -> all -> one -> off (phase-9-queue.md III.3). */
      static const char * const name[] = { "off", "all", "one" };
      RepeatMode next = (RepeatMode) ((queue_repeat(p->q) + 1) % 3);
      queue_set_repeat(p->q, next);
      player_queue_changed(p);
      player_report(p, "repeat %s", name[next]);
      tui_mark_dirty();
      return;
    }

    case ACT_CLEAR_QUEUE:
      player_clear_queue(p);
      return;

    case ACT_REMOVE:
      player_queue_remove(p);
      return;

    case ACT_MOVE_UP:
      player_queue_move(p, -1);
      return;

    case ACT_MOVE_DOWN:
      player_queue_move(p, 1);
      return;

    case ACT_JUMP_TO_PLAYING:
      player_jump_to_playing(p);
      return;

    case ACT_RETRY_DEVICE:
      player_retry_device(p);
      return;

    case ACT_RESCAN:
      player_rescan(p);
      return;

    case ACT_ENQUEUE: {
      if (p->br == NULL || p->q == NULL) {
        player_report(p, "nothing to queue");
        return;
      }
      const BrowserRow * sel = browser_selection(p->br);
      if (sel == NULL) {
        player_report(p, "nothing selected");
        return;
      }
      player_enqueue_row(p, sel);
      return;
    }

    case ACT_PLAY_NOW: {
      if (p->br == NULL || p->q == NULL) {
        player_report(p, "nothing to play");
        return;
      }
      const BrowserRow * sel = browser_selection(p->br);
      if (sel == NULL) {
        player_report(p, "nothing selected");
        return;
      }
      if (sel->is_dir) {
        player_play_container(p, sel);
        return;
      }

      /* Two things this must not do, and the second is the one that
         looks right and is not.
         If the path is already queued, play that entry rather than
         inserting a second copy: Enter on a row the user already added
         is "go there", not "queue it twice". Deliberate duplicates are
         still reachable through ACT_ENQUEUE, which never dedupes.
         Otherwise insert directly after the current entry rather than
         appending. Appending would put the new track last in play
         order, so queue_advance would return NULL the moment it
         finished and everything already queued behind it would never
         play -- a queue that always stops after exactly one track. */
      char *  ppath = NULL;
      int64_t pid   = 0;
      if (player_row_target(p, sel, &ppath, &pid) != 0) {
        player_report(p, "could not play %s", sel->name);
        return;
      }

      size_t at = 0;
      if (!player_queue_find(p->q, ppath, &at)) {
        at = (queue_count(p->q) == 0) ? 0 : queue_cursor(p->q) + 1;
        if (queue_insert(p->q, at, pid, ppath) != 0) {
          free(ppath);
          player_report(p, "could not queue %s", sel->name);
          return;
        }
      }
      free(ppath);
      player_queue_changed(p);
      /* Set the cursor explicitly: queue_insert adjusts it to keep
         referring to whatever was current *before* the insert, which is
         the opposite of what playing the new entry needs. */
      if (queue_set_cursor(p->q, at) != 0) {
        player_report(p, "could not select %s", sel->name);
        return;
      }
      p->fail_count = 0; /* an explicit user action starts a fresh streak */
      (void) player_play_entry(p, queue_at(p->q, at));
      return;
    }

    default:
      break;
  }

  player_report(p, "%s (arg %d) -- not implemented yet", action_name(a),
                (int) arg);
} /* player_action */

/*
 * ScanUi: the ScanProgressFn ctx for ACT_RESCAN. Carries the Player the
 * progress line is painted onto and the same two counts the stderr
 * tally keeps, so the closing line can name what was indexed.
 */
typedef struct {
  Player * p;
  int      done;
  int      total;
} ScanUi;

/*
 * scan_progress_statusbar: the second implementation of ScanProgressFn,
 * and the reason the seam exists (DESIGN.md V.5). Takes the ctx, the
 * counts and the current path; returns nothing.
 *
 * Never writes to stderr: the terminal is claimed by termbox2 here, and
 * a printf on this path corrupts the display rather than logging
 * anything. The scan is synchronous on T1, so this is also the only
 * thing repainting for its duration -- the event loop does not run
 * again until scan_run returns.
 */
static void
scan_progress_statusbar(void * ctx, int done, int total, const char * path)
{
  ScanUi * u = (ScanUi *) ctx;
  (void) path;

  if (u == NULL || u->p == NULL) {
    return;
  }
  u->done  = done;
  u->total = total;

  /* ponytail: repaint every SCAN_UI_EVERY files rather than on a clock.
     One tb_present per file is the thing to avoid and this avoids it;
     if a slow root ever makes the line look frozen between two
     multiples, this becomes a timerfd read, not a smaller modulus. */
  if (done % SCAN_UI_EVERY != 0 && done != total) {
    return;
  }

  snprintf(u->p->message, sizeof u->p->message, "scanning %d/%d", done, total);

  TuiModel m;
  player_fill_model(u->p, &m);
  tui_draw(&m);
} /* scan_progress_statusbar */

/*
 * player_rescan: ACT_RESCAN. Takes the Player, returns nothing.
 *
 * The scan is synchronous on T1 and blocks the UI for its duration,
 * which is accepted for v1 (DESIGN.md V.5). A warm rescan is stat-bound;
 * a cold one on a large library is not, and after the gapless transition
 * lands a rescan long enough to starve the decoder's lead will underrun
 * and can miss a track boundary. A background scan is post-v1 and lands
 * on this same callback without changing scan.c.
 */
static void
player_rescan(Player * p)
{
  if (p->db == NULL) {
    player_report(p, "no library index");
    return;
  }

  const char ** root_paths = calloc(p->cfg.nroots, sizeof *root_paths);
  if (root_paths == NULL) {
    player_report(p, "out of memory, rescan skipped");
    return;
  }
  for (size_t i = 0; i < p->cfg.nroots; i++) {
    root_paths[i] = p->cfg.roots[i].path;
  }

  ScanUi   ui = { p, 0, 0 };
  ScanOpts so = {
    .roots        = root_paths,
    .nroots       = p->cfg.nroots,
    .ignore_names = (const char * const *) p->cfg.ignore,
    .nignore      = p->cfg.nignore,
    .on_progress  = scan_progress_statusbar,
    .ctx          = &ui,
  };

  int skipped = scan_run(p->db, &so);
  free(root_paths);

  if (skipped < 0) {
    /* A transaction-level failure rolls back, so the index is exactly
       what it was and nothing below needs invalidating. */
    player_report(p, "rescan failed, index unchanged");
    tui_mark_dirty();
    return;
  }

  /* Every column and the queue's cached metadata are now stale: rows
     may have gone, and a queued entry's track_id may no longer resolve.
     The queue itself is not touched -- an entry whose id is dangling
     falls back to its path, which is the branch DESIGN.md III.2 has
     carried since phase 9 and which this is the v1 consumer of. */
  for (int v = 0; v < BROWSER_VIEW_COUNT; v++) {
    if (p->brv[v] != NULL) {
      (void) browser_reload(p->brv[v]);
    }
  }
  queueview_rebuild(p->q, p->db);

  int indexed = ui.done - skipped;
  if (skipped > 0) {
    player_report(p, "rescan complete, %d track(s) indexed, %d skipped",
                  (indexed > 0) ? indexed : 0, skipped);
  } else {
    player_report(p, "rescan complete, %d track(s) indexed",
                  (indexed > 0) ? indexed : 0);
  }
  tui_mark_dirty();
} /* player_rescan */

/*
 * player_watchdog: DESIGN.md IV.8.a's authoritative detector. On every
 * redraw tick, if the engine is playing and the device is not started,
 * declare device loss. One atomic load per tick, and the tick is armed
 * only while playing, so it costs nothing when it cannot fire.
 *
 * It exists because the notification cannot be trusted: miniaudio raises
 * `stopped` only when onDeviceStop returned MA_SUCCESS, and PulseAudio's
 * onDeviceStop is a cork that fails when the stream is already dead --
 * exactly the yanked-DAC case.
 *
 * Amendment 07 (2026-08-15): the ma_device_get_state poll this function
 * was built on is necessary and NOT sufficient, and calling it
 * authoritative was wrong. Measured: killing the sound server under a
 * running stream produces no notification AND no state change, so the
 * loss went undetected until a keypress next tried to start the device.
 * ma_device_get_state records what we last asked for, not whether the
 * device lives. The authoritative detector is callback liveness --
 * g_cb_seq, below. Either detector firing declares a loss. Worst-case
 * latency is CB_STALL_TICKS ticks from the last callback invocation.
 *
 * Takes the Player, returns nothing.
 */
static void
player_watchdog(Player * p)
{
  unsigned seq;

  if (atomic_load_explicit(&g_shutdown, memory_order_relaxed)) {
    return; /* teardown stops the device on purpose */
  }
  /* T1's PlayState, not engine_state. Amendment 05's correction: T2
     adopts a queued CMD_PAUSE whenever it next drains, so between T1's
     pause and T2 noticing it, engine_state still reads ENG_PLAYING
     while the device is legitimately stopped -- this predicate exactly,
     and a false loss on every pause. T1 is the thread that stops the
     device, so T1's own view is the one that can never disagree with
     it. See PlayState's definition. */
  if (p->dev != DEV_READY || p->play != PLAY_PLAYING) {
    return;
  }
  if (!sink_is_started(&p->sink)) {
    /* Phase 8's original detector. Kept: it catches a loss we caused
       ourselves getting the IV.9.a.i edge ordering wrong, which is what
       amendment 05's note calls its second job. It does NOT catch a dead
       sound server -- see the stall check below. */
    player_ladder_begin(p, "poll");
    return;
  }

  /* Amendment 07. The device claims to be started; ask T3 whether it is
     actually running. Five consecutive silent ticks is ~165 ms, or nine
     missed periods at the 900-frame period measured on this host.
     ponytail: fixed tick count rather than derived from
     SessionFormat.period_frames, whose comment reserves it for the
     status bar. The ceiling is a backend whose period exceeds ~165 ms;
     derive it from period_frames if one ever appears. */
  seq = atomic_load_explicit(sink_cb_seq(), memory_order_acquire);
  if (seq != p->cb_seq_last) {
    p->cb_seq_last    = seq;
    p->cb_stall_ticks = 0;
    return;
  }
  if (++p->cb_stall_ticks < CB_STALL_TICKS) {
    return;
  }

  /* Deliberately not g_frames_consumed: IV.2 counts frames actually read
     out of pcm_ring, so a legitimate underrun advances it by nothing and
     decoder starvation would be indistinguishable from device death.
     g_cb_seq advances through an underrun because the callback still ran
     and still wrote its silence. */
  player_ladder_begin(p, "cb-stall");
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
      /* The first point at which the track is provably open and its
         duration is known, which is what the progress bar needs. It is
         deliberately NOT where the failure counter resets -- see
         player_track_failed for why that would defeat the counter. */
      if (ev->track_id > 0 && ev->track_id != p->now_id) {
        /* amendment 09's prefetch: T2 has opened what follows, but it
           will not be heard until its boundary. Parked, not applied --
           dur_ms names what is sounding. */
        p->pend_dur_id = ev->track_id;
        p->pend_dur_ms = ev->info.duration_ms;
      } else {
        p->dur_ms = ev->info.duration_ms;
      }
      if (p->ui) {
        /* The first point at which the current track is provably open,
           and therefore the point at which "what follows it" is worth
           telling T2 (amendment 09). */
        player_prefetch_next(p);
        tui_mark_dirty();
        break;
      }
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
          drive_seek_paused(p, sink_frames_consumed(),
                            (int) p->fmt.sample_rate, p->mode_arg);
          p->running = 0;
          break;
        case PLAY_PLAIN:
          break;
      }
      break;

    /* The three terminal events split by the kind of run. A headless
       --play is a one-shot harness: one file, then exit, and its status
       is the verdict. The TUI is a player and has a queue to fall
       through to, so the same event advances instead of ending the run.
       Neither branch is the other's special case -- --play must keep
       reporting a failed file through its exit status, because that is
       what every sanitiser invocation in the verify tables reads. */
    case EV_OPEN_FAILED:
      if (!p->ui) {
        player_report(p, "open failed (error %d)", ev->averror);
        p->status  = 1;
        p->running = 0;
        break;
      }
      player_track_failed(p, "open failed", ev->averror);
      break;

    case EV_DECODE_ERROR:
      if (!p->ui) {
        player_report(p, "decode error (error %d)", ev->averror);
        p->status  = 1;
        p->running = 0;
        break;
      }
      player_track_failed(p, "decode error", ev->averror);
      break;

    case EV_EOF: {
      /* A track that reached EOF played, whatever happened earlier in
         the run, so the failure streak is broken here as well as by the
         one-second rule in player_track_failed. Amendment 06 rule 3: at
         EOF, not at drain completion -- what happens to the tail says
         nothing about whether the file decoded. */
      p->fail_count = 0;

      /* Amendment 06: EOF is T2's news, not T3's. Up to PCM_LEAD_SECONDS
         of this track is still sitting in pcm_ring, and the old code ran
         the queue policy on top of it -- player_stop stopped the device
         over the tail, a fresh CMD_PLAY flushed it away. Hold
         PLAY_PLAYING until the consumed counter reaches T2's final write
         count. Nothing to wait for is the normal case for a file that
         decoded nothing, and for the drain that finished while the event
         sat in the queue. */
      uint64_t consumed =
          atomic_load_explicit(sink_frames_consumed(), memory_order_acquire);
      if (consumed >= ev->end_frame) {
        player_drain_finish(p);
        break;
      }
      p->drain_until = ev->end_frame;
      p->drain_ticks = 0;
      break;
    }
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

  /* Amendment 06's drain completes here: the tick already runs at 30 Hz
     while playing and already loads g_frames_consumed for the watchdog,
     so this costs no new fd, no new timer and nothing in the callback.
     The bound is rule 7 -- advance anyway rather than hang if T3 stops
     consuming. */
  uint64_t consumed =
      atomic_load_explicit(sink_frames_consumed(), memory_order_acquire);

  /* DESIGN.md V.3 step 5, on the same counter load: a track transition
     becomes visible when the frames T2 wrote before it have actually
     been read out of pcm_ring, not when T2 decoded them.

     The two counters compared here live in the same domain only because
     IV.2 step 6 counts frames actually read from the ring and never the
     device's requested frameCount. A change there makes every boundary
     after the first underrun fire early, silently and cumulatively. */
  if (p->e != NULL) {
    player_drain_boundaries(p, consumed);
  }

  if (p->drain_until != 0) {
    if (consumed >= p->drain_until || ++p->drain_ticks >= PLAYER_DRAIN_MAX_TICKS) {
      player_drain_finish(p);
    }
  }

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
 * player_fill_model: describe the Player to the draw code. Takes the
 * Player and the model to fill, returns nothing.
 *
 * Separate from player_repaint so the scan-progress callback can repaint
 * from the same description without rebuilding the derived columns --
 * during a rescan those would re-query a database whose write
 * transaction is still open, once per file.
 */
static void
player_fill_model(Player * p, TuiModel * out)
{
  TuiModel m;
  memset(&m, 0, sizeof m);
  m.now_playing = (p->now_name[0] != '\0') ? p->now_name : NULL;
  m.message     = (p->message[0] != '\0') ? p->message : NULL;
  m.pos_ms      = (p->dev == DEV_READY && p->play != PLAY_STOPPED)
                      ? player_position_ms(p)
                      : -1;
  m.dur_ms      = p->dur_ms;
  m.playing     = (p->play == PLAY_PLAYING);
  m.paused      = (p->play == PLAY_PAUSED);
  m.volume_pct  = p->vol_pct;
  m.muted       = p->muted;
  m.browser     = p->br;
  m.view        = p->view;
  m.search_rows = p->search_rows;
  m.search_n    = p->search_n;
  m.queue_rows  = queueview_rows();
  m.queue_n     = queueview_count();
  m.queue_cursor = queueview_cursor();
  m.queue_playing = player_queue_playing_index(p);
  m.music_dir   = (p->cfg.nroots > 0) ? p->cfg.roots[0].path : NULL;

  /* DESIGN.md XI.7: a result set that ran out of memory is reported and
     rendered, never aborted. Drained here because the repaint is the
     one place that runs after every query. */
  if (p->br != NULL && browser_library_truncated(&p->brsrc[p->view])) {
    player_report(p, "out of memory, result truncated");
    m.message = (p->message[0] != '\0') ? p->message : NULL;
  }
  if (p->q != NULL) {
    m.shuffle = queue_shuffle(p->q);
    m.repeat  = (int) queue_repeat(p->q);
  }

  /* Borrowed, never owned, and never used for arithmetic
     (DESIGN.md II.2's warning) -- the status bar is the only legal
     consumer of the internal_* fields. */
  m.underruns         = sink_underrun_count();
  m.rate              = p->fmt.sample_rate;
  m.internal_format   = p->fmt.internal_format;
  m.internal_channels = p->fmt.internal_channels;

  *out = m;
} /* player_fill_model */

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

  /* The one place the derived columns are rebuilt: once per repaint,
     never in a cursor action. Holding a movement key therefore costs one
     pair of loads per frame rather than one per repeat
     (phase-16-browser.md III, "Column reload"). It is also deliberately
     not in player_fill_model: the scan-progress repaint shares the fill
     and must not re-query the database while a scan holds the write
     transaction open. */
  browser_refresh_derived(p->br);
  player_fill_model(p, &m);
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

  /* 7. Every prepared statement finalised, then sqlite3_close_v2. After
        step 5 T2 is joined, so nothing can still be querying the index
        -- which is why this sits here and not earlier. */
  db_close(p->db);
  p->db = NULL;

  /* 8. tb_shutdown. After it, and only after it, may stderr be written
        again -- which is exactly why step 10's trace drain and summary
        sit below it rather than at step 6. Idempotent and a no-op for
        every diagnostic mode, none of which claimed the terminal. */
  tui_shutdown();

  if (cb_alive) {
    fprintf(stderr, "kurono: WARNING: data callback still alive after "
                    "AudioSink.stop returned (DESIGN.md IV.9.b step 2)\n");
  }

  /* 9. UI state, then the config strings and the event loop -- which
        owns the epoll fd, the signalfd, both timerfds, and both ends of
        the self-pipe. The pipe's write end closes here and nowhere
        earlier, which is why event.c owns it and sink.c only cached it.
        The browser goes before config_free: its rows are its own, but
        the fs source borrows Config's ignore list, and freeing the list
        under a live source is the kind of ordering error this table
        exists to make unwriteable. */
  /* Before the browsers only for tidiness -- the results borrow nothing
     from them. A run can end with a search still open, which is the only
     reason this is here at all. The queue pane's rows are in the same
     position and freed for the same reason: they borrow nothing, and a
     run can end with the pane up. Both must go before db_close, though,
     which is step 7 below -- the rows were built from it. */
  player_search_free(p);
  queueview_free();

  for (int v = 0; v < BROWSER_VIEW_COUNT; v++) {
    browser_destroy(p->brv[v]);
    p->brv[v] = NULL;
  }
  p->br = NULL;
  /* After the browsers, never before: every library source holds this
     array and a destroy can still touch it. */
  free(p->rootv);
  p->rootv = NULL;
  p->br = NULL;

  /* The queue owns copies of its paths and borrows nothing, so its
     position here is free choice rather than a constraint -- it sits
     with the browser because both are UI state and step 9 is where UI
     state goes. It is reached after the engine is gone, so there is no
     window in which T2 could be handed an entry that was just freed. */
  queue_destroy(p->q);
  p->q = NULL;

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
          /* The browser keeps the cursor visible, so it needs the row
             count the layout just recomputed. Done after the drain
             rather than inside the resize branch because tb_peek_event
             can deliver a resize on either fd (see above), and a
             viewport that lags a resize scrolls the selection off
             screen. */
          if (p->br != NULL) {
            const TuiLayout * l = tui_layout();
            if (l != NULL) {
              browser_set_viewport(p->br, l->body_h);
            }
          }
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
 * scan_progress_stderr: the startup scan's progress sink. Legal only
 * because startup step 4 runs before tui_init claims the terminal
 * (DESIGN.md IV.9.a) -- ACT_RESCAN supplies a status-bar repaint
 * instead, which is the whole reason ScanProgressFn is a callback and
 * not a hardcoded printf. Carriage return with no newline so a scan of
 * five thousand files is one line, not five thousand; the final newline
 * comes from the caller once the scan returns. Takes the unused ctx,
 * the counts, and the path just handled; returns nothing.
 */
/*
 * scan_line_close: erase whatever scan_progress_stderr last left on the
 * line, so a shorter closing message cannot leave the tail of a longer
 * progress line behind it. Takes and returns nothing.
 *
 * "kurono: scanning 38/38" is one character longer than "kurono: scan
 * complete", so a 38-file scan ended by printing "scan complete8".
 * Erase-to-end-of-line rather than padding to a fixed width, because the
 * progress line's length depends on the file count and any fixed width
 * is wrong for a large enough library. The escape is safe here for the
 * same reason the carriage return already is: this output only exists
 * before tb_init, on a terminal (DESIGN.md IV.9.a).
 */
static void
scan_line_close(void)
{
  fputs("\r\033[K", stderr);
} /* scan_line_close */

/*
 * ScanTally: the last counts scan_progress_stderr saw, so the closing
 * line can say how much was indexed. It is the ScanOpts ctx, which is
 * what that field is for -- scan_run returns only the skipped count, and
 * DESIGN.md V.5 fixes both that return and ScanProgressFn's signature,
 * so the totals exist nowhere else once the scan has ended.
 */
typedef struct {
  int done;
  int total;
} ScanTally;

static void
scan_progress_stderr(void * ctx, int done, int total, const char * path)
{
  ScanTally * t = (ScanTally *) ctx;
  (void) path;

  if (t != NULL) {
    t->done  = done;
    t->total = total;
  }
  fprintf(stderr, "\rkurono: scanning %d/%d", done, total);
  fflush(stderr);
} /* scan_progress_stderr */

/*
 * scan_report: the closing line -- what was indexed, and what was not.
 * Takes the tally and scan_run's skipped count, returns nothing.
 *
 * The counts print unconditionally. They used to appear only when
 * something was skipped, so a clean scan said "scan complete" and
 * nothing else -- and an empty root, a mistyped KURONO_MUSIC_DIR and a
 * good scan of a full library all produced the identical line.
 */
static void
scan_report(const ScanTally * t, int skipped)
{
  int indexed = t->done - skipped;

  scan_line_close();
  fprintf(stderr, "kurono: scan complete, %d track(s) indexed",
          (indexed > 0) ? indexed : 0);
  if (skipped > 0) {
    fprintf(stderr, ", %d file(s) skipped", skipped);
  }
  fputc('\n', stderr);
} /* scan_report */

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
  /* Full volume, unmuted, on every run. Set here rather than in either
     caller because a memset-zeroed Player would otherwise start silent,
     and the engine's own default gain is 1.0 -- the two must agree from
     the first frame, and nothing pushes a CMD_SET_GAIN until the user
     touches the volume. */
  p->gain       = 1.0f;
  p->vol_pct    = 100;
  p->mute_saved = 1.0f;
  p->muted      = 0;

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

  /* Step 3: db_open. Never returns NULL -- every failure inside is
     either self-healing (corruption unlinks and rebuilds) or fatal via
     kurono_die (a database written by a newer kurono). A cleared index
     is reported through rescanned, which step 4 treats exactly like a
     missing database. */
  const char ** root_paths = malloc(p->cfg.nroots * sizeof(*root_paths));
  if (root_paths == NULL) {
    kurono_die("out of memory resolving music roots");
  }
  for (size_t i = 0; i < p->cfg.nroots; i++) {
    root_paths[i] = p->cfg.roots[i].path;
  }

  int rescanned = 0;
  p->db = db_open(p->cfg.db_path, root_paths, p->cfg.nroots, &rescanned);

  /* Step 4: scan_run. This is the last point at which stderr may be
     written on a TUI run; tui_init is step 9, and everything after it
     reports through player_report instead. KURONO_NO_SCAN skips the
     walk and uses the index as-is (DESIGN.md X) -- including when
     rescanned says the index was just cleared, since the flag means
     "do not walk the tree", not "walk it unless it would be slow". */
  if (!p->cfg.no_scan) {
    ScanTally tally = { 0, 0 };
    ScanOpts  so    = {
      .roots        = root_paths,
      .nroots       = p->cfg.nroots,
      .ignore_names = (const char * const *) p->cfg.ignore,
      .nignore      = p->cfg.nignore,
      .on_progress  = scan_progress_stderr,
      .ctx          = &tally,
    };
    int skipped = scan_run(p->db, &so);
    if (skipped < 0) {
      /* A transaction-level failure leaves the index exactly as it was,
         so this is a degraded start, not a fatal one (DESIGN.md XI.7). */
      scan_line_close();
      fprintf(stderr, "kurono: %s: scan failed, using the existing index\n", who);
    } else {
      /* Closes the carriage-returned progress line scan_progress_stderr
         left open. */
      scan_report(&tally, skipped);
    }
  }
  free(root_paths);

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
      db_close(p->db);
      p->db = NULL;
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
     command condvar with no track loaded.

     The source is constructed here and handed over, rather than built
     inside the engine: that is what makes DESIGN.md VI's doubles
     substitutable (phase 18). This is the production path, so it is the
     real decoder; a harness passes a testsrc_create instead. The engine
     owns it from this call on, including on failure. */
  p->e = engine_create(&p->fmt, decoder_create(&p->fmt), &p->sink);
  if (p->e == NULL) {
    fprintf(stderr, "kurono: %s: engine_create failed\n", who);
    p->sink.close(p->sink.self);
    sink_free_rings();
    db_close(p->db);
    p->db = NULL;
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
    db_close(p->db);
    p->db = NULL;
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

  /* VIII.3's warning: the tick is armed BEFORE the first CMD_PLAY, not
     after, or the first boundary is observed late. */
  player_set_redraw(&p, 1);

  /* IV.9.a.i (amendment 05): there is no startup step 8 any more. The
     device is started here, by the play transition, through the one
     function that owns the edge ordering -- the same path the TUI
     takes, so the headless harness exercises the real policy rather
     than a second transcription of it. T3 begins and immediately
     underruns into silence, which is correct: the first invocation
     happens on this thread, inside the start, from PulseAudio's
     pre-fill. */
  if (player_set_play_state(&p, PLAY_PLAYING) != 0) {
    /* The ladder has it now. Headless, that means the run ends when the
       ladder gives up; there is nothing to browse. */
    player_loop(&p);
    shutdown_all(&p);
    return p.status;
  }
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

  /* No ma_device_start here, and as of amendment 05 that is the general
     policy rather than this run's departure from it -- IV.9.a step 8 is
     deleted and IV.9.a.i replaces it. The device is started by the play
     transition in player_set_play_state and stopped on every transition
     out of PLAY_PLAYING.
     The measurement that produced the amendment, kept because it is the
     reason (2026-08-12, per-thread sampling of each task's stat file
     under /proc over 12 s of idle): with the device started and nothing
     to play, T1 sat in epoll_wait at 0.00% and T2 in futex at 0.00% --
     VIII.3's "zero wakeups, zero CPU" holding exactly -- while
     miniaudio's T3 burned 1.33% in ppoll, waking ~53 times a second at
     periodFrames = 900 to memset silence and count an underrun. Against
     that, --dev-latency measured a start at under 0.7 ms worst case, so
     the trade is 1.33% forever against ~0.4 ms per transition. */
  if (!have_device) {
    player_browse_only(&p, "no audio device at startup");
  }

  /* The browser, built before tb_init so a failure here still reaches
     stderr. M1 lists the first music root with the filesystem source
     (DESIGN.md XIV.1, plan.md VII gap 8); phase 16 hands the same
     widget a library/query.c source instead.
     A root that does not exist is not fatal (DESIGN.md XI.5): the
     browser simply fails to build, every navigation action becomes a
     no-op, and the status bar names the path -- a first-run state, not
     an error state. */
  {
    p.rootv = malloc(p.cfg.nroots * sizeof *p.rootv);
    if (p.rootv == NULL) {
      kurono_die("out of memory resolving music roots");
    }
    for (size_t i = 0; i < p.cfg.nroots; i++) {
      p.rootv[i] = p.cfg.roots[i].path;
    }
    const char ** rootv = p.rootv;

    for (int v = 0; v < BROWSER_VIEW_COUNT; v++) {
      browser_library_source(&p.brsrc[v], v, p.db, rootv, p.cfg.nroots);

      char * rootkey = browser_library_root(v, rootv, p.cfg.nroots);
      if (rootkey != NULL) {
        p.brv[v] = browser_create(&p.brsrc[v], rootkey);
        free(rootkey);
      }
      if (p.brv[v] == NULL) {
        fprintf(stderr, "kurono: view %d unavailable -- browsing disabled\n", v);
      }
    }
    p.view = BROWSER_VIEW_DIRECTORY;
    p.br   = p.brv[p.view];
  }

  /* The queue. Seeded from the clock rather than internally, because
     queue.c takes the seed as a parameter specifically so that a fuzz
     failure is replayable (phase 9). Like the browser, a failure here is
     not fatal: queueing actions report themselves and everything else
     still works. */
  p.dur_ms = -1;
  p.q      = queue_create((unsigned) time(NULL));
  if (p.q == NULL) {
    fprintf(stderr, "kurono: could not create the queue -- playback disabled\n");
  }

  /* Step 9: tb_init, then the remaining epoll registrations. Its
     position is unaffected by amendment 05 -- what used to precede it
     was step 8, which no longer exists -- and it comes before anything
     can write to stderr again -- from here to tui_shutdown, every diagnostic
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

  /* The browser needs a viewport before the first paint, not only on
     the first resize: without this the widget keeps its minimum of one
     row and a listing taller than the terminal would draw one line and
     scroll on every cursor move. */
  if (p.br != NULL) {
    const TuiLayout * l0 = tui_layout();
    if (l0 != NULL) {
      browser_set_viewport(p.br, l0->body_h);
    }
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
 * run_scan: implements --scan. Opens the database, walks every root
 * with the stderr progress printer, closes, and exits -- no device, no
 * threads, no terminal. This is phase 14's primary verification surface
 * and it drives exactly the code path startup steps 3 and 4 use, which
 * is the point: a scan bug reproduces here without a TUI in the way.
 * KURONO_NO_SCAN is deliberately ignored, since asking for --scan is
 * asking for the walk. Takes nothing, returns the process exit status:
 * 0 if the scan committed, 1 if it did not. Files skipped are reported
 * but do not fail the run (DESIGN.md XI.1).
 */
static int
run_scan(void)
{
  Config cfg;
  if (config_init(&cfg) != 0) {
    kurono_die("failed to initialize configuration");
  }

  const char ** root_paths = malloc(cfg.nroots * sizeof(*root_paths));
  if (root_paths == NULL) {
    kurono_die("out of memory resolving music roots");
  }
  for (size_t i = 0; i < cfg.nroots; i++) {
    root_paths[i] = cfg.roots[i].path;
  }

  int  rescanned = 0;
  Db * db = db_open(cfg.db_path, root_paths, cfg.nroots, &rescanned);

  ScanTally tally = { 0, 0 };
  ScanOpts  so    = {
    .roots        = root_paths,
    .nroots       = cfg.nroots,
    .ignore_names = (const char * const *) cfg.ignore,
    .nignore      = cfg.nignore,
    .on_progress  = scan_progress_stderr,
    .ctx          = &tally,
  };
  int skipped = scan_run(db, &so);

  if (skipped < 0) {
    scan_line_close();
    fprintf(stderr, "kurono: --scan: scan failed, index unchanged\n");
  } else {
    scan_report(&tally, skipped);
  }

  db_close(db);
  free(root_paths);
  config_free(&cfg);
  return (skipped < 0) ? 1 : 0;
} /* run_scan */



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

  } else if (argc == 2 && strcmp(argv[1], "--scan") == 0) {
    status = run_scan();

  } else if (argc >= 3 && strcmp(argv[1], "--play") == 0) {
    /* The one diagnostic mode that survived phase 18's scaffolding cull
       (plan.md VII gap 1). It exercises audio/engine.c's T2 headless,
       wired to a real sink and a real decoder, and its --seek-stress and
       --seek-paused drivers are how the flush handshake gets hammered
       under a sanitiser without a person at the keyboard. The others --
       --tone, --decode, --dev-latency, --search, --dirs -- had each
       served exactly one phase and were deleted rather than left to rot
       as untested entry points into the middle of the program. */
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
    fprintf(stderr, "kurono: usage: kurono [--version | --paths | --scan | "
                    "--play FILE [--seek MS | --seek-stress N | "
                    "--seek-paused MS]]\n");
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
