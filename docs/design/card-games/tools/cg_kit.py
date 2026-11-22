#!/usr/bin/env python3
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
"""Drawing kit for the card-games redesign mockups (docs/design/card-games).

Everything is vector: cards, suits, chips, seats and cloth are SVG built here,
set in IBM Plex Sans Condensed, IBM Plex Mono (the product's families) and
Fraunces (SIL OFL 1.1, proposed for art only). No glyph of a system font is
used for a suit: the four suits are paths.
"""

COND = "'IBM Plex Sans Condensed'"
MONO = "'IBM Plex Mono'"
SERIF = "'Fraunces'"

IVORY, GOLD, INK, RED = "#f3ead6", "#d9b66a", "#1a1714", "#b3261e"

SUIT_PATH = {
    "h": "M0 40C-12 28-43 8-43-15C-43-32-30-41-19-41C-9-41-2-35 0-26C2-35 9-41 19-41C30-41 43-32 43-15C43 8 12 28 0 40Z",
    "d": "M0-46C9-29 22-13 33 0C22 13 9 29 0 46C-9 29-22 13-33 0C-22-13-9-29 0-46Z",
    "s": "M0-45C10-29 43-8 43 14C43 30 31 37 20 37C12 37 6 33 3 26C4 35 8 41 15 45L-15 45C-8 41-4 35-3 26"
         "C-6 33-12 37-20 37C-31 37-43 30-43 14C-43-8-10-29 0-45Z",
    "c": "M0-45A19 19 0 0 1 16.5-16.6A19 19 0 1 1 3.5 12C4.5 28 8 39 15 45L-15 45C-8 39-4.5 28-3.5 12"
         "A19 19 0 1 1-16.5-16.6A19 19 0 0 1 0-45Z",
}
RANKS = {1: "A", 11: "J", 12: "Q", 13: "K"}
PIPS = {
    2: [(0, -1), (0, 1)], 3: [(0, -1), (0, 0), (0, 1)], 4: [(-1, -1), (1, -1), (-1, 1), (1, 1)],
    5: [(-1, -1), (1, -1), (0, 0), (-1, 1), (1, 1)],
    6: [(-1, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (1, 1)],
    7: [(-1, -1), (1, -1), (0, -.5), (-1, 0), (1, 0), (-1, 1), (1, 1)],
    8: [(-1, -1), (1, -1), (0, -.5), (-1, 0), (1, 0), (0, .5), (-1, 1), (1, 1)],
    9: [(-1, -1), (1, -1), (-1, -.34), (1, -.34), (0, 0), (-1, .34), (1, .34), (-1, 1), (1, 1)],
    10: [(-1, -1), (1, -1), (0, -.67), (-1, -.34), (1, -.34), (-1, .34), (1, .34), (0, .67), (-1, 1), (1, 1)],
}
CROWN = {
    13: '<path d="M-16 8L-19-9L-8 0L0-12L8 0L19-9L16 8Z"/>',
    12: '<path d="M-17 8Q0-4 17 8Z"/><circle cx="-12" cy="-5" r="3"/><circle cx="0" cy="-10" r="3"/><circle cx="12" cy="-5" r="3"/>',
    11: '<path d="M-15 8L15 8L15 3L-15 3Z"/><path d="M1 3Q16-6 7-14Q5-4-8 3Z"/>',
}


def rk(rank):
    return RANKS.get(rank, str(rank))


def T(x, y, s, size, fill, font=COND, w=700, anchor="start", ls=0, extra=""):
    return ('<text x="%.1f" y="%.1f" font-family="%s" font-size="%s" font-weight="%s" fill="%s" text-anchor="%s" '
            'letter-spacing="%s" %s>%s</text>' % (x, y, font, size, w, fill, anchor, ls, extra, s))


def R(x, y, w, h, fill, rx=0, extra=""):
    return '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%s" fill="%s" %s/>' % (x, y, w, h, rx, fill, extra)


def suit(cx, cy, size, s, fill, rot=0, extra=""):
    return '<path d="%s" fill="%s" transform="translate(%.1f %.1f) rotate(%s) scale(%.4f)" %s/>' % (
        SUIT_PATH[s], fill, cx, cy, rot, size / 92.0, extra)


# ------------------------------------------------------------------ cards
FOUR = {"s": "#15151a", "h": "#e23d3d", "d": "#2f6fe0", "c": "#1e9b62"}


def card(x, y, w, rank, s, style="lamp", rot=0, shadow=True, dim=0, lift=0):
    """A face. w < 100 is the compact face (one index, one large mark)."""
    h = round(w * 1.4)
    red = s in "hd"
    o = []
    if style == "imprint":
        ink = "#d1432b" if red else "#17140f"
        o.append(R(0, 0, w, h, "#efe6d2", w * .035, 'stroke="#12100d" stroke-width="2"'))
        o.append(T(w / 2, h * .66, rk(rank), w * (.6 if rank != 10 else .52), ink, SERIF, 600, "middle", -2))
        o.append(suit(w * .15, w * .17, w * .17, s, ink))
        o.append(suit(w * .85, h - w * .17, w * .17, s, ink, 180))
    elif style == "signal":
        ink = FOUR[s]
        o.append(R(0, 0, w, h, "#f4f4f0", 7))
        o.append(T(w * .1, w * .47, rk(rank), w * .5, ink, COND, 700, "start", -1))
        o.append(suit(w * .68, h - w * .34, w * .46, s, ink))
    else:
        ink = RED if red else INK
        r = w * .075
        o.append('<rect width="%s" height="%s" rx="%.1f" fill="url(#stock)" stroke="#b9ad92" stroke-width="1"/>' % (w, h, r))
        if w >= 100:
            fs, ps, px = w * .23, w * .14, w * .12
            idx = (T(px, fs * .93 + w * .03, rk(rank), fs, ink, COND, 700, "middle", -1.5 if rank == 10 else 0)
                   + suit(px, fs + w * .13, ps, s, ink))
            o.append(idx + '<g transform="rotate(180 %s %s)">%s</g>' % (w / 2, h / 2, idx))
            if rank >= 11:
                fx, fy, fw, fh = w * .24, h * .09, w * .52, h * .82
                o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="5" fill="%s" stroke="%s" stroke-width="1.2"/>'
                         % (fx, fy, fw, fh, "#f0dccb" if red else "#e2dccb", ink))
                o.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="3" fill="none" stroke="%s" stroke-width=".6" opacity=".6"/>'
                         % (fx + 4, fy + 4, fw - 8, fh - 8, ink))
                half = ('<g transform="translate(%.1f %.1f) scale(%.2f)" fill="%s">%s</g>' % (w / 2, h * .19, w / 125.0, ink, CROWN[rank])
                        + T(w / 2, h * .435, rk(rank), w * .3, ink, SERIF, 600, "middle"))
                o.append(half + '<g transform="rotate(180 %s %s)">%s</g>' % (w / 2, h / 2, half))
                o.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width=".8" opacity=".5"/>'
                         % (fx + 8, h / 2, fx + fw - 8, h / 2, ink))
                o.append(suit(w / 2, h / 2, w * .1, s, "#f0dccb" if red else "#e2dccb"))
                o.append(suit(w / 2, h / 2, w * .075, s, ink))
            elif rank == 1:
                o.append('<circle cx="%s" cy="%s" r="%.1f" fill="none" stroke="%s" stroke-width=".8" opacity=".45"/>' % (w / 2, h / 2, w * .3, ink))
                o.append(suit(w / 2, h / 2, w * .4, s, ink))
            else:
                for cx, cy in PIPS[rank]:
                    o.append(suit(w / 2 + cx * w * .165, h / 2 + cy * h * .285, w * .165, s, ink, 180 if cy > 0 else 0))
        else:
            fs = w * .37
            o.append(T(w * .09, fs * .88 + w * .04, rk(rank), fs, ink, COND, 700, "start", -1.5 if rank == 10 else 0))
            o.append(suit(w * .79, w * .22, w * .25, s, ink))
            if rank >= 11:
                o.append('<g transform="translate(%.1f %.1f) scale(%.2f)" fill="%s" opacity=".85">%s</g>' % (w / 2, h * .47, w / 95.0, ink, CROWN[rank]))
                o.append(T(w / 2, h * .9, rk(rank), w * .5, ink, SERIF, 600, "middle"))
            else:
                o.append(suit(w / 2, h * .66, w * (.56 if rank == 1 else .46), s, ink))
    if dim:
        o.append('<rect width="%s" height="%s" rx="%.1f" fill="#000" opacity="%s"/>' % (w, h, w * .075, dim))
    return _place(x, y - lift, w, h, rot, "".join(o), shadow and style == "lamp", lift)


