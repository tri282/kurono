/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * audio/decoder.c -- the AudioSource implementation over ffmpeg:
 * demux, decode, and resample any supported file to the session format
 * (DESIGN.md VI, phase-6-decoder.md).
 *
 * Every ffmpeg type and every ffmpeg call in the whole tree lives in
 * this one file (CLAUDE.md; DESIGN.md III.2, XIII) -- no ffmpeg symbol
 * appears in include/audio/decoder.h. This is the T2 (decoder thread)
 * side of the split; T2 is explicitly allowed to allocate and do I/O
 * (DESIGN.md II.1) -- none of the audio-callback contract in §IV.2
 * applies here, that is T3's file (audio/sink.c) only.
 */

#include "../../include/audio/decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SessionFormat.channels is always 2 (DESIGN.md II.2) -- decoder.c
   targets stereo unconditionally rather than reading it back, the same
   pinning discipline audio/sink.c applies to SINK_CHANNELS. */
#define DECODER_CHANNELS 2

/* Initial capacity of the residual buffer, in frames. Grown by doubling
   in ensure_residual_capacity when a single decode step needs more --
   see that function for why a fixed compile-time bound cannot be
   correct for every codec/resample-ratio combination. 8192 frames is
   comfortably above one AVFrame's worth of output for every codec this
   project expects to see (typical decoded frame sizes run a few hundred
   to a few thousand samples), so growth should be rare in practice. */
#define RESIDUAL_INITIAL_FRAMES 8192

/* Frames a capped file may decode past its stated length before the cap
   is judged wrong and released. ~0.34 s at 48 kHz: above any single
   block of encoder padding. */
#define TRIM_HOLD_MAX_FRAMES 16384L

/* Containers (AVInputFormat.name) whose stream duration is exact in
   samples and whose decode overruns it by encoder padding, on measured
   evidence. Ogg only: m4a's duration field is not exact. */
static const char * const TRIM_CAP_FORMATS[] = { "ogg" };

/*
 * Decoder: the ffmpeg-side state behind one AudioSource.self. Opaque to
 * every other file in the tree -- decoder.h declares only a forward-free
 * void * for it. One instance is created by decoder_create and reused
 * across every track T2 plays for the process's lifetime (open() closes
 * the previous file's ffmpeg objects before opening the next).
 */
typedef struct {
  AVFormatContext * fmt_ctx;
  AVCodecContext  * codec_ctx;
  SwrContext      * swr;
  AVPacket        * pkt;
  AVFrame         * frame;
  int               stream_index;

  /* The session format this source was constructed against (DESIGN.md
     II.2). Never changes across opens. The single exception is the
     device-reopen ladder's decoder_retarget_rate (IV.8.b step 4), which
     may replace the rate precisely because T3 provably does not exist
     and T2 is parked at that moment. */
  SessionFormat target_fmt;

  /* Residual buffer: interleaved stereo f32 frames swr_convert produced
     beyond what the caller's current read() request needed. Carried
     across read() calls rather than discarded (phase-6-decoder.md I.4)
     -- discarding it would be a silent gap at every read boundary. */
  float * residual;
  long    residual_cap; /* frames */
  long    residual_len; /* valid frames, starting at offset 0 */
  long    residual_off; /* next unread frame within residual */

  int reading_done; /* av_read_frame hit EOF/error; NULL packet already sent to the codec */
  int swr_flushed;  /* codec fully drained; swr already flushed with a NULL input */

  /* d->pkt currently holds a packet the codec refused with EAGAIN and
     that must be re-sent before any further packet is read. Unref'ing
     it instead would silently drop a packet's worth of audio. */
  int pkt_pending;

  /* The AVERROR from a genuine mid-stream decode failure, latched until
     read()/decoder_diag_step reports it. 0 when there is none. Without
     this the file's error is indistinguishable from its EOF, and
     phase 7 can never raise EV_DECODE_ERROR (phase-6-decoder.md I.6,
     DESIGN.md XI.1). */
  int last_error;

  /* Tail cap: the true length in session-rate frames, or -1 when the file
     is uncapped. out_frames counts frames handed to the caller since open
     or the last seek. */
  int64_t cap_out;
  int64_t out_frames;
} Decoder;

