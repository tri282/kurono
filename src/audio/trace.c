/*
 * audio/trace.c -- the SPSC trace ring: fixed-size records recorded out
 * of T3 (the audio thread) via trace_emit, rendered on T1 via
 * trace_drain. See include/audio/trace.h and DESIGN.md IV.10.
 *
 * trace_ring is a third instance of audio/ring.c's SPSC algorithm --
 * same struct, same memory-order discipline, same TSan evidence as
 * pcm_ring and viz_ring. Its producer is exclusively T3 and its
 * consumer is exclusively T1 (amendment-02-trace-ring.md decision 2);
 * a second producer would break it silently, the way a second producer
 * or consumer breaks any Ring (ring.h's binding SPSC assumption).
 *
 * Nothing in this file may include miniaudio.h or reach into
 * audio/sink.c's statics -- the two facts trace_emit and
 * trace_summary_line need from the sink (the frame counter, the
 * negotiated sample rate) cross that boundary through the registration
 * functions in trace.h instead. See their comments for why.
 */

#include "../../include/audio/trace.h"
#include "../../include/audio/ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* 4096 records (phase-5b-trace.md I.4, DESIGN.md IV.10). ring_init rounds
   the byte count up to a power of two, the same way pcm_ring's
   PCM_LEAD_SECONDS is a floor rather than an exact size (phase-5.md
   "Deviations" #2) -- the ring ends up somewhat larger than 4096*24 B,
   never smaller, so this is a floor on capacity, not a shrinkage. */
#define TRACE_RING_RECORDS 4096u
#define TRACE_RING_BYTES   ((size_t) TRACE_RING_RECORDS * sizeof(TraceRec))

/* Names, read only by trace_drain on T1 -- never crosses back to T3.
   Designated initialisers keep this in step with TraceEvent even as the
   enum grows (DESIGN.md IV.10: "the enum is open"). */
static const char * const TRACE_NAMES[TRACE_COUNT] = {
  [TRACE_UNDERRUN] = "UNDERRUN",
  [TRACE_RING_LOW] = "RING_LOW",
  [TRACE_FLUSH]    = "FLUSH", /* reserved, never emitted -- amendment 03 */
  [TRACE_NOTIFY]   = "NOTIFY",
};

static Ring        g_trace_ring;
static atomic_uint  g_trace_dropped = 0;

/* Registered by sink.c; see trace_set_frame_counter's comment in
   trace.h. Read only with a relaxed load, from T3, inside trace_emit. */
static const atomic_uint_least64_t * g_frame_counter = NULL;

/* Registered by sink.c; see trace_set_sample_rate's comment in trace.h.
   Read only from T1, inside trace_summary_line. 0 == unknown. */
static uint32_t g_sample_rate = 0;

/* T1-only aggregates. Written exclusively inside trace_drain, read
   inside trace_summary_line -- both T1, so no atomics are needed here,
   unlike the counters T3 and T1 share. */
static uint32_t g_agg_underrun_count   = 0;
static uint32_t g_agg_low_water_frames = 0;
static int      g_agg_low_water_set    = 0;

static FILE * g_log_file = NULL;

/*
 * open_log: attempt to open log_path for the per-event text log,
 * first trying to create its immediate parent directory (only the
 * "kurono" leaf under $XDG_STATE_HOME/$HOME/.local/state -- the base
 * state directory itself is assumed to already exist, the same
 * assumption config.c makes about $XDG_CACHE_HOME's parent). Never
 * fatal: on any failure -- missing base directory, permission denied,
 * anything -- warns once to stderr and returns with g_log_file left
 * NULL, so the caller runs with recording-only tracing instead of no
 * player at all. Takes the path (NULL is a silent no-op, the "no file
 * requested" case), returns nothing.
 */
static void
open_log(const char * log_path)
{
  if (log_path == NULL) {
    return;
  }

  char * dir = strdup(log_path);
  if (dir != NULL) {
    char * slash = strrchr(dir, '/');
    if (slash != NULL) {
      *slash = '\0';
      mkdir(dir, 0700); /* best-effort; fopen below reports any real failure */
    }
    free(dir);
  }

  g_log_file = fopen(log_path, "w");
  if (g_log_file == NULL) {
    fprintf(stderr,
            "kurono: warning: could not open trace log %s -- "
            "continuing without a trace file\n",
            log_path);
  }
} /* open_log */

/*
 * trace_init: see include/audio/trace.h.
 */
int
trace_init(const char * log_path)
{
  if (ring_init(&g_trace_ring, TRACE_RING_BYTES) != 0) {
    return -1;
  }

  atomic_store_explicit(&g_trace_dropped, 0u, memory_order_relaxed);
  g_agg_underrun_count   = 0;
  g_agg_low_water_frames = 0;
  g_agg_low_water_set    = 0;

  open_log(log_path);
  return 0;
} /* trace_init */

/*
 * trace_shutdown: see include/audio/trace.h.
 */
void
trace_shutdown(void)
{
  if (g_log_file != NULL) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
  ring_free(&g_trace_ring);
} /* trace_shutdown */

/*
 * trace_set_frame_counter: see include/audio/trace.h.
 */
void
trace_set_frame_counter(const atomic_uint_least64_t * counter)
{
  g_frame_counter = counter;
} /* trace_set_frame_counter */

/*
 * trace_set_sample_rate: see include/audio/trace.h.
 */
void
trace_set_sample_rate(uint32_t rate)
{
  g_sample_rate = rate;
} /* trace_set_sample_rate */

/*
 * trace_emit: see include/audio/trace.h. T3 ONLY -- the data callback
 * and the notification callback. Bounds check, 24-byte memcpy (inside
 * ring_write), release store (also inside ring_write) -- no format, no
 * allocation, no lock, no other syscall. The bounds check is explicit
 * here rather than left to ring_write's own internal clipping: a
 * partial ring_write of a TraceRec would desynchronise every record
 * boundary after it, since trace_drain always reads exactly
 * sizeof(TraceRec) at a time. Checking free space first keeps every
 * write all-or-nothing.
 */
void
trace_emit(uint32_t event, uint32_t a, uint32_t b, uint32_t c)
{
  if (ring_free_space(&g_trace_ring) < sizeof(TraceRec)) {
    atomic_fetch_add_explicit(&g_trace_dropped, 1u, memory_order_relaxed);
    return;
  }

  TraceRec rec;
  rec.frame = (g_frame_counter != NULL)
                ? atomic_load_explicit(g_frame_counter, memory_order_relaxed)
                : 0;
  rec.event = event;
  rec.a     = a;
  rec.b     = b;
  rec.c     = c;

  ring_write(&g_trace_ring, &rec, sizeof(rec));
} /* trace_emit */

/*
 * trace_drain: see include/audio/trace.h. T1 ONLY. Reads exactly
 * sizeof(TraceRec) at a time -- trace_emit only ever writes whole
 * records (see its bounds check above), so the ring's byte stream stays
 * aligned to 24-byte records and ring_read returning less than a full
 * record means the ring is empty, not that a record is torn.
 */
void
trace_drain(void)
{
  TraceRec rec;
  while (ring_read(&g_trace_ring, &rec, sizeof(rec)) == sizeof(rec)) {
    if (rec.event == TRACE_UNDERRUN) {
      g_agg_underrun_count++;
    } else if (rec.event == TRACE_RING_LOW) {
      if (!g_agg_low_water_set || rec.a < g_agg_low_water_frames) {
        g_agg_low_water_frames = rec.a;
        g_agg_low_water_set    = 1;
      }
    }

    if (g_log_file != NULL) {
      const char * name = (rec.event < TRACE_COUNT) ? TRACE_NAMES[rec.event] : "?";
      fprintf(g_log_file, "%llu %s %u %u %u\n",
              (unsigned long long) rec.frame, name, rec.a, rec.b, rec.c);
    }
  }

  if (g_log_file != NULL) {
    fflush(g_log_file);
  }
} /* trace_drain */

/*
 * trace_summary_line: see include/audio/trace.h.
 */
void
trace_summary_line(char * buf, size_t n)
{
  unsigned dropped = atomic_load_explicit(&g_trace_dropped, memory_order_relaxed);

  if (g_agg_low_water_set && g_sample_rate > 0) {
    double seconds = (double) g_agg_low_water_frames / (double) g_sample_rate;
    snprintf(buf, n,
             "kurono: %u underruns, %u trace records dropped, "
             "pcm_ring low-water %.2f s",
             g_agg_underrun_count, dropped, seconds);
  } else {
    snprintf(buf, n,
             "kurono: %u underruns, %u trace records dropped, "
             "pcm_ring low-water n/a",
             g_agg_underrun_count, dropped);
  }
} /* trace_summary_line */
