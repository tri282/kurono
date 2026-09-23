/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef KURONO_AUDIO_TESTSRC_H
#define KURONO_AUDIO_TESTSRC_H

#include <stdint.h>

#include "decoder.h"
#include "format.h"

/*
 * audio/testsrc.c -- the AudioSource doubles DESIGN.md VI names, made
 * real by phase 18's injection seam (plan.md VII gap 5b). Before that
 * seam, audio/engine.c built its own decoder and called two
 * decoder-only functions on it, so §VI's claim that these are "the only
 * way to exercise the ring and the callback in isolation" described
 * something the code did not permit.
 *
 * One implementation with a kind rather than three files: the three
 * differ only in what read() writes into the buffer and when it stops,
 * which is a switch, not a type hierarchy. They share open(), seek(),
 * close(), the frame accounting and the EOF arithmetic -- and sharing
 * that arithmetic is the point, because getting it wrong is how a
 * double starts lying about EOF and quietly invalidates whatever it was
 * built to prove.
 *
 * These take no path: open() accepts any string, including NULL, and
 * ignores it. A double that needed a file on disk would not be a
 * double.
 *
 * Not linked into any test binary yet -- it is wired into SRCS so that
 * `make analyze` and the sanitiser builds see it, the same way every
 * earlier phase wired a file in before its first caller existed.
 */
typedef enum {
  /* Silence. The NullSource of §VI: exercises the ring, the callback,
     the frame counter and the boundary path with no decode cost and no
     audio file. */
  TESTSRC_SILENCE = 0,
  /* A 440 Hz sine at -6 dBFS. The SineSource of §VI. Audible, so a
     human can confirm the path end to end, and periodic, so a
     discontinuity at a seek or a track boundary is visible in a
     recording rather than a matter of opinion. */
  TESTSRC_SINE,
  /* Decodes normally for at least `fault_after_frames` (whole blocks,
     so up to one read's worth more), then returns a negative
     AVERROR-shaped value from read() forever.
     
     This is the one that exists for a specific unreachable branch:
     player_track_failed's EV_DECODE_ERROR arm has never executed, and
     no corrupt file will make it. Four recipes were tried on
     2026-08-14 and all four failed -- libavcodec recovers from FLAC
     frame damage rather than erroring, so a mid-stream hard failure
     cannot be produced from input at all (plan.md VII gap 7). It can
     only be injected. */
  TESTSRC_FAULT
} TestSrcKind;

/*
 * testsrc_create: build one AudioSource of the given kind, bound to
 * fmt -- the session format it pretends to have decoded to, which is
 * the same contract decoder_create has and for the same reason: the
 * ring holds exactly one format for a device's lifetime.
 *
 * duration_ms is what open() reports in StreamInfo and where read()
 * signals EOF by returning short. Zero means "never ends", which is
 * what a ring or callback stress wants; a boundary test wants a real
 * length.
 *
 * fault_after_frames applies to TESTSRC_FAULT alone and is ignored by
 * the other two. Frames are counted from open(), not from create().
 *
 * Takes the session format, the kind, the reported duration, and the
 * fault point. Returns an AudioSource ready for open(); on allocation
 * failure every vtable pointer and self are NULL, exactly as
 * decoder_create does, so a caller that checks one checks both.
 */
AudioSource testsrc_create(const SessionFormat * fmt, TestSrcKind kind,
                           int64_t duration_ms, long fault_after_frames);

#endif // KURONO_AUDIO_TESTSRC_H
