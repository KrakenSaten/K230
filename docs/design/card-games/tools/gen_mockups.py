#!/usr/bin/env python3
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
"""Render the card-games redesign mockups (docs/design/card-games/mockups).

    python docs/design/card-games/tools/gen_mockups.py [name ...]

Design-time only. Each screen is one SVG at the panel's size (568 x 1232, or
1232 x 568), rendered by a headless Chromium-family browser (Edge or Chrome;
MOCKUP_BROWSER overrides). The lettering is fetched from Google Fonts, so a
render needs network access. Nothing here is built into the product.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cg_kit import *  # noqa: E402,F401,F403

HERE = pathlib.Path(__file__).resolve().parent.parent
OUT = HERE / "mockups"
W, H = 568, 1232
BJ = ("#7d2130", "#3a0d16", "#050102")
PK = ("#40377c", "#1c183c", "#04040a")
SO = ("#226347", "#0e2f22", "#010504")


def rule(y, x1=24, x2=544, op=.22):
    return '<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="%s" stroke-width="1" opacity="%s"/>' % (x1, y, x2, y, GOLD, op)


def badge(cx, cy, r, s, hero=False, gold=False):
    fill = GOLD if gold else IVORY if hero else "#0d0508"
    return ('<circle cx="%s" cy="%s" r="%s" fill="%s" stroke="%s" stroke-width="1.5" filter="url(#sh)"/>' % (cx, cy, r, fill, GOLD if gold else IVORY)
            + T(cx, cy + r * .33, s, r * .95, INK if (hero or gold) else IVORY, COND, 700, "middle"))


def arcs():
    return ('<path id="a1" d="M62 408Q284 516 506 408" fill="none"/><path id="a2" d="M112 462Q284 540 456 462" fill="none"/>'
            '<path d="M48 384Q284 500 520 384" fill="none" stroke="%s" stroke-width="1" opacity=".3"/>'
            '<path d="M92 478Q284 566 476 478" fill="none" stroke="%s" stroke-width="1" opacity=".3"/>'
            '<text font-family="%s" font-size="23" font-weight="700" fill="%s" letter-spacing="6" opacity=".62" text-anchor="middle">'
            '<textPath href="#a1" startOffset="50%%">BLACKJACK PAYS 3 TO 2</textPath></text>'
            '<text font-family="%s" font-size="12.5" font-weight="500" fill="%s" letter-spacing="4.5" opacity=".42" text-anchor="middle">'
            '<textPath href="#a2" startOffset="50%%">DEALER STANDS ON ALL 17s</textPath></text>') % (GOLD, GOLD, COND, GOLD, MONO, GOLD)


def bank(bankv, betv, delta=None):
    o = rule(934) + T(28, 962, "BANK", 12, GOLD, MONO, 500, "start", 3, 'opacity=".75"') + T(28, 1002, bankv, 38, IVORY, COND, 700)
    if delta:
        o += T(150, 1000, delta, 22, GOLD, COND, 700)
    return o + T(540, 962, "BET", 12, GOLD, MONO, 500, "end", 3, 'opacity=".75"') + T(540, 1002, betv, 38, IVORY, COND, 700, "end")


def bj_base(sub="TWO DECKS · 62 IN THE SHOE"):
    return cloth(W, H) + R(0, 880, W, 352, "url(#fade)") + topbar(W, "BLACKJACK", sub) + arcs()


def bj_play():
    o = bj_base() + T(284, 132, "DEALER", 12, GOLD, MONO, 500, "middle", 5, 'opacity=".8"')
    o += card(180, 150, 124, 9, "s") + back(264, 150, 124, tone="#1f2a44") + badge(126, 237, 26, "9")
    o += '<circle cx="284" cy="614" r="58" fill="#000" fill-opacity=".14" stroke="%s" stroke-opacity=".5" stroke-width="1.4"/>' % GOLD
    o += '<circle cx="284" cy="614" r="50" fill="none" stroke="%s" stroke-opacity=".3" stroke-width="1" stroke-dasharray="2 5"/>' % GOLD
    o += stack(284, 622, 30, [10, 10, 10, 50])
    o += card(140, 700, 124, 4, "h") + card(222, 700, 124, 3, "d") + card(304, 700, 124, 11, "s") + badge(86, 787, 32, "17", hero=True)
    o += T(284, 906, "YOUR HAND", 12, GOLD, MONO, 500, "middle", 5, 'opacity=".8"') + bank("920", "80")
    o += button(24, 1036, 204, 88, "HIT", "primary", "H") + button(240, 1036, 152, 88, "STAND", "secondary", "S")
    o += button(404, 1036, 140, 88, "DOUBLE", "secondary", "D", "+80")
    o += T(284, 1170, "17 AGAINST A 9 · HIT OR STAND", 13, IVORY, MONO, 500, "middle", 2, 'opacity=".5"')
    return svg(W, H, o, defs(*BJ))


def bj_bet():
    o = bj_base("TWO DECKS · FRESH SHOE") + T(284, 132, "DEALER", 12, GOLD, MONO, 500, "middle", 5, 'opacity=".5"')
    o += slot(152, 150, 124) + slot(292, 150, 124)
    o += '<circle cx="284" cy="640" r="70" fill="#000" fill-opacity=".14" stroke="%s" stroke-width="2.4" filter="url(#glow)"/>' % IVORY
    o += '<circle cx="284" cy="640" r="60" fill="none" stroke="%s" stroke-opacity=".35" stroke-width="1" stroke-dasharray="2 5"/>' % GOLD
    o += stack(284, 650, 36, [10, 10, 10, 50]) + T(284, 752, "80", 44, IVORY, COND, 700, "middle")
    o += T(284, 776, "YOUR BET · 10 TO 200", 11, GOLD, MONO, 500, "middle", 3, 'opacity=".7"')
    o += rule(826) + T(28, 854, "BANK", 12, GOLD, MONO, 500, "start", 3, 'opacity=".75"') + T(28, 894, "1,000", 38, IVORY, COND, 700)
    o += T(540, 854, "TAP A CHIP TO ADD IT", 11, IVORY, MONO, 500, "end", 2, 'opacity=".5"')
    for i, v in enumerate((10, 50, 100)):
        o += stack(330 + i * 82, 900, 34, [v, v, v], 4)
    o += button(24, 968, 120, 76, "CLEAR", "secondary", "0", size=20) + button(156, 968, 120, 76, "− 10", "secondary", "←", size=22)
    o += button(288, 968, 120, 76, "+ 10", "secondary", "→", size=22) + button(420, 968, 124, 76, "MAX", "secondary", "M", size=20)
    o += button(24, 1060, 520, 92, "DEAL", "primary", "ENTER", size=28)
    o += T(284, 1190, "LAST HAND −40 · 12 HANDS · BEST RUN 4", 13, IVORY, MONO, 500, "middle", 2, 'opacity=".5"')
    return svg(W, H, o, defs(*BJ, cy="52%"))


def bj_win():
    o = bj_base("TWO DECKS · 58 IN THE SHOE") + T(284, 132, "DEALER", 12, GOLD, MONO, 500, "middle", 5, 'opacity=".8"')
    o += card(180, 150, 124, 4, "c", dim=.3) + card(264, 150, 124, 12, "c", dim=.3) + badge(126, 237, 26, "14")
    o += R(0, 548, W, 132, "#050102", 0, 'opacity=".72"') + rule(548, 0, 568, .7) + rule(680, 0, 568, .7) + rule(554, 0, 568, .25) + rule(674, 0, 568, .25)
    o += T(284, 624, "Blackjack", 68, IVORY, SERIF, 500, "middle", -1, 'font-style="italic"')
    o += T(284, 660, "PAYS 3 TO 2  ·  +75", 15, GOLD, COND, 700, "middle", 6)
    o += stack(84, 620, 26, [50]) + stack(484, 624, 26, [10, 10, 50]) + suit(40, 614, 18, "s", GOLD) + suit(528, 614, 18, "s", GOLD)
    o += card(180, 710, 124, 1, "h", lift=8) + card(264, 710, 124, 12, "d", lift=8) + badge(126, 790, 32, "21", gold=True)
    o += T(284, 916, "YOUR HAND", 12, GOLD, MONO, 500, "middle", 5, 'opacity=".8"') + bank("1,075", "50", "+75")
    o += button(24, 1036, 300, 88, "DEAL AGAIN", "primary", "ENTER", "SAME BET · 50") + button(336, 1036, 98, 88, "− 10", "secondary", "←")
    o += button(446, 1036, 98, 88, "+ 10", "secondary", "→")
    o += T(284, 1170, "HAND 13 · 7 WON · 1 BLACKJACK", 13, IVORY, MONO, 500, "middle", 2, 'opacity=".5"')
    return svg(W, H, o, defs(*BJ))


# ------------------------------------------------------------------ poker
ADA, LIN, GRA = "#56c2ae", "#f0924a", "#d59ad8"


def pk_table(x, y, w, h):
    r = min(w, h) / 2
    return ('<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="#0f0d17" stroke="#3a3350" stroke-width="1.5"/>' % (x, y, w, h, r)
            + '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="%s" stroke-opacity=".35" stroke-width="1"/>' % (x + 6, y + 6, w - 12, h - 12, r - 6, GOLD)
            + '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="url(#cloth)"/>' % (x + 24, y + 24, w - 48, h - 48, r - 24)
            + '<clipPath id="tc"><rect x="%s" y="%s" width="%s" height="%s" rx="%s"/></clipPath>' % (x + 24, y + 24, w - 48, h - 48, r - 24)
            + '<g clip-path="url(#tc)"><rect x="%s" y="%s" width="%s" height="%s" fill="#fff" filter="url(#grain)" opacity=".16"/></g>' % (x + 24, y + 24, w - 48, h - 48)
            + '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="#000" stroke-opacity=".5" stroke-width="6"/>' % (x + 26, y + 26, w - 52, h - 52, r - 26)
            + '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="none" stroke="%s" stroke-opacity=".22" stroke-width="1"/>' % (x + 62, y + 62, w - 124, h - 124, r - 62, GOLD))


def who(cx, cy, name, stackv, color, anchor="middle", out=False):
    return ('<g opacity="%s">' % (.4 if out else 1) + T(cx, cy, name, 17, IVORY, COND, 700, anchor, 3)
            + T(cx, cy + 20, stackv, 14, color, MONO, 500, anchor, 1) + '</g>')


def dealer_btn(cx, cy):
    return '<circle cx="%s" cy="%s" r="15" fill="%s" stroke="#8a7a52" stroke-width="1.5" filter="url(#sh)"/>' % (cx, cy, IVORY) + T(cx, cy + 6, "D", 16, INK, COND, 700, "middle")


def street(cx, y, n, label):
    o = T(cx, y, label, 12, GOLD, MONO, 500, "middle", 5, 'opacity=".8"')
    for i in range(4):
        o += '<circle cx="%s" cy="%s" r="3" fill="%s" opacity="%s"/>' % (cx - 21 + i * 14, y + 14, GOLD, .9 if i < n else .25)
    return o


def pk_controls(x, y, w, landscape=False):
    o = ""
    pre = [("MIN", "40"), ("½ POT", "120"), ("POT", "240"), ("ALL IN", "940")]
    if landscape:
        pw = (w - 12) / 2
        for i, (a, b) in enumerate(pre):
            o += button(x + (i % 2) * (pw + 12), y + (i // 2) * 66, pw, 56, a, "primary" if i == 1 else "secondary", size=17)
        y2 = y + 150
    else:
        pw = (w - 36) / 4
        for i, (a, b) in enumerate(pre):
            o += button(x + i * (pw + 12), y, pw, 56, a, "primary" if i == 1 else "secondary", sub=b, size=16)
        y2 = y + 84
    o += button(x, y2, 64, 64, "−", "secondary", size=30) + button(x + w - 64, y2, 64, 64, "+", "secondary", size=30)
    tx1, tx2 = x + 84, x + w - 84
    o += '<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="%s" stroke-width="3" stroke-linecap="round" opacity=".25"/>' % (tx1, y2 + 46, tx2, y2 + 46, IVORY)
    kx = tx1 + (tx2 - tx1) * .22
    o += '<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="%s" stroke-width="3" stroke-linecap="round"/>' % (tx1, y2 + 46, kx, y2 + 46, GOLD)
    o += '<circle cx="%s" cy="%s" r="10" fill="%s" stroke="%s" stroke-width="2"/>' % (kx, y2 + 46, IVORY, GOLD)
    o += T((tx1 + tx2) / 2, y2 + 24, "RAISE TO 120", 20, IVORY, COND, 700, "middle", 3)
    if landscape:
        o += button(x, y2 + 84, w, 72, "CALL 40", "primary", "C", size=24)
        o += button(x, y2 + 166, (w - 12) / 2, 64, "FOLD", "secondary", "F", size=20) + button(x + (w + 12) / 2, y2 + 166, (w - 12) / 2, 64, "RAISE", "secondary", "R", size=20)
    else:
        bw = (w - 24 - 208) / 2
        o += button(x, y2 + 84, bw, 88, "FOLD", "secondary", "F") + button(x + bw + 12, y2 + 84, 208, 88, "CALL", "primary", "C", "40")
        o += button(x + bw + 232, y2 + 84, bw, 88, "RAISE", "secondary", "R", "TO 120")
    return o


def pk_turn():
    o = R(0, 0, W, H, "#050409") + topbar(W, "HOLD'EM", "HAND 14 · BLINDS 10 / 20") + pk_table(24, 122, 520, 780)
    # Linus (top), raised
    o += back(234, 196, 48, rot=-7) + back(268, 196, 48, rot=7) + seat(284, 150, 38, "seed", LIN) + who(336, 140, "LINUS", "1,020", LIN, "start")
    o += stack(262, 318, 15, [10, 10, 10, 10], 3) + stack(296, 322, 15, [10, 10, 10, 10], 3) + tag(284, 356, "RAISE 80", LIN, stroke=LIN)
    # Ada (left), called
    o += back(92, 306, 48, rot=-7) + back(126, 306, 48, rot=7) + seat(56, 340, 36, "cog", ADA) + who(56, 404, "ADA", "900", ADA)
    o += stack(172, 396, 15, [10, 10, 50], 3) + tag(176, 430, "CALL 80", ADA, stroke=ADA)
    # Grace (right), folded
    o += seat(512, 340, 36, "moth", GRA, out=True) + who(512, 404, "GRACE", "980", GRA, out=True) + tag(430, 350, "FOLD", "#8d8799", stroke="#5c566b")
    # pot and board
    o += stack(204, 482, 17, [10, 10, 50, 100], 3) + stack(236, 488, 17, [10, 50, 50], 3)
    o += T(266, 476, "POT", 12, GOLD, MONO, 500, "start", 4, 'opacity=".8"') + T(266, 502, "240", 28, IVORY, COND, 700)
    for i, (r_, s_) in enumerate(((4, "h"), (12, "s"), (3, "s"), (13, "c"))):
        o += card(67 + i * 88, 524, 82, r_, s_)
    o += slot(67 + 4 * 88, 524, 82) + street(284, 668, 3, "TURN")
    # you
    o += dealer_btn(146, 742) + stack(430, 742, 15, [10, 10, 10, 10], 3) + T(430, 778, "40", 14, IVORY, MONO, 500, "middle", 1)
    o += card(174, 724, 108, 9, "d", rot=-5) + card(286, 724, 108, 12, "c", rot=5)
    o += tag(284, 712, "PAIR OF QUEENS", INK, IVORY, IVORY, 13)
    o += T(284, 928, "YOU · 940", 15, IVORY, COND, 700, "middle", 4) + T(284, 948, "40 TO CALL · POT ODDS 6 TO 1", 11.5, GOLD, MONO, 500, "middle", 2, 'opacity=".8"')
    o += pk_controls(24, 966, 520)
    return svg(W, H, o, defs(*PK, cy="48%", rr="62%"))


def pk_showdown():
    o = R(0, 0, W, H, "#050409") + topbar(W, "HOLD'EM", "HAND 14 · SHOWDOWN") + pk_table(24, 122, 520, 780)
    o += card(214, 192, 66, 13, "h", rot=-5, lift=4) + card(288, 192, 66, 8, "d", rot=5, lift=4)
    o += seat(284, 150, 38, "seed", LIN, active=True) + who(336, 140, "LINUS", "1,420", LIN, "start")
    o += tag(284, 304, "THREE KINGS", INK, GOLD, GOLD, 13)
    o += stack(246, 350, 17, [10, 10, 50, 100, 100], 3) + stack(282, 356, 17, [10, 50, 50, 100], 3) + T(312, 362, "+400", 26, GOLD, COND, 700)
    o += card(86, 300, 50, 10, "c", rot=-6, dim=.45) + card(124, 300, 50, 9, "c", rot=6, dim=.45) + seat(56, 340, 36, "cog", ADA) + who(56, 404, "ADA", "740", ADA)
    o += T(132, 396, "KING HIGH", 11, IVORY, MONO, 500, "middle", 2, 'opacity=".55"')
    o += seat(512, 340, 36, "moth", GRA, out=True) + who(512, 404, "GRACE", "980", GRA, out=True)
    for i, (r_, s_, used) in enumerate(((4, "h", 0), (12, "s", 1), (3, "s", 0), (13, "c", 1), (13, "d", 1))):
        o += card(67 + i * 88, 524, 82, r_, s_, lift=10 if used else 0, dim=0 if used else .5)
    o += street(284, 668, 4, "RIVER") + dealer_btn(146, 742)
    o += card(174, 724, 108, 9, "d", rot=-5, dim=.4) + card(286, 724, 108, 12, "c", rot=5, dim=.4) + tag(284, 712, "TWO PAIR · KINGS AND QUEENS", IVORY, "#0c0b12", IVORY, 13)
    o += T(284, 928, "YOU · 860", 15, IVORY, COND, 700, "middle", 4)
    o += T(284, 1010, "Linus takes it.", 46, IVORY, SERIF, 500, "middle", -1, 'font-style="italic"')
    o += T(284, 1046, "THREE KINGS BEAT KINGS AND QUEENS", 12, GOLD, MONO, 500, "middle", 2, 'opacity=".8"')
    o += button(24, 1082, 520, 92, "NEXT HAND", "primary", "ENTER", size=28) + T(284, 1206, "DEALS ITSELF IN 4 · TAP THE TABLE TO HOLD", 12, IVORY, MONO, 500, "middle", 2, 'opacity=".45"')
    return svg(W, H, o, defs(*PK, cy="48%", rr="62%"))


def pk_land():
    w, h = 1232, 568
    o = R(0, 0, w, h, "#050409") + pk_table(60, 28, 830, 512)
    o += '<path d="M42 34L26 50L42 66" fill="none" stroke="%s" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/>' % IVORY
    o += back(430, 86, 44, rot=-7) + back(462, 86, 44, rot=7) + seat(475, 56, 34, "seed", LIN) + who(522, 50, "LINUS", "1,020", LIN, "start")
    o += stack(560, 126, 14, [10, 10, 10, 10], 3) + tag(628, 130, "RAISE 80", LIN, stroke=LIN)
    o += back(128, 240, 44, rot=-7) + back(160, 240, 44, rot=7) + seat(92, 276, 34, "cog", ADA) + who(92, 336, "ADA", "900", ADA)
    o += stack(232, 300, 14, [10, 10, 50], 3) + tag(238, 334, "CALL 80", ADA, stroke=ADA)
    o += seat(858, 276, 34, "moth", GRA, out=True) + who(858, 336, "GRACE", "980", GRA, out=True) + tag(790, 222, "FOLD", "#8d8799", stroke="#5c566b")
    o += stack(388, 178, 16, [10, 10, 50, 100], 3) + stack(418, 184, 16, [10, 50, 50], 3) + T(444, 178, "POT", 12, GOLD, MONO, 500, "start", 4) + T(444, 202, "240", 26, IVORY, COND, 700)
    for i, (r_, s_) in enumerate(((4, "h"), (12, "s"), (3, "s"), (13, "c"))):
        o += card(258 + i * 88, 216, 82, r_, s_)
    o += slot(258 + 4 * 88, 216, 82) + street(730, 268, 3, "TURN")
    o += dealer_btn(352, 420) + card(380, 376, 96, 9, "d", rot=-5) + card(478, 376, 96, 12, "c", rot=5) + tag(476, 366, "PAIR OF QUEENS", INK, IVORY, IVORY, 13)
    o += stack(612, 420, 14, [10, 10, 10, 10], 3) + T(640, 426, "40", 14, IVORY, MONO, 500, "start", 1)
    o += T(1064, 46, "YOU · 940", 15, IVORY, COND, 700, "middle", 4) + T(1064, 66, "40 TO CALL · ODDS 6 TO 1", 11, GOLD, MONO, 500, "middle", 2)
    o += pk_controls(916, 84, 296, True)
    return svg(w, h, o, defs(*PK, cx="38%", cy="50%", rr="45%"))


# ------------------------------------------------------------------ solitaire
POS = [[], [(10, "d"), (9, "c"), (8, "d"), (7, "c"), (6, "h")], [(13, "h")], [(8, "h")], [(6, "d"), (5, "c"), (4, "h"), (3, "c")],
       [(12, "s"), (11, "h"), (10, "c"), (9, "h"), (8, "s"), (7, "h"), (6, "c"), (5, "h"), (4, "c")], [(12, "d")]]
DOWN = [0, 1, 0, 2, 4, 1, 2]
GREEN_BACK = dict(tone="#7a1f2b", line="#b0505c")


def so_icon(kind, cx, cy):
    p = {"undo": "M8-8H-4A9 9 0 0 0-4 10H8M-9-8L-3-14M-9-8L-3-2", "hint": "M0-13A9 9 0 0 1 5 3V7H-5V3A9 9 0 0 1 0-13ZM-4 12H4",
         "auto": "M-10-9L0 0L-10 9M2-9L12 0L2 9", "new": "M-10 0H10M0-10V10"}[kind]
    return '<path d="%s" transform="translate(%s %s)" fill="none" stroke="%s" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>' % (p, cx, cy, IVORY)


def so_bar(x, y, w, items):
    bw = (w - 12 * (len(items) - 1)) / len(items)
    o = ""
    for i, (kind, label, key) in enumerate(items):
        bx = x + i * (bw + 12)
        o += button(bx, y, bw, 76, "", "secondary", key) + so_icon(kind, bx + bw / 2, y + 28) + T(bx + bw / 2, y + 60, label, 13, IVORY, COND, 700, "middle", 3)
    return o


def so_play():
    cw, gap, x0, top, ty = 72, 6, 14, 104, 232
    cx = lambda i: x0 + i * (cw + gap)
    o = cloth(W, H) + R(0, 960, W, 272, "url(#fade)") + topbar(W, "KLONDIKE", "DRAW THREE · DEAL 4,812")
    o += back(cx(0), top, cw, **GREEN_BACK) + back(cx(0) - 2, top - 2, cw, **GREEN_BACK) + T(cx(0) + cw / 2, top - 9, "15 LEFT", 10, IVORY, MONO, 500, "middle", 1, 'opacity=".6"')
    for i, (r_, s_) in enumerate(((5, "d"), (11, "c"), (8, "c"))):
        o += card(cx(1) + i * 24, top, cw, r_, s_)
    for i, (r_, s_) in enumerate(((5, "s"), (2, "h"), (1, "c"), (4, "d"))):
        o += card(cx(3 + i), top, cw, r_, s_)
    o += slot(cx(0), ty, cw, glow=True) + T(cx(0) + cw / 2, ty + 62, "K", 34, IVORY, SERIF, 600, "middle", 0, 'opacity=".8"')
    for c in range(1, 7):
        y = ty
        for _ in range(DOWN[c]):
            o += back(cx(c), y, cw, shadow=False, **GREEN_BACK)
            y += 12
        for i, (r_, s_) in enumerate(POS[c]):
            o += card(cx(c), y, cw, r_, s_, shadow=(i == 0), lift=14 if c == 2 else 0)
            y += 34
    o += T(284, 1010, "King of hearts in hand", 30, IVORY, SERIF, 500, "middle", 0, 'font-style="italic"')
    o += T(284, 1040, "ONE PLACE TAKES IT · TAP THE LIT COLUMN", 12, GOLD, MONO, 500, "middle", 2.5, 'opacity=".8"')
    o += rule(1068) + T(28, 1098, "TIME", 11, GOLD, MONO, 500, "start", 3) + T(76, 1100, "04:12", 20, IVORY, COND, 700)
    o += T(284, 1098, "SCORE  ", 11, GOLD, MONO, 500, "end", 3) + T(288, 1100, "320", 20, IVORY, COND, 700)
    o += T(486, 1098, "MOVES", 11, GOLD, MONO, 500, "end", 3) + T(540, 1100, "60", 20, IVORY, COND, 700, "end")
    o += so_bar(14, 1126, 540, [("undo", "UNDO", "U"), ("hint", "HINT", "H"), ("auto", "FINISH", "F"), ("new", "NEW", "N")])
    return svg(W, H, o, defs(*SO, cy="36%"))


def so_win():
    cw, gap, x0, top = 72, 6, 14, 104
    cx = lambda i: x0 + i * (cw + gap)
    o = cloth(W, H) + R(0, 900, W, 332, "url(#fade)") + topbar(W, "KLONDIKE", "DRAW THREE · DEAL 4,812")
    o += slot(cx(0), top, cw) + slot(cx(1), top, cw)
    import math
    trail = [("s", 3, 96), ("h", 4, 250), ("c", 5, 400), ("d", 6, 520)]
    for s_, col, endx in trail:
        for k in range(9, 0, -1):
            t = k / 9.0
            x = cx(col) + (endx - cx(col) - 100) * t
            y = top + 760 * t * t + 60 * abs(math.sin(t * 7))
            o += card(x, y, cw, 13 - k if k < 12 else 1, s_, rot=(k * 23 + col * 11) % 50 - 25, shadow=k % 3 == 0)
    for i, s_ in enumerate("shcd"):
        o += card(cx(3 + i), top, cw, 13, s_)
    o += R(0, 430, W, 250, "#010504", 0, 'opacity=".78"') + rule(430, 0, 568, .7) + rule(680, 0, 568, .7)
    o += T(284, 524, "Solved.", 84, IVORY, SERIF, 500, "middle", -2, 'font-style="italic"')
    for i, (a, b) in enumerate((("TIME", "04:12"), ("MOVES", "212"), ("SCORE", "1,840"), ("STREAK", "3"))):
        x = 82 + i * 135
        o += T(x, 590, a, 11, GOLD, MONO, 500, "middle", 3) + T(x, 626, b, 28, IVORY, COND, 700, "middle")
    o += T(284, 660, "A NEW BEST TIME · 22 SECONDS FASTER", 12, GOLD, MONO, 500, "middle", 3)
    o += button(24, 1060, 340, 92, "NEW DEAL", "primary", "ENTER", size=28) + button(376, 1060, 168, 92, "STATS", "secondary", "S", size=22)
    o += T(284, 1190, "48 DEALS · 19 SOLVED · BEST 03:50", 13, IVORY, MONO, 500, "middle", 2, 'opacity=".5"')
    return svg(W, H, o, defs(*SO, cy="36%"))


def so_land():
    w, h, cw, x0, ty = 1232, 568, 96, 196, 28
    cx = lambda i: x0 + i * (cw + 14)
    o = cloth(w, h) + '<path d="M42 34L26 50L42 66" fill="none" stroke="%s" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/>' % IVORY
    o += back(72, 96, cw, **GREEN_BACK) + T(120, 250, "15", 12, IVORY, MONO, 500, "middle", 1, 'opacity=".6"')
    for i, (r_, s_) in enumerate(((5, "d"), (11, "c"), (8, "c"))):
        o += card(72, 270 + i * 40, cw, r_, s_)
    for i, (r_, s_) in enumerate(((5, "s"), (2, "h"), (1, "c"), (4, "d"))):
        o += card(1002 + (i % 2) * 108, 28 + (i // 2) * 146, cw, r_, s_)
    o += slot(cx(0), ty, cw, glow=True) + T(cx(0) + cw / 2, ty + 84, "K", 44, IVORY, SERIF, 600, "middle", 0, 'opacity=".8"')
    for c in range(1, 7):
        y = ty
        for _ in range(DOWN[c]):
            o += back(cx(c), y, cw, shadow=False, **GREEN_BACK)
            y += 12
        for i, (r_, s_) in enumerate(POS[c]):
            o += card(cx(c), y, cw, r_, s_, shadow=(i == 0), lift=14 if c == 2 else 0)
            y += 40
    o += T(1002, 356, "TIME", 11, GOLD, MONO, 500, "start", 3) + T(1002, 384, "04:12", 24, IVORY, COND, 700)
    o += T(1110, 356, "SCORE", 11, GOLD, MONO, 500, "start", 3) + T(1110, 384, "320", 24, IVORY, COND, 700)
    o += so_bar(1002, 400, 204, [("undo", "UNDO", "U"), ("hint", "HINT", "H")]) + so_bar(1002, 484, 204, [("auto", "FINISH", "F"), ("new", "NEW", "N")])
    return svg(w, h, o, defs(*SO, cx="48%", cy="45%", rr="70%"))


# ------------------------------------------------------------------ sheets and the other two directions
def sheet():
    w, h = 1232, 900
    o = R(0, 0, w, h, "#0b0a09") + T(40, 54, "LAMPLIGHT DECK", 16, GOLD, COND, 700, "start", 7)
    o += T(40, 78, "TABLE SIZE 124 px · BOARD 82 px · COLUMN 72 px · EVERY CARD SHOWN AT 1:1 PANEL PIXELS", 11, IVORY, MONO, 500, "start", 2, 'opacity=".55"')
    for i, (r_, s_) in enumerate(((1, "s"), (7, "h"), (10, "c"), (11, "d"), (12, "s"), (13, "h"))):
        o += card(40 + i * 140, 104, 124, r_, s_)
    o += back(40 + 6 * 140, 104, 124) + back(40 + 7 * 140, 104, 124, tone="#7a1f2b", line="#b0505c")
    for i, (r_, s_) in enumerate(((1, "h"), (2, "s"), (5, "d"), (9, "c"), (10, "h"), (11, "s"), (12, "h"), (13, "c"))):
        o += card(40 + i * 96, 320, 82, r_, s_)
    o += back(40 + 8 * 96, 320, 82, tone="#17402f", line="#3f7a60") + back(40 + 9 * 96, 320, 82, tone="#2a2350", line="#5a4f96")
    for i in range(13):
        o += card(40 + i * 80, 480, 72, i + 1, "shdc"[i % 4])
    o += back(40 + 13 * 80, 480, 72)
    o += T(40, 640, "FAN · 34 px", 11, IVORY, MONO, 500, "start", 2, 'opacity=".55"')
    for i, (r_, s_) in enumerate(((13, "s"), (12, "h"), (11, "c"), (10, "d"), (9, "s"))):
        o += card(40, 656 + i * 34, 72, r_, s_, shadow=i == 0)
    o += T(170, 640, "CHIPS · 10 / 50 / 100 / 500", 11, IVORY, MONO, 500, "start", 2, 'opacity=".55"')
    for i, v in enumerate((10, 50, 100, 500)):
        o += chip(210 + i * 96, 700, 38, v) + stack(210 + i * 96, 830, 26, [v] * (3 + i))
    o += T(590, 640, "SEATS · ADA, LINUS, GRACE · DEALER BUTTON · TAGS", 11, IVORY, MONO, 500, "start", 2, 'opacity=".55"')
    o += seat(630, 706, 40, "cog", ADA) + seat(740, 706, 40, "seed", LIN, active=True) + seat(850, 706, 40, "moth", GRA) + dealer_btn(930, 706)
    o += tag(650, 800, "RAISE 80", LIN, stroke=LIN) + tag(770, 800, "ALL IN", INK, GOLD, GOLD) + tag(890, 800, "PAIR OF QUEENS", INK, IVORY, IVORY)
    o += button(990, 670, 200, 80, "HIT", "primary", "H") + button(990, 764, 200, 80, "STAND", "secondary", "S")
    return svg(w, h, o, defs(*BJ))


def dir_imprint():
    P, V = "#efe6d2", "#d1432b"
    t = lambda x, y, s, size, fill=P, font=MONO, w=500, a="start", ls=2, e="": T(x, y, s, size, fill, font, w, a, ls, e)
    ln = lambda y, op=.3: '<line x1="28" y1="%s" x2="540" y2="%s" stroke="%s" stroke-width="1" opacity="%s"/>' % (y, y, P, op)
    o = R(0, 0, W, H, "#12100d") + t(28, 58, "‹", 40, P, SERIF, 500, "start", 0) + t(540, 52, "No. 13", 15, P, SERIF, 500, "end", 0, 'font-style="italic"')
    o += T(28, 150, "Blackjack", 76, P, SERIF, 600, "start", -3) + ln(176, .8) + ln(181)
    o += t(28, 208, "THE DEALER", 12) + t(540, 208, "STANDS ON ALL 17s", 12, P, MONO, 500, "end", 2, 'opacity=".5"')
    o += card(28, 228, 132, 9, "s", "imprint") + back(172, 228, 132, "imprint") + T(540, 380, "9", 150, P, SERIF, 500, "end", -6, 'opacity=".9"')
    o += ln(436) + t(28, 468, "THE STAKE", 12) + T(540, 520, "80", 64, V, SERIF, 600, "end", -2) + t(28, 512, "Blackjack pays three to two.", 19, P, SERIF, 500, "start", 0, 'font-style="italic" opacity=".7"')
    o += ln(548) + t(28, 580, "YOUR HAND", 12)
    o += card(28, 600, 132, 4, "h", "imprint") + card(118, 600, 132, 3, "d", "imprint") + card(208, 600, 132, 11, "s", "imprint")
    o += T(540, 770, "17", 170, P, SERIF, 500, "end", -8)
    o += ln(816) + t(28, 848, "IN THE BANK", 12) + T(540, 880, "920", 48, P, SERIF, 600, "end", -1) + ln(908, .8)
    for i, (a, k) in enumerate((("Hit", "H"), ("Stand", "S"), ("Double", "D"))):
        y = 980 + i * 82
        o += T(28, y, a, 52, V if i == 0 else P, SERIF, 600 if i == 0 else 500, "start", -1, 'font-style="italic"' if i else "") + t(540, y - 6, k, 15, P, MONO, 500, "end", 0, 'opacity=".5"') + ln(y + 22)
    o += t(28, 1204, "SEVENTEEN AGAINST A NINE", 12, P, MONO, 500, "start", 3, 'opacity=".5"')
    return svg(W, H, o, defs(*BJ))


def dir_signal():
    C = "#ff4d6d"
    o = R(0, 0, W, H, "#000")
    for i, r_ in enumerate((250, 330, 410, 520)):
        o += '<circle cx="284" cy="620" r="%s" fill="none" stroke="%s" stroke-width="%s" opacity="%s"/>' % (r_, C, 2 if i == 0 else 1, (.9, .35, .2, .1)[i])
    o += '<circle cx="284" cy="620" r="250" fill="%s" opacity=".07"/>' % C
    o += T(28, 60, "←", 26, "#fff", MONO, 500) + T(284, 58, "BLACKJACK", 15, "#fff", MONO, 500, "middle", 8) + T(540, 58, "SHOE 62", 12, C, MONO, 500, "end", 2)
    o += T(284, 136, "DEALER", 12, "#fff", MONO, 500, "middle", 6, 'opacity=".6"')
    o += card(160, 156, 120, 9, "s", "signal") + back(288, 156, 120, "signal")
    o += R(238, 344, 92, 44, "#000", 22, 'stroke="#fff" stroke-width="1.5"') + T(284, 374, "9", 24, "#fff", MONO, 500, "middle")
    o += T(284, 600, "80", 120, "#fff", COND, 700, "middle", -4) + T(284, 636, "ON THE LINE", 12, C, MONO, 500, "middle", 6)
    o += card(96, 706, 120, 4, "h", "signal") + card(224, 706, 120, 3, "d", "signal") + card(352, 706, 120, 11, "s", "signal")
    o += R(232, 890, 104, 48, C, 24) + T(284, 923, "17", 26, "#000", MONO, 500, "middle")
    o += T(28, 1000, "BANK", 12, "#fff", MONO, 500, "start", 4, 'opacity=".6"') + T(540, 1004, "920", 34, "#fff", COND, 700, "end")
    for i, (a, w_, f) in enumerate((("HIT", 196, 1), ("STAND", 156, 0), ("DOUBLE", 156, 0))):
        x = (24, 232, 400)[i] if i < 2 else 400
        w_ = (196, 156, 144)[i]
        o += R(x, 1040, w_, 88, C if f else "#000", 44, 'stroke="%s" stroke-width="1.5"' % (C if f else "#fff")) + T(x + w_ / 2, 1094, a, 22, "#000" if f else "#fff", COND, 700, "middle", 3)
    o += T(284, 1180, "17 VS 9 // HIT OR STAND", 12, "#fff", MONO, 500, "middle", 3, 'opacity=".5"')
    return svg(W, H, o, defs(*BJ))


SCREENS = {
    "blackjack-bet": (bj_bet, W, H), "blackjack-play": (bj_play, W, H), "blackjack-win": (bj_win, W, H),
    "poker-turn": (pk_turn, W, H), "poker-showdown": (pk_showdown, W, H), "poker-turn-landscape": (pk_land, 1232, 568),
    "solitaire-play": (so_play, W, H), "solitaire-win": (so_win, W, H), "solitaire-play-landscape": (so_land, 1232, 568),
    "deck-sheet": (sheet, 1232, 900), "direction-b-imprint": (dir_imprint, W, H), "direction-c-signal": (dir_signal, W, H),
}

FONTS = ('<link href="https://fonts.googleapis.com/css2?family=Fraunces:ital,opsz,wght@0,9..144,500;0,9..144,600;1,9..144,500&'
         'family=IBM+Plex+Mono:wght@500&family=IBM+Plex+Sans+Condensed:wght@500;700&display=block" rel="stylesheet">')
PAGE = ('<!doctype html><html><head><meta charset="utf-8">%s<style>html,body{margin:0;background:#000;overflow:hidden}svg,img{display:block}'
        'body{font-family:"IBM Plex Mono";color:#f3ead6}%s</style></head><body>%s</body></html>')


def browser():
    c = [os.environ.get("MOCKUP_BROWSER"), r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
         r"C:\Program Files\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Google\Chrome\Application\chrome.exe",
         shutil.which("chromium"), shutil.which("google-chrome")]
    b = next((x for x in c if x and os.path.exists(x)), None)
    if not b:
        raise SystemExit("no Edge or Chrome found (set MOCKUP_BROWSER)")
    return b


def shoot(b, tmp, html, png, w, h):
    page = pathlib.Path(tmp) / (png.stem + ".html")
    page.write_text(html, encoding="utf-8")
    if png.exists():
        png.unlink()
    p = subprocess.Popen([b, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1", "--allow-file-access-from-files",
                          "--user-data-dir=" + str(pathlib.Path(tmp) / "profile"), "--virtual-time-budget=9000",
                          "--window-size=%d,%d" % (w, h), "--screenshot=" + str(png), page.as_uri()], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p.wait(timeout=90)
    except subprocess.TimeoutExpired:
        p.kill()
    print(("ok   " if png.exists() else "FAIL ") + png.name)


def board(title, sub, cols, w):
    """A presentation board: cols = [(caption, [image paths...])]."""
    cells = ""
    for cap, imgs in cols:
        cells += '<div class="c"><div class="cap">%s</div>%s</div>' % (cap, "".join('<img src="%s">' % pathlib.Path(i).resolve().as_uri() for i in imgs))
    css = ('.b{padding:44px 48px;width:%dpx;box-sizing:border-box;background:#0b0a09}h1{font:600 44px Fraunces;margin:0 0 6px;letter-spacing:-1px}'
           '.s{font-size:13px;letter-spacing:3px;opacity:.6;margin-bottom:30px}.r{display:flex;gap:28px;align-items:flex-start}'
           '.cap{font-size:12px;letter-spacing:3px;color:#d9b66a;margin-bottom:12px;height:34px}img{border-radius:22px;border:1px solid #2a2622;margin-bottom:18px}' % w)
    return PAGE % (FONTS, css, '<div class="b"><h1>%s</h1><div class="s">%s</div><div class="r">%s</div></div>' % (title, sub, cells))


def main():
    names = sys.argv[1:] or list(SCREENS) + ["boards"]
    OUT.mkdir(parents=True, exist_ok=True)
    b, tmp = browser(), tempfile.mkdtemp(prefix="card-games-")
    try:
        for n in names:
            if n in SCREENS:
                fn, w, h = SCREENS[n]
                doc = fn()
                (OUT / (n + ".svg")).write_text(doc, encoding="utf-8", newline="\n")
                shoot(b, tmp, PAGE % (FONTS, "", doc), OUT / (n + ".png"), w, h)
        if "boards" in names:
            m, cur = (lambda n: OUT / (n + ".png")), (lambda n: HERE / "current" / (n + ".png"))
            shoot(b, tmp, board("Three directions", "THE SAME BLACKJACK HAND, THREE WAYS",
                                [("A · LAMPLIGHT — RECOMMENDED", [m("blackjack-play")]), ("B · IMPRINT", [m("direction-b-imprint")]),
                                 ("C · SIGNAL", [m("direction-c-signal")])], 1856), OUT / "board-directions.png", 1856, 1420)
            shoot(b, tmp, board("Before and after", "TODAY'S SIMULATOR CAPTURES BESIDE THE LAMPLIGHT MOCKUPS, SAME PANEL PIXELS",
                                [("BLACKJACK · TODAY", [cur("blackjack-play")]), ("BLACKJACK · LAMPLIGHT", [m("blackjack-play")]),
                                 ("POKER · TODAY", [cur("poker-flop")]), ("POKER · LAMPLIGHT", [m("poker-turn")]),
                                 ("SOLITAIRE · TODAY", [cur("solitaire-selected")]), ("SOLITAIRE · LAMPLIGHT", [m("solitaire-play")])], 3644),
                  OUT / "board-before-after.png", 3644, 1420)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
