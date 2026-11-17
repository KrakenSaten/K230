#!/usr/bin/env python3
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
"""Draw the Games Hub's poster art (DS §60, Concept C "Poster Wall").

    gen_games_hub.py            draw, render and crop the twenty posters
    gen_games_hub.py --svg      write the two wall SVGs only (no browser)
    gen_games_hub.py --sizes    print "<name> <w> <h> <x> <y>" for every poster

This is the design-time half of the hub's art pipeline, and the only half
that needs more than python3:

  1. here: every poster is drawn as vector art - each game in its own
     palette, after its own title screen where it has one - with its
     lettering, scrim and rounded corners baked in against the hub's ground
     (#0b0b0a). The whole wall of one orientation is one SVG at the panel's
     size, rendered once by a headless Chromium-family browser (Microsoft
     Edge or Chrome; MOCKUP_BROWSER overrides), and each poster is cut from
     that picture at exactly the rectangle ui/shell/hub_layout.c gives it:

         docs/design/games-hub/art/hub-<game>-<orientation>.png   (committed)
         docs/design/games-hub/art/wall-<orientation>.svg         (committed)

  2. tools/design/gen_doors_ui.py (standard library only, part of every
     art check): each PNG becomes ui/assets/doors/hub-*.bin, RGB565,
     Floyd-Steinberg dithered, with its MANIFEST.txt row.

The lettering is set in IBM Plex Sans Condensed, IBM Plex Sans and IBM Plex
Mono (SIL OFL 1.1), fetched by the page from Google Fonts, so rendering needs
network access; the picture carries a marker the script reads to refuse a
render whose fonts did not load. No other typeface, and no glyph of a system
font, is in the art: the card suits and every mark are drawn as paths.

What the shell draws live is not here: FEATURED, PLAY, PORTRAIT, COMING
SOON, the lobby's sentence and its two disabled buttons, the header and the
hint (ui/shell/hub.c).

The rectangles below are hub_layout.c's; tests/doors_ui_assets_test.sh holds
the two together through `tests/hub_layout_test --art`.
"""
import math
import os
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngstrict import read_png  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
ART = ROOT / "docs/design/games-hub/art"

BG = "#0b0b0a"
AMBER, RED, LOBBY, CREAM, CORAL = "#e0a83a", "#e5502e", "#8c9aff", "#fff1d6", "#ff8a6a"
SANS = "'IBM Plex Sans'"
COND = "'IBM Plex Sans Condensed'"
RADIUS = 14

# name: (x, y, w, h) on the wall - ui/shell/hub_layout.c, both orientations.
WALLS = {
    "portrait": (568, 1232, {
        "rotorterror": (20, 116, 528, 300), "relay-nine": (20, 426, 258, 310),
        "fleet": (288, 426, 125, 150), "radar": (423, 426, 125, 150),
        "timber": (288, 586, 125, 150), "2048": (423, 586, 125, 150),
        "solitaire": (20, 746, 169, 160), "blackjack": (199, 746, 169, 160),
        "poker": (379, 746, 169, 160), "lobby": (20, 916, 528, 200)}),
    "landscape": (1232, 568, {
        "rotorterror": (30, 76, 378, 468), "relay-nine": (418, 76, 290, 468),
        "fleet": (718, 76, 155, 146), "radar": (883, 76, 155, 146), "timber": (1047, 76, 155, 146),
        "solitaire": (718, 232, 155, 146), "blackjack": (883, 232, 155, 146),
        "poker": (1047, 232, 155, 146), "2048": (718, 388, 155, 156),
        "lobby": (883, 388, 319, 156)}),
}
ORDER = ["rotorterror", "relay-nine", "fleet", "radar", "timber", "solitaire", "blackjack", "poker", "2048", "lobby"]
NAMES = {"fleet": "FLEET", "radar": "RADAR", "timber": "TIMBER", "solitaire": "SOLITAIRE",
         "blackjack": "BLACKJACK", "poker": "POKER"}

_n = [0]


def uid():
    _n[0] += 1
    return "u%d" % _n[0]


def T(x, y, s, size, fill, font=SANS, w=700, anchor="start", ls=0, extra=""):
    return ('<text x="%s" y="%s" font-family="%s" font-size="%s" font-weight="%s" fill="%s" '
            'text-anchor="%s" letter-spacing="%s" %s>%s</text>'
            % (x, y, font, size, w, fill, anchor, ls, extra, s))