def back(x, y, w, style="lamp", rot=0, shadow=True, tone="#1f2a44", line="#3d4d78"):
    h = round(w * 1.4)
    if style == "imprint":
        body = (R(0, 0, w, h, "#efe6d2", w * .035) + R(w * .07, w * .07, w * .86, h - w * .14, "url(#pin)", 2)
                + suit(w / 2, h / 2, w * .3, "d", "#efe6d2") + suit(w / 2, h / 2, w * .2, "d", "#d1432b"))
    elif style == "signal":
        body = (R(0, 0, w, h, "#0d0d10", 7, 'stroke="#c8324a" stroke-width="1.5"') + R(3, 3, w - 6, h - 6, "url(#grid)", 5)
                + '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="#c8324a" stroke-width="1.5"/>' % (w / 2, h / 2, w * .16))
    else:
        r, i = w * .075, max(3, w * .055)
        body = ('<rect width="%s" height="%s" rx="%.1f" fill="url(#stock)" stroke="#b9ad92" stroke-width="1"/>' % (w, h, r)
                + R(i, i, w - 2 * i, h - 2 * i, tone, r * .6) + R(i, i, w - 2 * i, h - 2 * i, "url(#lattice)", r * .6, 'opacity=".9"'))
        if w >= 60:
            m = w * .2
            body += ('<circle cx="%s" cy="%s" r="%.1f" fill="%s" stroke="%s" stroke-width="1.2"/>' % (w / 2, h / 2, m, tone, GOLD)
                     + '<path d="M%.1f %.1fL%.1f %.1fA%.1f %.1f 0 0 1 %.1f %.1fL%.1f %.1fZ" fill="none" stroke="%s" stroke-width="1.4"/>'
                     % (w / 2 - m * .38, h / 2 + m * .5, w / 2 - m * .38, h / 2 - m * .1, m * .38, m * .38,
                        w / 2 + m * .38, h / 2 - m * .1, w / 2 + m * .38, h / 2 + m * .5, GOLD)
                     + '<circle cx="%.1f" cy="%.1f" r="1.3" fill="%s"/>' % (w / 2 + m * .18, h / 2 + m * .12, GOLD))
        body += '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" fill="none" stroke="%s" stroke-width=".8" opacity=".7"/>' % (
            i + 3, i + 3, w - 2 * i - 6, h - 2 * i - 6, r * .4, line)
    return _place(x, y, w, h, rot, body, shadow and style == "lamp", 0)