/*
 * ensure_residual_capacity: grow d->residual so it can hold at least
 * need_frames frames, doubling from RESIDUAL_INITIAL_FRAMES as needed.
 * Takes the decoder and the required capacity in frames, returns 0 on
 * success or -1 if realloc fails (existing buffer is left untouched on
 * failure, per the standard realloc contract).
 */
static int
ensure_residual_capacity(Decoder * d, long need_frames)
{
  if (need_frames <= d->residual_cap) {
    return 0;
  }

  long new_cap = (d->residual_cap > 0) ? d->residual_cap : RESIDUAL_INITIAL_FRAMES;
  while (new_cap < need_frames) {
    new_cap *= 2;
  }

  float * grown = realloc(d->residual,
                           (size_t) new_cap * DECODER_CHANNELS * sizeof(float));
  if (grown == NULL) {
    return -1;
  }

  d->residual = grown;
  d->residual_cap = new_cap;
  return 0;
} /* ensure_residual_capacity */

/*
 * convert_frame_to_residual: run one AVFrame (or, when frame is NULL,
 * the terminal flush) through swr and append the output to
 * d->residual, growing it first if needed. Takes the decoder and the
 * decoded frame (NULL to flush swr's internal delay buffer at end of
 * stream), returns 0 on success or a negative AVERROR/-ENOMEM on
 * failure.
 */
static int
convert_frame_to_residual(Decoder * d, const AVFrame * frame)
{
  const uint8_t * const * in = frame ? (const uint8_t * const *) frame->extended_data : NULL;
  int in_count = frame ? frame->nb_samples : 0;

  int64_t max_out = swr_get_out_samples(d->swr, in_count);
  if (max_out < 0) {
    max_out = RESIDUAL_INITIAL_FRAMES; /* defensive fallback only -- swr_convert
                                           below still reports the true count and
                                           cannot overflow the space we hand it,
                                           since out_count is derived from the
                                           buffer we just sized */
  }

  if (ensure_residual_capacity(d, d->residual_len + max_out) != 0) {
    return AVERROR(ENOMEM);
  }

  uint8_t * out_ptr = (uint8_t *) (d->residual + (size_t) d->residual_len * DECODER_CHANNELS);
  int out_count = (int) (d->residual_cap - d->residual_len);

  int produced = swr_convert(d->swr, &out_ptr, out_count, in, in_count);
  if (produced < 0) {
    return produced;
  }

  d->residual_len += produced;
  return 0;
} /* convert_frame_to_residual */

/*
 * decode_one_frame: pull packets and drive the codec until exactly one
 * decode-and-resample step lands new output in d->residual, or the
 * stream and swr are both provably exhausted. Handles
 * AVERROR(EAGAIN)/AVERROR_EOF from the send/receive pair, and drains
 * the codec with a NULL packet before the terminal swr flush
 * (phase-6-decoder.md I.4/I.5). A genuine mid-stream decode error ends
 * the file here rather than propagating -- per-file failure is never
 * fatal (DESIGN.md XI.1). Takes the decoder, returns 1 if d->residual
 * gained data, 0 at true end of stream (including after an error that
 * is treated as an early end).
 */
