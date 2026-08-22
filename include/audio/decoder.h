/* SPDX-License-Identifier: GPL-3.0-or-later */

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
  /* retarget and destroy complete the vtable so that a *holder* of an
     AudioSource never has to know which implementation it holds
     (phase 18, plan.md VII gap 5b). Before this, audio/engine.c named
     decoder_retarget_rate and decoder_destroy directly, which is what
     made DESIGN.md VI's claim that a NullSource/SineSource can be
     substituted untrue in practice: the engine constructed its own
     decoder and then called two decoder-only functions on it.

     retarget may be NULL. It exists for the device-reopen path alone
     (DESIGN.md II.2's warning) and a source that does not resample has
     nothing to retarget; the engine treats a NULL retarget as success,
     not as failure. destroy must not be NULL. */
  int  (* retarget)(void * self, uint32_t sample_rate);   /* may be NULL */
  void (* destroy)(void * self);
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
 * decoder_probe: read one file's stream properties without constructing
 * an AudioSource, opening a codec, or building a resampler. This exists
 * so library/scan.c can fill the duration_ms/bitrate/sample_rate/
 * channels/codec half of a track row (DESIGN.md VII.2 -- taglib_c owns
 * the tag half) without opening an AVFormatContext of its own, which
 * would put ffmpeg outside audio/decoder.c and break the confinement
 * rule (CLAUDE.md; DESIGN.md III.2).
 *
 * Cheaper than open() on purpose: the scan calls this once per new or
 * changed file across a whole library, so it stops at the container and
 * the stream's codec parameters and never calls avcodec_open2. The
 * consequence is that a file whose header parses but whose codec cannot
 * actually be opened still probes successfully and gets indexed --
 * correct, since the browser should list a track it cannot play and
 * report the failure at play time (DESIGN.md XI.1), not hide it.
 *
 * Takes the path and the StreamInfo to fill; returns 0 on success or a
 * negative AVERROR on any per-file failure -- unreadable, not an audio
 * file, no audio stream. Never fatal, never exits (DESIGN.md XI.1).
 * *out_info is zeroed before anything else, so a failed probe leaves it
 * well-defined rather than stale.
 */
int decoder_probe(const char * path, StreamInfo * out_info);

/*
 * decoder_destroy: release everything decoder_create allocated,
 * including any file left open by a prior open() that was never
 * close()d. Idempotent-safe to call once; src->self is NULL afterward.
 * Takes the AudioSource returned by decoder_create, returns nothing.
 */
void decoder_destroy(AudioSource * src);

/*
 * decoder_retarget_rate: replace the session sample rate this source
 * resamples to, and rebuild the SwrContext against it for whichever file
 * is currently open. This is DESIGN.md IV.8.b step 4's "reinitialise the
 * SwrContext to the new R" -- the one and only path on which the target
 * a source was constructed with may change (see decoder_create's note,
 * and DESIGN.md II.2's warning).
 *
 * Legal exclusively from inside the device-reopen ladder, on T1, and
 * only after engine_park_sync has returned: T3 provably does not exist
 * at that moment (step 2's ma_device_uninit joined it) and T2 is parked,
 * so this is single-threaded access to state T2 otherwise owns
 * exclusively. Reached through engine_device_reflush, never called
 * directly by T1.
 *
 * Any resample delay swr had buffered from before the reopen is dropped
 * along with the residual, since it belongs to the discarded ring
 * contents. Takes the AudioSource and the new rate in Hz; returns 0 on
 * success -- including when no file is open, in which case the rate is
 * simply recorded for the next open() -- or a negative AVERROR if the
 * resampler could not be rebuilt.
 */
int decoder_retarget_rate(AudioSource * src, uint32_t sample_rate);

/* decoder_diag_step is gone (phase 18, plan.md VII gap 1). It existed
   solely for --decode, which measured whether a LAME-encoded MP3's
   first decode step comes back short by the encoder delay (DESIGN.md
   V.3's note). The flag was deleted with the rest of the per-phase
   scaffolding, and a diagnostic with no way to invoke it is not a
   diagnostic. The measurement it was built for is still worth taking
   and is now a testsrc/decoder comparison rather than a print. */

#endif // KURONO_AUDIO_DECODER_H
