/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * audio/testsrc.c -- the AudioSource doubles of DESIGN.md VI. See
 * ../../include/audio/testsrc.h for the contract and for why they are
 * one file with a kind rather than three implementations.
 *
 * Deliberately dependency-free: no ffmpeg, no miniaudio, no SQLite, no
 * termbox. That is the whole value. A double that pulled in the decode
 * stack could not be used to prove anything about the ring or the
 * callback in isolation, which is the one job §VI gives it.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/audio/testsrc.h"

/* 440 Hz, and -6 dBFS rather than full scale: a double that clips is a
   double that hides a gain bug instead of showing it. */
/* Spelled out rather than M_PI: M_PI is XSI, not C11, and this build is
   -std=c11 -D_POSIX_C_SOURCE=200809L, where math.h does not have to
   define it. sink.c's tone generator hit the same wall and reached for
   miniaudio's MA_PI; this file is deliberately dependency-free and has
   nothing to reach for. */
#define TESTSRC_PI         3.14159265358979323846

#define TESTSRC_TONE_HZ    440.0
#define TESTSRC_TONE_GAIN  0.5f

/* The value TESTSRC_FAULT returns from read(). Shaped like an AVERROR
   (negative, large magnitude) without including any ffmpeg header --
   AudioSource.read's contract says a negative return is opaque to the
   caller, which is exactly what lets this file stay dependency-free.
   AVERROR(EIO) is -5 on Linux; this is deliberately not that value, so
   a trace showing it is unambiguously the injector and never a real
   decode failure. */
#define TESTSRC_FAULT_ERR  (-1094995529 - 1)

typedef struct {
  SessionFormat fmt;
  TestSrcKind   kind;
  int64_t       duration_ms;
  long          fault_after;

  int      open;         /* 1 between open() and close() */
  uint64_t frames_done;  /* frames produced since open(), for EOF and the fault */
  double   phase;        /* sine phase, carried across reads so seams are clean */
} TestSrc;

/* frames_total: how many frames this source produces before EOF, or 0
   for "never ends". Takes the source, returns the frame count. */
static uint64_t
frames_total(const TestSrc * t)
{
  if (t->duration_ms <= 0 || t->fmt.sample_rate == 0) {
    return 0;
  }
  return (uint64_t) t->duration_ms * (uint64_t) t->fmt.sample_rate / 1000u;
} /* frames_total */

/* testsrc_open_impl: AudioSource.open. Accepts any path, including
   NULL, and ignores it -- a double that needed a file on disk would not
   be a double. Resets the frame count and the phase, so reopening is a
   clean restart rather than a continuation. Takes the self pointer, the
   ignored path, and the StreamInfo to fill; returns 0 always, since
   there is nothing here that can fail. */
static int
testsrc_open_impl(void * self, const char * path, StreamInfo * out_info)
{
  (void) path;
  TestSrc * t = (TestSrc *) self;
  if (t == NULL) {
    return -1;
  }

  t->open        = 1;
  t->frames_done = 0;
  t->phase       = 0.0;

  if (out_info != NULL) {
    memset(out_info, 0, sizeof(*out_info));
    out_info->duration_ms = t->duration_ms;
    out_info->sample_rate = (int) t->fmt.sample_rate;
    out_info->channels    = (int) t->fmt.channels;
    out_info->bitrate     = 0;
    snprintf(out_info->codec, sizeof(out_info->codec), "%s",
             (t->kind == TESTSRC_SINE) ? "testsrc-sine"
             : (t->kind == TESTSRC_FAULT) ? "testsrc-fault"
                                          : "testsrc-silence");
  }
  return 0;
} /* testsrc_open_impl */

/* testsrc_read_impl: AudioSource.read, honouring the three-valued
   contract of DESIGN.md VI exactly as the real decoder does -- n ==
   frames is normal, 0 <= n < frames is EOF, n < 0 is a mid-stream
   failure. Getting this arithmetic right is the reason the three kinds
   share one implementation.

   Takes the self pointer, the interleaved destination and the frame
   count wanted; returns frames produced, or a negative error. */
