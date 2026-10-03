# Changelog

Notable changes to `kurono`. Format based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Gapless playback of Ogg Vorbis files no longer leaves up to about 2 ms of
  silence at each track boundary.

## [1.2.0] — 2026-09-23

### Added

- `--play FILE --null-sink` runs the headless playback diagnostics without an
  audio device, so seeking and teardown can be checked under a sanitiser on a
  machine with no sound server.
- `--play FILE --null-sink --fault MS` simulates a decode error mid-track, to
  check that a failing file is reported and skipped rather than fatal.

### Fixed

- Losing the audio device (unplugged, or the output stalling) at the moment a
  track was starting or a paused track was seeking could let the recovery and
  the decoder reset the same playback buffer at once, corrupting playback
  state. Recovery now waits for the play or seek to finish first.

## [1.1.0] — 2026-09-15

### Changed

- The cursor now stays centred in every list — the browser's main column,
  the queue pane, and search results — instead of riding to the edge of the
  viewport and pinning there. The first and last half-viewport are the
  exception: there are no rows past the ends to centre against, so the
  cursor rides out of the centre there, same as most terminal file managers.
- A single step up or down at the top or bottom of a list now wraps to the
  other end, instead of stopping there. A page up/down still clamps at the
  end rather than wrapping. Accepted cost: holding the arrow key down at the
  end of a list now runs off the end and continues from the top, since
  nothing distinguishes a held key from repeated presses.
- Search results beyond the 200-row cap are no longer the library-wide top
  200 by relevance; they are the first 200 matches in scan order, ranked
  among themselves. Every query whose matches fit within 200 — the common
  case, including every two-token search — is unaffected.

### Fixed

- Search latency on a broad first keystroke: roughly 53 ms down to roughly
  1.5 ms at 100,000 tracks, measured on disk. Selective queries were already
  fast and are unchanged.
- A track whose file changed on disk (retagged, re-muxed, or just touched)
  now keeps its library id across a rescan instead of getting a new one. A
  queued track could previously lose its title and duration in the queue
  pane after a rescan, or be reported as "no library entry" when jumped to.

## [1.0.1] — 2026-08-28

### Fixed

- Text too long for its column now ends in `…` instead of stopping mid-word as
  if it were complete. Applies everywhere text is drawn into a bounded width:
  browser columns, the queue pane, search results, the header and the help
  overlay. Very narrow fields still cut silently, where the marker would take
  more of the field than the text.
- The header now keeps the view indicator visible while a track is playing;
  the title takes the remaining width.
- The search prompt no longer draws over the last row of a full column; it
  shares the status bar with the mode indicator while searching. A status
  message moves there too, for the same reason.
- A status message now clears itself after three seconds, so the clock, volume
  and shuffle/repeat indicators come back on their own. Sharing the status bar
  with them made a message that never expired a readout that never returned.

## [1.0.0] — 2026-08-21

First release. Everything below is new, so this entry describes what `kurono`
*is* rather than replaying the 45 commits that built it — nobody upgraded
through those.

### Playback

- Standalone audio path: `kurono` demuxes, decodes and resamples with ffmpeg
  and drives the output device itself. No daemon, no MPD protocol, no external
  player process.
- Three threads with a lock-free single-producer/single-consumer ring between
  the decoder and the audio callback. The callback never allocates, never
  locks and never blocks; an underrun plays silence and is recorded rather than
  waited on.
- Gapless transitions. The next track is opened and decoding before the
  current one ends, so album boundaries have no silence and no click. Exact on
  lossless formats; approximate on lossy ones, where encoder padding is not yet
  compensated.
- Seek, with an explicit flush handshake so the ring and the frame counters
  can never disagree about what has been played. Seeking while paused is
  handled as its own case.
- Volume, mute, shuffle, and repeat off/all/one.
- Device-loss detection and an automatic reopen ladder, so suspending or
  switching an audio device does not end playback.
- Playback position is counted from frames actually consumed by the device,
  never from the decoder, which runs up to two seconds ahead.

### Library

- SQLite index of the music tree, built with TagLib, stored as a derived cache
  under `$XDG_CACHE_HOME`. It is never written inside the music directory.
- Warm rescans compare size and mtime per file and skip everything unchanged,
  which makes them `stat`-bound.
- Full-text search over the whole library through FTS5, ranked, updating as
  you type. Raw input is escaped into a MATCH string that always parses.
- Multiple music roots, `:`-separated. Roots that do not exist are reported
  rather than silently skipped.
- Directory pruning by name through `KURONO_IGNORE_DIRS`.
- Corruption recovery: a database that fails to open, or that was built
  against different music roots, is deleted and rebuilt.
- Per-file failures are never fatal. A corrupt tag or unsupported codec is
  reported and skipped, during both scanning and playback.

### Interface

- Three-column browser with Directory, Artists and Album Artists views — the
  same widget fed by different queries, not separate screens.
- A queue you can reorder and edit while it plays, in a pane that keeps the
  transport keys live.
- Search overlay, help overlay generated from the binding tables themselves,
  and a settings menu.
- Truecolor theme with every colour behind a named slot; no colour is written
  into drawing code.
- Every key resolves through a per-mode binding table to a named action, which
  is what will make them remappable from a config file.
- `epoll` over the terminal, signals, a repaint timer and the engine's pipe, so
  the interface stays responsive while a track opens or a scan runs.

### Build and tooling

- `make` targets for release, debug, ASan+UBSan, TSan, `-fanalyzer`, and five
  standalone test binaries that run with no device, no terminal and no music.
- `make check` enforces three architectural invariants with grep: the layering
  rule, the single legal `ring_reset` call site, and the no-literal-colours
  rule.
- Header dependency tracking, so editing a header rebuilds exactly what
  included it.

### Known limitations

- Linux only, deliberately. `epoll`, `inotify`, `signalfd` and `timerfd` are
  used directly and there is no portable fallback.
- Gapless playback is exact on MP3 (with a LAME/Xing header), Opus and Ogg
  Vorbis, verified against ffmpeg 8.1 (Fedora). AAC in M4A keeps about 5-8 ms
  of encoder padding at the end of each track. Files without encoder-delay
  metadata (raw AAC, MP3 without a LAME header) play with the encoder's
  padding, a few tens of milliseconds. When a file's sample rate differs from
  the output device's, boundaries are exact to within 1 frame.
- Configuration is environment variables only; `KURONO_CONFIG` is reserved but
  unread.
- With the queue pane open, view switching (`tab`, `1`/`2`/`3`) and search
  (`/`) are unbound. Close the pane with `esc` or `v` first.
- Holding a single-step cursor key at the end of a list runs off the end and
  continues from the other side, because nothing distinguishes a held key from
  repeated presses. Page keys clamp and can be used to stop at an end.
- Search returns at most 200 rows, and a query matching more than that returns
  the first 200 in scan order rather than the 200 most relevant. Narrowing the
  query is the fix; anything matching under 200 rows is ranked normally.

[Unreleased]: https://gitlab.com/tri282/kurono/-/compare/v1.2.0...main
[1.2.0]: https://gitlab.com/tri282/kurono/-/compare/v1.1.0...v1.2.0
[1.1.0]: https://gitlab.com/tri282/kurono/-/compare/v1.0.1...v1.1.0
[1.0.1]: https://gitlab.com/tri282/kurono/-/compare/v1.0.0...v1.0.1
[1.0.0]: https://gitlab.com/tri282/kurono/-/tags/v1.0.0