def R(x, y, w, h, fill, rx=0, extra=""):
    return '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s" %s/>' % (x, y, w, h, rx, fill, extra)


def place(inner, vb, x, y, w, h, r=0, align="xMidYMid", box=None):
    """Art drawn in its own viewBox, cropped ("slice") into a rounded box."""
    c = uid()
    bx, by, bw, bh = box or (x, y, w, h)
    return ('<clipPath id="%s"><rect x="%s" y="%s" width="%s" height="%s" rx="%s"/></clipPath>'
            '<g clip-path="url(#%s)"><svg x="%s" y="%s" width="%s" height="%s" viewBox="0 0 %s %s" '
            'preserveAspectRatio="%s slice">%s</svg></g>'
            % (c, x, y, w, h, r, c, bx, by, bw, bh, vb[0], vb[1], align, inner))


def grad(i, stops, kind="v"):
    a = {"v": 'x1="0" y1="0" x2="0" y2="1"', "h": 'x1="0" y1="0" x2="1" y2="0"'}.get(kind)
    body = "".join('<stop offset="%s" stop-color="%s" stop-opacity="%s"/>' % (o, c, op) for o, c, op in stops)
    if kind == "r":
        return '<radialGradient id="%s">%s</radialGradient>' % (i, body)
    return '<linearGradient id="%s" %s>%s</linearGradient>' % (i, a, body)


DEFS = "<defs>" + "".join([
    grad("gSky", [(0, "#140f33", 1), (.35, "#3a1d5c", 1), (.6, "#8a3468", 1), (.78, "#e8674f", 1), (.9, "#ffa45c", 1)]),
    grad("gSun", [(0, "#fff0a8", 1), (1, "#ff7a4a", 1)]),
    grad("gWater", [(0, "#2a2350", 1), (1, "#0d1026", 1)]),
    grad("gTile", [(0, "#000", 0), (1, "#000", .78)]),
    grad("gDoor", [(0, "#ffe7a0", 1), (.5, AMBER, 1), (1, "#6a3f0c", 1)], "r"),
    grad("gVig", [(0, "#000", 0), (.6, "#000", .1), (1, "#000", .8)], "r"),
    grad("gWallL", [(0, "#0a1512", 1), (1, "#16302a", 1)], "h"),
    grad("gWallR", [(0, "#16302a", 1), (1, "#0a1512", 1)], "h"),
    grad("gFleet", [(0, "#164a86", 1), (1, "#071a33", 1)]),
    grad("gRadar", [(0, "#0f4a31", 1), (1, "#03100b", 1)], "r"),
    grad("gTimber", [(0, "#2a8a58", 1), (1, "#0d3a25", 1)]),
    grad("gSol", [(0, "#1c9c66", 1), (1, "#08402a", 1)]),
    grad("gBj", [(0, "#b01a30", 1), (1, "#24050a", 1)]),
    grad("gPoker", [(0, "#6a38bd", 1), (1, "#1f0e45", 1)]),
    grad("g2048", [(0, "#ffd166", 1), (1, "#f08a2e", 1)]),
    grad("gLobby", [(0, "#262d78", 1), (1, "#0c0e2a", 1)]),
    '<pattern id="pScan" width="4" height="4" patternUnits="userSpaceOnUse">'
    '<rect width="4" height="1" fill="#000" opacity=".28"/></pattern>',
    '<pattern id="pTape" width="28" height="28" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
    '<rect width="14" height="28" fill="#8c9aff" opacity=".10"/></pattern>',
]) + "</defs>"