static long
testsrc_read_impl(void * self, float * dst, long frames)
{
  TestSrc * t = (TestSrc *) self;
  if (t == NULL || dst == NULL || frames <= 0) {
    return -1;
  }
  if (!t->open) {
    return 0; /* nothing open reads as immediate EOF, never as an error */
  }

  /* The fault fires the moment the count is reached and stays firing:
     a source that recovered would test the wrong thing -- the branch
     under test is the one that must survive a source that never comes
     back. */
  if (t->kind == TESTSRC_FAULT && t->fault_after >= 0
      && t->frames_done >= (uint64_t) t->fault_after) {
    return TESTSRC_FAULT_ERR;
  }

  long n = frames;

  /* Clip to EOF, and let a fault that lands mid-block still be a fault
     on the next call rather than being swallowed by the EOF return. */
  uint64_t total = frames_total(t);
  if (total > 0) {
    if (t->frames_done >= total) {
      return 0;
    }
    uint64_t left = total - t->frames_done;
    if ((uint64_t) n > left) {
      n = (long) left;
    }
  }
  if (t->kind == TESTSRC_FAULT && t->fault_after >= 0) {
    uint64_t until = (uint64_t) t->fault_after - t->frames_done;
    if ((uint64_t) n > until) {
      n = (long) until;
    }
  }

  const unsigned ch = (t->fmt.channels != 0) ? t->fmt.channels : 2u;

  if (t->kind == TESTSRC_SINE && t->fmt.sample_rate != 0) {
    const double step = 2.0 * TESTSRC_PI * TESTSRC_TONE_HZ / (double) t->fmt.sample_rate;
    for (long i = 0; i < n; i++) {
      float s = TESTSRC_TONE_GAIN * (float) sin(t->phase);
      t->phase += step;
      if (t->phase >= 2.0 * TESTSRC_PI) {
        t->phase -= 2.0 * TESTSRC_PI; /* bounded, so a long run cannot lose precision */
      }
      for (unsigned c = 0; c < ch; c++) {
        dst[(size_t) i * ch + c] = s;
      }
    }
  } else {
    memset(dst, 0, (size_t) n * ch * sizeof(float));
  }

  t->frames_done += (uint64_t) n;
  return n;
} /* testsrc_read_impl */

/* testsrc_seek_impl: AudioSource.seek. Moves the frame count, which is
   the only position this source has, and leaves the phase alone -- a
   sine that restarted its phase on every seek would put a
   discontinuity into exactly the recording a seek test is inspecting.
   Takes the self pointer and the target in ms, returns 0 on success and
   -1 if nothing is open. */
static int
testsrc_seek_impl(void * self, int64_t ms)
{
  TestSrc * t = (TestSrc *) self;
  if (t == NULL || !t->open) {
    return -1;
  }
  if (ms < 0) {
    ms = 0;
  }
  t->frames_done = (uint64_t) ms * (uint64_t) t->fmt.sample_rate / 1000u;
  return 0;
} /* testsrc_seek_impl */

/* testsrc_close_impl: AudioSource.close. Takes the self pointer,
   returns nothing. */
static void
testsrc_close_impl(void * self)
{
  TestSrc * t = (TestSrc *) self;
  if (t != NULL) {
    t->open = 0;
  }
} /* testsrc_close_impl */

/* testsrc_retarget_impl: AudioSource.retarget. There is no resampler
   here -- the samples are generated at whatever rate is asked for -- so
   this is a field assignment. Provided rather than left NULL so that
   the device-reopen ladder can be driven end to end against a double.
   Takes the self pointer and the new rate, returns 0. */
static int
testsrc_retarget_impl(void * self, uint32_t sample_rate)
{
  TestSrc * t = (TestSrc *) self;
  if (t == NULL || sample_rate == 0) {
    return -1;
  }
  t->fmt.sample_rate = sample_rate;
  return 0;
} /* testsrc_retarget_impl */

/* testsrc_destroy_impl: AudioSource.destroy. Takes the self pointer,
   returns nothing. */
static void
testsrc_destroy_impl(void * self)
{
  free(self);
} /* testsrc_destroy_impl */

/* testsrc_create: see include/audio/testsrc.h. */
AudioSource
testsrc_create(const SessionFormat * fmt, TestSrcKind kind,
               int64_t duration_ms, long fault_after_frames)
{
  AudioSource src;
  memset(&src, 0, sizeof(src));

  if (fmt == NULL) {
    return src; /* all-NULL vtable and self, exactly as decoder_create */
  }

  TestSrc * t = calloc(1, sizeof(TestSrc));
  if (t == NULL) {
    return src;
  }

  t->fmt         = *fmt;
  t->kind        = kind;
  t->duration_ms = duration_ms;
  t->fault_after = (fault_after_frames < 0) ? 0 : fault_after_frames;

  src.open     = testsrc_open_impl;
  src.read     = testsrc_read_impl;
  src.seek     = testsrc_seek_impl;
  src.close    = testsrc_close_impl;
  src.retarget = testsrc_retarget_impl;
  src.destroy  = testsrc_destroy_impl;
  src.self     = t;
  return src;
} /* testsrc_create */
