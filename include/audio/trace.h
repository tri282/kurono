#ifndef KURONO_AUDIO_TRACE_H
#define KURONO_AUDIO_TRACE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The SPSC trace ring (DESIGN.md IV.10, amendment-02-trace-ring.md). The
 * data callback and the notification callback -- both T3 -- may not
 * malloc, lock, syscall, printf, or assert; trace_emit is the one
 * permitted way for them to report a fact. It is a bounds check, a
 * 24-byte memcpy, and a release store into trace_ring -- never a format,
 * never an allocation, never a block. T1 renders on the way out.
 */

typedef enum {
  TRACE_UNDERRUN = 0,
  TRACE_RING_LOW,
  TRACE_FLUSH,
  TRACE_NOTIFY,
  TRACE_COUNT
} TraceEvent;

typedef struct {
  uint64_t frame;   /* g_frames_consumed at the moment of the event */
  uint32_t event;   /* TraceEvent -- index into a static string table */
  uint32_t a, b, c; /* event-specific numerics, meaning per event */
} TraceRec;

_Static_assert(sizeof(TraceRec) == 24, "TraceRec must stay 24 bytes");

/*
 * trace_init: allocate trace_ring (always, regardless of log_path) and,
 * if log_path is non-NULL, open it for the per-event text log gated by
 * KURONO_TRACE (DESIGN.md IV.10, X). Recording into the ring is
 * unconditional; only writing the file is opt-in. Failing to open
 * log_path is never fatal: warns once to stderr and continues without a
 * file -- CLAUDE.md is explicit that a diagnostic must never be able to
 * prevent the player from starting. Takes the resolved log path or
 * NULL, returns 0 on success (ring allocated) or -1 on ring allocation
 * failure.
 */
int  trace_init(const char * log_path);

/*
 * trace_shutdown: close the log file if one is open and free trace_ring.
 * T1 only. Safe to call after a failed or repeated trace_init, and safe
 * to call more than once. Takes and returns nothing.
 */
void trace_shutdown(void);

/*
 * trace_emit: record one fixed-size TraceRec into trace_ring. T3 ONLY --
 * the data callback and the notification callback, which DESIGN.md IV.3
 * establishes run on the same thread. T1 and T2 must never call this:
 * they are permitted syscalls and have their own channels (T2 already
 * reaches T1 through the mutex-protected event queue), and a second
 * producer breaks trace_ring's SPSC contract silently, with no error and
 * no crash (DESIGN.md IV.4, IV.10). Bounded, wait-free, and lossy: on a
 * full ring it increments an atomic dropped counter and returns rather
 * than blocking, retrying, or growing -- the same rule as the doorbell
 * pipe (DESIGN.md IV.3). `frame` is filled from the counter registered
 * with trace_set_frame_counter via a relaxed load, or recorded as 0 if
 * none is registered. Takes the event id and up to three event-specific
 * numerics; returns nothing.
 */
void trace_emit(uint32_t event, uint32_t a, uint32_t b, uint32_t c);

/*
 * trace_drain: read every record currently available out of trace_ring
 * and, for each one, update the in-memory aggregates (underrun count,
 * pcm_ring low-water mark, dropped count) and, if a log file is open,
 * format and write one line -- the only place formatting happens
 * (phase-5b-trace.md I.5). T1 ONLY. Called from the redraw tick and from
 * the pipe-wakeup branch (DESIGN.md IV.10) -- no new fd, no new epoll
 * registration, no new timer. Takes and returns nothing.
 */
void trace_drain(void);

/*
 * trace_summary_line: format the one-line exit summary -- underrun
 * count, dropped-record count, and the pcm_ring occupancy low-water
 * mark in seconds -- into buf, a caller-owned buffer of n bytes. Written
 * into a caller buffer rather than printed so a later phase can put the
 * same three numbers in the status bar without a second code path
 * (phase-5b-trace.md II). T1 ONLY. Takes the buffer and its size,
 * returns nothing.
 */
void trace_summary_line(char * buf, size_t n);

/*
 * trace_set_frame_counter: register the T3-owned playback-position
 * counter that trace_emit reads (relaxed) to fill TraceRec.frame.
 *
 * Why this exists: trace_emit's four-argument signature is fixed by
 * phase-5b-trace.md I.1 and DESIGN.md IV.10, and g_frames_consumed is
 * file-scope static state owned by audio/sink.c -- it must stay there
 * (sink.c is T3-owned state's owner) and audio/trace.c must not reach
 * across the module boundary to touch it directly or include
 * miniaudio.h to get at it. A registered pointer satisfies both: the
 * counter never moves out of sink.c, trace_emit keeps its specced
 * shape, and trace.c learns nothing about miniaudio. Call once, from
 * sink.c (sink_open_impl), after the counter exists and before the
 * device is started. Passing NULL is legal and makes trace_emit record
 * frame as 0 instead of crashing, rather than requiring every caller to
 * prove the counter is already registered.
 */
void trace_set_frame_counter(const atomic_uint_least64_t * counter);

/*
 * trace_set_sample_rate: register the session format's negotiated
 * sample rate in Hz, so trace_summary_line can convert the pcm_ring
 * low-water mark from frames to seconds for the exit line
 * (phase-5b-trace.md I.6: "pcm_ring low-water 1.87 s"). trace.c has no
 * other way to learn the rate without including miniaudio.h or reaching
 * into sink.c's statics, for the identical reason given in
 * trace_set_frame_counter. Call once, from sink.c (sink_open_impl),
 * alongside trace_set_frame_counter, once R is known. 0 -- the default
 * before registration -- means "unknown"; trace_summary_line reports
 * the low-water mark as unavailable rather than dividing by zero.
 */
void trace_set_sample_rate(uint32_t rate);

#endif // KURONO_AUDIO_TRACE_H
