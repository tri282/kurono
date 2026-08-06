# kurono

A terminal music player in C. Standalone — it owns its own decode and output path, with no daemon, no MPD protocol, and no external player process. Three-column ranger/lf-style browsing over a SQLite-indexed music tree, keyboard driven throughout.

Goal: build `kurono` from source and run it against a local music directory.  
*Requirements: `gcc`, GNU `make`, `pkg-config`, and the system libraries in section I. Linux only.*

---

## 0. Status

**Nothing is implemented yet.** This repository currently contains the design and its supporting documents. Everything below section I describes the intended contract, not working behaviour.

`DESIGN.md` is the authoritative specification. Read it before writing any code.

Planned milestones, in order:

| Milestone | Contents | State |
|---|---|---|
| M1 — spine | Audio engine, ring buffer, decoder, sink, event loop, single-column directory list, playback controls, status bar | not started |
| M2 — library (v1) | SQLite index and scan, three-column browser, Artists / Album Artists views, FTS5 search, queue view | not started |
| post-v1 | Cover art, spectrum visualiser, playlists, config file, EQ, gapless, live rescan | not started |

---

## I. Dependencies

Third-party code is split between system libraries resolved by `pkg-config` and single-header sources vendored into `vendor/`.

| Library | Origin | Purpose | Needed by |
|---|---|---|---|
| ffmpeg ≥ 5.1 (`libavformat`, `libavcodec`, `libavutil`, `libswresample`) | system | Demux, decode, resample | M1 |
| `miniaudio` | vendored, pinned `0.11.25` | Device output (PipeWire / PulseAudio / ALSA / JACK) | M1 |
| `termbox2` | vendored, pinned `v2.5.0` | TUI rendering | M1 |
| `sqlite3` | system | Library index and FTS5 search | M2 |
| `taglib_c` | system | Tag reading during the scan | M2 |
| `chafa` | system | Cover art rendering | post-v1 |
| `kissfft` | vendored, tag `131.1.0` | Spectrum FFT | post-v1 |

**Install the system libraries and verification tooling on Fedora:**

```bash
sudo dnf install libavformat-free-devel libavcodec-free-devel libavutil-free-devel \
                 libswresample-free-devel sqlite-devel taglib-devel \
                 libasan libtsan libubsan valgrind cppcheck
```

`chafa-devel` is not installed yet — it is needed only for post-v1 cover-art work.

**Verify `pkg-config` can resolve everything:**

```bash
pkg-config --modversion libavformat libavcodec libavutil libswresample sqlite3 taglib_c
```

Expected — six version strings, no errors. A missing module prints `No package 'X' found` and names exactly what is absent.

*Note: package names differ across distributions (`libavcodec-free-devel` on Fedora, `libavcodec-dev` on Debian, `ffmpeg` on Arch). The `Makefile` never hardcodes `-l` flags or include paths — `pkg-config` is the single source of truth, so only the install command above is distro-specific.*

*Warning: install the four `lib*-free-devel` packages above, **not** `ffmpeg-free-devel`. `ffmpeg-free-devel` is a metapackage with no `pkgconfig()` provides of its own — it pulls in the four packages plus `libavdevice`, `libavfilter`, and `libswscale` devel, none of which `kurono` uses.*

*Warning: `pkgconfig(libavformat)` is also provided by RPM Fusion's `ffmpeg-devel`. **Do not install that one.** It mismatches the `ffmpeg-free` runtime libraries already on a stock Fedora system, and `-free` is what encodes the LGPL-only constraint this project relies on. If both are ever installed, `pkg-config` will resolve to whichever registered last — verify with `pkg-config --modversion libavformat` and check it reports a `-free` build.*

*Note: ffmpeg 5.1 is a hard floor. `AVChannelLayout` landed in 5.1 and replaced the older `channels` / `channel_layout` fields, and no compatibility shim is planned. This covers Fedora 38+, Debian 13, Ubuntu 24.04+, and current Arch. Ubuntu 22.04 ships ffmpeg 4.4 and will not build.*

**Vendored sources**, placed in `vendor/` and never edited — upgrades are wholesale replacements:

| File | Pin | URL |
|---|---|---|
| `vendor/miniaudio.h` | `0.11.25` | `https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h` |
| `vendor/termbox2.h` | `v2.5.0` | `https://raw.githubusercontent.com/termbox/termbox2/v2.5.0/termbox2.h` |

Every miniaudio and termbox2 API claim in `DESIGN.md` was verified against these exact revisions — do not vendor `master`.

---

## II. Build

**Clone and build:**

```bash
git clone <REPO_URL> kurono
cd kurono
make
```

**Verify:**

```bash
./kurono --version
```

### 1. Build targets

| Target | Flags | Use |
|---|---|---|
| `make` | `-g` | Default. Debug symbols, no optimization. |
| `make release` | `-O2 -DNDEBUG` | Optimized, assertions stripped. What `install` uses. |
| `make debug` | `-Og -fno-omit-frame-pointer` | Readable stack traces under a debugger. |
| `make asan` | `-fsanitize=address,undefined` | Memory error detection. |
| `make tsan` | `-fsanitize=thread` | Data race detection. The important one here. |
| `make clean` | | Remove objects and binaries. |

*Note: `CFLAGS` includes `-D_POSIX_C_SOURCE=200809L`. This is not optional — under strict `-std=c11` the POSIX `strdup` is undeclared, and gcc then assumes it returns `int`, truncating the pointer on 64-bit builds.*