def slot(x, y, w, mark=None, color=GOLD, opacity=.3, glow=False):
    h = round(w * 1.4)
    o = '<rect x="%.1f" y="%.1f" width="%s" height="%s" rx="%.1f" fill="#000" fill-opacity=".16" stroke="%s" stroke-opacity="%s" stroke-width="%s" %s/>' % (
        x, y, w, h, w * .075, IVORY if glow else color, .95 if glow else opacity, 2.5 if glow else 1.2,
        'filter="url(#glow)"' if glow else 'stroke-dasharray="1 0"')
    if mark in SUIT_PATH:
        o += suit(x + w / 2, y + h / 2, w * .4, mark, color, 0, 'opacity="%s"' % opacity)
    elif mark:
        o += T(x + w / 2, y + h / 2 + w * .16, mark, w * .46, color, SERIF, 600, "middle", 0, 'opacity="%s"' % opacity)
    return o


def _place(x, y, w, h, rot, body, shadow, lift):
    f = ' filter="url(#%s)"' % ("lift" if lift else "sh") if shadow else ""
    t = "translate(%.1f %.1f)" % (x, y) + (" rotate(%s %s %s)" % (rot, w / 2, h / 2) if rot else "")
    return '<g transform="%s"%s>%s</g>' % (t, f, body)


