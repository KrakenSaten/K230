#!/usr/bin/env python3
# Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0
"""Games Hub design exploration: generates the six concept mockups.

DESIGN MOCKUPS ONLY. Nothing here is built into Doors and none of the output
is a screenshot of implemented software. Writes SVG sources next to this
package (../mockups) and, when Microsoft Edge or Chrome is found, renders
them to PNG at the panel's own size (568 x 1232 and 1232 x 568).

    python docs/design/games-hub/tools/gen_mockups.py
"""
import math
import os
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE.parent / "mockups"

# The `doors` theme, Normal mode (docs/design/themes.json).
BG, SURF, RAISED, LINE = "#0b0b0a", "#161513", "#211f1c", "#36332e"
TEXT, SEC, MUTED, ACC = "#eeeae2", "#a8a399", "#6f6b63", "#d8bf94"
# Hub-owned colours (fixed in every theme, like the environment palette, DS 31.1).
MINT, CORAL, AMBER, RED, LOBBY = "#a5f0d2", "#ff8a6a", "#e0a83a", "#e5502e", "#8c9aff"
CREAM = "#fff1d6"

SANS = "'IBM Plex Sans','Segoe UI',sans-serif"
MONO = "'IBM Plex Mono',Consolas,monospace"
DISP = "Bahnschrift,'IBM Plex Sans Condensed','Arial Narrow',sans-serif"
IMPACT = "Impact,'Arial Black',sans-serif"

_n = [0]


def uid():
    _n[0] += 1
    return "u%d" % _n[0]


def T(x, y, s, size, fill=TEXT, font=SANS, w=400, anchor="start", ls=0, extra=""):
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
    grad("gFadeDown", [(0, BG, 0), (.55, BG, .78), (1, BG, 1)]),
    grad("gFadeRight", [(0, BG, 0), (1, BG, 1)], "h"),
    grad("gTop", [(0, "#000", .7), (1, "#000", 0)]),
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
    grad("gPlastic", [(0, "#2a2723", 1), (1, "#171513", 1)]),
    '<pattern id="pScan" width="4" height="4" patternUnits="userSpaceOnUse">'
    '<rect width="4" height="1" fill="#000" opacity=".28"/></pattern>',
    '<pattern id="pTape" width="28" height="28" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
    '<rect width="14" height="28" fill="#8c9aff" opacity=".10"/></pattern>',
]) + "</defs>"

# ---------------------------------------------------------------- pixel type
PIX = {
    "R": "11110 10001 10001 11110 10100 10010 10001", "E": "11111 10000 10000 11110 10000 10000 11111",
    "L": "10000 10000 10000 10000 10000 10000 11111", "A": "01110 10001 10001 11111 10001 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100", "N": "10001 11001 10101 10011 10001 10001 10001",
    "I": "11111 00100 00100 00100 00100 00100 11111", "9": "01110 10001 10001 01111 00001 00001 01110",
    "0": "01110 10001 10011 10101 11001 10001 01110", " ": "00000 " * 7,
}


def pix(text, x, y, s, fill, extra=""):
    o = ['<g fill="%s" %s>' % (fill, extra)]
    for i, ch in enumerate(text):
        for r, row in enumerate(PIX[ch].split()):
            for c, bit in enumerate(row):
                if bit == "1":
                    o.append('<rect x="%s" y="%s" width="%s" height="%s"/>' % (x + (i * 6 + c) * s, y + r * s, s, s))
    return "".join(o) + "</g>"


def relay_logo(x, y, s):
    """Node mark + RELAY / NINE, after the game's own title card."""
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


def rotor_logo(x, y, size, one_line=False):
    """ROTOR / TERROR, heavy italic; baked into art on the device, not a UI font."""
    a = 'font-style="italic"'
    if one_line:
        return ('<g transform="translate(%s %s) skewX(-8)">' % (x, y)
                + T(0, 0, 'ROTOR<tspan fill="%s">TERROR</tspan>' % CORAL, size, CREAM, IMPACT, 400, ls=1, extra=a)
                + "</g>")
    return ('<g transform="translate(%s %s) skewX(-8)">' % (x, y)
            + T(0, 0, "ROTOR", size, CREAM, IMPACT, 400, ls=1, extra=a)
            + T(0, size * .9, "TERROR", size, CORAL, IMPACT, 400, ls=1, extra=a) + "</g>")


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
    s.append('<polygon points="0,420 60,360 120,400 190,330 260,400 330,350 410,410 480,340 560,390 600,360 600,470 0,470" fill="#3c2358"/>')
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
    # the sentry in the doorway
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
        s.append('<path d="M%s %s H200" stroke="#5cc8ff" stroke-width="3" stroke-dasharray="20 12" opacity="%s"/>' % (i * 9 - 10, y, .7 - i * .18))
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


