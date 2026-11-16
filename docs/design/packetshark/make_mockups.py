#!/usr/bin/env python3
"""Render the PacketShark mockups (568x1232 portrait, 1232x568 landscape).

    python make_mockups.py            # writes *.png and layout.json beside this file

Deterministic: the same repository fonts give byte-identical PNGs. Sample
traffic uses RFC 1918 and RFC 5737 addresses, example.net names and
locally-administered MAC addresses; none of it is a real capture.
"""
import json
import math
import os

from pskdraw import (Canvas, T, CLASSES, class_rgb, F_CAPTION, F_DENSE, F_VALUE, F_BUTTON,
                     F_BODY, F_ROW, F_TITLE, F_HERO)

OUT = os.path.dirname(os.path.abspath(__file__))
W, H = 568, 1232
FX, FW = 20, 528            # the app frame: 528 x 1116 at (20, 96), DS 35.2
FY = 96
FOOT = 1202                 # frame foot kept 30 px clear of the bottom corners (DS 22.2)
BAR_Y, BAR_H = 1138, 64     # the capture bar
TAB_H = 56
LAYOUTS = {}


# ---- shared parts ------------------------------------------------------------

def header(c, hint, hint_color=None):
    """The shell's 72 px app header (DS 7, 36.3) under chrome NONE."""
    c.rrect(30, 8, 72, 56, 6, T["surface"])
    c.chevron(66, 36, 9, T["accent_primary"], "left")
    c.text(F_TITLE(), 118, (72 - F_TITLE().line_h) // 2, "PacketShark", T["text_primary"])
    c.text_r(F_CAPTION(), 538, (72 - F_CAPTION().line_h) // 2, hint,
             hint_color or T["text_secondary"])
    c.rect(0, 71, W, 1, T["line"])
    c.mark("header", 0, 0, W, 72)
    c.mark("header.back", 30, 8, 72, 56)


def face(c, x, y, w, h, label, kind="secondary", label_color=None):
    """A button face. primary / secondary / disabled / selected (DS 9)."""
    f = F_BUTTON()
    if kind == "primary":
        c.rrect(x, y, w, h, 6, T["accent_primary"])
        col = T["text_on_accent"]
    elif kind == "disabled":
        c.rrect(x, y, w, h, 6, T["surface"])
        col = T["disabled_fg"]
    elif kind == "selected":
        c.border(x, y, w, h, 6, T["focus"], 2, T["surface"])
        col = T["accent_primary"]
    else:
        c.border(x, y, w, h, 6, T["line"], 1, T["surface"])
        col = T["text_primary"]
    c.text_c(f, x + w // 2, y + (h - f.line_h) // 2, label, label_color or col)


def tabs(c, active, inspect_enabled=True, y=FY):
    """The section strip: three 56 px faces (DS 51.3 look)."""
    names = ["LIVE", "PACKETS", "INSPECT"]
    xs, ws = [20, 198, 377], [170, 171, 171]
    f = F_BUTTON()
    for i, n in enumerate(names):
        c.border(xs[i], y, ws[i], TAB_H, 6, T["line"], 1, T["surface"])
        if i == active:
            col = T["accent_primary"]
        elif i == 2 and not inspect_enabled:
            col = T["disabled_fg"]
        else:
            col = T["text_primary"]
        tw = f.width(n)
        c.text(f, xs[i] + (ws[i] - tw) // 2, y + (TAB_H - f.line_h) // 2, n, col)
        if i == active:
            c.rect(xs[i] + (ws[i] - tw) // 2, y + TAB_H - 8, tw, 2, T["accent_primary"])
        c.mark("tab." + n.lower(), xs[i], y, ws[i], TAB_H)


def capture_bar(c, buttons):
    """Three 64 px buttons along the foot: view action, HOLD/RESUME, STOP."""
    xs, ws = [20, 198, 377], [170, 171, 171]
    if len(buttons) == 1:
        label, kind = buttons[0]
        face(c, FX, BAR_Y, FW, BAR_H, label, kind)
        c.mark("bar.0", FX, BAR_Y, FW, BAR_H)
        return
    for i, (label, kind) in enumerate(buttons):
        face(c, xs[i], BAR_Y, ws[i], BAR_H, label, kind)
        c.mark("bar.%d" % i, xs[i], BAR_Y, ws[i], BAR_H)


def chip(c, x, y, label, kind="active", dot=True):
    """A 36 px chip (DS 7, 32.4). Returns its width."""
    f = F_CAPTION()
    w = 12 + (16 if dot else 0) + f.width(label) + 12
    if kind == "active":
        fill, col = T["accent_primary"], T["text_on_accent"]
    elif kind == "warn":
        fill, col = T["surface"], T["status_warn"]
    elif kind == "error":
        fill, col = T["surface"], T["status_error"]
    else:
        fill, col = T["surface"], T["text_secondary"]
    c.rrect(x, y, w, 36, 6, fill)
    tx = x + 12
    if dot == "pause":
        c.rect(tx, y + 13, 3, 10, col)
        c.rect(tx + 5, y + 13, 3, 10, col)
        tx += 16
    elif dot:
        c.disc(tx + 4, y + 18, 4, col)
        tx += 16
    c.text(f, tx, y + (36 - f.line_h) // 2, label, col)
    return w


def panel(c, x, y, w, h, title=None, color=None, t=1, title_color=None):
    """A hairline panel with its caption set in the top border (DS 2)."""
    c.border(x, y, w, h, 6, color or T["line"], t)
    if title:
        f = F_CAPTION()
        tw = f.width(title)
        c.rect(x + 16, y - 9, tw + 12, 18, T["bg"])
        c.text(f, x + 22, y - 9, title, title_color or T["text_secondary"])


def kv_row(c, x, y, w, key, value, value_color=None, divider=True, h=64):
    c.text(F_BODY(), x, y + (h - F_BODY().line_h) // 2, key, T["text_secondary"])
    c.text_r(F_VALUE(), x + w, y + (h - F_VALUE().line_h) // 2, value,
             value_color or T["text_primary"])
    if divider:
        c.rect(x, y + h - 1, w, 1, T["surface_raised"])


def fmt(n):
    return "{:,}".format(n).replace(",", " ")


# ---- sample traffic for the graph ---------------------------------------------

def lcg(seed):
    s = seed
    while True:
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        yield s / 0x7FFFFFFF


def make_bins():
    """Sixty seconds of per-class packet counts: a quiet start, ordinary web
    traffic, a UDP media stream (seconds 22-37) and a TCP burst (40-43)."""
    r = lcg(20261115)
    bins = []
    burst = {40: 5200, 41: 6800, 42: 6100, 43: 3900}
    for i in range(60):
        wave = 0.5 + 0.5 * math.sin(i / 4.3)
        tcp = int(8 + 30 * next(r)) if i < 9 else int(140 + 900 * wave + 500 * next(r))
        tcp = burst.get(i, tcp)
        udp = int(20 + 40 * next(r)) + (int(380 + 160 * next(r)) if 22 <= i <= 37 else 0)
        dns = int(2 + 9 * next(r)) + (24 if i in burst else 0)
        dhcp = 1 if i in (5, 33) else 0
        arp = int(1 + 3 * next(r))
        icmp = int(3 * next(r))
        ip6 = int(1 + 5 * next(r))
        other = 1
        bins.append([tcp, udp, dns, dhcp, arp, icmp, ip6, other])
    return bins


GRAPH_H = 208
COLS, COL_W, COL_GAP = 60, 5, 2


def log_h(n):
    """Column height on the fixed log ladder: 1 pps = 4 px, +50 px per decade."""
    if n <= 0:
        return 0
    return min(GRAPH_H, int(4 + 50 * math.log10(n) + 0.5))


def split(counts, h):
    """rift_graph.c's rule: proportional, every class present gets >= 1 px."""
    total = sum(counts)
    px, given, big = [0] * len(counts), 0, 0
    for i, n in enumerate(counts):
        if n > counts[big]:
            big = i
        if total and n:
            px[i] = max(1, (h * n + total // 2) // total)
            given += px[i]
    px[big] += h - given
    return px


def traffic_graph(c, x, y, bins, selected=None, held_from=None):
    base = y + GRAPH_H
    # decade rules and their labels
    for n, lab in ((10, "10"), (100, "100"), (1000, "1k"), (10000, "10k")):
        ry = base - log_h(n)
        c.rect(x, ry, COLS * (COL_W + COL_GAP) - COL_GAP, 1, T["surface_raised"])
        c.text_r(F_DENSE(), x + 488, ry - 9, lab, T["text_muted"])
    for i in range(COLS):
        cx = x + i * (COL_W + COL_GAP)
        c.rect(cx, base, COL_W, 1, T["line"])       # a baseline under every second
        if held_from is not None and i >= held_from:
            c.rect(cx + 1, base - 5, 3, 2, T["text_muted"])   # held: counted, not stored
            continue
        total = sum(bins[i])
        h = log_h(total)
        px = split(bins[i], h)
        top = base - 1
        for k in range(len(CLASSES)):
            if px[k] > 0:
                c.rect(cx, top - px[k] + 1, COL_W, px[k], class_rgb(k))
                top -= px[k]
    if selected is not None:
        cx = x + selected * (COL_W + COL_GAP)
        c.border(cx - 3, y - 4, COL_W + 6, GRAPH_H + 9, 2, T["focus"], 2, None)
        # redraw the selected column inside the outline
        total = sum(bins[selected])
        px = split(bins[selected], log_h(total))
        top = base - 1
        c.rect(cx, base, COL_W, 1, T["line"])
        for k in range(len(CLASSES)):
            if px[k] > 0:
                c.rect(cx, top - px[k] + 1, COL_W, px[k], class_rgb(k))
                top -= px[k]
    c.mark("live.graph", x, y, COLS * (COL_W + COL_GAP) - COL_GAP, GRAPH_H + 1)


SHARES = [71.2, 14.6, 4.1, 0.2, 3.3, 0.4, 2.9, 3.3]
TOTAL = 48417


def protocols_grid(c, x, y, rates):
    cw, ch = 236, 52
    for k, (name, _) in enumerate(CLASSES):
        cx = x + (k % 2) * (cw + 16)
        cy = y + (k // 2) * ch
        col = class_rgb(k)
        c.rect(cx, cy + 12, 10, 10, col)
        c.text(F_CAPTION(), cx + 18, cy + 8, name, col)
        c.text(F_DENSE(), cx + 84, cy + 8, "%s/s" % fmt(rates[k]), T["text_muted"])
        c.text_r(F_VALUE(), cx + cw, cy + 2, "%.1f%%" % SHARES[k], T["text_primary"])
        c.rect(cx, cy + 36, cw, 3, T["surface_raised"])
        c.rect(cx, cy + 36, max(2, int(cw * SHARES[k] / 100)), 3, col)
        c.mark("live.proto.%s" % name.lower(), cx, cy, cw, ch)


def stat_cells(c, x, y, cells):
    cw = FW // len(cells) - 10
    for i, (label, value, color) in enumerate(cells):
        cx = x + i * (cw + 0)
        c.text(F_CAPTION(), cx, y, label, T["text_secondary"])
        c.text(F_VALUE(), cx, y + 22, value, color or T["text_primary"])
        if i:
            c.rect(cx - 12, y + 2, 1, 44, T["surface_raised"])


# ---- A: Live Capture ------------------------------------------------------------

def live(state="capturing"):
    c = Canvas(W, H)
    bins = make_bins()
    hold = state == "hold"
    header(c, "HOLD · ETH0" if hold else "CAPTURING · ETH0")
    tabs(c, 0)

    # status row: state chip, interface, elapsed
    y = 166
    if hold:
        w = chip(c, FX, y, "HOLD", "off", dot="pause")
    else:
        w = chip(c, FX, y, "CAPTURING", "active")
    c.text(F_VALUE(), FX + w + 14, y + 5, "eth0", T["text_primary"])
    c.text(F_CAPTION(), FX + w + 14 + F_VALUE().width("eth0") + 12, y + 9,
           "100 MB/S · HEADERS", T["text_secondary"])
    c.text_r(F_VALUE(), FX + FW, y + 5, "00:04:13", T["text_primary"])
    c.mark("live.status", FX, y, FW, 36)

    # TRAFFIC: hero rate, the graph, its readout
    py, ph = 228, 374
    panel(c, FX, py, FW, ph, "TRAFFIC · LAST 60 S")
    now = bins[59] if not hold else bins[50]
    pps = sum(now)
    c.text(F_HERO(), FX + 20, py + 16, fmt(pps), T["text_primary"])
    hx = FX + 20 + F_HERO().width(fmt(pps)) + 10
    c.text(F_CAPTION(), hx, py + 50, "PKT/S", T["text_secondary"])
    c.text_r(F_VALUE(), FX + FW - 20, py + 22, "9.6 Mb/s", T["text_primary"])
    c.text_r(F_CAPTION(), FX + FW - 20, py + 50, "THROUGHPUT", T["text_secondary"])
    gy = py + 92
    sel = None if hold else 41
    traffic_graph(c, FX + 20, gy, bins, selected=sel, held_from=51 if hold else None)
    ay = gy + GRAPH_H + 8
    c.text(F_DENSE(), FX + 20, ay, "-60 S", T["text_muted"])
    c.text_r(F_DENSE(), FX + 20 + 418, ay, "NOW", T["text_muted"])
    ry = ay + 26
    c.rect(FX + 20, ry - 6, FW - 40, 1, T["surface_raised"])
    if hold:
        c.text(F_CAPTION(), FX + 20, ry + 2, "HOLD 9 S · 9 204 COUNTED, NOT STORED",
               T["text_secondary"])
    else:
        tot = sum(bins[41])
        c.text(F_CAPTION(), FX + 20, ry + 2,
               "03:55 · %s PKT · TCP %d%%" % (fmt(tot), round(100 * bins[41][0] / tot)),
               T["text_primary"])
        c.text_r(F_CAPTION(), FX + FW - 20, ry + 2, "PACKETS", T["accent_primary"])
        c.chevron(FX + FW - 20 - F_CAPTION().width("PACKETS") - 10, ry + 11, 4,
                  T["accent_primary"], "right", 2)
    c.mark("live.traffic", FX, py, FW, ph)

    # PROTOCOLS: share of the session, tap to isolate
    py2, ph2 = 624, 240
    panel(c, FX, py2, FW, ph2, "PROTOCOLS · SESSION")
    protocols_grid(c, FX + 20, py2 + 18, now)
    c.mark("live.protocols", FX, py2, FW, ph2)

    # SESSION counters
    py3, ph3 = 886, 84
    panel(c, FX, py3, FW, ph3, "SESSION")
    stat_cells(c, FX + 20, py3 + 18, [
        ("PACKETS", fmt(TOTAL), None), ("BYTES", "31.4 MB", None),
        ("DROPPED", "0", T["status_ok"]), ("BUFFER", "74%", None)])
    c.mark("live.session", FX, py3, FW, ph3)

    # SIGNALS: findings, each a filter
    py4 = 992
    panel(c, FX, py4, FW, 110, "SIGNALS · TAP TO LIST")
    w = 114
    for i, (label, value) in enumerate((("DNS FAIL", 3), ("TCP RST", 12),
                                        ("RETRANS", 41), ("UNREACH", 1))):
        sx = FX + 14 + i * (w + 14)
        c.rrect(sx, py4 + 22, w, 68, 6, T["surface"])
        c.text(F_CAPTION(), sx + 12, py4 + 30, label, T["text_secondary"])
        c.text(F_VALUE(), sx + 12, py4 + 54, fmt(value), T["status_warn"])
        c.chevron(sx + w - 14, py4 + 67, 4, T["accent_primary"], "right", 2)
        c.mark("live.signal.%d" % i, sx, py4 + 22, w, 68)

    capture_bar(c, [("FILTER", "secondary"),
                    ("RESUME", "primary") if hold else ("HOLD", "secondary"),
                    ("STOP", "secondary")])
    return c


def live_setup(link=True):
    """Idle: nothing is captured until START. Also the no-link state."""
    c = Canvas(W, H)
    header(c, "NOT CAPTURING")
    tabs(c, 0, inspect_enabled=False)
    y = 166
    if link:
        chip(c, FX, y, "READY", "off", dot=False)
        c.text(F_CAPTION(), FX + 86, y + 9, "NOTHING IS CAPTURED UNTIL YOU START",
               T["text_secondary"])
    else:
        chip(c, FX, y, "NO LINK", "warn", dot=False)
        c.text(F_CAPTION(), FX + 104, y + 9, "CONNECT A CABLE TO ETH0", T["text_secondary"])

    py = 228
    panel(c, FX, py, FW, 2 * 72 + 20, "INTERFACE")
    # eth0
    ry = py + 10
    if link:
        c.border(FX + 6, ry + 4, FW - 12, 64, 6, T["focus"], 2, T["surface"])
    c.text(F_ROW(), FX + 20, ry + 10, "eth0", T["text_primary"])
    c.text(F_CAPTION(), FX + 20, ry + 40,
           "ETHERNET · 100 MB/S · 192.168.1.44" if link else "ETHERNET · UP, NO CARRIER",
           T["text_secondary"])
    if link:
        w = F_CAPTION().width("LINK UP") + 24
        c.rrect(FX + FW - 20 - w, ry + 18, w, 36, 6, T["accent_primary"])
        c.text(F_CAPTION(), FX + FW - 20 - w + 12, ry + 27, "LINK UP", T["text_on_accent"])
    else:
        w = F_CAPTION().width("NO LINK") + 24
        c.rrect(FX + FW - 20 - w, ry + 18, w, 36, 6, T["surface"])
        c.text(F_CAPTION(), FX + FW - 20 - w + 12, ry + 27, "NO LINK", T["status_warn"])
    c.rect(FX + 20, ry + 72, FW - 40, 1, T["surface_raised"])
    c.mark("setup.iface.eth0", FX + 6, ry + 4, FW - 12, 64)
    # wlan0: listed, not selectable in v0.1
    ry += 76
    c.text(F_ROW(), FX + 20, ry + 8, "wlan0", T["disabled_fg"])
    c.text(F_CAPTION(), FX + 20, ry + 38, "WI-FI · NOT AVAILABLE IN THIS VERSION",
           T["disabled_fg"])
    w = F_CAPTION().width("DOWN") + 24
    c.rrect(FX + FW - 20 - w, ry + 16, w, 36, 6, T["surface"])
    c.text(F_CAPTION(), FX + FW - 20 - w + 12, ry + 25, "DOWN", T["disabled_fg"])
    c.mark("setup.iface.wlan0", FX + 6, ry + 2, FW - 12, 64)

    py2 = 228 + 164 + 28
    panel(c, FX, py2, FW, 4 * 64 + 8, "CAPTURE OPTIONS")
    rows = [("Stored bytes", "HEADERS"), ("Stop after", "15 MIN"),
            ("Capture filter", "EVERYTHING"), ("Promiscuous mode", "OFF")]
    for i, (k, v) in enumerate(rows):
        kv_row(c, FX + 20, py2 + 4 + i * 64, FW - 40 - 22, k, v, divider=i < 3)
        c.chevron(FX + FW - 26, py2 + 4 + i * 64 + 32, 5, T["accent_primary"], "right", 2)
        c.mark("setup.option.%d" % i, FX, py2 + 4 + i * 64, FW, 64)

    py3 = py2 + 264 + 28
    panel(c, FX, py3, FW, 150, "PRIVACY")
    lines = ["Packets stay in memory on this device and are",
             "discarded when you leave, unless you save them.",
             "HEADERS keeps addresses, ports and protocol",
             "fields; payload only for DNS, DHCP, ARP and ICMP."]
    for i, line in enumerate(lines):
        c.text(F_BODY(), FX + 20, py3 + 20 + i * 26, line, T["text_secondary"])

    # the empty graph, as the DS empty state (dashed hairline panel)
    py4 = py3 + 150 + 28
    eh = BAR_Y - 22 - py4
    c.dashed(FX, py4, FW, eh, T["line"])
    c.disc(W // 2, py4 + eh // 2 - 18, 28, T["surface"])
    for i, hh in enumerate((8, 14, 20, 12)):
        c.rect(W // 2 - 13 + i * 7, py4 + eh // 2 - 8 - hh, 4, hh, T["text_secondary"])
    c.text_c(F_CAPTION(), W // 2, py4 + eh // 2 + 22, "NO CAPTURE YET", T["text_secondary"])
    c.mark("setup.empty", FX, py4, FW, eh)

    capture_bar(c, [("START CAPTURE", "primary" if link else "disabled")])
    return c


def live_error():
    """The helper ended on its own: an error panel, the last counters kept."""
    c = Canvas(W, H)
    header(c, "CAPTURE ENDED", T["status_error"])
    tabs(c, 0, inspect_enabled=False)
    chip(c, FX, 166, "STOPPED", "error", dot=False)
    c.text(F_CAPTION(), FX + 104, 175, "ETH0 · 00:04:13", T["text_secondary"])

    py, ph = 228, 372
    panel(c, FX, py, FW, ph, "CAPTURE HELPER · STOPPED", T["status_error"], 2,
          T["status_error"])
    c.text(F_TITLE(), FX + 20, py + 24, "Capture ended unexpectedly", T["text_primary"])
    for i, s in enumerate(["The capture helper stopped while it was running.",
                           "The packets it held in memory went with it.",
                           "Nothing was written to storage."]):
        c.text(F_BODY(), FX + 20, py + 66 + i * 26, s, T["text_secondary"])
    kv_row(c, FX + 20, py + 156, FW - 40, "Reason", "CRASHED (SIG 11)", T["status_error"])
    kv_row(c, FX + 20, py + 220, FW - 40, "Interface", "RELEASED", T["status_ok"])
    kv_row(c, FX + 20, py + 284, FW - 40, "Report", "LOGGED", divider=False)
    c.mark("error.panel", FX, py, FW, ph)

    py3 = 628
    panel(c, FX, py3, FW, 84, "LAST SESSION")
    stat_cells(c, FX + 20, py3 + 18, [
        ("PACKETS", fmt(TOTAL), None), ("BYTES", "31.4 MB", None),
        ("DROPPED", "0", None), ("SAVED", "NO", T["status_warn"])])
    capture_bar(c, [("NEW CAPTURE", "primary")])
    return c


# ---- B: Packet List ---------------------------------------------------------------

# seq, time, delta, dir, proto word, class index, addresses, len, info, problem
PACKETS = [
    (48191, "04:11.902114", "+0.210447", "↑", "DNS", 2, "192.168.1.20 → 192.168.1.1", 78,
     "Query A api.example.net", False),
    (48192, "04:11.914870", "+0.012756", "↓", "DNS", 2, "192.168.1.1 → 192.168.1.20", 94,
     "Response A 203.0.113.42", False),
    (48193, "04:11.915533", "+0.000663", "↑", "TCP", 0, "192.168.1.20 → 203.0.113.42", 74,
     "51234 → 443 [SYN] Seq=0 Win=64240", False),
    (48194, "04:11.941207", "+0.025674", "↓", "TCP", 0, "203.0.113.42 → 192.168.1.20", 74,
     "443 → 51234 [SYN ACK] Seq=0 Ack=1", False),
    (48195, "04:12.130876", "+0.189669", "↓", "TCP", 0, "203.0.113.42 → 192.168.1.20", 1514,
     "443 → 51234 [ACK] Seq=1449 Len=1448", False),
    (48196, "04:12.318430", "+0.187554", "↓", "TCP", 0, "203.0.113.42 → 192.168.1.20", 1514,
     "Retransmission Seq=1449 Len=1448", True),
    (48197, "04:12.318642", "+0.000212", "↑", "TCP", 0, "192.168.1.20 → 203.0.113.42", 66,
     "51234 → 443 [ACK] Seq=518 Ack=2897", False),
    (48198, "04:12.402991", "+0.084349", "↑", "DNS", 2, "192.168.1.20 → 192.168.1.1", 71,
     "Query AAAA printer.lan", False),
    (48199, "04:12.404120", "+0.001129", "↓", "DNS", 2, "192.168.1.1 → 192.168.1.20", 71,
     "NXDOMAIN printer.lan", True),
    (48200, "04:12.455009", "+0.050889", "·", "ARP", 4, "192.168.1.1 → Broadcast", 60,
     "Who has 192.168.1.44? Tell 192.168.1.1", False),
    (48201, "04:12.600001", "+0.144992", "·", "ICMPv6", 6, "fe80::1 → ff02::1", 118,
     "Router Advertisement", False),
    (48202, "04:12.733410", "+0.133409", "·", "DHCP", 3, "192.168.1.1 → 255.255.255.255", 342,
     "ACK 192.168.1.44 lease 24 h", False),
    (48203, "04:12.801775", "+0.068365", "·", "mDNS", 2, "192.168.1.31 → 224.0.0.251", 87,
     "Query PTR _airplay._tcp.local", False),
    (48204, "04:12.850112", "+0.048337", "↓", "ICMP", 5, "192.168.1.1 → 192.168.1.20", 98,
     "Destination unreachable (port)", True),
    (48205, "04:12.900412", "+0.050300", "·", "RRCP", 7, "02:00:5e:10:00:01 → Broadcast", 60,
     "Ethertype 0x8899", False),
]
ROW_H = 56
LIST_X, LIST_W = 20, 508


def packet_row(c, y, p, selected=False):
    seq, t, d, direction, word, k, addr, length, info, problem = p
    f = F_DENSE()
    if selected:
        c.border(LIST_X, y, LIST_W, ROW_H - 1, 6, T["focus"], 2, T["surface"])
    else:
        c.rect(LIST_X + 14, y + ROW_H - 1, LIST_W - 14, 1, T["surface_raised"])
    c.rect(LIST_X + (3 if selected else 0), y + 6, 4, ROW_H - 13, class_rgb(k))
    c.text(f, 34, y + 8, t, T["text_secondary"])
    c.text(f, 34, y + 30, d, T["text_muted"])
    c.text(f, 138, y + 8, direction, T["text_secondary"])
    c.text(f, 154, y + 8, word, class_rgb(k))
    c.text(f, 210, y + 8, addr, T["text_primary"])
    c.text_r(f, 520, y + 8, str(length), T["text_secondary"])
    if problem:
        c.text(f, 154, y + 30, "! " + info, T["status_warn"])
    else:
        c.text(f, 154, y + 30, info, T["text_secondary"])


def packets():
    c = Canvas(W, H)
    header(c, "CAPTURING · ETH0")
    tabs(c, 1)

    # filter bar
    y = 164
    face(c, 20, y, 118, 56, "FILTER")
    c.text(F_CAPTION(), 150, y + 9, "ALL PROTOCOLS", T["text_primary"])
    c.text(F_CAPTION(), 150, y + 29, "48 417 PACKETS", T["text_secondary"])
    face(c, 334, y, 104, 56, "TIME ↓")
    face(c, 446, y, 102, 56, "FOLLOW")
    c.mark("list.filter", 20, y, 118, 56)
    c.mark("list.sort", 334, y, 104, 56)
    c.mark("list.follow", 446, y, 102, 56)

    # column header
    hy = 232
    f = F_DENSE()
    c.text(f, 34, hy, "TIME / GAP", T["text_muted"])
    c.text(f, 154, hy, "PROTO", T["text_muted"])
    c.text(f, 210, hy, "SOURCE → DESTINATION", T["text_muted"])
    c.text_r(f, 520, hy, "LEN", T["text_muted"])
    c.rect(20, hy + 22, 508, 1, T["line"])

    ly = 256
    for i, p in enumerate(PACKETS):
        packet_row(c, ly + i * ROW_H, p, selected=(p[0] == 48196))
    c.mark("list.rows", LIST_X, ly, LIST_W, ROW_H * len(PACKETS))
    c.mark("list.row", LIST_X, ly, LIST_W, ROW_H)

    # scrubber rail with signal marks
    rail_h = ROW_H * len(PACKETS)
    c.rect(540, ly, 4, rail_h, T["surface_raised"])
    for frac in (0.08, 0.21, 0.22, 0.47, 0.63, 0.81, 0.955, 0.962, 0.985):
        c.rect(533, ly + int(rail_h * frac), 5, 2, T["status_warn"])
    c.rrect(538, ly + int(rail_h * 0.95), 8, 36, 3, T["text_secondary"])
    c.mark("list.rail", 528, ly, 20, rail_h)

    fy = ly + rail_h + 10
    c.text(F_CAPTION(), 20, fy, "VIEW HELD AT #48 205", T["text_secondary"])
    c.text_r(F_CAPTION(), 548, fy, "BUFFER FROM #1", T["text_muted"])

    capture_bar(c, [("LIVE ↓ 212", "primary"), ("HOLD", "secondary"), ("STOP", "secondary")])
    return c


# ---- C: Packet Inspector ------------------------------------------------------------

HEX = [
    ("0000", "02 00 5e 10 20 14 02 00", "..^. ..."),
    ("0008", "5e 10 00 01 08 00 45 00", "^.....E."),
    ("0010", "05 dc 3a 1c 40 00 39 06", "..:.@.9."),
    ("0018", "91 e4 cb 00 71 2a c0 a8", "....q*.."),
    ("0020", "01 14 01 bb c8 22 5b 1e", '....."[.'),
]
TCP_FIELDS = [
    ("SOURCE PORT", "443 https"), ("DESTINATION PORT", "51234"), ("SEQUENCE", "1449"),
    ("ACKNOWLEDGMENT", "518"), ("HEADER LENGTH", "32 bytes"), ("FLAGS", "ACK"),
    ("WINDOW", "501"), ("OPTIONS", "NOP NOP TS"), ("PAYLOAD", "1448 B · not stored"),
]
LAYER_H, FIELD_H, HEX_H = 56, 36, 22


def layer_head(c, y, name, summary, k=None, open_=False):
    col = T["text_secondary"] if k is None else class_rgb(k)
    c.rect(FX, y + 10, 4, LAYER_H - 20, col)
    c.tri(FX + 16, y + 22, 11, T["accent_primary"], "down" if open_ else "right")
    c.text(F_ROW(), FX + 38, y + (LAYER_H - F_ROW().line_h) // 2, name, T["text_primary"])
    c.text_r(F_CAPTION(), FX + FW, y + (LAYER_H - F_CAPTION().line_h) // 2, summary,
             T["text_secondary"])
    c.rect(FX, y + LAYER_H - 1, FW, 1, T["surface_raised"])


def inspector():
    c = Canvas(W, H)
    header(c, "CAPTURING · ETH0")
    tabs(c, 2)

    # the packet: number, class, finding; previous / next
    y = 164
    x = c.text(F_VALUE(), FX, y + 2, "#48196", T["text_primary"])
    x = c.text(F_CAPTION(), x + 14, y + 7, "TCP", class_rgb(0))
    c.text(F_CAPTION(), x + 12, y + 7, "! RETRANSMISSION", T["status_warn"])
    c.text(F_CAPTION(), FX, y + 36, "04:12.318430 · +187.554 MS · IN", T["text_secondary"])
    c.text(F_CAPTION(), FX, y + 58, "1514 B ON WIRE · 66 B STORED", T["text_secondary"])
    for i, d in enumerate(("left", "right")):
        bx = 404 + i * 76
        c.border(bx, y, 68, 56, 6, T["line"], 1, T["surface"])
        c.chevron(bx + 34, y + 28, 8, T["accent_primary"], d)
        c.mark("inspect.nav.%s" % ("prev" if i == 0 else "next"), bx, y, 68, 56)

    # endpoints: each a slab that filters on its host
    ey = 258
    for i, (cap, addr, port) in enumerate((("SOURCE", "203.0.113.42", "PORT 443 · HTTPS"),
                                           ("DESTINATION", "192.168.1.20", "PORT 51234"))):
        ex = FX + i * 284
        c.rrect(ex, ey, 244, 84, 6, T["surface"])
        c.text(F_CAPTION(), ex + 12, ey + 8, cap, T["text_secondary"])
        c.text(F_VALUE(), ex + 12, ey + 28, addr, T["text_primary"])
        c.text(F_CAPTION(), ex + 12, ey + 58, port, T["text_secondary"])
        c.mark("inspect.endpoint.%d" % i, ex, ey, 244, 84)
    c.text_c(F_VALUE(), FX + 264, ey + 28, "→", T["text_secondary"])

    # layers
    ly = 356
    layer_head(c, ly, "Frame", "1514 B · ETHERNET II")
    layer_head(c, ly + 56, "Ethernet", "02:00:5E:10:00:01 → 02:00:5E:10:20:14")
    layer_head(c, ly + 112, "IPv4", "TTL 57 · DF · ID 0x3A1C")
    ty = ly + 168
    layer_head(c, ty, "TCP", "443 → 51234 · ACK", 0, open_=True)
    ny = ty + LAYER_H
    c.text(F_CAPTION(), FX + 38, ny + 9, "! RETRANSMISSION OF #48195 · 187.6 MS EARLIER",
           T["status_warn"])
    fy = ny + FIELD_H
    for i, (name, value) in enumerate(TCP_FIELDS):
        ry = fy + i * FIELD_H
        if i == 1:
            c.border(FX + 26, ry + 1, FW - 26, FIELD_H - 2, 6, T["focus"], 2, T["surface"])
        c.text(F_CAPTION(), FX + 38, ry + 9, name, T["text_secondary"])
        c.text_r(F_VALUE(), FX + FW - 12, ry + 5, value, T["text_primary"])
        if i not in (0, 1):
            c.rect(FX + 38, ry + FIELD_H - 1, FW - 38, 1, T["surface_raised"])
    c.mark("inspect.field", FX + 26, fy, FW - 26, FIELD_H)
    by = fy + len(TCP_FIELDS) * FIELD_H
    layer_head(c, by, "Bytes", "66 STORED · HEX", open_=True)
    hy = by + LAYER_H + 4
    f = F_DENSE()
    for i, (off, hx, asc) in enumerate(HEX):
        ry = hy + i * HEX_H
        c.text(f, FX + 38, ry, off, T["text_muted"])
        bx = FX + 86
        for j, b in enumerate(hx.split()):
            px = bx + j * 24 + (8 if j >= 4 else 0)
            hot = (i == 4 and j in (4, 5))      # the selected field's bytes
            if hot:
                c.rect(px - 3, ry, 22, 18, T["surface_raised"])
            c.text(f, px, ry, b, T["accent_primary"] if hot else T["text_primary"])
        c.text(f, FX + 308, ry, asc, T["text_secondary"])
    c.mark("inspect.layers", FX, ly, FW, FOOT - 86 - ly)
    c.mark("inspect.hex.row", FX + 38, hy, FW - 38, HEX_H)

    capture_bar(c, [("FILTER BY", "primary"), ("HOLD", "secondary"), ("STOP", "secondary")])
    return c


# ---- landscape: list and inspector side by side -----------------------------------------

def packets_landscape():
    LW, LH = 1232, 568
    c = Canvas(LW, LH)
    # the app's own top row (POCKETOS_HEADER_NONE_LANDSCAPE, DS 37.2, 51.3)
    c.rrect(50, 8, 72, 56, 6, T["surface"])
    c.chevron(86, 36, 9, T["accent_primary"], "left")
    f = F_BUTTON()
    for i, (n, x, w) in enumerate((("LIVE", 138, 120), ("PACKETS", 266, 150))):
        c.border(x, 8, w, 56, 6, T["line"], 1, T["surface"])
        tw = f.width(n)
        c.text(f, x + (w - tw) // 2, 8 + (56 - f.line_h) // 2, n,
               T["accent_primary"] if i == 1 else T["text_primary"])
        if i == 1:
            c.rect(x + (w - tw) // 2, 56, tw, 2, T["accent_primary"])
    w = chip(c, 440, 18, "CAPTURING", "active")
    c.text(F_CAPTION(), 440 + w + 12, 27, "ETH0 · 00:04:13 · 48 417 PKT · 0 DROPPED",
           T["text_secondary"])
    face(c, 944, 8, 116, 56, "HOLD")
    face(c, 1068, 8, 114, 56, "STOP")
    c.rect(0, 71, LW, 1, T["line"])
    c.mark("land.toprow", 0, 0, LW, 72)

    # list pane: one line per packet, 40 px
    lx, lw = 30, 716
    fd = F_DENSE()
    hy = 84
    c.text(fd, lx + 14, hy, "TIME", T["text_muted"])
    c.text(fd, lx + 134, hy, "PROTO", T["text_muted"])
    c.text(fd, lx + 190, hy, "SOURCE → DESTINATION", T["text_muted"])
    c.text_r(fd, lx + 470, hy, "LEN", T["text_muted"])
    c.text(fd, lx + 486, hy, "INFO", T["text_muted"])
    c.rect(lx, hy + 22, lw, 1, T["line"])
    rh = 40
    for i, p in enumerate(PACKETS[1:12]):
        seq, t, d, direction, word, k, addr, length, info, problem = p
        y = 108 + i * rh
        sel = seq == 48196
        if sel:
            c.border(lx, y, lw, rh - 1, 6, T["focus"], 2, T["surface"])
        else:
            c.rect(lx + 14, y + rh - 1, lw - 14, 1, T["surface_raised"])
        c.rect(lx + (3 if sel else 0), y + 6, 4, rh - 13, class_rgb(k))
        ty = y + 11
        c.text(fd, lx + 14, ty, t, T["text_secondary"])
        c.text(fd, lx + 118, ty, direction, T["text_secondary"])
        c.text(fd, lx + 134, ty, word, class_rgb(k))
        c.text(fd, lx + 190, ty, addr, T["text_primary"])
        c.text_r(fd, lx + 470, ty, str(length), T["text_secondary"])
        s = ("! " if problem else "") + info
        if len(s) > 27:
            s = s[:26] + "…"
        c.text(fd, lx + 486, ty, s, T["status_warn"] if problem else T["text_secondary"])
    c.mark("land.list", lx, 108, lw, rh * 11)

    # inspector pane
    c.rect(758, 84, 1, 464, T["line"])
    ix, iw = 772, 430
    x = c.text(F_VALUE(), ix, 84, "#48196", T["text_primary"])
    x = c.text(F_CAPTION(), x + 12, 89, "TCP", class_rgb(0))
    c.text(F_CAPTION(), x + 10, 89, "! RETRANSMISSION", T["status_warn"])
    c.text(fd, ix, 116, "203.0.113.42:443 → 192.168.1.20:51234", T["text_primary"])
    c.text(F_CAPTION(), ix, 138, "1514 B ON WIRE · 66 STORED · IN", T["text_secondary"])
    c.rect(ix, 164, iw, 1, T["surface_raised"])
    c.rect(ix, 172, 4, 28, class_rgb(0))
    c.tri(ix + 14, 181, 11, T["accent_primary"], "down")
    c.text(F_ROW(), ix + 34, 172, "TCP", T["text_primary"])
    c.text_r(F_CAPTION(), ix + iw, 177, "IPV4 · ETHERNET · FRAME",
             T["text_secondary"])
    fy = 208
    for i, (name, value) in enumerate(TCP_FIELDS):
        ry = fy + i * 34
        if i == 1:
            c.border(ix, ry + 1, iw, 32, 6, T["focus"], 2, T["surface"])
        c.text(F_CAPTION(), ix + 12, ry + 8, name, T["text_secondary"])
        c.text_r(F_VALUE(), ix + iw - 12, ry + 4, value, T["text_primary"])
        if i not in (0, 1):
            c.rect(ix + 12, ry + 33, iw - 12, 1, T["surface_raised"])
    c.mark("land.inspector", ix, 84, iw, 464)
    return c


def main():
    shots = [
        ("01-live-capture.png", live("capturing")),
        ("02-packet-list.png", packets()),
        ("03-packet-inspector.png", inspector()),
        ("04-live-setup-idle.png", live_setup(True)),
        ("05-live-no-link.png", live_setup(False)),
        ("06-live-hold.png", live("hold")),
        ("07-live-helper-stopped.png", live_error()),
        ("08-packets-landscape.png", packets_landscape()),
    ]
    for name, c in shots:
        c.save(os.path.join(OUT, name))
        LAYOUTS[name] = {"size": [c.w, c.h], "boxes": c.layout}
        print(name, c.w, "x", c.h)
    with open(os.path.join(OUT, "layout.json"), "w", encoding="utf-8") as fh:
        json.dump(LAYOUTS, fh, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
