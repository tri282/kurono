#!/usr/bin/env python3
"""
pty_drive.py -- drive kurono's TUI from a script and capture what it drew.

Phase 11 built this ad hoc and it closed six verify rows that the phase
plan had assumed needed a human at a tty. It was not kept, and phase 12
had to write it again; it lives in tools/ now for that reason.

Why a pty and not a pipe: termbox2 calls tcgetattr/tcsetattr and asks
the terminal for its size. On a pipe there is no terminal, and a pty
created without an explicit size is 0x0 -- kurono correctly decides it
is too small and draws nothing at all, which reads exactly like a hang.
So the size is set with TIOCSWINSZ before the child ever runs.

Why select and not a plain read: an idle kurono produces no output at
all (that is the whole point of DESIGN.md VIII.3's zero-wakeup idle), so
a blocking os.read after the last keystroke never returns.

Usage:
  pty_drive.py [--size WxH] [--env K=V ...] [--send KEY[:delay_ms] ...]
               [--settle MS] [--grid | --raw | --text] -- ./kurono [args]

KEY is a literal character, or one of: up down left right enter tab esc
space pgup pgdn home end ctrl-c ctrl-d, or \\xNN for a raw byte.
"""

import argparse
import errno
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import unicodedata
import time

KEYS = {
    "up": b"\x1b[A", "down": b"\x1b[B",
    "right": b"\x1b[C", "left": b"\x1b[D",
    "pgup": b"\x1b[5~", "pgdn": b"\x1b[6~",
    "home": b"\x1b[H", "end": b"\x1b[F",
    "enter": b"\r", "tab": b"\t", "esc": b"\x1b",
    "space": b" ", "ctrl-c": b"\x03", "ctrl-d": b"\x04",
}


def keybytes(spec):
    """Resolve one --send token to the bytes to write and its delay."""
    name, _, delay = spec.partition(":")
    ms = int(delay) if delay else 120
    if name in KEYS:
        return KEYS[name], ms
    if name.startswith("\\x"):
        return bytes([int(name[2:], 16)]), ms
    if len(name) == 1:
        return name.encode(), ms
    raise SystemExit(f"pty_drive: unknown key {name!r}")


CSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
OSC = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
ESC = re.compile(rb"\x1b[()#][0-9A-Za-z]|\x1b[=><78MHc]")


def strip(buf):
    return ESC.sub(b"", OSC.sub(b"", CSI.sub(b"", buf)))