# ------------------------------------------------------ marks drawn as paths
# Relay Nine's own pixel wordmark (5 x 7 cells), after the game's title card.
PIX = {
    "R": "11110 10001 10001 11110 10100 10010 10001", "E": "11111 10000 10000 11110 10000 10000 11111",
    "L": "10000 10000 10000 10000 10000 10000 11111", "A": "01110 10001 10001 11111 10001 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100", "N": "10001 11001 10101 10011 10001 10001 10001",
    "I": "11111 00100 00100 00100 00100 00100 11111", "9": "01110 10001 10001 01111 00001 00001 01110",
    "0": "01110 10001 10011 10101 11001 10001 01110",
}
# The card suits in a 24 x 24 box.
SUIT = {
    "heart": '<path d="M12 21 C12 21 3 14.5 3 8.5 C3 5.5 5.4 3.5 7.8 3.5 C9.6 3.5 11.2 4.5 12 6 '
             'C12.8 4.5 14.4 3.5 16.2 3.5 C18.6 3.5 21 5.5 21 8.5 C21 14.5 12 21 12 21 Z"/>',
    "spade": '<path d="M12 2 C12 2 3 9 3 14 C3 16.8 5.2 18.5 7.5 18.5 C9 18.5 10.3 17.9 11.2 16.9 '
             'C11 18.8 10.2 20.5 9 22 H15 C13.8 20.5 13 18.8 12.8 16.9 C13.7 17.9 15 18.5 16.5 18.5 '
             'C18.8 18.5 21 16.8 21 14 C21 9 12 2 12 2 Z"/>',
    "club": '<circle cx="12" cy="7.5" r="4.4"/><circle cx="7.2" cy="13.6" r="4.4"/>'
            '<circle cx="16.8" cy="13.6" r="4.4"/><path d="M11 12 C11 17 10.2 20 9 22 H15 C13.8 20 13 17 13 12 Z"/>',
}


def suit(name, cx, cy, size, fill):
    k = size / 24.0
    return '<g fill="%s" transform="translate(%s %s) scale(%s)">%s</g>' % (fill, cx - 12 * k, cy - 12 * k, k, SUIT[name])


def pix(text, x, y, s, fill, extra=""):
    o = ['<g fill="%s" %s>' % (fill, extra)]
    for i, ch in enumerate(text):
        for r, row in enumerate(PIX[ch].split()):
            for c, bit in enumerate(row):
                if bit == "1":
                    o.append('<rect x="%s" y="%s" width="%s" height="%s"/>' % (x + (i * 6 + c) * s, y + r * s, s, s))
    return "".join(o) + "</g>"


def relay_logo(x, y, s):
    """Node mark + RELAY / NINE."""
    o = ['<rect x="%s" y="%s" width="%s" height="%s" fill="none" stroke="%s" stroke-width="%s"/>'
         % (x, y + s, 11 * s, 14 * s, AMBER, s)]
    for r in range(3):
        for c in range(3):
            o.append(R(x + (2.5 + c * 2.5) * s, y + (4 + r * 3) * s, 1.2 * s, 1.2 * s, CREAM))
    o.append(R(x + 11.5 * s, y + 7.5 * s, 3.5 * s, s, RED))
    o.append(pix("RELAY", x + 16 * s, y, s, CREAM))
    o.append(pix("NINE", x + 16 * s, y + 9 * s, s, AMBER))
    for i in range(5):
        o.append(R(x + (16 + i * 5) * s, y + 18 * s, 4 * s, .8 * s, RED if i == 0 else "#55705f"))
    return "".join(o)


def rotor_logo(x, y, size):
    """ROTOR / TERROR: heavy, condensed, italic, leaning into the wind."""
    def word(dy, s, fill):
        return T(0, dy, s, size, fill, COND, 700, ls=1,
                 extra='font-style="italic" stroke="%s" stroke-width="%s" paint-order="stroke" '
                       'stroke-linejoin="round"' % (fill, size * .035))
    return '<g transform="translate(%s %s) skewX(-6)">%s%s</g>' % (x, y, word(0, "ROTOR", CREAM),
                                                                  word(size * .9, "TERROR", CORAL))


