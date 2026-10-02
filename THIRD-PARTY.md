# Third-party components

`kurono` is Copyright (C) 2026 Tri Minh Do and is licensed under
GPL-3.0-or-later (see `LICENSE`). It builds on the
components below, whose own terms are unaffected by that choice and are
reproduced or referenced here as those terms require.

---

## Vendored, compiled into the binary

These live in `vendor/` as single headers and are never edited. Upgrades are
wholesale replacements at a new pin.

### `vendor/miniaudio.h` - 0.11.25

Audio device output. Dual-licensed: **public domain (Unlicense) or MIT-0**, at
the user's choice. Both are compatible with GPL-3.0.

Copyright 2026 David Reid (MIT-0 alternative; the Unlicense alternative
dedicates the work to the public domain).

Upstream: <https://github.com/mackron/miniaudio>
Full terms: the licensing block at the end of `vendor/miniaudio.h`.

### `vendor/termbox2.h` - v2.5.0

Terminal rendering. **MIT.**

Copyright (c) 2010-2020 nsf <no.smile.face@gmail.com>
              2015-2024 Adam Saponara <as@php.net>

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

Upstream: <https://github.com/termbox/termbox2>
Full terms: the header block at the top of `vendor/termbox2.h`.

---

## System libraries - dynamically linked

Resolved by `pkg-config` at build time and not distributed with this source.

| Library | License |
|---|---|
| ffmpeg — `libavformat`, `libavcodec`, `libavutil`, `libswresample` | LGPL-2.1-or-later, or GPL when built `--enable-gpl` (see below) |
| TagLib (`taglib_c`) | LGPL-2.1-only **or** MPL-1.1 |
| SQLite 3 | public domain |

**On the ffmpeg build.** ffmpeg's libraries are LGPL-2.1-or-later by default
and GPL when the build is configured `--enable-gpl`. Distributions differ:
Fedora's `libav*-free` packages are configured `--enable-gpl --enable-version3`
and report `GPL version 3 or later`. Either licence is compatible with
`kurono`'s GPL-3.0-or-later, and `kurono` uses no GPL-only ffmpeg component. A
`--enable-nonfree` build is not redistributable and must not be used. The
README's install section steers Fedora users away from RPM Fusion's
`ffmpeg-devel` for an ABI reason, not a licensing one.

**On LGPL compatibility with GPL-3.0.** Both LGPL-2.1-or-later and
LGPL-2.1-only permit conveying the covered work under the GNU GPL (LGPL-2.1
section 3), so linking them into a GPL-3.0 program is permitted. A GPL-3.0-or-later
ffmpeg is compatible directly.

**On static linking.** `kurono` links these dynamically, which satisfies
LGPL-2.1 section 6 without further obligation. **A static build changes that:**
distributing a statically linked binary requires also providing whatever is
needed to relink against a modified version of the LGPL library — for a
GPL-3.0 program the published complete source discharges this, but the binary
must still say so. Do not ship a static release artifact without revisiting
this paragraph.
