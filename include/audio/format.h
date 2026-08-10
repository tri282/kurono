#ifndef KURONO_AUDIO_FORMAT_H
#define KURONO_AUDIO_FORMAT_H

#include <stdint.h>

/*
 * SessionFormat: the negotiated device format, fixed for the lifetime of
 * one ma_device (DESIGN.md II.2). f32 and 2 channels are pinned by us in
 * the device config and never renegotiated; only sample_rate is read
 * back from the device after ma_device_init succeeds. The internal_*
 * fields exist for the status bar only. internal_channels in particular
 * must never be used to size a ring or a callback's block arithmetic
 * (DESIGN.md II.2 warning): channels = 2 is pinned ahead of our
 * callback, so frameCount * 2 * sizeof(float) is correct by construction
 * regardless of what the physical device natively is.
 */
typedef struct {
  uint32_t sample_rate;       /* R, negotiated; f32 and 2 channels are pinned */
  uint32_t channels;          /* always 2 -- pinned by config, never read back */
  uint32_t period_frames;     /* internalPeriodSizeInFrames, status bar only  */
  uint32_t internal_rate;     /* for the status bar only                      */
  int      internal_format;   /* ma_format, status bar only                   */
  uint32_t internal_channels; /* status bar only -- NEVER used for arithmetic */
} SessionFormat;

/*
 * StreamInfo: per-track stream properties reported by AudioSource.open
 * (DESIGN.md VI, phase 6). Declared here alongside SessionFormat because
 * both are the types DESIGN.md VI names but does not define (plan.md
 * VII gap 3, closed 2026-08-07).
 */
typedef struct {
  int64_t duration_ms;
  int64_t bitrate;
  int     sample_rate;
  int     channels;
  char    codec[32];
} StreamInfo;

/*
 * SinkNotification: the device notifications of DESIGN.md IV.8.a's
 * policy table, in a form T1 can name. The underlying values are
 * miniaudio's ma_device_notification_type, which must never leave
 * audio/sink.c (CLAUDE.md; DESIGN.md III.2) -- sink.c translates into
 * this enum before it dispatches, so main.c can write the policy table
 * without learning what backend produced it. SINK_NOTIFY_UNKNOWN covers
 * any notification type the vendored header gains on an upgrade: an
 * unrecognised notification is reported, never silently mapped onto one
 * of the four that carry policy.
 */
typedef enum {
  SINK_NOTIFY_STARTED = 0,
  SINK_NOTIFY_STOPPED,
  SINK_NOTIFY_REROUTED,
  SINK_NOTIFY_INTERRUPTION_BEGAN,
  SINK_NOTIFY_INTERRUPTION_ENDED,
  SINK_NOTIFY_UNKNOWN
} SinkNotification;

/*
 * SinkNotifyCallback: T1-side handler for a device notification
 * (started/stopped/rerouted/etc, DESIGN.md IV.3). Device policy --
 * reopen, degrade, notify the user -- is decided here, on T1, never on
 * T3. Registered via AudioSink.open (include/audio/sink.h); invoked
 * outside the audio thread, from sink_dispatch_notifications, once T1
 * drains the event that the notification callback's pipe byte announced
 * (phase 8). notification_type is a SinkNotification, widened to int so
 * this header stays free of any ordering dependency on the enum.
 */
typedef void (* SinkNotifyCallback)(void * user, int notification_type);

#endif // KURONO_AUDIO_FORMAT_H
