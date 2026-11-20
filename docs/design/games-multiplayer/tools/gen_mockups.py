#!/usr/bin/env python3
# Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0
"""Multiplayer Lobby design package (M1): generates the mockups.

DESIGN MOCKUPS ONLY. Nothing here is built into Doors and none of the output
is a screenshot of implemented software; every frame carries a MOCKUP tag.
Writes SVG sources to ../mockups and, when Microsoft Edge or Chrome is found,
renders them to PNG at the panel's own size (568 x 1232 and 1232 x 568).
Follows docs/design/games-hub/tools/gen_mockups.py (same renderer, same
palette); the small game tiles are the hub's own committed art.

    python docs/design/games-multiplayer/tools/gen_mockups.py
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE.parent / "mockups"
HUB_ART = "../../games-hub/art/"          # relative to OUT

# The hub's fixed palette (DS 60.2).
BG, SURF, RAISED, LINE = "#0b0b0a", "#161513", "#211f1c", "#36332e"
TEXT, SEC, MUTED = "#eeeae2", "#a8a399", "#6f6b63"
MINT, CORAL, AMBER, P, PL = "#a5f0d2", "#ff8a6a", "#f0b23a", "#8c9aff", "#c5ccff"
# Proposed for the lobby: three indigo grounds under the hub's lobby blue.
I0, I1, I2, INK = "#12143a", "#1b1f55", "#2a2f78", "#10122e"
CYAN = "#5cc8ff"

SANS = "'IBM Plex Sans','Segoe UI',sans-serif"
MONO = "'IBM Plex Mono',Consolas,monospace"
COND = "'IBM Plex Sans Condensed',Bahnschrift,'Arial Narrow',sans-serif"

_n = [0]


def uid():
    _n[0] += 1
    return "m%d" % _n[0]


def T(x, y, s, size, fill=TEXT, font=SANS, w=600, anchor="start", ls=0, extra=""):
    return ('<text x="%s" y="%s" font-family="%s" font-size="%s" font-weight="%s" fill="%s" '
            'text-anchor="%s" letter-spacing="%s" %s>%s</text>' % (x, y, font, size, w, fill, anchor, ls, extra, s))


def sans(x, y, s, size=16, fill=TEXT, w=600, anchor="start"):
    return T(x, y, s, size, fill, SANS, w, anchor)


def mono(x, y, s, size=14, fill=SEC, w=400, anchor="start", ls=1.5):
    return T(x, y, s, size, fill, MONO, w, anchor, ls)


def cond(x, y, s, size, fill=TEXT, anchor="start", ls=0.5):
    return T(x, y, s, size, fill, COND, 700, anchor, ls)


def R(x, y, w, h, fill, rx=0, extra=""):
    return '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s" %s/>' % (x, y, w, h, rx, fill, extra)


def defs():
    return ('<defs>'
            '<linearGradient id="lob" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="%s"/></linearGradient>'
            '<linearGradient id="sea" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#0a1c36"/><stop offset=".62" stop-color="#123a66"/><stop offset="1" stop-color="#0b2444"/></linearGradient>'
            '<linearGradient id="scrim" x1="0" y1="0" x2="0" y2="1"><stop offset=".45" stop-color="#040a14" stop-opacity="0"/><stop offset="1" stop-color="#040a14" stop-opacity=".88"/></linearGradient>'
            '<pattern id="stripe" width="26" height="26" patternUnits="userSpaceOnUse" patternTransform="rotate(-35)"><rect width="12" height="26" fill="%s" opacity=".07"/></pattern>'
            '<radialGradient id="glow"><stop offset="0" stop-color="%s" stop-opacity=".5"/><stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>'
            '</defs>' % (I1, I0, PL, P, P))


# ---- small parts ----------------------------------------------------------

def chip(x, y, label, fill=None, tc=TEXT, stroke=None, dot=None, anchor="start"):
    w = int(len(label) * 9.9 + 24 + (16 if dot else 0))
    if anchor == "end":
        x -= w
    elif anchor == "mid":
        x -= w // 2
    s = R(x, y, w, 28, fill or "none", 14, 'stroke="%s" stroke-width="1.5"' % stroke if stroke else "")
    tx = x + 12
    if dot:
        s += '<circle cx="%s" cy="%s" r="4.5" fill="%s"/>' % (x + 15, y + 14, dot)
        tx += 16
    return s + mono(tx, y + 19, label, 14, tc, 500)


def chip_w(label, dot=False):
    return int(len(label) * 9.9 + 24 + (16 if dot else 0))


def slab(x, y, w, h, label, fill, tc, stroke=None, size=22, dash=False):
    ex = ""
    if stroke:
        ex = 'stroke="%s" stroke-width="2"%s' % (stroke, ' stroke-dasharray="7 6"' if dash else "")
    return (R(x, y, w, h, fill, 12, ex)
            + T(x + w / 2 + 2, y + h / 2 + size * 0.36, label, size, tc, MONO, 500, "middle", 3.5))


def avatar(cx, cy, r, initials, hue, ring=None):
    s = ""
    if ring:
        s += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="3"/>' % (cx, cy, r + 6, ring)
    s += '<circle cx="%s" cy="%s" r="%s" fill="%s"/>' % (cx, cy, r, hue)
    return s + T(cx, cy + r * 0.36, initials, r * 0.95, INK, COND, 700, "middle", 0.5)


def ico_lan(cx, cy, c, k=1.0):
    g = '<g transform="translate(%s %s) scale(%s)" fill="none" stroke="%s" stroke-width="3.4" stroke-linecap="round">' % (cx, cy, k, c)
    for r in (21, 13.5, 6):
        g += '<path d="M%.1f %.1f A%s %s 0 0 1 %.1f %.1f"/>' % (-r * 0.74, 8 - r * 0.67, r, r, r * 0.74, 8 - r * 0.67)
    return g + '<circle cx="0" cy="10" r="2.6" fill="%s" stroke="none"/></g>' % c


def ico_lora(cx, cy, c, k=1.0):
    return ('<g transform="translate(%s %s) scale(%s)" fill="none" stroke="%s" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round">'
            '<path d="M0 -8 L-9 17 M0 -8 L9 17 M-5.5 7 L5.5 7"/><circle cx="0" cy="-10" r="2.8" fill="%s" stroke="none"/>'
            '<path d="M-9 -18 A12 12 0 0 0 -9 -2"/><path d="M9 -18 A12 12 0 0 1 9 -2"/>'
            '<path d="M-16 -22 A20 20 0 0 0 -16 2"/><path d="M16 -22 A20 20 0 0 1 16 2"/></g>' % (cx, cy, k, c, c))


def ico_globe(cx, cy, c, k=1.0):
    return ('<g transform="translate(%s %s) scale(%s)" fill="none" stroke="%s" stroke-width="3">'
            '<circle r="17"/><ellipse rx="7.5" ry="17"/><path d="M-17 0 H17 M-14.5 -9 H14.5 M-14.5 9 H14.5"/></g>' % (cx, cy, k, c))


ICONS = {"lan": ico_lan, "lora": ico_lora, "online": ico_globe}


def check(cx, cy, c, k=1.0):
    return ('<path transform="translate(%s %s) scale(%s)" d="M-6 0 L-2 4.5 L6.5 -5" fill="none" stroke="%s" stroke-width="2.6" '
            'stroke-linecap="round" stroke-linejoin="round"/>' % (cx, cy, k, c))


def chev(cx, cy, c, k=1.0):
    return ('<path transform="translate(%s %s) scale(%s)" d="M-4 -8 L4 0 L-4 8" fill="none" stroke="%s" stroke-width="3" '
            'stroke-linecap="round" stroke-linejoin="round"/>' % (cx, cy, k, c))


def star(cx, cy, c, fill="none"):
    return ('<path transform="translate(%s %s)" d="M0 -9 L2.6 -3 L9 -2.6 L4.2 1.8 L5.6 8.4 L0 5 L-5.6 8.4 L-4.2 1.8 L-9 -2.6 L-2.6 -3 Z" '
            'fill="%s" stroke="%s" stroke-width="1.6" stroke-linejoin="round"/>' % (cx, cy, fill, c))


def ticket(x, y, w, h, fill, stroke=None, perf=(), r=18, sw=2, inner=""):
    """A ticket: a rounded card with a perforation and its two notches.
    perf: ('h', dy) across at y+dy, ('v', dx) down at x+dx. On the device the
    notches are discs in the ground colour, the perforation a dashed line."""
    c = uid()
    st = 'stroke="%s" stroke-width="%s"' % (stroke, sw) if stroke else ""
    s = '<clipPath id="%s"><rect x="%s" y="%s" width="%s" height="%s" rx="%s"/></clipPath>' % (c, x, y, w, h, r)
    s += R(x, y, w, h, fill, r)
    s += '<g clip-path="url(#%s)">%s</g>' % (c, inner)
    s += R(x, y, w, h, "none", r, st)
    dash = stroke or LINE
    for kind, d in perf:
        if kind == "h":
            pts = ((x, y + d), (x + w, y + d))
            s += '<path d="M%s %s H%s" stroke="%s" stroke-width="2" stroke-dasharray="3 8" stroke-linecap="round" opacity=".7"/>' % (x + 18, y + d, x + w - 18, dash)
        else:
            pts = ((x + d, y), (x + d, y + h))
            s += '<path d="M%s %s V%s" stroke="%s" stroke-width="2" stroke-dasharray="3 8" stroke-linecap="round" opacity=".7"/>' % (x + d, y + 18, y + h - 18, dash)
        for px, py in pts:
            s += '<circle cx="%s" cy="%s" r="11" fill="%s" clip-path="url(#%s)"/>' % (px, py, BG, c)
            if stroke:
                s += '<circle cx="%s" cy="%s" r="11" fill="none" stroke="%s" stroke-width="%s" clip-path="url(#%s)"/>' % (px, py, stroke, sw, c)
    return s


def fleet_art(x, y, w, h, wordmark=0, hit=True):
    """Representative Fleet key art after the hub poster: a warship on a
    plotting grid. Production art would come from tools/design/gen_games_hub.py."""
    s = R(x, y, w, h, "url(#sea)")
    for gx in range(int(x) + 14, int(x + w), 28):
        s += '<path d="M%s %s V%s" stroke="%s" stroke-width="1" opacity=".16"/>' % (gx, y, y + h, CYAN)
    for gy in range(int(y) + 14, int(y + h), 28):
        s += '<path d="M%s %s H%s" stroke="%s" stroke-width="1" opacity=".16"/>' % (x, gy, x + w, CYAN)
    u = min(w * 0.62, h * 1.5) / 200.0
    cx, by = x + w * (0.5 if wordmark == 0 else 0.64), y + h * (0.60 if wordmark == 0 else 0.50)
    s += '<g transform="translate(%s %s) scale(%s)">' % (cx, by, u)
    s += '<path d="M-3 -62 V-34" stroke="#dfe8f0" stroke-width="3"/><path d="M-12 -52 H6" stroke="#dfe8f0" stroke-width="2"/>'
    s += '<rect x="-22" y="-36" width="38" height="18" fill="#dfe8f0"/><rect x="-42" y="-20" width="76" height="20" fill="#b9c8d8"/>'
    s += '<rect x="-78" y="-9" width="24" height="9" fill="#9fb3c8"/><rect x="50" y="-9" width="24" height="9" fill="#9fb3c8"/>'
    s += '<path d="M-104 0 H104 L82 24 H-86 Z" fill="#cdd9e5"/><path d="M-104 0 H104 L98 6 H-99 Z" fill="#eef3f8"/>'
    s += '<path d="M-120 34 H-70 M-52 34 H20 M40 34 H118" stroke="%s" stroke-width="3" opacity=".5"/></g>' % CYAN
    if hit:
        hx, hy, hr = x + w - min(52, w * 0.2), y + min(46, h * 0.28), min(17, w * 0.085)
        s += ('<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="3"/>'
              '<path d="M%s %s l%s %s M%s %s l%s %s" stroke="%s" stroke-width="3" stroke-linecap="round"/>'
              % (hx, hy, hr, CORAL, hx - hr * .5, hy - hr * .5, hr, hr, hx + hr * .5, hy - hr * .5, -hr, hr, CORAL))
    s += R(x, y, w, h, "url(#scrim)")
    if wordmark:
        s += cond(x + 20, y + h - 18, "FLEET", wordmark, "#ffffff", ls=1)
    return s


def tile(game, x, y, w, h, r=12, dim=False, orient="portrait"):
    c = uid()
    return ('<clipPath id="%s"><rect x="%s" y="%s" width="%s" height="%s" rx="%s"/></clipPath>'
            '<image href="%shub-%s-%s.png" x="%s" y="%s" width="%s" height="%s" preserveAspectRatio="xMidYMid slice" '
            'clip-path="url(#%s)" opacity="%s"/>' % (c, x, y, w, h, r, HUB_ART, game, orient, x, y, w, h, c, ".32" if dim else "1"))


def focus(x, y, w, h, r=18):
    return R(x - 5, y - 5, w + 10, h + 10, "none", r + 5, 'stroke="%s" stroke-width="3"' % TEXT)


# ---- the frame ------------------------------------------------------------

def frame(w, h, title, caption, body):
    land = w > h
    s = '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h)
    s += defs() + R(0, 0, w, h, BG)
    bw = 108 if land else 100
    s += '<path d="M0 0 H%s V60 a12 12 0 0 1 -12 12 H0 Z" fill="%s" stroke="%s"/>' % (bw, SURF, LINE)
    s += '<path d="M%s 26 l-10 10 l10 10" fill="none" stroke="%s" stroke-width="3.5" stroke-linecap="round" stroke-linejoin="round"/>' % (bw / 2 + 5, TEXT)
    cx = w - (137 if land else 117)
    s += R(cx, 14, 87, 44, SURF, 8, 'stroke="%s"' % LINE) + ico_lan(cx + 28, 34, SEC, 0.62)
    s += '<circle cx="%s" cy="36" r="4" fill="%s"/>' % (cx + 62, MINT)
    if land:
        s += sans(124, 46, title, 28) + mono(cx - 24, 42, caption, 14, SEC, anchor="end")
    else:
        s += sans(117, 54, title, 48) + mono(548, 100, caption, 14, SEC, anchor="end")
    s += body
    tag = "DESIGN MOCKUP · NOT A SCREENSHOT"
    if land:
        s += mono(w / 2, 561, tag, 11, MUTED, anchor="middle", ls=2)
    else:
        s += mono(w / 2, 1212, tag, 11, MUTED, anchor="middle", ls=2)
    return s + "</svg>"


def hint(text):
    return mono(284, 1160, text, 14, MUTED, anchor="middle", ls=2)


# ---- lobby components -----------------------------------------------------

def hero(x, y, w, h, ts=42):
    inner = R(x, y, w, h, "url(#lob)") + R(x, y, w, h, "url(#stripe)")
    s = ticket(x, y, w, h, I1, P, r=20, inner=inner)
    s += cond(x + 22, y + 18 + ts * 0.86, "MULTIPLAYER", ts) + cond(x + 22, y + 18 + ts * 1.84, "LOBBY", ts, P)
    s += avatar(x + w - 50, y + 50, 28, "AW", P, ring=MINT)
    s += sans(x + w - 94, y + 46, "Andre", 20, anchor="end") + mono(x + w - 94, y + 68, "K230-A · 3FA2", 14, PL, anchor="end")
    by = y + h - 46
    s += '<path d="M%s %s H%s" stroke="%s" opacity=".35"/>' % (x + 20, by, x + w - 20, PL)
    s += mono(x + 22, by + 29, "NEARBY", 14, PL, ls=2)
    s += avatar(x + 112, by + 24, 14, "AN", CORAL) + avatar(x + 144, by + 24, 14, "KE", MINT)
    s += chip(x + w - 18, by + 9, "VISIBLE", None, MINT, MINT, dot=MINT, anchor="end")
    return s


def host_ticket(x, y, w, h):
    s = ticket(x, y, w, h, P, perf=(("h", h - 60),))
    s += cond(x + 22, y + 90, "HOST", 82, INK, ls=1) + cond(x + 24, y + 128, "A GAME", 30, INK, ls=2)
    if w >= 300:
        s += '<circle cx="%s" cy="%s" r="21" fill="none" stroke="%s" stroke-width="3"/>' % (x + w - 44, y + 44, INK)
        s += '<path d="M%s %s h20 M%s %s v20" stroke="%s" stroke-width="3" stroke-linecap="round"/>' % (x + w - 54, y + 44, x + w - 44, y + 34, INK)
    s += mono(x + 22, y + h - 24, "OPEN A TABLE", 14, INK, 500, ls=2) + chev(x + w - 30, y + h - 29, INK)
    return s


def join_ticket(x, y, w, h):
    inner = R(x, y, w, h, "url(#lob)") + R(x, y, w, h, "url(#stripe)")
    s = ticket(x, y, w, h, I1, PL, perf=(("h", h - 60),), inner=inner)
    s += cond(x + 22, y + 90, "JOIN", 82, TEXT, ls=1) + cond(x + 24, y + 128, "A GAME", 30, PL, ls=2)
    s += chip(x + w - 18, y + (22 if w >= 300 else 106), "2 OPEN", MINT, INK, anchor="end")
    s += '<circle cx="%s" cy="%s" r="5" fill="%s"/>' % (x + 28, y + h - 29, AMBER)
    s += mono(x + 42, y + h - 24, "1 INVITATION", 14, AMBER, 500, ls=2) + chev(x + w - 30, y + h - 29, PL)
    return s


def resume_h(x, y, w, h, line2="YOUR TURN · SHOT 12", c2=MINT, line3="LORA · SAVED", btn="RESUME"):
    s = ticket(x, y, w, h, SURF, LINE, perf=(("v", 160),), inner=fleet_art(x, y, 160, h, hit=False))
    x0 = x + 182
    s += chip(x0, y + 18, "MATCH IN PROGRESS", None, MINT, MINT)
    s += sans(x0, y + 82, "Fleet · vs Anna", 28) + mono(x0, y + 110, line2, 16, c2, 500)
    s += mono(x0, y + 134, line3, 14, SEC)
    s += slab(x + w - 162, y + h - 62, 146, 46, btn, MINT, INK, size=20)
    return s


def resume_v(x, y, w, h):
    s = ticket(x, y, w, h, SURF, LINE, perf=(("h", 118),), inner=fleet_art(x, y, w, 118, wordmark=40))
    s += chip(x + 20, y + 134, "MATCH IN PROGRESS", None, MINT, MINT)
    s += sans(x + 20, y + 196, "Fleet · vs Anna", 28) + mono(x + 20, y + 222, "YOUR TURN · SHOT 12 · LORA", 16, MINT, 500)
    s += slab(x + 16, y + h - 64, w - 32, 50, "RESUME", MINT, INK)
    return s


CONN = {
    "lan": ("Local network", MINT, "INSTANT"),
    "lora": ("LoRa mesh", AMBER, "LONG RANGE"),
    "online": ("Online", MUTED, "LATER"),
}


def conn_row(x, y, w, h, kind, lines, chip_text=None, col=None, name=None):
    nm, c, ct = CONN[kind]
    c, ct, nm = col or c, chip_text or ct, name or nm
    off = kind == "online"
    s = R(x, y, w, h, SURF, 16, 'stroke="%s"' % LINE)
    s += R(x + 16, y + 16, 64, 64, c, 14, 'opacity=".14"') + ICONS[kind](x + 48, y + 48, c)
    s += sans(x + 96, y + 42, nm, 20, MUTED if off else TEXT)
    for i, ln in enumerate(lines):
        s += mono(x + 96, y + 66 + i * 22, ln, 14, MUTED if off else SEC)
    if h >= 130:
        s += chip(x + 96, y + h - 40, ct, None, c, c)
    else:
        s += chip(x + w - 16, y + 18, ct, None, c, c, anchor="end")
    return s


def game_ticket(x, y, w, h, art_h):
    s = ticket(x, y, w, h, SURF, PL, perf=(("h", art_h + 10),), sw=3, inner=fleet_art(x, y, w, art_h, wordmark=56))
    s += chip(x + 16, y + 16, "SELECTED", MINT, INK)
    ty = y + art_h + 10
    s += sans(x + 22, ty + 42, "Fleet", 28) + mono(x + 22, ty + 66, "2 PLAYERS · TURN-BASED · RESUMABLE", 14)
    s += chip(x + w - 18, ty + 18, "LORA", None, AMBER, AMBER, anchor="end")
    s += chip(x + w - 18 - chip_w("LORA") - 8, ty + 18, "LOCAL", None, MINT, MINT, anchor="end")
    return s


def mini_row(x, y, tw, th, gap):
    s = ""
    for i, g in enumerate(("fleet", "poker", "2048", "relay-nine", "rotorterror")):
        tx = x + i * (tw + gap)
        s += tile(g, tx, y, tw, th, 10, dim=i > 0)
        if i == 0:
            s += R(tx, y, tw, th, "none", 10, 'stroke="%s" stroke-width="3"' % PL)
        else:
            s += R(tx + tw / 2 - 27, y + th - 27, 54, 20, BG, 10, 'opacity=".8"')
            s += mono(tx + tw / 2 + 1, y + th - 13, "SOLO", 12, SEC, 500, "middle", 2)
    return s


def link_card(x, y, w, h, kind, lines, selected=False):
    nm, c, _ = CONN[kind]
    s = R(x, y, w, h, SURF, 16, 'stroke="%s" stroke-width="%s"' % (c if selected else LINE, 3 if selected else 1))
    if selected:
        s += R(x, y, w, h, c, 16, 'opacity=".07"')
    s += R(x + 16, y + 16, 64, 64, c, 14, 'opacity=".14"') + ICONS[kind](x + 48, y + 48, c)
    s += sans(x + 96, y + 40, nm, 20)
    for i, ln in enumerate(lines):
        s += mono(x + 96, y + 64 + i * 22, ln, 14, c if (i == 0 and selected) else SEC)
    s += '<circle cx="%s" cy="%s" r="12" fill="none" stroke="%s" stroke-width="2.5"/>' % (x + w - 34, y + 36, c if selected else MUTED)
    if selected:
        s += '<circle cx="%s" cy="%s" r="6" fill="%s"/>' % (x + w - 34, y + 36, c)
    return s


def online_slim(x, y, w, h):
    s = R(x, y, w, h, "none", 16, 'stroke="%s" stroke-dasharray="6 6"' % LINE)
    s += ico_globe(x + 48, y + h / 2, MUTED, 0.8) + sans(x + 96, y + h / 2 - 2, "Online", 20, MUTED)
    s += mono(x + 96, y + h / 2 + 20, "NOT AVAILABLE YET", 14, MUTED)
    return s


def invite_ticket(x, y, w, h, stub=130, bw=150, l1="FLEET · CLASSIC · LORA DIRECT"):
    s = ticket(x, y, w, h, SURF, AMBER, perf=(("v", stub),), inner=fleet_art(x, y, stub, h, hit=False))
    x0 = x + stub + 24
    s += chip(x0, y + 18, "INVITATION", AMBER, INK)
    s += avatar(x + w - 44, y + 34, 20, "AN", CORAL)
    s += sans(x0, y + 84, "Anna invites you", 28) + mono(x0, y + 108, l1, 14)
    s += mono(x0, y + 130, "RECEIVED 40 S AGO", 14, MUTED)
    s += slab(x0, y + h - 58, bw, 44, "ACCEPT", MINT, INK, size=18)
    s += slab(x0 + bw + 10, y + h - 58, max(bw - 20, 124), 44, "DECLINE", "none", SEC, LINE, size=18)
    return s


def table_ticket(x, y, w, h, initials, hue, who, l1, l2, c2=MINT, stale=False):
    s = ticket(x, y, w, h, SURF, LINE, perf=(("v", 100),), inner=tile("fleet", x, y, 100, h, 0))
    x0 = x + 122
    s += avatar(x0 + 18, y + 36, 18, initials, hue) + sans(x0 + 46, y + 43, who, 20)
    s += mono(x0, y + 76, l1, 14) + mono(x0, y + 100, l2, 14, c2, 500)
    if stale:
        s += chip(x + w - 16, y + h / 2 - 14, "NEEDS UPDATE", None, CORAL, CORAL, anchor="end")
        s = '<g opacity=".55">%s</g>' % s
    else:
        s += slab(x + w - 112, y + h / 2 - 24, 96, 48, "JOIN", P, INK, size=18)
    return s


def player_pill(x, y, w, initials, hue, name, sub, known=False):
    s = R(x, y, w, 72, SURF, 16, 'stroke="%s"' % LINE) + avatar(x + 38, y + 36, 22, initials, hue)
    s += sans(x + 72, y + 32, name, 20) + mono(x + 72, y + 54, sub, 14, AMBER)
    return s + star(x + w - 26, y + 26, AMBER if known else MUTED, AMBER if known else "none")


def code_slab(x, y, w, h=64):
    return slab(x, y, w, h, "ENTER TABLE CODE", "none", PL, PL)


def seat(x, y, w, h, initials, hue, name, sub, ready, ar=34):
    c = MINT if ready else AMBER
    s = R(x, y, w, h, RAISED, 16, 'stroke="%s" stroke-width="%s"' % (c if ready else LINE, 2 if ready else 1))
    if ready:
        s += R(x, y, w, h, MINT, 16, 'opacity=".06"')
    cy = y + 24 + ar
    s += avatar(x + w / 2, cy, ar, initials, hue, ring=c if ready else None)
    s += sans(x + w / 2, cy + ar + 38, name, 20, anchor="middle") + mono(x + w / 2, cy + ar + 60, sub, 14, anchor="middle")
    label = "READY" if ready else "GETTING READY"
    cw = chip_w(label, True)
    cx = x + w / 2 - cw / 2
    cyy = cy + ar + 76
    if ready:
        s += R(cx, cyy, cw, 28, MINT, 14) + check(cx + 16, cyy + 14, INK) + mono(cx + 28, cyy + 19, label, 14, INK, 500)
    else:
        s += chip(cx, cyy, label, None, AMBER, AMBER, dot=AMBER)
    return s


def table_head(x, y, w, art_h):
    s = fleet_art(x, y, w, art_h, wordmark=64)
    s += chip(x + 16, y + 16, "CLASSIC · 2 SEATS", INK, PL)
    return s


def code_row(x, y, w):
    s = mono(x + 24, y + 30, "TABLE CODE", 14, SEC, ls=2) + T(x + 22, y + 84, "K7QM", 48, TEXT, SANS, 600, "start", 8)
    s += R(x + w - 24 - 56, y + 14, 56, 56, MINT, 14, 'opacity=".14"') + ico_lan(x + w - 52, y + 42, MINT, 0.9)
    s += mono(x + w - 92, y + 36, "LOCAL NETWORK", 14, MINT, 500, "end") + mono(x + w - 92, y + 58, "KRAKEN-5G", 14, SEC, anchor="end")
    s += mono(x + w - 24, y + 92, "VISIBLE NEARBY", 14, SEC, anchor="end")
    return s


# ---- the eight final screens ----------------------------------------------

def lobby_portrait():
    b = hero(20, 116, 528, 200)
    b += host_ticket(20, 330, 259, 230) + join_ticket(289, 330, 259, 230)
    b += resume_h(20, 576, 528, 184)
    b += mono(20, 798, "CONNECTIONS", 14, SEC, ls=2)
    b += conn_row(20, 812, 528, 96, "lan", ["WI-FI · KRAKEN-5G · 2 NEARBY"])
    b += conn_row(20, 918, 528, 96, "lora", ["MESH ONLINE · 3 HEARD · AIRTIME 12/90 S"])
    b += conn_row(20, 1024, 528, 96, "online", ["NOT AVAILABLE YET"])
    b += hint("ENTER SELECT · ESC BACK")
    return frame(568, 1232, "Lobby", "MULTIPLAYER · 2 NEARBY", b)


def lobby_landscape():
    b = host_ticket(30, 76, 378, 229) + join_ticket(30, 315, 378, 229) + focus(30, 76, 378, 229)
    b += hero(418, 76, 382, 150, ts=30) + resume_v(418, 236, 382, 308)
    b += conn_row(810, 76, 392, 149, "lan", ["WI-FI · KRAKEN-5G", "2 PLAYERS NEARBY"])
    b += conn_row(810, 235, 392, 150, "lora", ["MESH ONLINE · 3 HEARD", "AIRTIME 12 / 90 S THIS HOUR"])
    b += conn_row(810, 395, 392, 149, "online", ["NOT AVAILABLE YET"])
    return frame(1232, 568, "Lobby", "MULTIPLAYER · 2 NEARBY", b)


LAN_LINES = ["WI-FI · KRAKEN-5G", "OPEN TABLE · INSTANT MOVES · NO INTERNET"]
LORA_LINES = ["MESH ONLINE · 3 PLAYERS HEARD", "DIRECT INVITE · ABOUT 5 S A MOVE", "AIRTIME 12 / 90 S THIS HOUR"]


def host_portrait():
    b = mono(20, 132, "1 · GAME", 14, PL, 500, ls=2) + mono(548, 132, "1 OF 9 GAMES PLAYS LINKED", 14, MUTED, anchor="end")
    b += game_ticket(20, 146, 528, 262, 170) + mini_row(20, 422, 96, 108, 12)
    b += mono(20, 572, "2 · LINK", 14, PL, 500, ls=2)
    b += link_card(20, 586, 528, 132, "lan", LAN_LINES, True) + link_card(20, 728, 528, 132, "lora", LORA_LINES)
    b += online_slim(20, 870, 528, 68)
    b += mono(20, 976, "3 · TABLE", 14, PL, 500, ls=2)
    b += slab(20, 990, 528, 76, "OPEN TABLE", P, INK)
    b += mono(284, 1096, "FLEET · CLASSIC · 2 SEATS · LOCAL NETWORK", 14, SEC, anchor="middle")
    b += hint("ARROWS CHOOSE · ENTER OPENS · ESC BACK")
    return frame(568, 1232, "Host", "STEP 1 OF 2 · SET UP", b)


def host_landscape():
    b = mono(30, 94, "1 · GAME", 14, PL, 500, ls=2)
    b += game_ticket(30, 104, 378, 270, 178) + mini_row(30, 388, 68, 82, 9.5)
    b += mono(30, 500, "1 OF 9 GAMES PLAYS LINKED", 14, MUTED)
    b += mono(418, 94, "2 · LINK", 14, PL, 500, ls=2)
    b += link_card(418, 104, 392, 140, "lan", ["WI-FI · KRAKEN-5G", "OPEN TABLE · INSTANT MOVES", "NO INTERNET NEEDED"], True)
    b += link_card(418, 254, 392, 140, "lora", ["MESH ONLINE · 3 HEARD", "DIRECT INVITE · ~5 S A MOVE", "AIRTIME 12 / 90 S THIS HOUR"])
    b += online_slim(418, 404, 392, 68)
    b += mono(820, 94, "3 · TABLE", 14, PL, 500, ls=2)
    b += ticket(820, 104, 382, 350, SURF, LINE, perf=(("h", 262),))
    rows = (("GAME", "Fleet"), ("RULES", "Classic"), ("SEATS", "2"), ("LINK", "Local network"), ("SEEN BY", "Players on Kraken-5G"))
    for i, (k, v) in enumerate(rows):
        b += mono(842, 146 + i * 46, k, 14, MUTED, ls=2) + sans(1180, 148 + i * 46, v, 20, anchor="end")
    b += mono(842, 402, "TABLE CODE", 14, MUTED, ls=2) + mono(1180, 402, "SHOWN WHEN OPEN", 14, SEC, anchor="end")
    b += slab(820, 468, 382, 76, "OPEN TABLE", P, INK)
    return frame(1232, 568, "Host", "STEP 1 OF 2 · SET UP", b)


def join_portrait():
    b = mono(20, 132, "INVITATIONS", 14, AMBER, 500, ls=2) + invite_ticket(20, 146, 528, 206)
    b += mono(20, 386, "OPEN TABLES NEARBY", 14, MINT, 500, ls=2)
    b += table_ticket(20, 400, 528, 120, "KE", MINT, "Ken's table", "FLEET · CLASSIC", "LOCAL · 1 OF 2 SEATS")
    b += table_ticket(20, 530, 528, 120, "LI", AMBER, "Liv's table", "FLEET · CLASSIC", "LOCAL · 1 OF 2 SEATS")
    b += table_ticket(20, 660, 528, 120, "OL", PL, "Ola's table", "FLEET · PROTOCOL 2", "THIS DEVICE SPEAKS 1", CORAL, stale=True)
    b += mono(20, 814, "HEARD ON LORA", 14, AMBER, 500, ls=2)
    b += player_pill(20, 828, 259, "AN", CORAL, "Anna", "DIRECT · JUST NOW", True)
    b += player_pill(289, 828, 259, "M5", PL, "Mstr_m5", "1 HOP · 12 MIN")
    b += sans(20, 928, "Tap a player to invite them to a table you host.", 16, SEC, 400)
    b += code_slab(20, 960, 528)
    b += sans(284, 1056, "No tables? Ask the host for the code on their screen.", 16, MUTED, 400, "middle")
    b += hint("ARROWS CHOOSE · ENTER JOINS · ESC BACK")
    return frame(568, 1232, "Join", "2 OPEN · 1 INVITATION", b)


def join_landscape():
    b = mono(30, 94, "INVITATIONS", 14, AMBER, 500, ls=2) + invite_ticket(30, 104, 410, 196, stub=106, bw=124, l1="FLEET · LORA DIRECT")
    b += mono(30, 330, "HEARD ON LORA", 14, AMBER, 500, ls=2)
    b += player_pill(30, 342, 200, "AN", CORAL, "Anna", "DIRECT · NOW", True)
    b += player_pill(240, 342, 200, "M5", PL, "Mstr_m5", "1 HOP · 12 M")
    b += code_slab(30, 430, 410) + sans(30, 524, "No tables? Ask the host for their code.", 16, MUTED, 400)
    b += mono(460, 94, "OPEN TABLES NEARBY", 14, MINT, 500, ls=2)
    b += table_ticket(460, 104, 742, 136, "KE", MINT, "Ken's table", "FLEET · CLASSIC", "LOCAL · KRAKEN-5G · 1 OF 2 SEATS")
    b += table_ticket(460, 250, 742, 136, "LI", AMBER, "Liv's table", "FLEET · CLASSIC", "LOCAL · KRAKEN-5G · 1 OF 2 SEATS")
    b += table_ticket(460, 396, 742, 136, "OL", PL, "Ola's table", "FLEET · PROTOCOL 2", "THIS DEVICE SPEAKS PROTOCOL 1", CORAL, stale=True)
    return frame(1232, 568, "Join", "2 OPEN · 1 INVITATION", b)


NEXT = ("Both ready: Fleet opens on Deploy.", "The match is saved on both devices.", "A dropped link pauses it, never forfeits.")


def wait_portrait():
    x, y, w = 20, 116, 528
    inner = table_head(x, y, w, 200)
    b = ticket(x, y, w, 594, SURF, PL, perf=(("h", 210), ("h", 322)), inner=inner)
    b += code_row(x, y + 214, w)
    b += mono(x + 24, y + 354, "SEATS · 2 OF 2", 14, SEC, ls=2)
    b += seat(x + 24, y + 370, 232, 200, "AW", P, "Andre", "YOU · HOST", True)
    b += seat(x + 272, y + 370, 232, 200, "AN", CORAL, "Anna", "K230-B · 9C41", False)
    b += slab(20, 728, 528, 76, "STARTS WHEN ANNA IS READY", I1, PL, P, dash=True, size=20)
    b += slab(20, 818, 528, 58, "CLOSE TABLE", "none", SEC, LINE, size=18)
    b += R(20, 904, 528, 150, SURF, 16, 'stroke="%s"' % LINE) + mono(42, 938, "WHAT HAPPENS NEXT", 14, PL, 500, ls=2)
    for i, ln in enumerate(NEXT):
        b += sans(42, 968 + i * 28, ln, 16, SEC, 400)
    b += hint("ANNA JOINED 12 S AGO · LINK GOOD")
    return frame(568, 1232, "Table", "FLEET · LOCAL NETWORK", b)


def wait_landscape():
    x, y, w = 30, 76, 440
    b = ticket(x, y, w, 468, SURF, PL, perf=(("h", 192), ("h", 304)), inner=table_head(x, y, w, 182))
    b += code_row(x, y + 196, w)
    b += mono(x + 24, y + 340, "WHAT HAPPENS NEXT", 14, PL, 500, ls=2)
    for i, ln in enumerate(NEXT):
        b += sans(x + 24, y + 370 + i * 28, ln, 16, SEC, 400)
    b += mono(490, 94, "SEATS · 2 OF 2", 14, SEC, ls=2) + mono(1202, 94, "ANNA JOINED 12 S AGO · LINK GOOD", 14, MUTED, anchor="end")
    b += seat(490, 106, 348, 272, "AW", P, "Andre", "YOU · HOST", True, ar=48)
    b += seat(854, 106, 348, 272, "AN", CORAL, "Anna", "K230-B · 9C41", False, ar=48)
    b += slab(490, 398, 452, 76, "STARTS WHEN ANNA IS READY", I1, PL, P, dash=True, size=19)
    b += slab(958, 398, 244, 76, "CLOSE TABLE", "none", SEC, LINE, size=18)
    b += sans(490, 512, "Leaving this screen closes the table.", 16, MUTED, 400)
    return frame(1232, 568, "Table", "FLEET · LOCAL NETWORK", b)


# ---- concept B, "Orbit" (explored, not chosen) -----------------------------

def orbit(cx, cy, r1, r2, r3):
    s = '<circle cx="%s" cy="%s" r="%s" fill="url(#glow)"/>' % (cx, cy, r1 * 0.95)
    s += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="2" opacity=".8"/>' % (cx, cy, r1, MINT)
    s += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="2" stroke-dasharray="4 9" opacity=".8"/>' % (cx, cy, r2, AMBER)
    s += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-width="1.5" stroke-dasharray="2 12" opacity=".7"/>' % (cx, cy, r3, MUTED)
    s += avatar(cx, cy, 44, "AW", P, ring=PL) + mono(cx, cy + 74, "YOU", 14, PL, 500, "middle", 3)
    import math
    for ang, r, ini, hue, nm in ((205, r1, "KE", MINT, "KEN"), (330, r1, "LI", AMBER, "LIV"), (150, r2, "AN", CORAL, "ANNA"),
                                 (20, r2, "M5", PL, "MSTR_M5"), (262, r2, "SE", "#d8bf94", "SE-GOT")):
        px, py = cx + r * math.cos(math.radians(ang)), cy + r * math.sin(math.radians(ang))
        s += avatar(px, py, 24, ini, hue) + mono(px, py + 44, nm, 12, SEC, 500, "middle", 1.5)
    px, py = cx + r1 * math.cos(math.radians(78)), cy + r1 * math.sin(math.radians(78))
    s += tile("fleet", px - 34, py - 40, 68, 80, 10) + R(px - 34, py - 40, 68, 80, "none", 10, 'stroke="%s" stroke-width="2"' % MINT)
    s += mono(cx + r1 * 0.5 + 8, cy - r1 * 0.87 - 6, "LOCAL", 12, MINT, 500, ls=2) + mono(cx + r2 * 0.5 + 8, cy - r2 * 0.87 - 6, "LORA", 12, AMBER, 500, ls=2)
    s += mono(cx + r3 * 0.34 + 8, cy - r3 * 0.94 - 6, "ONLINE · LATER", 12, MUTED, 500, ls=2)
    return s


def orbit_portrait():
    c = uid()
    b = '<clipPath id="%s"><rect x="0" y="110" width="568" height="760"/></clipPath><g clip-path="url(#%s)">%s</g>' % (c, c, orbit(284, 480, 140, 235, 330))
    b += R(20, 884, 528, 84, SURF, 16, 'stroke="%s"' % LINE) + tile("fleet", 32, 894, 54, 64, 8)
    b += sans(102, 922, "Fleet · vs Anna", 20) + mono(102, 946, "YOUR TURN · SHOT 12", 14, MINT, 500) + slab(410, 902, 122, 48, "RESUME", MINT, INK, size=16)
    b += slab(20, 986, 259, 84, "HOST", P, INK, size=26) + slab(289, 986, 259, 84, "JOIN", "none", PL, PL, size=26)
    b += hint("TAP A PLAYER OR A TABLE")
    return frame(568, 1232, "Lobby", "5 IN REACH · 1 TABLE", b)


def orbit_landscape():
    c = uid()
    b = '<clipPath id="%s"><rect x="30" y="76" width="800" height="468"/></clipPath><g clip-path="url(#%s)">%s</g>' % (c, c, orbit(430, 310, 120, 205, 300))
    b += R(850, 76, 352, 150, SURF, 16, 'stroke="%s"' % LINE) + tile("fleet", 866, 92, 70, 84, 8)
    b += sans(952, 122, "Fleet · vs Anna", 20) + mono(952, 146, "YOUR TURN · SHOT 12", 14, MINT, 500) + slab(952, 164, 150, 46, "RESUME", MINT, INK, size=16)
    b += slab(850, 246, 352, 140, "HOST", P, INK, size=30) + slab(850, 404, 352, 140, "JOIN", "none", PL, PL, size=30)
    return frame(1232, 568, "Lobby", "5 IN REACH · 1 TABLE", b)


# ---- the states sheet (components, not a device frame) ----------------------

def states_sheet():
    w, h = 1232, 740
    s = '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h)
    s += defs() + R(0, 0, w, h, BG)
    s += sans(30, 50, "Lobby states", 28) + mono(1202, 46, "DESIGN MOCKUP · COMPONENTS AT PANEL SCALE · NOT A SCREENSHOT", 12, MUTED, anchor="end")
    cols = (30, 434, 838)

    def cap(i, j, t):
        return mono(cols[i], 96 + j * 360, t, 14, PL, 500, ls=2)

    y0, y1 = 112, 472
    # 1 invitation notice (the shell's notice banner, app.h pocketos_shell_notify)
    s += cap(0, 0, "1 · INVITATION NOTICE, ANY SCREEN")
    s += R(30, y0, 364, 96, RAISED, 14, 'stroke="%s"' % LINE) + R(30, y0, 6, 96, AMBER, 3)
    s += mono(54, y0 + 28, "LOBBY · FLEET", 12, AMBER, 500) + sans(54, y0 + 56, "Anna invites you", 20)
    s += sans(54, y0 + 80, "Over LoRa. Tap to answer.", 16, SEC, 400) + avatar(354, y0 + 48, 20, "AN", CORAL)
    s += sans(30, y0 + 130, "Shown only from a fresh invitation. The", 16, MUTED, 400)
    s += sans(30, y0 + 152, "shell listens; it never answers by itself.", 16, MUTED, 400)
    # 2 opponent's turn / quiet
    s += cap(1, 0, "2 · RESUME, OPPONENT'S TURN")
    s += '<g transform="translate(%s %s) scale(.69)">%s</g>' % (434, y0, resume_h(0, 0, 528, 184, "WAITING FOR ANNA", AMBER, "NO WORD FOR 7 MIN · LORA", "OPEN"))
    s += sans(434, y0 + 160, "One match per game, on the link it began on.", 16, MUTED, 400)
    # 3 reconnecting
    s += cap(2, 0, "3 · OPPONENT OUT OF REACH")
    s += R(838, y0, 364, 200, SURF, 16, 'stroke="%s" stroke-width="2"' % AMBER)
    s += chip(858, y0 + 18, "PAUSED", AMBER, INK) + sans(858, y0 + 84, "Anna is out of reach", 20)
    s += sans(858, y0 + 110, "The match is paused, not lost.", 16, SEC, 400) + mono(858, y0 + 134, "RETRY 5 OF 6 · NEXT IN 40 S", 14, AMBER)
    s += slab(858, y0 + 148, 170, 40, "CHECK LINK", AMBER, INK, size=15) + slab(1040, y0 + 148, 142, 40, "RESUME", "none", SEC, LINE, size=15)
    # 4 offline
    s += cap(0, 1, "4 · OFFLINE, HONESTLY")
    s += '<g transform="translate(30 %s) scale(.69)">%s%s</g>' % (y1, conn_row(0, 0, 528, 96, "lan", ["WI-FI IS OFF · NO CABLE"], "OFF", MUTED),
                                                                conn_row(0, 106, 528, 96, "lora", ["MESH SERVICE IS OFF ON THIS DEVICE"], "OFF", MUTED))
    s += sans(30, y1 + 170, "HOST and JOIN stay reachable; each says", 16, MUTED, 400)
    s += sans(30, y1 + 192, "what to switch on. The lobby starts nothing.", 16, MUTED, 400)
    # 5 failure
    s += cap(1, 1, "5 · COULD NOT CONNECT")
    s += R(434, y1, 364, 226, SURF, 16, 'stroke="%s" stroke-width="2"' % CORAL)
    s += chip(454, y1 + 18, "NO ANSWER", CORAL, INK) + sans(454, y1 + 84, "Couldn't reach table K7QM", 20)
    s += sans(454, y1 + 110, "This network may keep devices apart.", 16, SEC, 400)
    s += sans(454, y1 + 132, "Check the code, or play over LoRa.", 16, SEC, 400)
    s += slab(454, y1 + 166, 160, 42, "TRY AGAIN", P, INK, size=15) + slab(626, y1 + 166, 152, 42, "BACK", "none", SEC, LINE, size=15)
    # 6 remembered players
    s += cap(2, 1, "6 · REMEMBERED PLAYERS (FOUNDATION)")
    s += player_pill(838, y1, 364, "AN", CORAL, "Anna", "PLAYED 3 · LAST YESTERDAY", True)
    s += player_pill(838, y1 + 82, 364, "M5", PL, "Mstr_m5", "NEW · KEY 5A1D")
    s += R(838, y1 + 164, 364, 62, SURF, 16, 'stroke="%s" stroke-width="2"' % CORAL)
    s += sans(858, y1 + 190, "Anna · different key", 16, CORAL) + mono(858, y1 + 212, "NOT THE DEVICE YOU REMEMBERED", 12, SEC)
    return s + "</svg>"


MOCKUPS = [
    ("final-lobby-portrait", 568, 1232, lobby_portrait), ("final-lobby-landscape", 1232, 568, lobby_landscape),
    ("final-host-portrait", 568, 1232, host_portrait), ("final-host-landscape", 1232, 568, host_landscape),
    ("final-join-portrait", 568, 1232, join_portrait), ("final-join-landscape", 1232, 568, join_landscape),
    ("final-table-portrait", 568, 1232, wait_portrait), ("final-table-landscape", 1232, 568, wait_landscape),
    ("concept-b-orbit-portrait", 568, 1232, orbit_portrait), ("concept-b-orbit-landscape", 1232, 568, orbit_landscape),
    ("states-sheet", 1232, 740, states_sheet),
]

WRAP = ('<!doctype html><html><head><meta charset="utf-8">'
        '<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;600;700'
        '&family=IBM+Plex+Sans+Condensed:wght@600;700&display=block" rel="stylesheet">'
        '<style>html,body{margin:0;background:#000;overflow:hidden}svg,img{display:block}%s</style></head><body>%s</body></html>')

SHEET_CSS = ("body{background:#050505;color:#eeeae2;font-family:'IBM Plex Sans','Segoe UI',sans-serif;width:%dpx}"
             "h1{font-size:40px;margin:36px 48px 4px}p{margin:0 48px 26px;color:#a8a399;font-size:21px}"
             ".row{display:flex;gap:36px;align-items:flex-start;padding:0 48px 44px}"
             ".c h2{font-family:'IBM Plex Mono',Consolas,monospace;font-size:22px;letter-spacing:3px;margin:0 0 14px}"
             ".p{display:flex;gap:20px;align-items:flex-start}"
             ".c img{border-radius:30px;border:3px solid #2a2824}.pt{width:284px}.ls{width:616px}")


def find_browser():
    cands = [os.environ.get("MOCKUP_BROWSER"), r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
             r"C:\Program Files\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Google\Chrome\Application\chrome.exe",
             shutil.which("chromium"), shutil.which("google-chrome"), shutil.which("msedge")]
    return next((c for c in cands if c and os.path.exists(c)), None)


def shoot(browser, tmp, html, png, w, h):
    # The page lives beside the SVGs so their relative art links resolve.
    page = OUT / ("_render_" + png.stem + ".html")
    page.write_text(html, encoding="utf-8")
    if png.exists():
        png.unlink()
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                             "--user-data-dir=" + str(pathlib.Path(tmp) / "profile"), "--virtual-time-budget=6000",
                             "--allow-file-access-from-files", "--window-size=%d,%d" % (w, h), "--screenshot=" + str(png), page.as_uri()],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=45)
    except subprocess.TimeoutExpired:
        proc.kill()
    page.unlink()
    if not png.exists():
        raise SystemExit("render failed: %s" % png.name)


def sheet(browser, tmp, name, title, note, groups, width, height):
    cells = []
    for head, key in groups:
        cells.append('<div class="c"><h2>%s</h2><div class="p"><img class="pt" src="%s-portrait.png"><img class="ls" src="%s-landscape.png"></div></div>'
                     % (head, key, key))
    rows = "".join("<div class='row'>%s</div>" % "".join(cells[i:i + 2]) for i in range(0, len(cells), 2))
    body = "<h1>%s</h1><p>%s</p>%s" % (title, note, rows)
    shoot(browser, tmp, WRAP % (SHEET_CSS % width, body), OUT / (name + ".png"), width, height)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    only = set(sys.argv[1:])
    browser = find_browser()
    # Beside the output, not in %TEMP%: Edge refused a profile under a
    # non-ASCII user path on the design host.
    tmp = tempfile.mkdtemp(prefix="_render_", dir=str(OUT))
    for name, w, h, fn in MOCKUPS:
        if only and name not in only:
            continue
        _n[0] = 0
        doc = fn()
        (OUT / (name + ".svg")).write_text(doc, encoding="utf-8")
        if browser:
            shoot(browser, tmp, WRAP % ("", doc), OUT / (name + ".png"), w, h)
        print(name, "ok" if browser else "svg only (no browser found)")
    if browser and not only:
        note = "Design mockups for owner review. Not screenshots of implemented DOORS software. Panel 568 &#215; 1232, shown at half size."
        sheet(browser, tmp, "overview", "DOORS Multiplayer Lobby &#8212; Concept A, &#8220;Box Office&#8221; (final)", note,
              (("MAIN LOBBY", "final-lobby"), ("HOST GAME", "final-host"), ("JOIN GAME", "final-join"), ("WAITING ROOM", "final-table")),
              2040, 1580)
        sheet(browser, tmp, "concepts", "DOORS Multiplayer Lobby &#8212; two concepts", note,
              (("A  BOX OFFICE (CHOSEN)", "final-lobby"), ("B  ORBIT", "concept-b-orbit")), 2040, 810)
        print("sheets ok")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