# ----------------------------------------------------------------------- art
def art_rotor():
    rnd = random.Random(7)
    s = [R(0, 0, 600, 600, "url(#gSky)")]
    for _ in range(80):
        s.append(R(round(rnd.uniform(0, 600)), round(rnd.uniform(0, 270)), 2, 2, "#fff",
                   extra='opacity="%.2f"' % rnd.uniform(.2, .8)))
    s.append('<circle cx="335" cy="318" r="128" fill="url(#gSun)"/>')
    for x, y, w in [(30, 150, 180), (390, 118, 190), (90, 236, 120), (440, 252, 150)]:
        s.append(R(x, y, w, 8, "#b0507a", 4, 'opacity=".6"') + R(x + 24, y + 9, w * .55, 5, "#8a3468", 3, 'opacity=".7"'))
    s.append('<polygon points="0,420 60,360 120,400 190,330 260,400 330,350 410,410 480,340 560,390 600,360 '
             '600,470 0,470" fill="#3c2358"/>')
    x = -6
    while x < 600:
        w, h = rnd.randint(30, 60), rnd.randint(46, 150)
        y = 470 - h
        s.append(R(x, y, w, h, "#121631"))
        if rnd.random() < .3:
            s.append(R(x + w / 2 - 1, y - 18, 2, 18, "#121631"))
        for wy in range(y + 10, 462, 14):
            for wx in range(x + 6, x + w - 6, 10):
                if rnd.random() < .34:
                    s.append(R(wx, wy, 3, 5, "#ffb86b", extra='opacity=".9"'))
        x += w + rnd.randint(2, 8)
    s.append(R(0, 470, 600, 130, "url(#gWater)"))
    for i in range(9):
        w = 160 - i * 13
        s.append(R(335 - w / 2, 478 + i * 13, w, 3, "#ff9a5c", extra='opacity="%.2f"' % (.6 - .05 * i)))
    for _ in range(26):
        s.append(R(rnd.randint(0, 580), rnd.randint(476, 596), rnd.randint(10, 34), 2, "#7b6bd0", extra='opacity=".35"'))
    # the Kestrel
    s.append(
        '<g transform="translate(300 250) rotate(-7) scale(.8) translate(-300 -250)">'
        '<ellipse cx="318" cy="176" rx="205" ry="7" fill="#fff1d6" opacity=".35"/>'
        '<rect x="118" y="174" width="400" height="3" fill="#fff1d6"/>'
        '<rect x="313" y="176" width="10" height="26" fill="#1c6f73"/>'
        '<polygon points="232,240 70,222 70,238 232,272" fill="#22a39b"/>'
        '<polygon points="70,240 44,172 78,172 100,226" fill="#35c9bd"/>'
        '<circle cx="60" cy="200" r="30" fill="none" stroke="#fff1d6" stroke-width="2" opacity=".55"/>'
        '<path d="M38 180 L82 220 M82 180 L38 220" stroke="#fff1d6" stroke-width="2" opacity=".7"/>'
        '<path d="M215 262 q0 -58 75 -62 h70 q62 4 92 52 q8 30 -24 44 h-160 q-50 -4 -53 -34z" fill="#35c9bd"/>'
        '<path d="M218 272 q8 24 50 24 h160 q22 -8 26 -24z" fill="#1f8f8a"/>'
        '<path d="M362 204 q56 6 86 52 l-72 6 q-18 -2 -22 -20z" fill="#0d3f52"/>'
        '<path d="M372 212 q36 6 58 34" stroke="#9ff0e6" stroke-width="3" fill="none" opacity=".8"/>'
        '<rect x="286" y="218" width="54" height="46" rx="5" fill="#1f8f8a"/>'
        '<rect x="292" y="224" width="42" height="18" rx="3" fill="#0d3f52"/>'
        '<rect x="296" y="300" width="78" height="13" rx="6" fill="#0f2c3d"/>'
        '<rect x="368" y="303" width="14" height="7" rx="3" fill="#ff8a6a"/>'
        '<path d="M250 334 h176" stroke="#dff5ef" stroke-width="5" stroke-linecap="round"/>'
        '<path d="M282 296 L270 334 M392 296 L402 334" stroke="#dff5ef" stroke-width="4"/>'
        '<circle cx="452" cy="264" r="6" fill="#ff8a6a"/>'
        '<path d="M470 270 L590 286" stroke="#ffe08a" stroke-width="4" stroke-dasharray="16 12"/>'
        '</g>')
    return "".join(s)