def _card(x, y, rank, suit, col, rot, w=58, h=84):
    return ('<g transform="rotate(%s %s %s)">' % (rot, x + w / 2, y + h)
            + '<rect x="%s" y="%s" width="%s" height="%s" rx="7" fill="#fbf7ee" stroke="#00000033"/>' % (x, y, w, h)
            + T(x + 7, y + 22, rank, 20, col, SANS, 700)
            + T(x + w / 2, y + h * .72, suit, 38, col, "'Segoe UI Symbol',sans-serif", 400, "middle") + "</g>")


def art_solitaire():
    return (R(0, 0, 200, 200, "url(#gSol)")
            + '<rect x="14" y="14" width="172" height="172" rx="12" fill="none" stroke="#bff0d0" stroke-width="1.5" opacity=".3"/>'
            + _card(44, 56, "Q", "&#9829;", "#c8102e", -20) + _card(71, 50, "J", "&#9827;", "#1a1a1a", 0)
            + _card(98, 56, "K", "&#9824;", "#1a1a1a", 20))


def art_blackjack():
    return (R(0, 0, 200, 200, "url(#gBj)")
            + '<path d="M-10 170 Q100 110 210 170" fill="none" stroke="#ffd166" stroke-width="2" opacity=".5"/>'
            + _card(40, 44, "A", "&#9824;", "#1a1a1a", -10) + _card(92, 50, "K", "&#9829;", "#c8102e", 9)
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
    s.append(T(100, 118, "&#9824;", 50, "#f2c14e", "'Segoe UI Symbol',sans-serif", 400, "middle"))
    return "".join(s)


def art_2048():
    s = [R(0, 0, 200, 200, "url(#g2048)")]
    tiles = [(36, 36, "#fff6de", "2", 30, "#5a3a10"), (104, 36, "#ffe7b0", "4", 30, "#5a3a10"),
             (36, 104, "#ff8f4d", "8", 30, "#fff6de"), (104, 104, "#3a2408", "2048", 20, "#ffd166")]
    for x, y, c, t, fs, tc in tiles:
        s.append(R(x, y, 60, 60, c, 9) + T(x + 30, y + 30 + fs * .36, t, fs, tc, SANS, 700, "middle"))
    return "".join(s)


# id, name, genre, accent, art, viewBox - the nine games of the launcher's Games folder
# (ui/shell/home_layout.c); Relay Nine's internal id is neon-outpost.
G = {
    "rotorterror": ("RotorTerror", "ARCADE ACTION", CORAL, art_rotor, (600, 600)),
    "relay": ("Relay Nine", "SCI-FI SHOOTER", AMBER, art_relay, (600, 600)),
    "fleet": ("Fleet", "NAVAL TACTICS", "#5cc8ff", art_fleet, (200, 200)),
    "radar": ("Radar", "SENSOR OPS", "#5dff9b", art_radar, (200, 200)),
    "timber": ("Timber", "BLOCK TOWER", "#e9b06a", art_timber, (200, 200)),
    "solitaire": ("Solitaire", "KLONDIKE", "#bff0d0", art_solitaire, (200, 200)),
    "blackjack": ("Blackjack", "CARD TABLE", "#ffd166", art_blackjack, (200, 200)),
    "poker": ("Poker", "HOLD'EM", "#f2c14e", art_poker, (200, 200)),
    "2048": ("2048", "PUZZLE", "#ffd166", art_2048, (200, 200)),
}
_cache = {}


def art(g, x, y, w, h, r=0, align="xMidYMid", box=None):
    if g not in _cache:
        _cache[g] = G[g][3]()
    return place(_cache[g], G[g][4], x, y, w, h, r, align, box)


# ---------------------------------------------------------------- components
def status(w, inset, crumb=None):
    s = T(inset, 35, "DOORS", 14, TEXT, MONO, 500, ls=2.4)
    if crumb:
        s += T(inset + 78, 35, "/ " + crumb, 14, SEC, MONO, 400, ls=2.4)
    return s + T(w - inset, 35, "LORA &#183; WIFI &#183; 87%", 14, SEC, MONO, 400, "end", 1.2)


def back(x, y):
    return '<path d="M%s %s l-10 10 l10 10" fill="none" stroke="%s" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>' % (x + 10, y - 10, TEXT)


def chip(x, y, label, fill, fg="#0b0b0a", w=None):
    w = w or (len(label) * 9.6 + 22)
    return R(x, y, w, 26, fill, 13) + T(x + w / 2, y + 18, label, 14, fg, MONO, 500, "middle", 1.2)


def play(x, y, w, h, fill, label="PLAY", size=22):
    cx = x + w / 2
    tw = len(label) * size * .72
    return (R(x, y, w, h, fill, 10)
            + '<polygon points="%s,%s %s,%s %s,%s" fill="#0b0b0a"/>'
            % (cx - tw / 2 - 26, y + h / 2 - 10, cx - tw / 2 - 26, y + h / 2 + 10, cx - tw / 2 - 9, y + h / 2)
            + T(cx + 8, y + h / 2 + size * .36, label, size, "#0b0b0a", MONO, 500, "middle", size * .16))


def ghost(x, y, w, h, label, col=MUTED):
    lx = x + w / 2 - (len(label) * 10.4) / 2 - 12
    lock = ('<rect x="%s" y="%s" width="12" height="10" rx="2" fill="%s"/>'
            '<path d="M%s %s v-4 a4 4 0 0 1 8 0 v4" fill="none" stroke="%s" stroke-width="2"/>'
            % (lx, y + h / 2 - 3, col, lx + 2, y + h / 2 - 3, col))
    return ('<rect x="%s" y="%s" width="%s" height="%s" rx="8" fill="none" stroke="%s" stroke-width="1.5" stroke-dasharray="6 5"/>'
            % (x, y, w, h, col) + lock + T(x + w / 2 + 10, y + h / 2 + 5.5, label, 16, col, MONO, 500, "middle", 1.6))


def devices(cx, cy, col, k=1.0):
    """Two handhelds and the link that is not there yet."""
    def unit(x):
        return ('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="#0b0b0a" stroke="%s" stroke-width="2.5"/>'
                % (x, cy - 38 * k, 40 * k, 76 * k, 7 * k, col)
                + R(x + 6 * k, cy - 30 * k, 28 * k, 46 * k, col, 2, 'opacity=".22"')
                + '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (x + 20 * k, cy + 27 * k, 3.5 * k, col))
    a, b = cx - 62 * k, cx + 22 * k
    s = unit(a) + unit(b)
    s += ('<path d="M%s %s H%s" stroke="%s" stroke-width="2.5" stroke-dasharray="3 7" stroke-linecap="round"/>'
          % (a + 46 * k, cy, b - 6 * k, col))
    return s


def slots(x, y, col, n=4):
    return "".join('<circle cx="%s" cy="%s" r="6" fill="none" stroke="%s" stroke-width="2"/>' % (x + i * 20, y, col)
                   for i in range(n))


def focus(x, y, w, h, r=10):
    return ('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="%s" stroke-width="3"/>'
            % (x - 5, y - 5, w + 10, h + 10, r + 4, TEXT))


def svg(w, h, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s%s%s</svg>'
            % (w, h, w, h, DEFS, R(0, 0, w, h, BG), body))


# ------------------------------------------------------- concept A: MARQUEE
def lib_card(g, x, y, w, ah, star=False, note=None):
    s = art(g, x, y, w, ah, 10) + T(x + 2, y + ah + 22, G[g][0], 16, TEXT, SANS, 400)
    if star:
        s += '<circle cx="%s" cy="%s" r="5" fill="%s"/>' % (x + w - 12, y + 12, AMBER)
    if note:
        s += R(x + 6, y + ah - 24, len(note) * 8.4 + 10, 18, "#0b0b0a", 4, 'opacity=".85"') + T(x + 11, y + ah - 10, note, 12, TEXT, MONO, 500, ls=.6)
    return s


A_ORDER = ["relay", "fleet", "radar", "timber", "solitaire", "blackjack", "poker", "2048"]


def a_portrait():
    s = [art("rotorterror", 0, 0, 568, 700), R(0, 0, 568, 130, "url(#gTop)"), R(0, 330, 568, 372, "url(#gFadeDown)"),
         status(568, 30), back(30, 92), T(58, 98, "GAMES", 16, TEXT, MONO, 500, ls=3)]
    s.append(chip(30, 388, "FEATURED", CORAL) + T(140, 406, "ARCADE ACTION &#183; 1 PLAYER", 14, TEXT, MONO, 400, ls=1.4))
    s.append(rotor_logo(34, 500, 82))
    s.append(T(30, 604, "An endless horizon. One very loud helicopter.", 19, TEXT, SANS))
    s.append(play(30, 624, 300, 68, MINT))
    s.append(T(354, 650, "BEST", 14, SEC, MONO, ls=1.4) + T(354, 680, "128 400", 24, TEXT, MONO))
    s.append(R(486, 664, 26, 6, CORAL, 3) + R(520, 664, 6, 6, MUTED, 3) + T(538, 650, "1 / 2", 14, SEC, MONO, 400, "end", 1.2))
    s.append(T(26, 744, "LIBRARY", 14, SEC, MONO, ls=1.6) + T(542, 744, "8 MORE GAMES", 14, MUTED, MONO, 400, "end", 1.4))
    for i, g in enumerate(A_ORDER):
        s.append(lib_card(g, 26 + (i % 4) * 132, 760 + (i // 4) * 142, 120, 104, star=(g == "relay")))
    y = 1050
    s.append('<rect x="26" y="%s" width="516" height="152" rx="12" fill="#101223" stroke="#2b3162"/>' % y)
    s.append(R(26, y, 516, 152, "url(#pTape)", 12))
    s.append(devices(104, y + 78, LOBBY) + slots(74, y + 132, "#4a5296"))
    s.append(T(184, y + 36, "MULTIPLAYER LOBBY", 16, TEXT, MONO, 500, ls=1.6) + chip(400, y + 16, "COMING SOON", LOBBY, w=126))
    s.append(T(184, y + 64, "Play together on nearby DOORS devices.", 16, SEC, SANS))
    s.append(ghost(184, y + 86, 164, 48, "HOST GAME", "#6a73b8") + ghost(360, y + 86, 164, 48, "JOIN GAME", "#6a73b8"))
    return svg(568, 1232, "".join(s))


def a_landscape():
    s = [art("rotorterror", 0, 0, 730, 568), R(0, 0, 730, 110, "url(#gTop)"), R(0, 250, 730, 318, "url(#gFadeDown)"),
         R(520, 0, 212, 568, "url(#gFadeRight)"), status(1232, 50), back(50, 86), T(78, 92, "GAMES", 16, TEXT, MONO, 500, ls=3)]
    s.append(chip(50, 292, "FEATURED", CORAL) + T(160, 310, "ARCADE ACTION &#183; 1 PLAYER", 14, TEXT, MONO, 400, ls=1.4))
    s.append(rotor_logo(54, 376, 70))
    s.append(T(50, 470, "An endless horizon. One very loud helicopter.", 18, TEXT, SANS))
    s.append(play(50, 488, 270, 60, MINT) + focus(50, 488, 270, 60))
    s.append(T(346, 512, "BEST", 14, SEC, MONO, ls=1.4) + T(346, 540, "128 400", 24, TEXT, MONO))
    s.append(R(470, 530, 26, 6, CORAL, 3) + R(504, 530, 6, 6, MUTED, 3))
    s.append(T(744, 92, "LIBRARY", 14, SEC, MONO, ls=1.6) + T(1196, 92, "8 MORE GAMES", 14, MUTED, MONO, 400, "end", 1.4))
    for i, g in enumerate(A_ORDER):
        s.append(lib_card(g, 744 + (i % 4) * 116, 106 + (i // 4) * 124, 104, 86, star=(g == "relay"),
                          note="PORTRAIT" if g == "timber" else None))
    y = 362
    s.append('<rect x="744" y="%s" width="452" height="180" rx="12" fill="#101223" stroke="#2b3162"/>' % y)
    s.append(R(744, y, 452, 180, "url(#pTape)", 12))
    s.append(T(766, y + 38, "MULTIPLAYER LOBBY", 16, TEXT, MONO, 500, ls=1.6) + chip(1054, y + 18, "COMING SOON", LOBBY, w=126))
    s.append(T(766, y + 66, "Play together on nearby DOORS devices.", 16, SEC, SANS))
    s.append(devices(810, y + 124, LOBBY, .7))
    s.append(ghost(868, y + 98, 152, 52, "HOST GAME", "#6a73b8") + ghost(1030, y + 98, 150, 52, "JOIN GAME", "#6a73b8"))
    return svg(1232, 568, "".join(s))


# ------------------------------------------------- concept B: CARTRIDGE DECK
PLASTIC = "#e6dcc6"


def cart(g, x, y, w, h):
    s = R(x, y, w, h, "url(#gPlastic)", 8, 'stroke="#4a443c" stroke-width="1.5"')
    s += R(x + 12, y + 6, w - 24, 6, G[g][2], 3)
    s += R(x + 3, y + 30, 4, 26, "#0f0e0d", 2) + R(x + w - 7, y + 30, 4, 26, "#0f0e0d", 2)
    s += art(g, x + 11, y + 18, w - 22, h - 34, 5)
    s += R(x + 18, y + h - 10, w - 36, 5, "#0b0b0a", 2)
    return s


def cart_slot(x, y, w, h, label):
    return ('<rect x="%s" y="%s" width="%s" height="%s" rx="8" fill="none" stroke="%s" stroke-width="1.5" stroke-dasharray="5 5"/>'
            % (x, y, w, h, MUTED) + T(x + w / 2, y + h / 2 + 5, label, 14, MUTED, MONO, 400, "middle", 1.2))


def deck(x, y, w, h, sw, sh):
    """The console: a plastic bezel, the screen with the loaded game, its name and PLAY."""
    sx, sy = x + (w - sw) / 2, y + 22
    s = [R(x, y, w, h, "#1b1916", 22, 'stroke="#3d3830" stroke-width="2"'),
         R(sx - 8, sy - 8, sw + 16, sh + 16, "#080807", 12),
         art("relay", sx, sy, sw, sh, 6),
         T(sx + 16, sy + 28, "STATION 09 // EMERGENCY POWER", 14, CREAM, MONO, 400, ls=1.2, extra='opacity=".85"'),
         relay_logo(sx + 18, sy + sh - 100, 4 if sw < 500 else 4.4),
         R(sx, sy, sw, sh, "url(#pScan)", 6)]
    by = sy + sh + 26
    s.append('<circle cx="%s" cy="%s" r="5" fill="%s"/>' % (sx + 6, by + 8, AMBER))
    s.append(T(sx + 20, by + 13, "LOADED", 14, AMBER, MONO, 500, ls=1.6))
    s.append(T(sx, by + 46, "Relay Nine", 28, PLASTIC, SANS, 600))
    s.append(T(sx, by + 70, "SCI-FI SHOOTER &#183; 1 PLAYER", 14, SEC, MONO, 400, ls=1.2))
    bw = 176
    bx = sx + sw - bw
    s.append(R(bx, by + 8, bw, 66, "#8a5f14", 33) + play(bx, by, bw, 66, "#f0b23a").replace('rx="10"', 'rx="33"'))
    return "".join(s), (bx, by, bw, 66)


B_ROWS = [["rotorterror", "relay", "fleet", "radar", "timber"], ["solitaire", "blackjack", "poker", "2048", None]]


def shelf(x, y, pitch, cw, ch, landscape=False):
    s = []
    for r, row in enumerate(B_ROWS):
        ry = y + r * (ch + 54)
        for i, g in enumerate(row):
            cx = x + i * pitch
            if g == "relay":
                s.append(cart_slot(cx, ry, cw, ch, "IN DECK"))
            elif g is None:
                s.append(cart_slot(cx, ry, cw, ch, "EMPTY"))
            else:
                s.append(cart(g, cx, ry, cw, ch))
                if landscape and g == "timber":
                    s.append(R(cx + 14, ry + ch - 40, 78, 18, "#0b0b0a", 4, 'opacity=".85"')
                             + T(cx + 19, ry + ch - 26, "PORTRAIT", 12, TEXT, MONO, 500, ls=.6))
            if g:
                s.append(T(cx + cw / 2, ry + ch + 30, G[g][0], 16, AMBER if g == "relay" else SEC, SANS, 400, "middle"))
        wd = pitch * 4 + cw
        s.append(R(x - 8, ry + ch + 4, wd + 16, 6, "#3a342c", 2) + R(x - 8, ry + ch + 10, wd + 16, 30, "#141210", 0)
                 .replace("/>", ' opacity=".0"/>'))
    return "".join(s)


def b_portrait():
    d, _ = deck(20, 108, 528, 444, 480, 300)
    s = [status(568, 30), back(30, 82), T(58, 88, "GAME DECK", 16, TEXT, MONO, 500, ls=3),
         T(538, 88, "9 CARTRIDGES", 14, MUTED, MONO, 400, "end", 1.4), d]
    s.append(T(24, 590, "SHELF", 14, SEC, MONO, ls=1.6) + T(544, 590, "TAP A CARTRIDGE TO LOAD IT", 14, MUTED, MONO, 400, "end", 1.2))
    s.append(shelf(24, 606, 106, 96, 118))
    y = 966
    s.append('<rect x="20" y="%s" width="528" height="234" rx="16" fill="#12110f" stroke="%s"/>' % (y, LINE))
    s.append(T(44, y + 40, "MULTIPLAYER LOBBY", 16, PLASTIC, MONO, 500, ls=1.6) + chip(398, y + 20, "COMING SOON", AMBER, w=126))
    s.append(T(44, y + 68, "The link port is reserved for a future release.", 16, SEC, SANS))
    s.append(devices(284, y + 122, MUTED, .9))
    s.append(T(150, y + 120, "P1", 14, MUTED, MONO, 500, "end") + T(418, y + 120, "P2", 14, MUTED, MONO, 500))
    s.append('<circle cx="160" cy="%s" r="5" fill="none" stroke="%s" stroke-width="2"/>' % (y + 135, MUTED)
             + '<circle cx="408" cy="%s" r="5" fill="none" stroke="%s" stroke-width="2"/>' % (y + 135, MUTED))
    s.append(ghost(44, y + 168, 232, 50, "HOST GAME") + ghost(292, y + 168, 232, 50, "JOIN GAME"))
    return svg(568, 1232, "".join(s))


def b_landscape():
    d, _ = deck(30, 64, 560, 484, 512, 330)
    s = [status(1232, 50, "GAME DECK"), d]
    s.append(T(616, 84, "SHELF", 14, SEC, MONO, ls=1.6) + T(1198, 84, "9 CARTRIDGES", 14, MUTED, MONO, 400, "end", 1.4))
    s.append(shelf(616, 98, 119, 106, 100, True))
    s.append(focus(616, 98, 106, 100, 8))
    y = 410
    s.append('<rect x="610" y="%s" width="592" height="134" rx="14" fill="#12110f" stroke="%s"/>' % (y, LINE))
    s.append(devices(674, y + 68, MUTED, .75))
    s.append(T(748, y + 38, "MULTIPLAYER LOBBY", 16, PLASTIC, MONO, 500, ls=1.6) + chip(1058, y + 18, "COMING SOON", AMBER, w=126))
    s.append(T(748, y + 64, "The link port is reserved for a future release.", 16, SEC, SANS))
    s.append(ghost(748, y + 78, 210, 44, "HOST GAME") + ghost(970, y + 78, 214, 44, "JOIN GAME"))
    return svg(1232, 568, "".join(s))


# --------------------------------------------------- concept C: POSTER WALL
def poster(g, x, y, w, h, size=24, note=None):
    dark = g == "2048"
    s = art(g, x, y, w, h, 14)
    if not dark:
        s += R(x, y + h * .45, w, h * .55, "url(#gTile)", 14)
    if not dark:
        s += T(x + 12, y + h - 14, G[g][0].upper(), size, "#fff", DISP, 700, ls=.5,
                  extra='style="font-stretch:75%"')
    if note:
        s += R(x + 8, y + 8, len(note) * 8.4 + 10, 18, "#0b0b0a", 4, 'opacity=".85"') + T(x + 13, y + 22, note, 12, TEXT, MONO, 500, ls=.6)
    return s


def lobby_tile(x, y, w, h, wide=True):
    s = [R(x, y, w, h, "url(#gLobby)", 14), R(x, y, w, h, "url(#pTape)", 14),
         '<rect x="%s" y="%s" width="%s" height="%s" rx="14" fill="none" stroke="%s" stroke-width="2" stroke-dasharray="10 8"/>' % (x, y, w, h, LOBBY)]
    if wide:
        s.append(T(x + 22, y + 52, "MULTIPLAYER", 40, "#fff", DISP, 700, ls=1, extra='style="font-stretch:75%"'))
        s.append(T(x + 22, y + 92, "LOBBY", 40, LOBBY, DISP, 700, ls=1, extra='style="font-stretch:75%"'))
        s.append(chip(x + w - 148, y + 20, "COMING SOON", LOBBY, w=126))
        s.append(devices(x + w - 86, y + 100, "#c5ccff", .8))
        s.append(T(x + 22, y + 122, "Not available yet. Reserved for linked play.", 16, "#c5ccff", SANS))
        s.append(ghost(x + 22, y + h - 60, 150, 44, "HOST GAME", "#8c9aff") + ghost(x + 184, y + h - 60, 150, 44, "JOIN GAME", "#8c9aff"))
        s.append(slots(x + w - 124, y + h - 38, "#8c9aff"))
    else:
        s.append(T(x + 16, y + 40, "MULTIPLAYER LOBBY", 28, "#fff", DISP, 700, ls=.6, extra='style="font-stretch:75%"'))
        s.append(chip(x + 16, y + 54, "COMING SOON", LOBBY, w=126))
        s.append(devices(x + w - 58, y + 78, "#c5ccff", .55))
        s.append(ghost(x + 16, y + h - 54, 138, 40, "HOST GAME", "#8c9aff") + ghost(x + 164, y + h - 54, 138, 40, "JOIN GAME", "#8c9aff"))
    return "".join(s)


def c_portrait():
    s = [status(568, 30), back(28, 104), T(58, 118, "Games", 48, TEXT, SANS, 600, ls=-1),
         T(544, 100, "9 GAMES", 14, SEC, MONO, 400, "end", 1.6), T(544, 120, "2 FEATURED", 14, MUTED, MONO, 400, "end", 1.6)]
    s.append(art("rotorterror", 20, 140, 528, 300, 14) + R(20, 280, 528, 160, "url(#gTile)", 14))
    s.append(chip(34, 154, "FEATURED", CORAL) + rotor_logo(38, 372, 58) + play(398, 372, 134, 52, MINT, size=18))
    s.append(art("relay", 20, 450, 258, 310, 14, box=(-51, 367, 400, 400)) + R(20, 600, 258, 160, "url(#gTile)", 14))
    s.append(chip(34, 464, "FEATURED", AMBER) + relay_logo(38, 626, 3.5) + play(36, 702, 226, 46, "#f0b23a", size=18))
    for i, g in enumerate(["fleet", "radar", "timber", "2048"]):
        s.append(poster(g, 288 + (i % 2) * 135, 450 + (i // 2) * 160, 125, 150))
    for i, g in enumerate(["solitaire", "blackjack", "poker"]):
        s.append(poster(g, 20 + i * 179.5, 770, 169, 160))
    s.append(lobby_tile(20, 940, 528, 200))
    s.append(T(284, 1180, "TAP A POSTER TO PLAY &#183; HOLD FOR DETAILS", 14, MUTED, MONO, 400, "middle", 1.4))
    return svg(568, 1232, "".join(s))


def c_landscape():
    s = [status(1232, 50, "GAMES")]
    s.append(art("rotorterror", 24, 62, 390, 482, 14) + R(24, 300, 390, 244, "url(#gTile)", 14))
    s.append(chip(40, 78, "FEATURED", CORAL) + rotor_logo(44, 406, 60) + play(40, 484, 180, 46, MINT, size=18))
    s.append(T(398, 520, "ARCADE ACTION", 14, "#fff", MONO, 400, "end", 1.2))
    s.append(art("relay", 424, 62, 290, 482, 14) + R(424, 330, 290, 214, "url(#gTile)", 14))
    s.append(chip(440, 78, "FEATURED", AMBER) + relay_logo(442, 372, 4.8) + play(440, 484, 180, 46, "#f0b23a", size=18))
    for i, g in enumerate(["fleet", "radar", "timber", "solitaire", "blackjack", "poker"]):
        s.append(poster(g, 724 + (i % 3) * 164.5, 62 + (i // 3) * 162, 155, 152, 22, "PORTRAIT" if g == "timber" else None))
    s.append(poster("2048", 724, 386, 155, 158, 22))
    s.append(lobby_tile(888.5, 386, 319.5, 158, wide=False))
    s.append(focus(724, 62, 155, 152, 14))
    return svg(1232, 568, "".join(s))


MOCKUPS = [
    ("a-marquee-portrait", 568, 1232, a_portrait), ("a-marquee-landscape", 1232, 568, a_landscape),
    ("b-cartridge-deck-portrait", 568, 1232, b_portrait), ("b-cartridge-deck-landscape", 1232, 568, b_landscape),
    ("c-poster-wall-portrait", 568, 1232, c_portrait), ("c-poster-wall-landscape", 1232, 568, c_landscape),
]

WRAP = ('<!doctype html><html><head><meta charset="utf-8">'
        '<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;600;700&display=block" rel="stylesheet">'
        '<style>html,body{margin:0;background:#000;overflow:hidden}svg,img{display:block}%s</style></head><body>%s</body></html>')

SHEET_CSS = ("body{background:#050505;color:#eeeae2;font-family:'IBM Plex Sans','Segoe UI',sans-serif;width:2560px}"
             "h1{font-size:44px;margin:40px 48px 4px}p{margin:0 48px 28px;color:#a8a399;font-size:22px}"
             ".row{display:flex;gap:40px;align-items:flex-start;padding:0 48px 48px}"
             ".c{flex:1}.c h2{font-family:'IBM Plex Mono',Consolas,monospace;font-size:24px;letter-spacing:3px;margin:0 0 16px}"
             ".p{display:flex;gap:24px;align-items:flex-start}"
             ".c img{border-radius:34px;border:3px solid #2a2824}.pt{width:284px}.ls{width:468px}")


def find_browser():
    cands = [os.environ.get("MOCKUP_BROWSER"), r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
             r"C:\Program Files\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Google\Chrome\Application\chrome.exe",
             shutil.which("chromium"), shutil.which("google-chrome"), shutil.which("msedge")]
    return next((c for c in cands if c and os.path.exists(c)), None)


def shoot(browser, tmp, html, png, w, h):
    page = pathlib.Path(tmp) / (png.stem + ".html")
    page.write_text(html, encoding="utf-8")
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--user-data-dir=" + str(pathlib.Path(tmp) / "profile"), "--virtual-time-budget=6000",
                    "--window-size=%d,%d" % (w, h), "--screenshot=" + str(png), page.as_uri()],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=45)
    except subprocess.TimeoutExpired:  # Edge sometimes lingers after writing the file
        proc.kill()
    if not png.exists():
        raise SystemExit("render failed: %s" % png.name)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    browser = find_browser()
    tmp = tempfile.mkdtemp(prefix="games-hub-")
    for name, w, h, fn in MOCKUPS:
        doc = fn()
        (OUT / (name + ".svg")).write_text(doc, encoding="utf-8")
        if browser:
            shoot(browser, tmp, WRAP % ("", doc), OUT / (name + ".png"), w, h)
        print(name, "ok" if browser else "svg only (no browser found)")
    if browser:
        cells = []
        for title, key in (("A  MARQUEE", "a-marquee"), ("B  CARTRIDGE DECK", "b-cartridge-deck"), ("C  POSTER WALL", "c-poster-wall")):
            cells.append('<div class="c"><h2>%s</h2><div class="p"><img class="pt" src="%s"><img class="ls" src="%s"></div></div>'
                         % (title, (OUT / (key + "-portrait.png")).as_uri(), (OUT / (key + "-landscape.png")).as_uri()))
        body = ("<h1>DOORS Games Hub &#8212; three concepts</h1><p>Design mockups for owner review. Not screenshots of "
                "implemented DOORS software. Panel 568 &#215; 1232, shown at half size.</p><div class='row'>%s</div>" % "".join(cells))
        shoot(browser, tmp, WRAP % (SHEET_CSS, body), OUT / "overview.png", 2560, 860)
        print("overview ok")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
