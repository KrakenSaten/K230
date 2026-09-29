#!/usr/bin/env python3
"""Generate the DOORS shell's runtime art from the approved visual package.

    gen_doors_ui.py [--check] [--only backgrounds|icons|glyphs] [--compare]

Sources (never shipped): docs/design/brand/doors-visual-pack-v1 (the B
package: backgrounds, portal icon masters, system glyphs), the Doors icon
masters in docs/design/brand/doors-threshold and doors-icon-extension, and
the first-party app icons in docs/design/doors-app-icons.

Outputs, all committed so a build needs neither this script nor the sources:

  ui/assets/doors/bg-<screen>-<orientation>.bin
      lock, open and home backgrounds, 568 x 1232 and 1232 x 568, as LVGL 9
      image files: the 12-byte lv_image_header_t, then RGB565 little endian.
      That is the panel's own format (LV_COLOR_DEPTH 16), so the shell reads
      one into memory and draws it with no decoder and no conversion. The
      static scrims of the package's ui-layers are baked in (they are
      decoration, not state), and the result is Floyd-Steinberg dithered to
      565 so the dark gradients do not band on the AMOLED.
  ui/assets/doors/icon-<app>.bin, icon-frame.bin
      the B portal icon for every launcher app, ICON_PX square, RGB565A8.
      Drawn from vector: the package's portal frame (its app-icon SVG, less
      the glyph) with the app's glyph in its colour. Apps the package has a
      glyph for use it; the others use their Doors icon from the earlier
      packages, redrawn in the B line weight. icon-frame.bin is the empty
      portal the shell puts an app's own mask on when it has no icon here.
  ui/assets/doors/MANIFEST.txt
      every output with its SHA-256 and the SHA-256 of what it was made from;
      tests/doors_ui_assets_test.sh holds the two together.
  ui/pocketui/pos_glyphs.c / .h
      the B system glyphs (32 px) as A8 masks, compiled in: the shell tints
      them, and they are small enough that a missing file must never be a
      reason for a missing lock or power symbol.

--check regenerates into memory and compares with what is committed.
--compare prints how far the drawn portal icons and 32 px glyphs are from
the package's own PNG exports of the same art (a check of the renderer, not
of the art). docs/design/doors-glyphs holds the glyphs the package lacks.
Standard library only.
"""
import hashlib
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.dont_write_bytecode = True          # a build must leave no __pycache__ in the tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svgraster as sr  # noqa: E402
from pngstrict import read_png  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PACK = "docs/design/brand/doors-visual-pack-v1"
THRESHOLD = "docs/design/brand/doors-threshold/icons/svg/"
EXTENSION = "docs/design/brand/doors-icon-extension/svg/"
FIRST_PARTY = "docs/design/doors-app-icons/svg/"
OUT = "ui/assets/doors"

ICON_PX = 96
LV_MAGIC, CF_RGB565, CF_RGB565A8, CF_A8 = 0x19, 0x12, 0x14, 0x0E

# The package's colours (b_ui_layout.json) and one per launcher app. The
# shell's registry (ui/shell/home.c) carries the same colour for the focus
# mark; tests/doors_ui_assets_test.sh checks the two agree.
PALETTE = {"radio": "#b5cfa5", "mesh": "#a6c4da", "network": "#9fbdd5", "tools": "#dcb387",
           "ai": "#b9afd4", "games": "#dfad85", "settings": "#d5d7d5", "files": "#d7b78a",
           "apps": "#e5e2d4"}
