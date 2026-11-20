#!/usr/bin/env python3
"""Check that Camera.app's preview shows fresh frames, not a loop of old ones.

The camera test pattern (boot/common/cam_pattern.h) has a grey patch in the
middle of every frame whose brightness is a frame counter (Y = 40 + n mod 160).
With Camera.app showing the preview in a running QEMU session (emu/session.py),
this takes screenshots for --seconds and reads the patch at the preview's
centre. Fresh frames make the value climb (and wrap from ~199 to ~40 once per
160 frames); a preview that loops over a few old frames steps back by less
than a wrap, a frozen one never moves.

  python3 tools/ios3ds-camera/tests/preview_counter.py [--seconds 10] [--x 120 --y 160]
  python3 tools/ios3ds-camera/tests/preview_counter.py --selftest

Exit 0 = PASS. Stdlib only.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
WRAP = 80          # a step back by more than this is the counter's wrap (160 levels)
NOISE = 3          # scaling/filtering noise allowed on a step back


def read_png(path):
    """8-bit RGB/RGBA PNG -> (w, h, channels, rows as bytes)."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, b"", 0
    while pos < len(d):
        n, typ = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError("unsupported PNG (depth %d type %d)" % (depth, ctype))
            ch = 3 if ctype == 2 else 4
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride, rows, prev = zlib.decompress(idat), w * ch, [], bytearray(w * ch)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b, c = prev[i], prev[i - ch] if i >= ch else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, ch, rows


def patch_level(path, cx, cy, r=4):
    """Mean grey of a (2r+1)^2 square at (cx, cy); None if it is not grey."""
    w, h, ch, rows = read_png(path)
    tot, n = 0, 0
    for y in range(max(0, cy - r), min(h, cy + r + 1)):
        for x in range(max(0, cx - r), min(w, cx + r + 1)):
            px = rows[y][x * ch:x * ch + 3]
            if max(px) - min(px) > 12:
                return None
            tot += sum(px) / 3
            n += 1
    return tot / n if n else None


def verdict(levels):
    """levels: patch values in time order -> (ok, explanation)."""
    vals = [v for v in levels if v is not None]
    if len(vals) < 5:
        return False, "only %d readable samples (is the preview on screen, pattern camera?)" % len(vals)
    back, moved, progress = [], 0, 0.0
    for a, b in zip(vals, vals[1:]):
        d = b - a
        if d < -WRAP:                          # the counter wrapped: still forward
            progress += 160 + d
            moved += 1
        elif d < -NOISE:
            back.append(round(d, 1))
        elif d >= 1:
            moved += 1
            progress += d
    if back:
        return False, "steps back by %s: the preview repeats old frames" % back
    if progress < 5 or moved < 3:
        return False, "the counter advanced by only %d levels in %d steps: frozen preview" % (progress, moved)
    return True, "counter advanced by ~%d levels over %d samples, never back" % (progress, len(vals))


def selftest():
    fresh = [50, 60, 71, 80, 95, 110, 130, 150, 170, 190, 45, 60]
    loop = [50, 58, 66, 52, 60, 68, 54, 62, 70, 56, 64, 72]
    frozen = [80] * 12
    assert verdict(fresh)[0], verdict(fresh)
    assert not verdict(loop)[0]
    assert not verdict(frozen)[0]
    print("preview_counter selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--x", type=int, default=120, help="preview centre in the 240x360 screenshot")
    ap.add_argument("--y", type=int, default=160)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    tmp = tempfile.mkdtemp(prefix="ios3ds-counter-")
    levels, t0, i = [], time.time(), 0
    while time.time() - t0 < a.seconds or len(levels) < 6:
        name = "counter%02d" % i
        out = subprocess.run([sys.executable, os.path.join(ROOT, "emu", "session.py"), "shot", name],
                             capture_output=True, text=True).stdout.strip().splitlines()
        path = next((tok for line in out for tok in line.split() if tok.endswith(".png")), "")
        if not os.path.isfile(path):
            path = os.path.join(tmp, name + ".png")
        lv = patch_level(path, a.x, a.y) if os.path.isfile(path) else None
        levels.append(lv)
        print("t=%5.1fs  patch %s" % (time.time() - t0, "-" if lv is None else "%.1f" % lv))
        i += 1
        if i > 60:
            break
    ok, why = verdict(levels)
    print("preview counter: %s (%s)" % ("PASS" if ok else "FAIL", why))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
