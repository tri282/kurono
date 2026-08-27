# Changelog

Notable changes to `kurono`. Format based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Text too long for its column now ends in `…` instead of stopping mid-word as
  if it were complete. Applies everywhere text is drawn into a bounded width:
  browser columns, the queue pane, search results, the header and the help
  overlay. Very narrow fields still cut silently, where the marker would take
  more of the field than the text.

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
- Gapless is approximate on lossy formats.
- Configuration is environment variables only; `KURONO_CONFIG` is reserved but
  unread.
- With the queue pane open, view switching (`tab`, `1`/`2`/`3`) and search
  (`/`) are unbound. Close the pane with `esc` or `v` first.
- The search prompt can overlap column 1 when that column is full.
- Now-playing in the header can overwrite the view indicator.

[Unreleased]: https://gitlab.com/tri282/kurono/-/compare/v1.0.0...main
[1.0.0]: https://gitlab.com/tri282/kurono/-/tags/v1.0.0