B_GLYPH = PACK + "/source/system-icons/svg/"
# app id: (glyph source, palette colour)
APP_ICONS = {
    "rift": (B_GLYPH + "mesh.svg", "mesh"),
    "radio": (B_GLYPH + "radio.svg", "radio"),
    "wave": (EXTENSION + "wave.svg", "network"),
    "notes": (THRESHOLD + "notes.svg", "files"),
    "calendar": (THRESHOLD + "calendar.svg", "tools"),
    "clock": (THRESHOLD + "clock.svg", "ai"),
    "calculator": (THRESHOLD + "calculator.svg", "apps"),
    "fleet": (THRESHOLD + "fleet.svg", "games"),
    "radar": (THRESHOLD + "radar.svg", "radio"),
    "timber": (THRESHOLD + "timber.svg", "files"),
    "settings": (B_GLYPH + "settings.svg", "settings"),
    "system": (THRESHOLD + "system.svg", "apps"),
    "files": (B_GLYPH + "files.svg", "files"),
    "camera": (EXTENSION + "camera.svg", "tools"),
    "recorder": (EXTENSION + "recorder.svg", "tools"),
    "zabbix": (FIRST_PARTY + "zabbix.svg", "tools"),
    "browser": (FIRST_PARTY + "browser.svg", "network"),
    "vision": (FIRST_PARTY + "vision.svg", "ai"),
    # Not an app: the GAMES folder's cell (ui/shell/home_layout.h, folders).
    "games": (FIRST_PARTY + "games.svg", "games"),
    "solitaire": (FIRST_PARTY + "solitaire.svg", "games"),
    "blackjack": (FIRST_PARTY + "blackjack.svg", "games"),
    "2048": (FIRST_PARTY + "2048.svg", "games"),
}
# The system glyphs the shell uses, from the package's 32 px exports.
GLYPHS = ["lock", "power", "wifi", "radio", "sun", "display", "info", "settings", "apps",
          "network", "mesh", "sound"]
# Glyphs the package does not have, drawn first-party in its line language
# (docs/design/doors-glyphs/README.md) and rasterised here at 32 px by the
# renderer --compare checks against the package's own 32 px exports.
DRAWN = "docs/design/doors-glyphs/"
DRAWN_GLYPHS = {"mode": DRAWN + "mode.svg"}