# ------------------------------------------------------------------ chips
CHIP = {10: ("#2c5f93", IVORY), 50: ("#1c7455", IVORY), 100: ("#17171b", GOLD), 500: ("#6a3f9e", IVORY), 0: ("#8a8273", IVORY)}


def chip(cx, cy, r, value, label=True):
    body, ins = CHIP[value]
    c = 2 * 3.14159 * r * .86
    o = ('<circle cx="%.1f" cy="%.1f" r="%s" fill="%s" stroke="#000" stroke-opacity=".5" stroke-width="1"/>' % (cx, cy, r, body)
         + '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="%s" stroke-width="%.1f" stroke-dasharray="%.2f %.2f"/>'
         % (cx, cy, r * .86, ins, r * .24, c / 12, c / 12)
         + '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" stroke="%s" stroke-opacity=".7" stroke-width="1"/>' % (cx, cy, r * .62, body, ins))
    if label and r >= 13:
        o += T(cx, cy + r * .2, value, r * .56, ins, COND, 700, "middle")
    return o


def stack(cx, cy, r, values, step=None):
    """A stack seen from above: the first value is the bottom chip."""
    step = step or max(3, r * .16)
    o = '<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="#000" opacity=".4" filter="url(#soft)"/>' % (cx + 2, cy + 5, r * 1.05, r * .95)
    for i, v in enumerate(values):
        o += chip(cx, cy - i * step, r, v, i == len(values) - 1)
    return o


# ------------------------------------------------------------------ poker seats
def emblem(kind, color):
    if kind == "cog":      # Ada: the engine's wheel
        teeth = "".join('<rect x="-3.4" y="-17" width="6.8" height="8" rx="1.2" transform="rotate(%d)"/>' % a for a in range(0, 360, 45))
        return '<g fill="%s">%s<circle r="11.5"/></g><circle r="5" fill="#0c0b12"/>' % (color, teeth)
    if kind == "seed":     # Linus: the kernel
        return ('<path d="M0-17C13-9 13 9 0 17C-13 9-13-9 0-17Z" fill="%s"/><path d="M0-12L0 12" stroke="#0c0b12" stroke-width="2.2" stroke-linecap="round"/>'
                '<path d="M0-2L6-7M0 5L6 0" stroke="#0c0b12" stroke-width="1.6" stroke-linecap="round"/>' % color)
    return ('<g fill="%s"><path d="M-2-4C-12-16-21-12-19-2C-18 5-9 4-2 1Z"/><path d="M2-4C12-16 21-12 19-2C18 5 9 4 2 1Z"/>'   # Grace: the moth
            '<path d="M-2 3C-10 5-13 12-8 15C-4 16-2 10-1 5Z"/><path d="M2 3C10 5 13 12 8 15C4 16 2 10 1 5Z"/></g>'
            '<rect x="-1.8" y="-9" width="3.6" height="22" rx="1.8" fill="%s"/>' % (color, IVORY))


def seat(cx, cy, r, kind, color, active=False, out=False):
    o = '<circle cx="%s" cy="%s" r="%s" fill="#0c0b12" stroke="%s" stroke-width="%s" %s/>' % (
        cx, cy, r, IVORY if active else color, 3 if active else 1.6, 'filter="url(#glow)"' if active else "")
    o += '<circle cx="%s" cy="%s" r="%s" fill="none" stroke="%s" stroke-opacity=".35" stroke-width="1"/>' % (cx, cy, r - 5, color)
    o += '<g transform="translate(%s %s) scale(%.2f)">%s</g>' % (cx, cy, r / 30.0, emblem(kind, color))
    return '<g opacity="%s">%s</g>' % (.35 if out else 1, o)


def tag(cx, cy, s, fill=IVORY, bg="#0c0b12", stroke=None, size=13):
    w = len(s) * size * .62 + 18
    return (R(cx - w / 2, cy - size * .85, w, size * 1.7, bg, size * .85, 'stroke="%s" stroke-width="1" fill-opacity=".88"' % (stroke or fill))
            + T(cx, cy + size * .35, s, size, fill, COND, 700, "middle", 1.5))


