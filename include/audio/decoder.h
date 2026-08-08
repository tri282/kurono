#ifndef KURONO_AUDIO_DECODER_H
#define KURONO_AUDIO_DECODER_H

#include <stdint.h>

#include "format.h"

/*
 * AudioSource: anything that produces PCM in the session format
 * (DESIGN.md VI). audio/decoder.c is the only implementation and the
 * only file permitted to use ffmpeg (CLAUDE.md; DESIGN.md III.2) -- no
 * ffmpeg type or symbol appears in this header. read's contract: a
 * non-negative return less than the requested frame count is the entire
 * EOF signal, per DESIGN.md VI -- there is no separate "at EOF" flag. A
 * negative return is a mid-stream decode failure (a negative AVERROR,
 * opaque to the caller), which is a different event: EOF advances the
 * queue, a failure raises EV_DECODE_ERROR (phase-6-decoder.md I.6,
 * DESIGN.md XI.1). Both are per-file and neither is ever fatal.
 */
typedef struct {
  int  (* open)(void * self, const char * path, StreamInfo * out_info);
  long (* read)(void * self, float * dst, long frames);   /* 0 <= n < frames == EOF; < 0 == error */
  int  (* seek)(void * self, int64_t ms);
  void (* close)(void * self);
  void * self;
} AudioSource;

/*
 * decoder_create: build one AudioSource bound to fmt -- the session
 * format every track this source ever opens gets resampled to
 * (DESIGN.md II.2). fmt is copied by value; the caller's SessionFormat
 * does not need to outlive this call. The resampler itself is (re)built
 * per file inside open(), against this stored target and that file's
 * own codec parameters, and is never rebuilt against a *different*
 * target -- except on phase 8's device-reopen path, which is legal
 * there and only there because T3 provably does not exist at that
 * moment (DESIGN.md II.2 warning). Takes the session format, returns an
 * AudioSource ready for open(); on allocation failure every vtable
 * pointer and self are NULL -- the caller must check src.self != NULL
 * before use, per §XI.1's "per-file failure is never fatal" extended to
 * "construction failure is reported, not fatal."
 */
AudioSource decoder_create(const SessionFormat * fmt);

/*
 * decoder_destroy: release everything decoder_create allocated,
 * including any file left open by a prior open() that was never
 * close()d. Idempotent-safe to call once; src->self is NULL afterward.
 * Takes the AudioSource returned by decoder_create, returns nothing.
 */
void decoder_destroy(AudioSource * src);

/*
 * decoder_diag_step: --decode-only diagnostic (plan.md VI.8,
 * phase-6-decoder.md I.7), not part of the AudioSource contract and
 * never called by phase 7's engine. AudioSource.read clips its return
 * to the caller's requested frame count, which can hide a short first
 * decode step inside a larger request -- exactly the thing
 * phase-6-decoder.md III.1 needs to observe (whether the first decoded
 * chunk of a LAME-encoded MP3 is short by the encoder delay, DESIGN.md
 * V.3 note). This performs exactly one internal decode-and-resample
 * step (or the terminal flush at end of stream) and returns its raw,
 * unclipped frame count. It must never be interleaved with
 * AudioSource.read calls on the same open source -- both drain the same
 * internal buffer and would corrupt each other's accounting. Takes the
 * AudioSource, returns the frame count produced by this step (0 at true
 * EOF, and every call after that also returns 0), or a negative AVERROR
 * mapped through as an opaque negative int -- the caller never needs to
 * decode which one, only that it is an error.
 */
long decoder_diag_step(const AudioSource * src);

#endif // KURONO_AUDIO_DECODER_H