static int
decode_one_frame(Decoder * d)
{
  for (;;) {
    int rret = avcodec_receive_frame(d->codec_ctx, d->frame);

    if (rret == 0) {
      int cret = convert_frame_to_residual(d, d->frame);
      av_frame_unref(d->frame);
      if (cret < 0) {
        d->last_error = cret; /* end this file, do not crash -- but report it */
        return 0;
      }
      if (d->residual_len > 0) {
        return 1;
      }
      continue; /* swr buffered internally without emitting yet -- decode more */
    }

    if (rret == AVERROR_EOF) {
      if (!d->swr_flushed) {
        d->swr_flushed = 1;
        if (convert_frame_to_residual(d, NULL) == 0 && d->residual_len > 0) {
          return 1;
        }
      }
      return 0; /* codec and swr both fully drained */
    }

    if (rret != AVERROR(EAGAIN)) {
      d->last_error = rret; /* genuine mid-stream decode error -- per-file, not fatal */
      return 0;
    }

    /* EAGAIN: the codec needs another packet before it can emit more. */
    if (d->reading_done) {
      /* A draining codec -- one that has already been handed the NULL
         packet -- answers receive_frame with a frame or with
         AVERROR_EOF, never with EAGAIN. Should it ever do so anyway,
         returning here ends the file instead of spinning forever on a
         codec that cannot make progress. */
      return 0;
    }

    int fed = 0;
    while (!fed) {
      if (!d->pkt_pending) {
        int rr = av_read_frame(d->fmt_ctx, d->pkt);
        if (rr < 0) {
          avcodec_send_packet(d->codec_ctx, NULL); /* begin draining the codec */
          d->reading_done = 1;
          break;
        }
        if (d->pkt->stream_index != d->stream_index) {
          av_packet_unref(d->pkt);
          continue; /* not our stream -- skip and read the next packet */
        }
        d->pkt_pending = 1;
      }

      int sret = avcodec_send_packet(d->codec_ctx, d->pkt);
      if (sret == AVERROR(EAGAIN)) {
        /* The codec is full and did NOT consume this packet. Keep it
           for the next attempt and go drain a frame out first --
           unref'ing here would discard a packet's worth of audio
           silently, which is inaudible in testing and wrong forever. */
        break;
      }
      av_packet_unref(d->pkt);
      d->pkt_pending = 0;
      if (sret == 0) {
        fed = 1;
      }
      /* else: reject this one packet and read the next rather than
         failing the whole file over one bad packet */
    }
  }
} /* decode_one_frame */

/*
 * decoder_build_swr: (re)build d->swr to convert the currently open
 * file's codec output into interleaved f32 stereo at d->target_fmt's
 * sample rate. Any existing context is freed first rather than
 * reconfigured in place, so the caller never has to reason about which
 * options swr_alloc_set_opts2 does and does not overwrite on a live
 * context. Requires d->codec_ctx to be open. Takes the decoder, returns
 * 0 on success or a negative AVERROR.
 */
static int
decoder_build_swr(Decoder * d)
{
  if (d->swr != NULL) {
    swr_free(&d->swr);
  }

  AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
  int ret = swr_alloc_set_opts2(&d->swr,
          &out_ch, AV_SAMPLE_FMT_FLT, (int) d->target_fmt.sample_rate,
          &d->codec_ctx->ch_layout, d->codec_ctx->sample_fmt, d->codec_ctx->sample_rate,
          0, NULL);
  if (ret < 0 || d->swr == NULL) {
    return (ret < 0) ? ret : AVERROR(ENOMEM);
  }

  return swr_init(d->swr);
} /* decoder_build_swr */

/*
 * decoder_close_file: release the ffmpeg objects for whichever file is
 * currently open (if any) and reset the per-file bookkeeping, including
 * the residual buffer's contents (but not its allocation -- that is
 * kept and reused by the next open()). Safe to call when nothing is
 * open. Takes the decoder, returns nothing.
 */