def art_relay():
    s = [R(0, 0, 600, 600, "#050b09"),
         '<polygon points="0,0 600,0 370,200 230,200" fill="#08120f"/>',
         '<polygon points="0,600 600,600 370,380 230,380" fill="#0c1915"/>',
         '<polygon points="0,0 230,200 230,380 0,600" fill="url(#gWallL)"/>',
         '<polygon points="600,0 370,200 370,380 600,600" fill="url(#gWallR)"/>']
    for i in range(-7, 8):
        s.append('<line x1="%s" y1="380" x2="%s" y2="600" stroke="#17332a" stroke-width="1.5"/>' % (300 + i * 10, 300 + i * 52))
    for t in (0, .22, .42, .6, .76, .88):
        s.append('<rect x="%s" y="%s" width="%s" height="%s" fill="none" stroke="#21443a" stroke-width="%s"/>'
                 % (230 * t, 200 * t, 600 - 460 * t, 600 - 420 * t, 5 * (1 - t) + 1))
    s.append('<polygon points="232,380 368,380 540,600 60,600" fill="%s" opacity=".14"/>' % AMBER)
    s.append('<circle cx="300" cy="290" r="210" fill="url(#gDoor)" opacity=".16"/>')
    s.append(R(230, 200, 140, 180, "url(#gDoor)"))
    s.append('<rect x="230" y="200" width="140" height="180" fill="none" stroke="#2a1d08" stroke-width="7"/>')
    s.append(R(230, 236, 140, 4, "#2a1d08") + R(230, 344, 140, 4, "#2a1d08"))
    # the sentry in the relay door
    s.append(R(282, 300, 36, 52, "#0f0a03", 4) + R(288, 276, 24, 22, "#0f0a03", 3) + R(293, 284, 14, 4, "#ff4a2a")
             + R(272, 306, 10, 34, "#0f0a03", 3) + R(318, 306, 10, 34, "#0f0a03", 3)
             + R(286, 350, 10, 30, "#0f0a03") + R(304, 350, 10, 30, "#0f0a03"))
    s.append('<path d="M0 0 L230 200 M600 0 L370 200" stroke="%s" stroke-width="4" opacity=".85"/>' % RED)
    s.append('<path d="M0 600 L230 380 M600 600 L370 380" stroke="%s" stroke-width="3" opacity=".5"/>' % AMBER)
    s.append('<path d="M0 118 L230 238 M0 138 L230 246" stroke="#2c5a4a" stroke-width="5"/>')
    s.append('<path d="M600 430 L370 322" stroke="#2c5a4a" stroke-width="5"/>')
    s.append(pix("09", 24, 150, 8, AMBER, 'opacity=".5" transform="skewY(14)"'))
    for i in range(6):
        s.append('<polygon points="%s,470 %s,470 %s,520 %s,520" fill="%s" opacity=".5"/>'
                 % (470 + i * 26, 482 + i * 26, 470 + i * 26, 458 + i * 26, AMBER))
    s.append(R(0, 0, 600, 600, "url(#pScan)") + R(0, 0, 600, 600, "url(#gVig)"))
    return "".join(s)


def art_fleet():
    s = [R(0, 0, 200, 200, "url(#gFleet)")]
    for i in range(1, 8):
        s.append('<path d="M%s 0 V200 M0 %s H200" stroke="#5cc8ff" stroke-width="1" opacity=".16"/>' % (i * 25, i * 25))
    for i, y in enumerate((146, 160, 174)):
        s.append('<path d="M%s %s H200" stroke="#5cc8ff" stroke-width="3" stroke-dasharray="20 12" opacity="%s"/>'
                 % (i * 9 - 10, y, .7 - i * .18))
    s.append('<polygon points="30,118 172,118 156,140 46,140" fill="#e6eef7"/>'
             '<rect x="76" y="98" width="52" height="20" fill="#b9c9da"/><rect x="90" y="82" width="24" height="16" fill="#e6eef7"/>'
             '<rect x="101" y="58" width="3" height="24" fill="#e6eef7"/><rect x="128" y="108" width="28" height="5" fill="#b9c9da"/>'
             '<rect x="46" y="108" width="24" height="5" fill="#b9c9da"/>')
    s.append('<circle cx="154" cy="52" r="15" fill="none" stroke="#ff5a4a" stroke-width="4"/>'
             '<path d="M144 42 L164 62 M164 42 L144 62" stroke="#ff5a4a" stroke-width="4"/>'
             '<circle cx="38" cy="38" r="5" fill="#fff" opacity=".8"/><circle cx="88" cy="38" r="5" fill="#fff" opacity=".5"/>')
    return "".join(s)


def art_radar():
    s = [R(0, 0, 200, 200, "#03100b"), '<circle cx="100" cy="100" r="140" fill="url(#gRadar)"/>']
    for r in (30, 56, 82):
        s.append('<circle cx="100" cy="100" r="%s" fill="none" stroke="#2aa567" stroke-width="1.6"/>' % r)
    s.append('<path d="M100 14 V186 M14 100 H186" stroke="#2aa567" stroke-width="1" opacity=".6"/>')
    s.append('<path d="M100 100 L100 18 A82 82 0 0 1 171 59 Z" fill="#5dff9b" opacity=".2"/>'
             '<path d="M100 100 L135 26 A82 82 0 0 1 171 59 Z" fill="#5dff9b" opacity=".22"/>'
             '<path d="M100 100 L171 59" stroke="#5dff9b" stroke-width="3"/>')
    s.append('<circle cx="134" cy="66" r="6" fill="#5dff9b"/><circle cx="62" cy="128" r="4" fill="#5dff9b" opacity=".7"/>'
             '<circle cx="118" cy="146" r="3" fill="#5dff9b" opacity=".5"/>'
             '<rect x="-7" y="-7" width="14" height="14" fill="#ff5a4a" transform="translate(54 62) rotate(45)"/>'
             '<circle cx="134" cy="66" r="13" fill="none" stroke="#5dff9b" stroke-width="1.5" stroke-dasharray="4 4"/>')
    return "".join(s)


