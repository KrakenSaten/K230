#!/usr/bin/env python3
"""Turn DeskBuddy demo frames into animated PNGs (APNG).

The frames come from the app test's capture mode (tests/deskbuddy_app_test.c,
$DESKBUDDY_DEMO_DIR): the real app drawn by LVGL on the host, one PPM per
70 ms, and frames.txt with the finger for each. This script draws a ring
where the finger is pressed (an overlay added here, not part of the app),
halves the size, drops repeated frames by lengthening the one before, and
writes one APNG per orientation (frames change size when the display turns).

Standard library only.

    deskbuddy_demo.py <frames dir> <out prefix> [--stills N,N,...]

writes <out prefix>-portrait.png, <out prefix>-landscape.png and, for each
still asked for, <out prefix>-still-<N>.png.

Copyright (c) 2026 PocketOS authors.
SPDX-License-Identifier: Apache-2.0
"""
import os
import struct
import sys
import zlib

FRAME_MS = 70
RING = (200, 200, 200)  # the touch overlay; the app itself names no colour


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, bytearray(parts[3][: w * h * 3])


def ring(px, w, h, cx, cy, r=20, t=4):
    for y in range(max(0, cy - r), min(h, cy + r + 1)):
        for x in range(max(0, cx - r), min(w, cx + r + 1)):
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            if (r - t) ** 2 <= d2 <= r * r:
                i = (y * w + x) * 3
                px[i:i + 3] = bytes(RING)


def half(px, w, h):
    """Average 2 x 2 blocks."""
    w2, h2 = w // 2, h // 2
    out = bytearray(w2 * h2 * 3)
    for y in range(h2):
        r0 = (2 * y) * w * 3
        r1 = r0 + w * 3
        o = y * w2 * 3
        for x in range(w2):
            a = r0 + x * 6
            b = r1 + x * 6
            for c in range(3):
                out[o + c] = (px[a + c] + px[a + 3 + c] + px[b + c] + px[b + 3 + c]) // 4
            o += 3
    return w2, h2, out


def chunk(kind, body):
    c = kind + body
    return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)


def scanlines(px, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += px[y * w * 3:(y + 1) * w * 3]
    return zlib.compress(bytes(raw), 9)


def write_png(path, w, h, px):
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", scanlines(px, w, h)))
        f.write(chunk(b"IEND", b""))


def write_apng(path, w, h, frames):
    """frames: [(pixels, ms)], all w x h."""
    seq = 0
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"acTL", struct.pack(">II", len(frames), 0)))
        for i, (px, ms) in enumerate(frames):
            f.write(chunk(b"fcTL", struct.pack(">IIIIIHHBB", seq, w, h, 0, 0, min(ms, 65535), 1000, 0, 0)))
            seq += 1
            data = scanlines(px, w, h)
            if i == 0:
                f.write(chunk(b"IDAT", data))
            else:
                f.write(chunk(b"fdAT", struct.pack(">I", seq) + data))
                seq += 1
        f.write(chunk(b"IEND", b""))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, prefix = argv[1], argv[2]
    stills = set()
    if len(argv) > 4 and argv[3] == "--stills":
        stills = {int(v) for v in argv[4].split(",") if v}
    segments = {}
    order = []
    with open(os.path.join(src, "frames.txt")) as f:
        lines = [ln.split() for ln in f if ln.strip()]
    for n, (name, down, fx, fy) in enumerate(lines):
        w, h, px = read_ppm(os.path.join(src, name))
        if n in stills:
            write_png("%s-still-%d.png" % (prefix, n), w, h, px)
        if down == "1":
            ring(px, w, h, int(fx), int(fy))
        w2, h2, small = half(px, w, h)
        key = "portrait" if h >= w else "landscape"
        if key not in segments:
            segments[key] = (w2, h2, [])
            order.append(key)
        seg = segments[key]
        if (w2, h2) != seg[:2]:
            continue
        if seg[2] and seg[2][-1][0] == small:
            seg[2][-1] = (small, seg[2][-1][1] + FRAME_MS)
        else:
            seg[2].append((small, FRAME_MS))
    for key in order:
        w, h, frames = segments[key]
        out = "%s-%s.png" % (prefix, key)
        write_apng(out, w, h, frames)
        print("%s: %d x %d, %d distinct frames, %.1f s" % (out, w, h, len(frames), sum(ms for _, ms in frames) / 1000))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