static void
decoder_close_file(Decoder * d)
{
  if (d->pkt != NULL && d->pkt_pending) {
    av_packet_unref(d->pkt);
  }
  if (d->swr != NULL) {
    swr_free(&d->swr);
  }
  if (d->pkt != NULL) {
    av_packet_free(&d->pkt);
  }
  if (d->frame != NULL) {
    av_frame_free(&d->frame);
  }
  if (d->codec_ctx != NULL) {
    avcodec_free_context(&d->codec_ctx);
  }
  if (d->fmt_ctx != NULL) {
    avformat_close_input(&d->fmt_ctx);
  }

  d->stream_index = -1;
  d->reading_done = 0;
  d->swr_flushed  = 0;
  d->pkt_pending  = 0;
  d->last_error   = 0;
  d->residual_len = 0;
  d->residual_off = 0;
  d->cap_out      = -1;
  d->out_frames   = 0;
} /* decoder_close_file */

/* The two vtable slots whose bodies sit below decoder_create, beside the
   public wrappers that share them. Forward-declared rather than moved:
   decoder_destroy_impl and decoder_retarget_impl belong next to
   decoder_destroy and decoder_retarget_rate, which is where anyone
   changing one will look for the other. */
static void decoder_destroy_impl(void * self);
static int  decoder_retarget_impl(void * self, uint32_t sample_rate);

/*
 * trim_cap_open: enable the tail cap for the file just opened when its
 * container is in TRIM_CAP_FORMATS and its stream duration is an exact
 * sample count (time base 1/sample_rate). Takes the decoder and the audio
 * stream; sets d->cap_out and never fails -- any doubt leaves the file
 * uncapped (-1).
 */
static void
trim_cap_open(Decoder * d, const AVStream * st)
{
  d->cap_out = -1;

  const AVInputFormat * fmt = d->fmt_ctx->iformat;
  if (fmt == NULL || fmt->name == NULL) {
    return;
  }

  int listed = 0;
  for (size_t i = 0; i < sizeof(TRIM_CAP_FORMATS) / sizeof(TRIM_CAP_FORMATS[0]); i++) {
    if (strcmp(fmt->name, TRIM_CAP_FORMATS[i]) == 0) {
      listed = 1;
    }
  }

  if (!listed || st->duration == (int64_t) AV_NOPTS_VALUE || st->duration <= 0
      || d->codec_ctx->sample_rate <= 0
      || st->time_base.num != 1 || st->time_base.den != d->codec_ctx->sample_rate) {
    return;
  }

  d->cap_out = av_rescale(st->duration, (int64_t) d->target_fmt.sample_rate,
                          d->codec_ctx->sample_rate);
} /* trim_cap_open */

/*
 * decoder_open_impl: AudioSource.open. Closes any previously open file
 * first (this Decoder is reused across every track, DESIGN.md V.1),
 * then avformat_open_input -> avformat_find_stream_info ->
 * av_find_best_stream -> avcodec_alloc_context3 ->
 * avcodec_parameters_to_context -> avcodec_open2, builds a SwrContext to
 * interleaved f32 stereo at the stored target rate
 * (phase-6-decoder.md I.3), and fills *out_info. Deliberately does not
 * set AV_CODEC_FLAG2_SKIP_MANUAL -- libavcodec applies
 * AV_PKT_DATA_SKIP_SAMPLES automatically, which is what makes DESIGN.md
 * V.3's gapless transition exact in frames for the containers
 * tools/gapless_test checks (MP3 with a LAME header, Opus, Vorbis),
 * measured, not assumed; Vorbis relies on the tail cap above. AAC in M4A
 * is declared untrimmed (a few ms of padding at each track end). Setting
 * that flag would hand the side data back to us instead and turn a few ms
 * of error into an audible click on every lossy track. Every early-return path frees everything this call
 * allocated (decoder_close_file, called at both entry and on every
 * failure branch). Takes self, the path, and the StreamInfo to fill;
 * returns 0 on success or a negative AVERROR on any per-file failure
 * (DESIGN.md XI.1 -- never fatal, never exits, never aborts).
 */