# Backgrounds, and the scrims their ui-layers put over them (reference/ui-layers).
# (x, y, w, h, colour, opacity) in the orientation's own pixels.
BACKGROUNDS = {
    ("lock", "portrait"): [(0, 0, 568, 82, "#080a0d", 0.26)],
    ("lock", "landscape"): [(0, 0, 1232, 82, "#080a0d", 0.26)],
    ("open", "portrait"): [(0, 0, 568, 82, "#080a0d", 0.26), (0, 1162, 568, 70, "#080a0d", 0.28)],
    ("open", "landscape"): [(0, 0, 1232, 82, "#080a0d", 0.26), (0, 498, 1232, 70, "#080a0d", 0.28)],
    ("home", "portrait"): [(0, 0, 568, 1232, "#0a1014", 0.24)],
    ("home", "landscape"): [(0, 0, 1232, 568, "#0a1014", 0.24)],
}
BG_SOURCE = {"lock": "lock", "open": "open", "home": "launcher"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def lv_header(cf, w, h, stride):
    return struct.pack("<BBHHHHH", LV_MAGIC, cf, 0, w, h, stride, 0)


# ---- vector sources --------------------------------------------------------

def load_svg(path, force_width=None):
    """Stroke subpaths of an icon SVG, in its user units, and its stroke width.

    Every drawable element is taken as stroked (the icon families are line
    art: fill="none" on the group). Only the transform on the group is
    applied; the icons carry no other transform."""
    tree = ET.parse(str(ROOT / path))
    ns = "{http://www.w3.org/2000/svg}"
    subpaths = []
    width = None

    def walk(node, sx, tx, ty):
        nonlocal width
        tf = node.get("transform")
        if tf:
            t = tf.replace(",", " ")
            if "translate(" in t:
                a = t.split("translate(")[1].split(")")[0].split()
                tx, ty = tx + float(a[0]) * sx, ty + float(a[1]) * sx
            if "scale(" in t:
                sx *= float(t.split("scale(")[1].split(")")[0].split()[0])
        if node.get("stroke-width"):
            width = float(node.get("stroke-width"))
        tag = node.tag.replace(ns, "")
        f = lambda k: float(node.get(k, "0"))  # noqa: E731
        sp = None
        if tag == "path":
            sp = sr.parse_path(node.get("d"))
        elif tag == "rect":
            sp = sr.rect_path(f("x"), f("y"), f("width"), f("height"), f("rx"))
        elif tag == "circle":
            sp = sr.ellipse_path(f("cx"), f("cy"), f("r"), f("r"))
        elif tag == "ellipse":
            sp = sr.ellipse_path(f("cx"), f("cy"), f("rx"), f("ry"))
        if sp:
            subpaths.extend(sr.transform(sp, sx, tx, ty))
        for child in node:
            walk(child, sx, tx, ty)

    walk(tree.getroot(), 1.0, 0.0, 0.0)
    return subpaths, (force_width or width or 1.0)


def glyph_in_portal(path):
    """The glyph's subpaths in the 128-unit portal design space.

    B glyphs (viewBox 0 -6 48 48) sit at translate(40,46), exactly as the
    package's app icons place them. The Doors icons (24-unit, drawn for
    1.5 px at 24 px) are scaled into the same box, centred on the B glyph
    box's centre, and given the B stroke width."""
    if path.startswith(B_GLYPH):
        sp, _ = load_svg(path)
        return sr.transform(sp, 1.0, 40.0, 46.0), 2.0
    sp, _ = load_svg(path)
    s = 1.8
    return sr.transform(sp, s, 64.0 - 12.0 * s, 63.0 - 12.0 * s), 2.0


def portal(px, colour_hex, glyph=None):
    """One portal icon at px x px; the geometry and colours of the package's
    source/app-icons/svg (identical in all nine but for the colour)."""
    k = px / 128.0
    c = sr.Canvas(px, px)
    col = sr.parse_color(colour_hex)
    P = lambda sp: sr.transform(sp, k, 0.0, 0.0)  # noqa: E731
    outer = P(sr.parse_path("M22 9 H106 V119 H22 Z"))
    inner = P(sr.parse_path("M32 20 H96 V110 H32 Z"))
    c.fill(outer, sr.gradient([(0.0, sr.parse_color("#8b8e88")), (0.36, sr.parse_color("#363b3c")),
                               (1.0, sr.parse_color("#171b1d"))], 0, 0, 1, 1,
                              bbox=(22 * k, 9 * k, 84 * k, 110 * k)))
    c.stroke(outer, 1 * k, sr.solid(sr.parse_color("#707671")))
    c.fill(inner, sr.gradient([(0.0, sr.parse_color("#0d1115")), (1.0, sr.parse_color("#242a2d"))],
                              0, 0, 0, 1, bbox=(32 * k, 20 * k, 64 * k, 90 * k)))
    c.stroke(inner, 2 * k, sr.solid(sr.parse_color("#07090b")))
    c.stroke(P(sr.parse_path("M22 9 L32 20 M106 9 L96 20 M22 119 L32 110 M106 119 L96 110")), 1 * k,
             sr.solid(sr.parse_color("#a2a59c")), 0.42)
    c.fill(P(sr.parse_path("M32 20 L38 25 V105 L32 110")), sr.solid(sr.parse_color("#343b3d")))
    c.stroke(P(sr.parse_path("M37 25 V104")), 1 * k, sr.solid(col), 0.4)
    if glyph:
        sp, w = glyph
        c.stroke(P(sp), w * k, sr.solid(col))
    return c.rgba()


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def icon_bin(rgba, px):
    colour = bytearray()
    alpha = bytearray()
    for r, g, b, a in rgba:
        colour += struct.pack("<H", rgb565(r, g, b) if a else 0)
        alpha.append(a)
    return lv_header(CF_RGB565A8, px, px, px * 2) + bytes(colour) + bytes(alpha)


# ---- backgrounds -----------------------------------------------------------

def background_bin(src, scrims):
    png = read_png(str(ROOT / src))
    w, h = png.width, png.height
    buf = png.rgba
    # Straight float RGB, with the scrims composited over it.
    px = [[float(buf[i]), float(buf[i + 1]), float(buf[i + 2])] for i in range(0, len(buf), 4)]
    for x0, y0, sw, sh, colour, op in scrims:
        cr = sr.parse_color(colour)
        for y in range(y0, min(h, y0 + sh)):
            for x in range(x0, min(w, x0 + sw)):
                p = px[y * w + x]
                for k in range(3):
                    p[k] = p[k] * (1 - op) + cr[k] * op
    # Floyd-Steinberg to 5-6-5, serpentine, deterministic.
    out = bytearray(w * h * 2)
    levels = (31, 63, 31)
    for y in range(h):
        xs = range(w) if y % 2 == 0 else range(w - 1, -1, -1)
        step = 1 if y % 2 == 0 else -1
        for x in xs:
            p = px[y * w + x]
            q = []
            for k in range(3):
                v = min(255.0, max(0.0, p[k]))
                n = int(v * levels[k] / 255.0 + 0.5)
                q.append(n)
                err = v - n * 255.0 / levels[k]
                if err:
                    nx = x + step
                    if 0 <= nx < w:
                        px[y * w + nx][k] += err * 7 / 16
                    if y + 1 < h:
                        if 0 <= x - step < w:
                            px[(y + 1) * w + x - step][k] += err * 3 / 16
                        px[(y + 1) * w + x][k] += err * 5 / 16
                        if 0 <= nx < w:
                            px[(y + 1) * w + nx][k] += err * 1 / 16
            struct.pack_into("<H", out, (y * w + x) * 2, (q[0] << 11) | (q[1] << 5) | q[2])
    return lv_header(CF_RGB565, w, h, w * 2) + bytes(out)


# ---- glyphs ------------------------------------------------------------------

def drawn_glyph_alpha(path, px=32):
    """A B-language glyph SVG (viewBox 0 -6 48 48) as px x px alpha.

    Unlike load_svg, an element may be filled: fill on the element draws its
    shape filled, stroke="none" on it leaves the group's stroke off."""
    tree = ET.parse(str(ROOT / path))
    ns = "{http://www.w3.org/2000/svg}"
    k = px / 48.0
    c = sr.Canvas(px, px)
    white = sr.solid((255, 255, 255))

    def walk(node, stroke, width):
        stroke = node.get("stroke", stroke)
        width = float(node.get("stroke-width", width))
        tag = node.tag.replace(ns, "")
        f = lambda a: float(node.get(a, "0"))  # noqa: E731
        sp = None
        if tag == "path":
            sp = sr.parse_path(node.get("d"))
        elif tag == "rect":
            sp = sr.rect_path(f("x"), f("y"), f("width"), f("height"), f("rx"))
        elif tag == "circle":
            sp = sr.ellipse_path(f("cx"), f("cy"), f("r"), f("r"))
        if sp:
            sp = sr.transform(sp, k, 0.0, 6.0 * k)
            if node.get("fill", "none") != "none":
                c.fill(sp, white)
            if stroke != "none":
                c.stroke(sp, width * k, white)
        for child in node:
            walk(child, stroke, width)

    walk(tree.getroot(), "none", 1.0)
    return bytes(int(p[3] * 255 + 0.5) for p in c.px)


def glyphs_c():
    lines = ["/* Generated by tools/design/gen_doors_ui.py. Do not edit; regenerate.",
             " *",
             " * The DOORS B system glyphs as LVGL A8 masks, 32 x 32, the whole canvas,",
             " * alpha exactly as exported. Colourless: the style's image_recolor draws",
             " * them. Sources (%s/device/system-icons/png32/):" % PACK, " *"]
    body = []
    header = ["/* Generated by tools/design/gen_doors_ui.py. Do not edit; regenerate. */",
              "#ifndef POS_GLYPHS_H", "#define POS_GLYPHS_H", "",
              "#ifdef LV_LVGL_H_INCLUDE_SIMPLE", '#include "lvgl.h"', "#else", '#include "lvgl/lvgl.h"',
              "#endif", "", "/* DOORS B system glyphs, A8 32 x 32 (pos_glyphs.c). */"]
    sources = [(name, None) for name in GLYPHS] + sorted(DRAWN_GLYPHS.items())
    for name, drawn in sources:
        if drawn:
            alpha = drawn_glyph_alpha(drawn)
            lines.append(" *   %s: %s sha256 %s (first-party, drawn)" % (name, drawn,
                                                                     sha((ROOT / drawn).read_bytes())))
        else:
            path = ROOT / PACK / "device/system-icons/png32" / (name + ".png")
            png = read_png(str(path))
            if (png.width, png.height) != (32, 32) or png.colour_type != 6:
                raise ValueError("%s: not a 32 x 32 RGBA glyph" % path)
            alpha = png.rgba[3::4]
            lines.append(" *   %s.png sha256 %s" % (name, sha(path.read_bytes())))
        sym = "pos_glyph_" + name
        body += ["", "static const uint8_t %s_map[] LV_ATTRIBUTE_MEM_ALIGN = {" % sym]
        for y in range(32):
            body.append("    " + ",".join("0x%02x" % v for v in alpha[y * 32:(y + 1) * 32]) + ",")
        body += ["};", "", "const lv_image_dsc_t %s = {" % sym,
                 "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
                 "    .header.cf = LV_COLOR_FORMAT_A8,",
                 "    .header.flags = 0,",
                 "    .header.w = 32,", "    .header.h = 32,", "    .header.stride = 32,",
                 "    .data_size = sizeof(%s_map)," % sym, "    .data = %s_map," % sym, "};"]
        header.append("extern const lv_image_dsc_t %s;" % sym)
    lines += [" */", "#ifdef LV_LVGL_H_INCLUDE_SIMPLE", '#include "lvgl.h"', "#else",
              '#include "lvgl/lvgl.h"', "#endif"] + body
    header += ["", "#endif", ""]
    return ("\n".join(lines) + "\n").encode(), "\n".join(header).encode()


# ---- main --------------------------------------------------------------------

def build(only=None):
    """{relative path: bytes}, and the manifest rows."""
    files = {}
    rows = []
    if only in (None, "icons"):
        frame = portal(ICON_PX, PALETTE["apps"])
        files[OUT + "/icon-frame.bin"] = icon_bin(frame, ICON_PX)
        rows.append((OUT + "/icon-frame.bin", PACK + "/source/app-icons/svg/apps.svg (frame only)"))
        for app, (src, colour) in sorted(APP_ICONS.items()):
            rgba = portal(ICON_PX, PALETTE[colour], glyph_in_portal(src))
            name = OUT + "/icon-%s.bin" % app
            files[name] = icon_bin(rgba, ICON_PX)
            rows.append((name, src))
    if only in (None, "backgrounds"):
        for (screen, orient), scrims in sorted(BACKGROUNDS.items()):
            src = "%s/device/backgrounds/%s/%s.png" % (PACK, orient, BG_SOURCE[screen])
            name = "%s/bg-%s-%s.bin" % (OUT, screen, orient)
            files[name] = background_bin(src, scrims)
            rows.append((name, src))
    if only in (None, "glyphs"):
        c, h = glyphs_c()
        files["ui/pocketui/pos_glyphs.c"] = c
        files["ui/pocketui/pos_glyphs.h"] = h
    return files, rows


def manifest(files, rows):
    lines = ["# DOORS runtime art, generated by tools/design/gen_doors_ui.py. Do not edit.",
             "# <sha256 of output>  <output>  <sha256 of source>  <source>"]
    for out, src in rows:
        src_path = src.split(" ")[0]
        lines.append("%s  %s  %s  %s" % (sha(files[out]), out, sha((ROOT / src_path).read_bytes()), src))
    return ("\n".join(lines) + "\n").encode()


def compare():
    """Mean absolute error of the drawn icons against the package's PNG exports."""
    for name in ("sun", "display", "lock", "info"):
        ref = read_png(str(ROOT / PACK / ("device/system-icons/png32/%s.png" % name))).rgba[3::4]
        mine = drawn_glyph_alpha(B_GLYPH + name + ".svg")
        err = sum(abs(a - b) for a, b in zip(mine, ref)) / len(ref)
        print("glyph %-7s 32 px: mean |alpha error| %.2f (of 255), worst %d"
              % (name, err, max(abs(a - b) for a, b in zip(mine, ref))))
    for app in ("radio", "settings"):
        for px in (96, 128):
            ref = read_png(str(ROOT / PACK / ("device/app-icons/png%d/%s.png" % (px, app))))
            src = B_GLYPH + app + ".svg"
            mine = portal(px, PALETTE[app], glyph_in_portal(src))
            err = [0, 0, 0, 0]
            for i, p in enumerate(mine):
                q = ref.rgba[i * 4:i * 4 + 4]
                a = q[3] / 255
                for k in range(3):
                    err[k] += abs(p[k] * p[3] / 255 - q[k] * a)
                err[3] += abs(p[3] - q[3])
            n = px * px
            print("%-9s %3d px: mean |error| R %.2f G %.2f B %.2f A %.2f (of 255)"
                  % (app, px, err[0] / n, err[1] / n, err[2] / n, err[3] / n))


def main():
    args = sys.argv[1:]
    if "--compare" in args:
        compare()
        return 0
    only = args[args.index("--only") + 1] if "--only" in args else None
    files, rows = build(only)
    if only is None:
        files[OUT + "/MANIFEST.txt"] = manifest(files, rows)
    if "--check" in args:
        bad = [p for p, data in files.items() if not (ROOT / p).is_file() or (ROOT / p).read_bytes() != data]
        for p in bad:
            print("gen_doors_ui: %s is not what its sources make; regenerate" % p, file=sys.stderr)
        return 1 if bad else 0
    for p, data in sorted(files.items()):
        dst = ROOT / p
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        print("%-44s %9d bytes" % (p, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