*Note: every target must pass `-DTB_OPT_ATTR_W=32` when compiling the translation unit that includes `vendor/termbox2.h`. `termbox2` defaults `TB_OPT_ATTR_W` to 16, which silently drops `TB_OUTPUT_TRUECOLOR` support. `ui/tui.c` carries `_Static_assert(TB_OPT_ATTR_W == 32, ...)` so a dropped flag is a build failure, not a degraded runtime.*

---

## III. Install

Installs to `~/.local/bin` by default, which needs no `sudo`.

**Install:**

```bash
make install
```

**Verify — from a directory that is not the repo:**

```bash
cd ~ && kurono --version
```

If the shell reports `kurono: command not found`, `~/.local/bin` is not on your `PATH`. **Add it:**

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

**Uninstall:**

```bash
make uninstall
```

---

## IV. Configuration

v1 is configured entirely through environment variables. There is no config file yet.

| Variable | Default | Purpose |
|---|---|---|
| `KURONO_MUSIC_DIR` | `$XDG_MUSIC_DIR`, then `~/Music` | Root of the music tree, traversed recursively |
| `KURONO_PLAYLIST_DIR` | `$KURONO_MUSIC_DIR/playlists` | Where playlists are read from and written to |
| `KURONO_NO_SCAN` | unset | If set, skip the startup scan entirely |
| `KURONO_CONFIG` | `$XDG_CONFIG_HOME/kurono/config.toml` | Reserved. Unused in v1. |

**Point it at your library:**

```bash
export KURONO_MUSIC_DIR="$HOME/Music"
```

The index is written to `$XDG_CACHE_HOME/kurono/library.db`, falling back to `~/.cache/kurono/library.db`. It is never written inside the music tree — the music directory is treated as read-only, with the playlist directory as the sole exception.

**Verify the cache location is writable:**

```bash
mkdir -p "${XDG_CACHE_HOME:-$HOME/.cache}/kurono" && echo ok
```

---

## V. Usage

On first launch, `kurono` scans the music tree and builds the index, printing progress to `stderr` before the interface starts. Subsequent launches compare `mtime` and size per file and skip unchanged tracks, making rescans effectively instant.

### 1. Layout

```txt
+--------------------------------------------------------------+
| header: now playing / mode indicators                        |
+---------------+----------------+-----------------------------+
| column 1      | column 2       | column 3                    |
| artists       | albums         | tracks                      |
| (or dirs)     | (or entries)   | (or preview)                |
+---------------+----------------+-----------------------------+
| status bar: progress, time, volume, shuffle, repeat          |
+--------------------------------------------------------------+
```

Directory, Artists, and Album Artists are the same browser widget fed by different queries, not separate screens.

*Note: a crash (`SIGSEGV`, `SIGABRT`, a failed assertion) can leave the terminal in raw mode with the alternate screen active — this is a known, accepted tradeoff (see `DESIGN.md` §IV.9.c), not a bug to report. Run `reset` to restore the terminal.*

### 2. Default keybindings

Vim-like. Every binding resolves through a table to an action, so a config file can remap all of them later without touching the input path.

| Key | Action |
|---|---|
| `h` `j` `k` `l` | Move between columns and entries |
| `space` | Play / pause |
| `enter` | Play the selection now |
| `a` | Add the selection to the queue |
| `d` | Remove from the queue |
| `n` / `p` | Next / previous track |
| `+` / `-` | Volume up / down |
| `/` | Search |
| `s` | Toggle shuffle |
| `r` | Toggle repeat |
| `tab` | Cycle view |
| `q` | Quit |

---

## VI. Terminal support

Cover art rendering (post-v1) is chosen by runtime terminal query, not by `$TERM`.

| Terminal | Kitty graphics | Sixel | Renderer used |
|---|---|---|---|
| kitty | yes | no | kitty protocol |
| ghostty | yes | yes | kitty protocol |
| Konsole | no | yes | sixel |
| foot | no | yes | sixel |
| alacritty | no | no | halfblock |

Halfblock is always available and is the fallback whenever detection is inconclusive.

*Warning: both graphics protocols need passthrough sequences under `tmux` and behave inconsistently there. When `tmux` is detected, `kurono` drops to halfblock.*

---

## VII. Project layout

```txt
kurono/
├── src/
│   ├── main.c              startup, config load, thread spawn, teardown
│   ├── event.c             epoll loop over tty/signal/timer/engine fds
│   ├── config.c            env var resolution, XDG paths
│   ├── queue.c             playback queue: order, shuffle, repeat
│   ├── audio/              engine, ring buffer, decoder, sink
│   ├── library/            sqlite index, filesystem scan, queries
│   └── ui/                 tui, theme, keymap, browser, queue view, status bar
├── include/                mirrors src/, one header per source file
├── vendor/                 single-header third-party, never edited
├── Makefile
├── DESIGN.md               thread model, ownership, schema, interfaces
└── CLAUDE.md               AI-assistant working context
```

Read `DESIGN.md` before changing anything in `src/`. The audio-callback contract and the seek handshake are not evident from the headers alone.

---

## VIII. Scope

Deliberately absent, and not planned:

* **No MPD protocol, no daemon, no client/server split.** Single process.
* **No mouse support.**
* **No network features** — no streaming, no scrobbling, no online metadata lookup.
* **No tag editing.** The music tree is read-only apart from the playlist directory.
* **No transcoding.**
* **Linux only.** `epoll`, `inotify`, `signalfd`, and `timerfd` are used directly, with no portable fallback.