def render(buf, w, h):
    """
    Reconstruct the visible screen.

    termbox positions absolutely, so concatenating the printable runs in
    stream order gives nonsense. This walks the stream, tracks the cursor
    through CUP/CUD/CUF and erases, and paints printable text into a
    grid -- enough of a terminal to assert on what a user would see.
    """
    grid = [[" "] * w for _ in range(h)]
    # Parallel grid of background colours, so a caller can ask which row
    # carries the cursor's slot. Selection is a core browser property and
    # grepping the byte stream for it is not decisive -- fill_row paints
    # the row in the cursor background before the text is drawn, so the
    # colour and the text are not adjacent in the stream.
    bgs = [[None] * w for _ in range(h)]
    bg = None
    row = col = 0
    i = 0
    n = len(buf)
    while i < n:
        b = buf[i:i + 1]
        if b == b"\x1b":
            m = CSI.match(buf, i) or OSC.match(buf, i) or ESC.match(buf, i)
            if not m:
                i += 1
                continue
            seq = m.group(0)
            i = m.end()
            if seq.endswith(b"m"):
                body = seq[2:-1]
                if body in (b"", b"0"):
                    bg = None
                else:
                    mm = re.search(rb"48;2;(\d+);(\d+);(\d+)", body)
                    if mm:
                        bg = tuple(int(g) for g in mm.groups())
            elif seq.endswith(b"H") or seq.endswith(b"f"):
                nums = seq[2:-1].split(b";")
                try:
                    row = int(nums[0] or 1) - 1
                    col = int(nums[1] or 1) - 1 if len(nums) > 1 else 0
                except ValueError:
                    row = col = 0
            elif seq.endswith(b"J"):
                grid = [[" "] * w for _ in range(h)]
                row = col = 0
            elif seq.endswith(b"K"):
                if 0 <= row < h:
                    for c in range(col, w):
                        grid[row][c] = " "
            continue
        if b == b"\r":
            col = 0
            i += 1
            continue
        if b == b"\n":
            row += 1
            col = 0
            i += 1
            continue
        ln = 1
        if b[0] >= 0xF0:
            ln = 4
        elif b[0] >= 0xE0:
            ln = 3
        elif b[0] >= 0xC0:
            ln = 2
        ch = buf[i:i + ln].decode("utf-8", "replace")
        # A real terminal advances two columns for East Asian Wide and
        # Fullwidth characters. Modelling that is what makes the grid
        # show the overlap a CJK filename actually causes, rather than
        # hiding it behind one-cell-per-codepoint bookkeeping.
        cw = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
        if 0 <= row < h and 0 <= col < w:
            grid[row][col] = ch
            bgs[row][col] = bg
            if cw == 2 and col + 1 < w:
                grid[row][col + 1] = ""
        col += cw
        i += ln
    return ["".join(r).rstrip() for r in grid], bgs


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--size", default="90x24")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--send", action="append", default=[])
    ap.add_argument("--settle", type=int, default=400)
    ap.add_argument("--grid", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--text", action="store_true")
    ap.add_argument("--snap", type=int, default=None,
                    help="which step to render: 0 = after startup, "
                         "k = after the k'th --send. Default is the state "
                         "just before the last key.")
    ap.add_argument("--mark-bg", default=None,
                    help="R,G,B -- prefix rows whose background "
                         "uses this colour with '<=='. The cursor "
                         "slot is 224,175,104 in the default theme.")
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("-h", "--help", action="store_true")
    args, rest = ap.parse_known_args()

    if args.help or not rest:
        print(__doc__)
        return 0
    if rest and rest[0] == "--":
        rest = rest[1:]

    w, _, hh = args.size.partition("x")
    w, h = int(w), int(hh)

    env = dict(os.environ)
    env["TERM"] = "xterm-256color"
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe(rest[0], rest, env)
        os._exit(127)

    # Before anything is drawn: a 0x0 pty is "terminal too small".
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", h, w, 0, 0))

    out = bytearray()
    deadline = time.time() + args.timeout

    def pump(ms):
        end = time.time() + ms / 1000.0
        while time.time() < end:
            if time.time() > deadline:
                return
            r, _, _ = select.select([fd], [], [], 0.05)
            if fd in r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError as e:
                    if e.errno in (errno.EIO, errno.EBADF):
                        return
                    raise
                if not chunk:
                    return
                out.extend(chunk)

    # The stream is replayed to reconstruct the screen, so rendering all
    # of it shows the state after teardown -- termbox leaves the alt
    # screen and clears on the way out, which looks like "nothing was
    # drawn". Mark the byte offset after each step so a snapshot can be
    # taken at the point of interest instead of at the end.
    marks = []
    pump(args.settle)
    marks.append(len(out))
    for spec in args.send:
        data, ms = keybytes(spec)
        try:
            os.write(fd, data)
        except OSError:
            break
        pump(ms)
        marks.append(len(out))
    pump(args.settle)
    marks.append(len(out))

    status = None
    try:
        os.close(fd)
    except OSError:
        pass
    for _ in range(50):
        wpid, st = os.waitpid(pid, os.WNOHANG)
        if wpid:
            status = st
            break
        time.sleep(0.05)
    if status is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        print("pty_drive: child did not exit; SIGKILLed", file=sys.stderr)
        status = -1

    # Default snapshot is the state just BEFORE the last key, which is
    # normally the quit -- i.e. the screen as the user last saw it.
    # marks is [after startup, after send[0], ..., after send[n-1], after
    # the final settle]. The state before the last key is therefore
    # marks[len(send) - 1], which for a bare "--send q" is the startup
    # screen -- the one the user was looking at when they quit.
    if args.snap is not None:
        cut = marks[max(0, min(args.snap, len(marks) - 1))]
    elif args.send:
        cut = marks[max(0, len(args.send) - 1)]
    else:
        cut = marks[0]

    if args.raw:
        sys.stdout.write(repr(bytes(out)) + "\n")
    elif args.text:
        sys.stdout.write(strip(bytes(out)).decode("utf-8", "replace"))
    else:
        lines, bgs = render(bytes(out[:cut]), w, h)
        mark = None
        if args.mark_bg:
            mark = tuple(int(v) for v in args.mark_bg.split(","))
        for y, line in enumerate(lines):
            flag = ""
            if mark is not None and any(c == mark for c in bgs[y]):
                flag = "<== "
            print(f"{flag}{line}")

    code = os.waitstatus_to_exitcode(status) if status not in (None, -1) else 1
    print(f"--- exit {code} ---", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