static int
decoder_open_impl(void * self, const char * path, StreamInfo * out_info)
{
  Decoder * d = (Decoder *) self;
  if (d == NULL || path == NULL || out_info == NULL) {
    return AVERROR(EINVAL);
  }

  decoder_close_file(d);

  int ret = avformat_open_input(&d->fmt_ctx, path, NULL, NULL);
  if (ret < 0) {
    d->fmt_ctx = NULL; /* avformat_open_input frees and nulls *ps itself on failure,
                           but the assignment is kept explicit as the documented
                           contract that decoder_close_file relies on */
    return ret;
  }

  ret = avformat_find_stream_info(d->fmt_ctx, NULL);
  if (ret < 0) {
    goto fail;
  }

  const AVCodec * codec = NULL;
  ret = av_find_best_stream(d->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (ret < 0) {
    goto fail;
  }
  d->stream_index = ret;

  d->codec_ctx = avcodec_alloc_context3(codec);
  if (d->codec_ctx == NULL) {
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  ret = avcodec_parameters_to_context(d->codec_ctx,
          d->fmt_ctx->streams[d->stream_index]->codecpar);
  if (ret < 0) {
    goto fail;
  }

  ret = avcodec_open2(d->codec_ctx, codec, NULL);
  if (ret < 0) {
    goto fail;
  }

  ret = decoder_build_swr(d);
  if (ret < 0) {
    goto fail;
  }

  d->pkt = av_packet_alloc();
  d->frame = av_frame_alloc();
  if (d->pkt == NULL || d->frame == NULL) {
    ret = AVERROR(ENOMEM);
    goto fail;
  }

  AVStream * stream = d->fmt_ctx->streams[d->stream_index];
  memset(out_info, 0, sizeof(*out_info));

  if (d->fmt_ctx->duration != (int64_t) AV_NOPTS_VALUE) {
    out_info->duration_ms = av_rescale_q(d->fmt_ctx->duration, AV_TIME_BASE_Q,
                                          (AVRational) { 1, 1000 });
  } else if (stream->duration != (int64_t) AV_NOPTS_VALUE) {
    out_info->duration_ms = av_rescale_q(stream->duration, stream->time_base,
                                          (AVRational) { 1, 1000 });
  }
  out_info->bitrate = d->codec_ctx->bit_rate;
  if (out_info->bitrate <= 0) {
    /* Lossless codecs commonly report no codec-level bit rate at all;
       the container's overall figure is the only number available. */
    out_info->bitrate = d->fmt_ctx->bit_rate;
  }
  out_info->sample_rate = d->codec_ctx->sample_rate;
  out_info->channels    = d->codec_ctx->ch_layout.nb_channels;
  const char * name = avcodec_get_name(d->codec_ctx->codec_id);
  snprintf(out_info->codec, sizeof(out_info->codec), "%s", name != NULL ? name : "?");

  trim_cap_open(d, stream);

  return 0;

fail:
  decoder_close_file(d);
  return (ret < 0) ? ret : AVERROR_UNKNOWN;
} /* decoder_open_impl */

/*
 * trim_hold_to_eof: called once a capped file has handed over cap_out
 * frames. Keeps decoding (decode_one_frame appends to the residual, which
 * is the hold) until EOF. Takes the decoder; returns 0 at EOF within
 * TRIM_HOLD_MAX_FRAMES, with the held frames dropped as padding, or 1 if
 * the hold overflowed: the cap is then disabled for the file and the held
 * frames flow to the caller.
 */
static int
trim_hold_to_eof(Decoder * d)
{
  while (decode_one_frame(d)) {
    if (d->residual_len - d->residual_off > TRIM_HOLD_MAX_FRAMES) {
      d->cap_out = -1; /* stated length was wrong: play untrimmed */
      return 1;
    }
  }
  d->residual_len = 0;
  d->residual_off = 0;
  return 0;
} /* trim_hold_to_eof */

/*
 * decoder_read_impl: AudioSource.read. Drains the residual buffer into
 * dst first, and only decodes more (via decode_one_frame) once it is
 * empty -- phase-6-decoder.md I.4's "drain swr first" ordering. A
 * return less than frames is the entire EOF signal (DESIGN.md VI); it
 * is produced by decode_one_frame returning 0 once both the codec and
 * swr are exhausted. A mid-stream decode failure is NOT folded into
 * that signal: it is latched and returned as a negative AVERROR once
 * the frames decoded before it have been handed over, so phase 7 can
 * tell EV_DECODE_ERROR from a clean end of track (phase-6-decoder.md
 * I.6). Never called concurrently with decoder_diag_step on the same
 * source. Takes self, the destination buffer (frames * 2 floats), and
 * the requested frame count; returns the frame count actually produced,
 * or a negative AVERROR when a failure ended the file and no frames
 * remain to hand over. A capped file (trim_cap_open) returns short exactly
 * at its cap, which is its EOF.
 */
static long
decoder_read_impl(void * self, float * dst, long frames)
{
  Decoder * d = (Decoder *) self;
  if (d == NULL || d->fmt_ctx == NULL || dst == NULL || frames <= 0) {
    return 0;
  }

  long produced = 0;

  while (produced < frames) {
    if (d->cap_out >= 0 && d->out_frames >= d->cap_out) {
      if (trim_hold_to_eof(d)) {
        continue; /* cap released: drain the hold normally */
      }
      break; /* EOF: short return, as before */
    }

    if (d->residual_off < d->residual_len) {
      long avail = d->residual_len - d->residual_off;
      long want  = frames - produced;
      if (d->cap_out >= 0 && want > d->cap_out - d->out_frames) {
        want = (long) (d->cap_out - d->out_frames);
      }
      long take  = (avail < want) ? avail : want;

      memcpy(dst + (size_t) produced * DECODER_CHANNELS,
             d->residual + (size_t) d->residual_off * DECODER_CHANNELS,
             (size_t) take * DECODER_CHANNELS * sizeof(float));

      d->residual_off += take;
      produced += take;
      d->out_frames += take;

      if (d->residual_off >= d->residual_len) {
        d->residual_off = 0;
        d->residual_len = 0;
      }
      continue;
    }

    if (!decode_one_frame(d)) {
      break; /* true EOF (or an early-ended file) -- short return is the signal */
    }
  }

  /* Frames already decoded are handed over first; the latched error is
     reported by the next call, which returns 0 frames and this value.
     Reporting it now would throw away good audio. */
  if (produced == 0 && d->last_error < 0) {
    long err = d->last_error;
    d->last_error = 0;
    return err;
  }

  return produced;
} /* decoder_read_impl */

/*
 * decoder_seek_impl: AudioSource.seek. Rescales ms into the audio
 * stream's own time base, av_seek_frame with AVSEEK_FLAG_BACKWARD, then
 * avcodec_flush_buffers and a fresh swr_init to drop any buffered
 * decode/resample state left over from before the seek, and clears the
 * residual buffer and end-of-stream bookkeeping. Touches neither
 * pcm_ring nor any frame counter -- phase 7's engine owns the ring reset
 * and counter resync inside its own flush window (phase-6-decoder.md
 * I.5; DESIGN.md IV.7). Takes self and the target position in
 * milliseconds; returns 0 on success or a negative AVERROR.
 */
static int
decoder_seek_impl(void * self, int64_t ms)
{
  Decoder * d = (Decoder *) self;
  if (d == NULL || d->fmt_ctx == NULL) {
    return AVERROR(EINVAL);
  }

  AVStream * stream = d->fmt_ctx->streams[d->stream_index];
  int64_t ts = av_rescale_q(ms, (AVRational) { 1, 1000 }, stream->time_base);

  int ret = av_seek_frame(d->fmt_ctx, d->stream_index, ts, AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    return ret;
  }

  avcodec_flush_buffers(d->codec_ctx);

  if (d->pkt_pending) {
    av_packet_unref(d->pkt); /* pre-seek packet, no longer wanted */
    d->pkt_pending = 0;
  }

  if (d->swr != NULL) {
    ret = swr_init(d->swr); /* drop any buffered resample delay from before the seek */
    if (ret < 0) {
      return ret;
    }
  }

  d->reading_done = 0;
  d->swr_flushed  = 0;
  d->last_error   = 0;
  d->residual_len = 0;
  d->residual_off = 0;
  d->out_frames   = ms * (int64_t) d->target_fmt.sample_rate / 1000; /* do_seek's resync formula */

  return 0;
} /* decoder_seek_impl */

/*
 * decoder_close_impl: AudioSource.close. Releases the currently open
 * file's ffmpeg objects (decoder_close_file) but keeps the Decoder
 * itself and its residual buffer allocation, so the same AudioSource
 * can open() the next track (DESIGN.md V.1) without a fresh
 * decoder_create. Takes self, returns nothing.
 */
static void
decoder_close_impl(void * self)
{
  Decoder * d = (Decoder *) self;
  if (d == NULL) {
    return;
  }
  decoder_close_file(d);
} /* decoder_close_impl */

/* decoder_create: see include/audio/decoder.h. */
AudioSource
decoder_create(const SessionFormat * fmt)
{
  AudioSource src;
  memset(&src, 0, sizeof(src));

  Decoder * d = calloc(1, sizeof(Decoder));
  if (d == NULL) {
    return src; /* all-NULL vtable and self -- caller must check src.self */
  }

  d->target_fmt   = *fmt;
  d->stream_index = -1;

  /* libav* writes to stderr unbidden, which corrupts the TUI once one
     exists (CLAUDE.md; phase-6-decoder.md I.6). Set once per source
     construction -- av_log_set_level is global ffmpeg state, so this is
     idempotent across however many decoder_create calls the process
     ever makes (DESIGN.md V.1 -- there is exactly one AudioSource for
     T2's lifetime, so in practice this runs once). */
  av_log_set_level(AV_LOG_QUIET);

  src.open     = decoder_open_impl;
  src.read     = decoder_read_impl;
  src.seek     = decoder_seek_impl;
  src.close    = decoder_close_impl;
  src.retarget = decoder_retarget_impl;
  src.destroy  = decoder_destroy_impl;
  src.self     = d;
  return src;
} /* decoder_create */

/*
 * decoder_probe: see include/audio/decoder.h.
 */
int
decoder_probe(const char * path, StreamInfo * out_info)
{
  if (path == NULL || out_info == NULL) {
    return AVERROR(EINVAL);
  }
  memset(out_info, 0, sizeof(*out_info));

  /* Same reason decoder_create does it, and this path needs it
     independently: the scan probes thousands of files without ever
     constructing an AudioSource, and libav* writes to stderr unbidden
     ("timescale not set" on m4a, among others). Harmless while the
     startup scan owns stderr, fatal to the display on the ACT_RESCAN
     path where the TUI already holds the terminal. Global ffmpeg state,
     so setting it here as well is idempotent. */
  av_log_set_level(AV_LOG_QUIET);

  AVFormatContext * fmt_ctx = NULL;
  int ret = avformat_open_input(&fmt_ctx, path, NULL, NULL);
  if (ret < 0) {
    return ret;
  }

  ret = avformat_find_stream_info(fmt_ctx, NULL);
  if (ret < 0) {
    goto done;
  }

  ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (ret < 0) {
    goto done;
  }

  AVStream *          stream = fmt_ctx->streams[ret];
  AVCodecParameters * par = stream->codecpar;

  if (fmt_ctx->duration != (int64_t) AV_NOPTS_VALUE) {
    out_info->duration_ms = av_rescale_q(fmt_ctx->duration, AV_TIME_BASE_Q,
                                          (AVRational) { 1, 1000 });
  } else if (stream->duration != (int64_t) AV_NOPTS_VALUE) {
    out_info->duration_ms = av_rescale_q(stream->duration, stream->time_base,
                                          (AVRational) { 1, 1000 });
  }

  /* Same codec-then-container fallback decoder_open_impl uses: lossless
     codecs commonly report no codec-level bit rate at all. */
  out_info->bitrate = par->bit_rate;
  if (out_info->bitrate <= 0) {
    out_info->bitrate = fmt_ctx->bit_rate;
  }
  out_info->sample_rate = par->sample_rate;
  out_info->channels    = par->ch_layout.nb_channels;
  const char * name = avcodec_get_name(par->codec_id);
  snprintf(out_info->codec, sizeof(out_info->codec), "%s", name != NULL ? name : "?");

  ret = 0;

done:
  avformat_close_input(&fmt_ctx);
  return ret;
} /* decoder_probe */

/* decoder_destroy_impl: the AudioSource.destroy slot. Frees the Decoder
   behind self and everything it owns. Takes the self pointer, returns
   nothing.

   Note what it does NOT do, unlike decoder_destroy below: it cannot zero
   the caller's AudioSource, because a vtable slot is handed the self
   pointer and never the struct that holds it. Clearing the struct is the
   holder's job -- audio/engine.c does it in engine_destroy. */
static void
decoder_destroy_impl(void * self)
{
  if (self == NULL) {
    return;
  }

  Decoder * d = (Decoder *) self;
  decoder_close_file(d);
  free(d->residual);
  free(d);
} /* decoder_destroy_impl */

/* decoder_destroy: see include/audio/decoder.h. */
void
decoder_destroy(AudioSource * src)
{
  if (src == NULL || src->self == NULL) {
    return;
  }

  decoder_destroy_impl(src->self);
  memset(src, 0, sizeof(*src));
} /* decoder_destroy */

/* decoder_retarget_impl: the AudioSource.retarget slot. Same body as
   decoder_retarget_rate below, reached through the vtable rather than
   through the concrete type. Takes the self pointer and the new rate in
   Hz, returns 0 on success and a negative AVERROR otherwise. */
static int
decoder_retarget_impl(void * self, uint32_t sample_rate)
{
  if (self == NULL || sample_rate == 0) {
    return AVERROR(EINVAL);
  }

  Decoder * d = (Decoder *) self;
  if (d->cap_out >= 0 && d->target_fmt.sample_rate > 0) {
    /* cap and progress are in session-rate frames: carry them across */
    d->cap_out    = av_rescale(d->cap_out, sample_rate, d->target_fmt.sample_rate);
    d->out_frames = av_rescale(d->out_frames, sample_rate, d->target_fmt.sample_rate);
  }
  d->target_fmt.sample_rate = sample_rate;

  /* The residual holds frames resampled to the *old* rate. They belong
     to the ring contents the ladder is about to discard, so they go with
     them -- keeping them would splice a few ms of wrong-rate audio into
     the first block after the reopen. */
  d->residual_len = 0;
  d->residual_off = 0;

  if (d->fmt_ctx == NULL || d->codec_ctx == NULL) {
    return 0; /* nothing open -- the new rate applies at the next open() */
  }

  int ret = decoder_build_swr(d);
  if (ret < 0) {
    return ret;
  }
  d->swr_flushed = 0; /* a fresh context has its own delay left to flush */
  return 0;
} /* decoder_retarget_impl */

/* decoder_retarget_rate: see include/audio/decoder.h. */
int
decoder_retarget_rate(AudioSource * src, uint32_t sample_rate)
{
  if (src == NULL) {
    return AVERROR(EINVAL);
  }
  return decoder_retarget_impl(src->self, sample_rate);
} /* decoder_retarget_rate */

