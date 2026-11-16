#!/usr/bin/env python3
"""Minimal 1:1 raster library for the PacketShark mockups.

Pure Python standard library (no Pillow, no network). Text is drawn with the
device's own LVGL bitmap fonts, parsed read-only from
<repo>/ui/pocketui/fonts/pos_font_*.c, so glyph shapes, advances and line
heights are the ones doors-shell draws. Colours are the `doors` theme's
Normal-mode tokens (DS section 32.1) and the identity hues (DS section 37.3).

Known differences from LVGL: no kerning (the Sans faces carry class kerning,
at most 1 px per pair) and corner anti-aliasing is 4x4 supersampled.
"""
import os
import re
import struct
import zlib

# The repository this file is in, three levels up (docs/design/packetshark).
REPO = os.environ.get("DOORS_REPO",
                      os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")))
FONT_DIR = os.path.join(REPO, "ui", "pocketui", "fonts")


def rgb(h):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255)


def mix(a, b, t):
    """pos_mix(): per-channel interpolation from a toward b, rounded half-up."""
    return tuple(int(a[i] + (b[i] - a[i]) * t + 0.5) for i in range(3))


# ---- tokens: theme `doors`, Normal mode (DS 32.1, derived by DS 4) ----------
T = {
    "bg": rgb(0x0B0B0A), "surface": rgb(0x161513), "surface_raised": rgb(0x211F1C),
    "line": rgb(0x36332E), "text_primary": rgb(0xEEEAE2), "text_secondary": rgb(0xA8A399),
    "accent_primary": rgb(0xD8BF94), "accent_secondary": rgb(0xB9B3A6),
    "status_ok": rgb(0x57C785), "status_warn": rgb(0xE3B341), "status_error": rgb(0xE5534B),
}
T["text_muted"] = mix(T["text_secondary"], T["bg"], 0.20)
T["text_on_accent"] = T["bg"]
T["disabled_fg"] = mix(T["text_secondary"], T["bg"], 0.50)
T["focus"] = T["accent_primary"]

# ---- identity hues (pos_theme.c identity_hues[], DS 37.3), by index ---------
IDENT = [rgb(v) for v in (0xF2917F, 0xE8AC6A, 0xD9CB6E, 0x9FD08A,
                          0x74D1C4, 0x86BFF0, 0xB7A8F2, 0xE89DD2)]

# ---- PacketShark protocol classes (proposed DS 59.2) ------------------------
# name, identity index (None = neutral text_secondary), stacking order = list order
CLASSES = [("TCP", 6), ("UDP", 5), ("DNS", 4), ("DHCP", 1),
           ("ARP", 2), ("ICMP", 7), ("IPV6", 3), ("OTHER", None)]


def class_rgb(i):
    idx = CLASSES[i][1]
    return T["text_secondary"] if idx is None else IDENT[idx]