def art_timber():
    s = [R(0, 0, 200, 200, "url(#gTimber)"), '<ellipse cx="100" cy="176" rx="84" ry="11" fill="#06241a" opacity=".7"/>']
    wood = ["#e9b06a", "#d1924a", "#f3c688"]
    for i in range(10):
        y = 163 - 14 * i
        g = '<g transform="rotate(3 100 %s)">' % y if i >= 8 else "<g>"
        if i % 2:
            g += R(58, y, 84, 12, wood[i % 3], 2) + R(58, y + 9, 84, 3, "#a36a2c", 0, 'opacity=".5"')
        else:
            for b in range(3):
                if i == 4 and b == 1:
                    continue
                g += R(58 + b * 28.5, y, 27, 12, wood[(i + b) % 3], 2)
        s.append(g + "</g>")
    s.append('<g transform="rotate(-8 150 106)">' + R(124, 100, 54, 12, "#f3c688", 2) + "</g>")
    s.append('<path d="M112 110 h10" stroke="#fff" stroke-width="2" stroke-dasharray="3 3" opacity=".7"/>')
    return "".join(s)


def _card(x, y, rank, name, col, rot, w=58, h=84):
    return ('<g transform="rotate(%s %s %s)">' % (rot, x + w / 2, y + h)
            + '<rect x="%s" y="%s" width="%s" height="%s" rx="7" fill="#fbf7ee" stroke="#000" stroke-opacity=".2"/>' % (x, y, w, h)
            + T(x + 7, y + 22, rank, 20, col, SANS, 700)
            + suit(name, x + w / 2, y + h * .60, 36, col) + "</g>")


def art_solitaire():
    return (R(0, 0, 200, 200, "url(#gSol)")
            + '<rect x="14" y="14" width="172" height="172" rx="12" fill="none" stroke="#bff0d0" stroke-width="1.5" opacity=".3"/>'
            + _card(44, 56, "Q", "heart", "#c8102e", -20) + _card(71, 50, "J", "club", "#1a1a1a", 0)
            + _card(98, 56, "K", "spade", "#1a1a1a", 20))


def art_blackjack():
    return (R(0, 0, 200, 200, "url(#gBj)")
            + '<path d="M-10 170 Q100 110 210 170" fill="none" stroke="#ffd166" stroke-width="2" opacity=".5"/>'
            + _card(40, 44, "A", "spade", "#1a1a1a", -10) + _card(92, 50, "K", "heart", "#c8102e", 9)
            + '<circle cx="160" cy="44" r="24" fill="#ffd166"/>' + T(160, 53, "21", 26, "#2a060c", SANS, 700, "middle"))