def button(x, y, w, h, label, kind="secondary", key=None, sub=None, size=24):
    fill, txt, st = {"primary": (IVORY, INK, IVORY), "secondary": ("#000", IVORY, IVORY), "off": ("#000", IVORY, IVORY)}[kind]
    op = {"primary": (1, 1, 1), "secondary": (.28, 1, .45), "off": (.15, .25, .15)}[kind]
    o = '<rect x="%s" y="%s" width="%s" height="%s" rx="16" fill="%s" fill-opacity="%s" stroke="%s" stroke-opacity="%s" stroke-width="1.5"/>' % (
        x, y, w, h, fill, op[0], st, op[2])
    ty = y + h / 2 + size * .34 - (9 if sub else 0)
    o += T(x + w / 2, ty, label, size, txt, COND, 700, "middle", 3, 'opacity="%s"' % op[1])
    if sub:
        o += T(x + w / 2, ty + 20, sub, 13, txt, MONO, 500, "middle", 1, 'opacity="%s"' % (op[1] * .7))
    if key:
        o += T(x + w - 12, y + 20, key, 11, txt, MONO, 500, "end", 0, 'opacity="%s"' % (op[1] * .45))
    return o


# ------------------------------------------------------------------ document
def defs(c0, c1, edge, cx="50%", cy="42%", rr="78%", back="#1f2a44", line="#3d4d78"):
    return ('<defs>'
            '<radialGradient id="cloth" cx="%s" cy="%s" r="%s"><stop offset="0" stop-color="%s"/><stop offset=".55" stop-color="%s"/>'
            '<stop offset="1" stop-color="%s"/></radialGradient>'
            '<linearGradient id="stock" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#faf4e4"/><stop offset="1" stop-color="#ebe1c9"/></linearGradient>'
            '<linearGradient id="fade" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity=".82"/></linearGradient>'
            '<pattern id="lattice" width="9" height="9" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
            '<path d="M0 0H9M0 0V9" stroke="%s" stroke-width="1.1"/></pattern>'
            '<pattern id="pin" width="5" height="5" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><rect width="5" height="5" fill="#d1432b"/><path d="M0 0V5" stroke="#efe6d2" stroke-width="1"/></pattern>'
            '<pattern id="grid" width="8" height="8" patternUnits="userSpaceOnUse"><path d="M0 0H8M0 0V8" stroke="#2c2c36" stroke-width="1"/></pattern>'
            '<filter id="sh" x="-30%%" y="-30%%" width="160%%" height="170%%"><feDropShadow dx="0" dy="3" stdDeviation="3.5" flood-color="#000" flood-opacity=".5"/></filter>'
            '<filter id="lift" x="-40%%" y="-40%%" width="180%%" height="200%%"><feDropShadow dx="0" dy="12" stdDeviation="9" flood-color="#000" flood-opacity=".6"/></filter>'
            '<filter id="soft" x="-50%%" y="-50%%" width="200%%" height="200%%"><feGaussianBlur stdDeviation="4"/></filter>'
            '<filter id="glow" x="-50%%" y="-50%%" width="200%%" height="200%%"><feGaussianBlur stdDeviation="5" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>'
            '<filter id="grain" x="0" y="0" width="100%%" height="100%%"><feTurbulence type="fractalNoise" baseFrequency=".85" numOctaves="2" seed="7"/>'
            '<feColorMatrix values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  .5 .5 .5 0 -.42"/></filter>'
            '</defs>') % (cx, cy, rr, c0, c1, edge, line)


def svg(w, h, body, d):
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="%d" height="%d" viewBox="0 0 %d %d">%s%s</svg>'
            % (w, h, w, h, d, body))


def cloth(w, h):
    return R(0, 0, w, h, "url(#cloth)") + R(0, 0, w, h, "#fff", 0, 'filter="url(#grain)" opacity=".16"')


def topbar(w, title, sub="", color=GOLD):
    o = '<path d="M42 34L26 50L42 66" fill="none" stroke="%s" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round" opacity=".9"/>' % IVORY
    o += T(w / 2, 48 if sub else 56, title, 16, color, COND, 700, "middle", 7)
    if sub:
        o += T(w / 2, 70, sub, 11, IVORY, MONO, 500, "middle", 1.5, 'opacity=".5"')
    o += "".join('<circle cx="%s" cy="50" r="2.6" fill="%s" opacity=".85"/>' % (w - 50 + i * 11, IVORY) for i in range(3))
    return o