class Font:
    def __init__(self, name, spacing=0):
        self.name = name
        self.spacing = spacing
        src = open(os.path.join(FONT_DIR, name + ".c"), encoding="utf-8").read()
        m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
        body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
        self.bmp = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body))
        self.dsc = [tuple(int(v) for v in t) for t in re.findall(
            r"\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
            r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)", src)]
        lists = {n: [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", b)]
                 for n, b in re.findall(
                     r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", src, re.S)}
        self.cmaps = []
        for rs, rl, gs, ul, ty in re.findall(
                r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),"
                r"\s*\.unicode_list = (\w+), \.glyph_id_ofs_list = \w+, "
                r"\.list_length = \d+, \.type = (\w+)", src):
            self.cmaps.append((int(rs), int(rl), int(gs), lists.get(ul), ty))
        self.line_h = int(re.search(r"\.line_height = (\d+)", src).group(1))
        self.base = int(re.search(r"\.base_line = (\d+)", src).group(1))

    def glyph(self, cp):
        for rs, rl, gs, ul, ty in self.cmaps:
            if not rs <= cp < rs + rl:
                continue
            if ty.endswith("FORMAT0_TINY"):
                return gs + cp - rs
            if ty.endswith("SPARSE_TINY") and (cp - rs) in ul:
                return gs + ul.index(cp - rs)
        return None

    def adv(self, ch):
        g = self.glyph(ord(ch))
        if g is None:
            raise KeyError("%s has no glyph for U+%04X" % (self.name, ord(ch)))
        return ((self.dsc[g][1] + 8) >> 4) + self.spacing

    def width(self, s):
        return sum(self.adv(ch) for ch in s) - (self.spacing if s else 0)


_fonts = {}


def font(name, spacing=0):
    key = (name, spacing)
    if key not in _fonts:
        _fonts[key] = Font(name, spacing)
    return _fonts[key]


# The role fonts at text size Small (DS 3, DS 46.2).
def F_CAPTION(): return font("pos_font_mono_14", 1)      # POS_STYLE_CAPTION
def F_DENSE(): return font("pos_font_mono_14", 0)        # caption, tracking off (DS 54.1)
def F_VALUE(): return font("pos_font_mono_20")           # POS_STYLE_VALUE
def F_BUTTON(): return font("pos_font_mono_16_medium", 2)  # POS_STYLE_BUTTON_LABEL
def F_BODY(): return font("pos_font_sans_16")            # body
def F_ROW(): return font("pos_font_sans_20")             # POS_STYLE_ROW_TITLE
def F_TITLE(): return font("pos_font_sans_24_semibold")  # POS_STYLE_TITLE
def F_HERO(): return font("pos_font_sans_48_semibold", -1)  # POS_STYLE_HERO_48


class Canvas:
    def __init__(self, w, h, bg=None):
        self.w, self.h = w, h
        self.px = bytearray(bytes(bg or T["bg"]) * (w * h))
        self.layout = {}

    def mark(self, name, x, y, w, h):
        """Record a named box for layout.json (the mockup's own numbers)."""
        self.layout[name] = [x, y, w, h]

    def rect(self, x, y, w, h, c):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(self.w, x + w), min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(c) * (x1 - x0)
        for yy in range(y0, y1):
            o = (yy * self.w + x0) * 3
            self.px[o:o + len(row)] = row

    def blend(self, x, y, c, a):
        if 0 <= x < self.w and 0 <= y < self.h and a > 0:
            o = (y * self.w + x) * 3
            p = self.px
            p[o] = (p[o] * (255 - a) + c[0] * a + 127) // 255
            p[o + 1] = (p[o + 1] * (255 - a) + c[1] * a + 127) // 255
            p[o + 2] = (p[o + 2] * (255 - a) + c[2] * a + 127) // 255

    def rrect(self, x, y, w, h, r, c):
        r = max(0, min(r, w // 2, h // 2))
        if r == 0:
            return self.rect(x, y, w, h, c)
        self.rect(x + r, y, w - 2 * r, h, c)
        self.rect(x, y + r, r, h - 2 * r, c)
        self.rect(x + w - r, y + r, r, h - 2 * r, c)
        for j in range(r):
            for i in range(r):
                cov = 0
                for sj in range(4):
                    for si in range(4):
                        dx = r - (i + (si + 0.5) / 4)
                        dy = r - (j + (sj + 0.5) / 4)
                        if dx * dx + dy * dy <= r * r:
                            cov += 1
                a = cov * 255 // 16
                for px_, py_ in ((x + i, y + j), (x + w - 1 - i, y + j),
                                 (x + i, y + h - 1 - j), (x + w - 1 - i, y + h - 1 - j)):
                    self.blend(px_, py_, c, a)

    def border(self, x, y, w, h, r, c, t=1, inner=None):
        """A t px rounded outline; the inside is refilled with `inner` (bg)."""
        self.rrect(x, y, w, h, r, c)
        self.rrect(x + t, y + t, w - 2 * t, h - 2 * t, max(0, r - t), inner or T["bg"])

    def dashed(self, x, y, w, h, c, dash=6, gap=5):
        for xx in range(x, x + w, dash + gap):
            self.rect(xx, y, min(dash, x + w - xx), 1, c)
            self.rect(xx, y + h - 1, min(dash, x + w - xx), 1, c)
        for yy in range(y, y + h, dash + gap):
            self.rect(x, yy, 1, min(dash, y + h - yy), c)
            self.rect(x + w - 1, yy, 1, min(dash, y + h - yy), c)

    def disc(self, cx, cy, r, c):
        for j in range(-r, r + 1):
            for i in range(-r, r + 1):
                cov = 0
                for sj in range(4):
                    for si in range(4):
                        dx = i + (si + 0.5) / 4 - 0.5
                        dy = j + (sj + 0.5) / 4 - 0.5
                        if dx * dx + dy * dy <= r * r:
                            cov += 1
                self.blend(cx + i, cy + j, c, cov * 255 // 16)

    def tri(self, x, y, size, c, direction="right"):
        """A small solid disclosure triangle in a size x size box."""
        for j in range(size):
            for i in range(size):
                if direction == "right":
                    inside = abs(j - (size - 1) / 2) * 2 <= (size - 1 - i)
                else:  # down
                    inside = abs(i - (size - 1) / 2) * 2 <= (size - 1 - j)
                if inside:
                    self.blend(x + i, y + j, c, 255)

    def chevron(self, cx, cy, size, c, direction="left", t=2):
        """A stroked chevron (DS 11: stroke icons), centred on (cx, cy)."""
        for k in range(size + 1):
            # k = 0 at the two open ends, k = size at the tip
            if direction == "left":
                x = cx + size // 2 - k
            else:
                x = cx - size // 2 + k
            for tt in range(t):
                xx = x + (tt if direction == "left" else -tt)
                self.blend(xx, cy - size + k, c, 255)
                self.blend(xx, cy + size - k, c, 255)

    def text(self, f, x, y, s, c):
        """Draw s with its line box's top at y (as an LVGL label does). Returns end x."""
        baseline = y + f.line_h - f.base
        pen = x
        for ch in s:
            g = f.glyph(ord(ch))
            if g is None:
                raise KeyError("%s has no glyph for U+%04X" % (f.name, ord(ch)))
            bi, adv, bw, bh, ox, oy = f.dsc[g]
            top = baseline - bh - oy
            n = 0
            for j in range(bh):
                for i in range(bw):
                    b = f.bmp[bi + (n >> 1)]
                    v = (b >> 4) if (n & 1) == 0 else (b & 15)
                    n += 1
                    if v:
                        self.blend(pen + ox + i, top + j, c, v * 17)
            pen += ((adv + 8) >> 4) + f.spacing
        return pen

    def text_r(self, f, xr, y, s, c):
        self.text(f, xr - f.width(s), y, s, c)

    def text_c(self, f, cx, y, s, c):
        self.text(f, cx - f.width(s) // 2, y, s, c)

    def save(self, path):
        raw = bytearray()
        stride = self.w * 3
        for y in range(self.h):
            raw.append(0)
            raw += self.px[y * stride:(y + 1) * stride]

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
        png = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
        with open(path, "wb") as fh:
            fh.write(png)