def art_poker():
    s = [R(0, 0, 200, 200, "url(#gPoker)")]

    def chip(cx, cy, r, c, face):
        circ = 2 * math.pi * (r - r * .09)
        d = circ / 16
        return ('<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (cx, cy, r, c)
                + '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="#fff" stroke-width="%s" stroke-dasharray="%.2f %.2f"/>'
                % (cx, cy, r - r * .09, r * .18, d, d)
                + '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (cx, cy, r * .66, face))
    s.append(chip(44, 158, 22, "#e5534b", "#b03a34") + chip(160, 42, 19, "#5cc8ff", "#2c7fb0"))
    s.append(chip(100, 100, 60, "#f2c14e", "#2b1560"))
    s.append('<circle cx="100" cy="100" r="33" fill="none" stroke="#f2c14e" stroke-width="2"/>')
    s.append(suit("spade", 100, 100, 46, "#f2c14e"))
    return "".join(s)


def art_2048():
    s = [R(0, 0, 200, 200, "url(#g2048)")]
    tiles = [(36, 36, "#fff6de", "2", 30, "#5a3a10"), (104, 36, "#ffe7b0", "4", 30, "#5a3a10"),
             (36, 104, "#ff8f4d", "8", 30, "#fff6de"), (104, 104, "#3a2408", "2048", 20, "#ffd166")]
    for x, y, c, t, fs, tc in tiles:
        s.append(R(x, y, 60, 60, c, 9) + T(x + 30, y + 30 + fs * .36, t, fs, tc, SANS, 700, "middle"))
    return "".join(s)


ARTS = {"rotorterror": (art_rotor, (600, 600)), "relay-nine": (art_relay, (600, 600)),
        "fleet": (art_fleet, (200, 200)), "radar": (art_radar, (200, 200)), "timber": (art_timber, (200, 200)),
        "solitaire": (art_solitaire, (200, 200)), "blackjack": (art_blackjack, (200, 200)),
        "poker": (art_poker, (200, 200)), "2048": (art_2048, (200, 200))}
_cache = {}


def art(g, x, y, w, h, box=None):
    if g not in _cache:
        _cache[g] = ARTS[g][0]()
    return place(_cache[g], ARTS[g][1], x, y, w, h, RADIUS, box=box)


def devices(cx, cy, col, k=1.0):
    """Two handhelds and the link that is not there yet. Illustrative: no player count."""
    def unit(x):
        return ('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="#0b0b0a" stroke="%s" stroke-width="2.5"/>'
                % (x, cy - 38 * k, 40 * k, 76 * k, 7 * k, col)
                + R(x + 6 * k, cy - 30 * k, 28 * k, 46 * k, col, 2, 'opacity=".22"')
                + '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (x + 20 * k, cy + 27 * k, 3.5 * k, col))
    a, b = cx - 62 * k, cx + 22 * k
    return (unit(a) + unit(b)
            + '<path d="M%s %s H%s" stroke="%s" stroke-width="2.5" stroke-dasharray="3 7" stroke-linecap="round"/>'
            % (a + 46 * k, cy, b - 6 * k, col))


# -------------------------------------------------------------------- posters
def poster(g, x, y, w, h, wide):
    """One poster at its place on the wall."""
    if g == "rotorterror":
        if wide:   # landscape: tall
            return (art(g, x, y, w, h) + R(x, y + h * .45, w, h * .55, "url(#gTile)", RADIUS)
                    + rotor_logo(x + 20, y + h - 138, 62))
        return (art(g, x, y, w, h) + R(x, y + h * .45, w, h * .55, "url(#gTile)", RADIUS)
                + rotor_logo(x + 18, y + 232, 58))
    if g == "relay-nine":
        if wide:
            return (art(g, x, y, w, h) + R(x, y + h * .5, w, h * .5, "url(#gTile)", RADIUS)
                    + relay_logo(x + 18, y + h - 170, 4.8))
        return (art(g, x, y, w, h, box=(x - 71, y - 83, 400, 400)) + R(x, y + h * .45, w, h * .55, "url(#gTile)", RADIUS)
                + relay_logo(x + 18, y + 172, 3.5))
    if g == "2048":   # the tiles are the name
        return art(g, x, y, w, h)
    if g == "lobby":
        s = [R(x, y, w, h, "url(#gLobby)", RADIUS), R(x, y, w, h, "url(#pTape)", RADIUS),
             '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="%s" stroke-width="2" '
             'stroke-dasharray="10 8"/>' % (x + 1, y + 1, w - 2, h - 2, RADIUS - 1, LOBBY)]
        if wide:   # landscape: the narrow lobby
            s.append(T(x + 16, y + 40, "MULTIPLAYER LOBBY", 28, "#fff", COND, 700, ls=.6))
            s.append(devices(x + w - 58, y + 72, "#c5ccff", .55))
        else:
            s.append(T(x + 22, y + 52, "MULTIPLAYER", 40, "#fff", COND, 700, ls=1))
            s.append(T(x + 22, y + 92, "LOBBY", 40, LOBBY, COND, 700, ls=1))
            s.append(devices(x + w - 86, y + 104, "#c5ccff", .8))
        return "".join(s)
    return (art(g, x, y, w, h) + R(x, y + h * .45, w, h * .55, "url(#gTile)", RADIUS)
            + T(x + 12, y + h - 14, NAMES[g], 22 if wide else 24, "#fff", COND, 700, ls=.5))


def wall_svg(orient):
    w, h, tiles = WALLS[orient]
    _cache.clear()
    _n[0] = 0
    body = "".join(poster(g, x, y, tw, th, orient == "landscape") for g, (x, y, tw, th) in tiles.items())
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s%s%s</svg>'
            % (w, h, w, h, DEFS, R(0, 0, w, h, BG), body))


# ------------------------------------------------------------------ rendering
FACES = ['700 20px "IBM Plex Sans Condensed"', 'italic 700 20px "IBM Plex Sans Condensed"', '700 20px "IBM Plex Sans"']
WRAP = ('<!doctype html><html><head><meta charset="utf-8">'
        '<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@700&'
        'family=IBM+Plex+Sans+Condensed:ital,wght@0,700;1,700&display=block" rel="stylesheet">'
        '<style>html,body{margin:0;background:%s;overflow:hidden}svg{display:block}'
        '#ok{position:absolute;left:0;top:0;width:4px;height:4px;background:#ff0000}</style></head>'
        '<body>%%s<div id="ok"></div><script>'
        'const faces=%s;Promise.all(faces.map(f=>document.fonts.load(f,"ROTOR 21"))).then(()=>{'
        'if(faces.every(f=>document.fonts.check(f,"ROTOR 21")))document.getElementById("ok").style.background="#00ff00";});'
        '</script></body></html>' % (BG, FACES))


def find_browser():
    cands = [os.environ.get("MOCKUP_BROWSER"), r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
             r"C:\Program Files\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Google\Chrome\Application\chrome.exe",
             shutil.which("chromium"), shutil.which("google-chrome"), shutil.which("msedge")]
    return next((c for c in cands if c and os.path.exists(c)), None)


def shoot(browser, tmp, html, png, w, h):
    page = pathlib.Path(tmp) / (png.stem + ".html")
    page.write_text(html, encoding="utf-8")
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                             "--user-data-dir=" + str(pathlib.Path(tmp) / "profile"), "--virtual-time-budget=8000",
                             "--window-size=%d,%d" % (w, h), "--screenshot=" + str(png), page.as_uri()],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=60)
    except subprocess.TimeoutExpired:   # Edge sometimes lingers after writing the file
        proc.kill()
    if not png.exists():
        raise SystemExit("render failed: %s" % png.name)


