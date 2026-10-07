#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Render a first-party Doors app icon to the PNGs the icon tools read.

    render_app_icon.py SVG OUTDIR

SVG is a 24-unit icon in the Doors icon extension's line language
(docs/design/doors-app-icons/README.md): viewBox 0 0 24 24, a group with
fill="none", stroke="currentColor" and stroke-width 1.5, holding <path>
elements. Written: OUTDIR/png-24/<name>.png and OUTDIR/png-32/<name>.png,
white on transparent like the extension package's own exports, the whole
24-unit canvas scaled to the size (the stroke becomes 1.5 and 2 px).
tools/design/gen_app_icons.py reads the 32 px one; gen_doors_ui.py reads the
SVG itself.

Strokes are drawn by svgraster, with round caps and joins (svgraster.py
explains why). The output is deterministic: running it again on the same SVG
gives the same bytes, which tests/app_icons_test.sh relies on. Standard
library only.
"""
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

sys.dont_write_bytecode = True          # a build must leave no __pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svgraster as sr  # noqa: E402

NS = "{http://www.w3.org/2000/svg}"


def subpaths_of(svg):
    """The icon's stroke subpaths in its 24 user units, and its stroke width."""
    root = ET.parse(svg).getroot()
    if root.get("viewBox") != "0 0 24 24":
        raise ValueError("%s: viewBox must be 0 0 24 24" % svg)
    width = None
    subpaths = []
    for node in root.iter():
        tag = node.tag.replace(NS, "")
        if node.get("stroke-width"):
            width = float(node.get("stroke-width"))
        if node.get("transform") not in (None, "translate(0 0) scale(1.0)"):
            raise ValueError("%s: only the identity transform is supported" % svg)
        if tag == "path":
            subpaths += sr.parse_path(node.get("d"))
        elif tag in ("rect", "circle", "ellipse", "line", "polyline", "polygon"):
            raise ValueError("%s: <%s> is not supported; draw it as a path" % (svg, tag))
    if width is None or not subpaths:
        raise ValueError("%s: no stroke-width or no path" % svg)
    return subpaths, width


def png(px_rgba, size):
    raw = b"".join(b"\x00" + bytes(v for p in px_rgba[y * size:(y + 1) * size] for v in p)
                   for y in range(size))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def render(svg, size):
    subpaths, width = subpaths_of(svg)
    k = size / 24.0
    canvas = sr.Canvas(size, size)
    canvas.stroke(sr.transform(subpaths, k, 0.0, 0.0), width * k, sr.solid((255, 255, 255)))
    # White under every visible pixel, as the icon tools require.
    return [(255, 255, 255, a) if a else (0, 0, 0, 0) for _, _, _, a in canvas.rgba()]


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    svg, outdir = argv[1], argv[2]
    name = os.path.splitext(os.path.basename(svg))[0]
    for size in (24, 32):
        d = os.path.join(outdir, "png-%d" % size)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name + ".png"), "wb") as f:
            f.write(png(render(svg, size), size))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