def write_png(path, w, h, rgb):
    """8-bit RGB, not interlaced: what tools/design/pngstrict.py reads."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    if "--sizes" in args:
        for orient in ("portrait", "landscape"):
            for g in ORDER:   # hub_layout.h's tile order
                x, y, w, h = WALLS[orient][2][g]
                print("hub-%s-%s %d %d %d %d" % (g, orient, w, h, x, y))
        return 0
    ART.mkdir(parents=True, exist_ok=True)
    browser = None if "--svg" in args else find_browser()
    if not browser and "--svg" not in args:
        raise SystemExit("no Edge or Chrome found (set MOCKUP_BROWSER); --svg writes the vector sources only")
    tmp = tempfile.mkdtemp(prefix="games-hub-art-")
    try:
        for orient in ("portrait", "landscape"):
            w, h, tiles = WALLS[orient]
            doc = wall_svg(orient)
            (ART / ("wall-%s.svg" % orient)).write_text(doc, encoding="utf-8", newline="\n")
            if not browser:
                print("wall-%s.svg" % orient)
                continue
            shot = pathlib.Path(tmp) / ("wall-%s.png" % orient)
            shoot(browser, tmp, WRAP % doc, shot, w, h)
            png = read_png(str(shot))
            if (png.width, png.height) != (w, h):
                raise SystemExit("%s: rendered %dx%d, wanted %dx%d" % (orient, png.width, png.height, w, h))
            px = png.rgba
            if tuple(px[4:7]) != (0, 255, 0):
                raise SystemExit("%s: the fonts did not load (marker %r); the render is refused" % (orient, tuple(px[4:7])))
            for g, (x, y, tw, th) in tiles.items():
                rgb = bytearray()
                for row in range(y, y + th):
                    line = px[(row * w + x) * 4:(row * w + x + tw) * 4]
                    for i in range(0, len(line), 4):
                        rgb += line[i:i + 3]
                out = ART / ("hub-%s-%s.png" % (g, orient))
                write_png(out, tw, th, bytes(rgb))
                print("%-36s %3d x %3d" % (out.name, tw, th))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
