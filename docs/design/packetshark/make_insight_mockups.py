#!/usr/bin/env python3
"""Render the INSIGHT / WHEN mockups of PacketShark N1.1 (for design review;
docs/apps/packetshark/neural/N1.1.md §6). Nothing here is implemented.

    python make_insight_mockups.py      # writes insight/*.png beside this file

Same toolkit, fonts and tokens as make_mockups.py (pskdraw.py: the device's
own LVGL bitmap fonts, DS theme tokens, identity hues), 568x1232 portrait and
1232x568 landscape. Every figure on these screens is taken from the ledgers
the engine produced for its replay fixtures (tests/data/packetshark/intel/):
slow-dns, loss-healthy, hold-recovery, one-sided and loss-unknown. Times of
day are illustrative. No real capture is shown.

Rules the screens follow (NEURAL.md §5, DS §59):
  * a protocol fact is drawn in its protocol's hue; a worsening carries the
    warning glyph and word; CAN'T TELL is a word, never a colour;
  * certainty is text (counts, "60 of 60"), never a percentage;
  * every verdict says how much of each minute was seen, and an abstention
    says why;
  * a packet that has left the buffer keeps its number and says so.
"""
import os

from pskdraw import Canvas, T, class_rgb, F_CAPTION, F_DENSE, F_VALUE, F_BUTTON, F_BODY, F_ROW
from make_mockups import header, face, tabs, capture_bar, chip, panel, live, W, H, FX, FW

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "insight")
DNS, DHCP, ARP = 2, 3, 4   # CLASSES indices
STRIP_Y = 1070             # N1.0's MARK strip, above the capture bar


NBSP = "\u00a0"  # keeps a name's words on one line; drawn as a space


def wrap(f, s, width):
    words, lines, cur = s.split(" "), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if f.width(t.replace(NBSP, " ")) <= width or not cur:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def para(c, f, x, y, s, width, col, gap=0):
    for ln in wrap(f, s, width):
        c.text(f, x, y, ln.replace(NBSP, " "), col)
        y += f.line_h + gap
    return y


def mark_strip(c, marks=1):
    """N1.0's MARK strip above the capture bar (unchanged by N1.1)."""
    face(c, FX, STRIP_Y, 340, 56, "MARK THIS MOMENT")
    face(c, FX + 352, STRIP_Y, FW - 352, 56, "%d MARK%s" % (marks, "" if marks == 1 else "S"))
    c.chevron(FX + FW - 24, STRIP_Y + 28, 5, T["accent_primary"], "right", 2)


def face_head(c, y, title="Mark 1 of 1"):
    c.text(F_ROW(), FX, y + 15, title, T["text_primary"])
    face(c, 300, y, 56, 56, "", "disabled")
    c.chevron(328, y + 28, 7, T["disabled_fg"], "left")
    face(c, 364, y, 56, 56, "", "disabled")
    c.chevron(392, y + 28, 7, T["disabled_fg"], "right")
    face(c, 436, y, 112, 56, "BACK")


def lenses(c, y):
    """WHEN is the lens N1.1 proposes; WHO and WHY are later phases (N3, N4)."""
    x = FX
    x += chip(c, x, y, "WHEN", "active", dot=False) + 10
    x += chip(c, x, y, "WHO · LATER", "off", dot=False) + 10
    chip(c, x, y, "WHY · LATER", "off", dot=False)


SEC_PX = 488 / 240.0  # the SEEING strip: 2 min reference, 1 min before, 1 min after


def seeing(c, y, segs, notes, vantage="SEEING OWN + BROADCAST"):
    """What each minute could show. segs: (from_s, to_s, kind) on the -180..+60 s
    axis, kind in held / lost / unknown; notes: the three windows' words."""
    panel(c, FX, y, FW, 132, "SEEING")
    if vantage:
        c.text_r(F_CAPTION(), FX + FW - 20, y + 14, vantage, T["text_secondary"])
    sx, sy, sh = FX + 20, y + 44, 22
    c.rect(sx, sy, 488, sh, T["surface_raised"])
    for a, b, kind in segs:
        x0, x1 = sx + int((a + 180) * SEC_PX), sx + int((b + 180) * SEC_PX)
        if kind == "held":
            c.rect(x0, sy, x1 - x0, sh, T["bg"])
            c.dashed(x0, sy, x1 - x0, sh, T["text_muted"], 4, 3)
        elif kind == "lost":
            for xx in range(x0, x1, 5):
                c.rect(xx, sy + 4, 2, sh - 8, T["status_warn"])
        elif kind == "unread":  # the counters could not be read: a bracket above the strip
            c.rect(x0, sy - 7, x1 - x0, 1, T["text_muted"])
            c.rect(x0, sy - 7, 1, 5, T["text_muted"])
            c.rect(x1 - 1, sy - 7, 1, 5, T["text_muted"])
            c.text(F_DENSE(), x0 + 4, sy - 25, "COUNTERS NOT READ", T["text_muted"])
        else:  # unknown: counters not read
            c.rect(x0, sy, x1 - x0, sh, T["surface"])
            for xx in range(x0 + 2, x1 - 4, 12):
                c.text(F_DENSE(), xx, sy + 2, "?", T["text_muted"])
    for at in (-60, 0):
        x = sx + int((at + 180) * SEC_PX)
        c.rect(x, sy - 6, 2 if at == 0 else 1, sh + 12, T["accent_primary"] if at == 0 else T["line"])
    c.text(F_DENSE(), sx, sy + sh + 6, "-3 MIN", T["text_muted"])
    c.text_c(F_DENSE(), sx + int(120 * SEC_PX), sy + sh + 6, "-1 MIN", T["text_muted"])
    c.text_c(F_DENSE(), sx + int(180 * SEC_PX), sy + sh + 6, "MARK", T["accent_primary"])
    c.text_r(F_DENSE(), sx + 488, sy + sh + 6, "+1 MIN", T["text_muted"])
    cols = ((sx, 230), (sx + int(120 * SEC_PX) + 4, 116), (sx + int(180 * SEC_PX) + 6, 116))
    for (x, w), (word, warn) in zip(cols, notes):
        c.text(F_DENSE(), x, y + 100, word, T["status_warn"] if warn else T["text_secondary"])
    c.mark("insight.seeing", FX, y, FW, 132)
    return y + 132


def section(c, y, s):
    c.text(F_CAPTION(), FX, y, s, T["accent_primary"])
    return y + 28


def card(c, y, metric, hue, verdict, kind, figure, sub, lines=(), chev=True):
    """One comparison. kind: worse / better / none / cant / small."""
    lh = F_CAPTION().line_h
    body = []
    for s, warn in lines:
        for ln in wrap(F_CAPTION(), s, FW - 64):
            body.append((ln.replace(NBSP, " "), warn))
    h = 16 + lh + 8 + F_VALUE().line_h + 4 + lh + (8 + len(body) * lh if body else 0) + 14
    c.rrect(FX, y, FW, h, 6, T["surface"])
    c.rect(FX, y + 10, 4, h - 20, hue)
    c.text(F_CAPTION(), FX + 18, y + 16, metric, hue)
    col = {"worse": T["status_warn"], "better": T["text_primary"], "none": T["text_secondary"],
           "cant": T["text_primary"], "small": T["text_primary"]}[kind]
    c.text_r(F_BUTTON(), FX + FW - (44 if chev else 18), y + 14, ("! " if kind == "worse" else "") + verdict, col)
    yy = y + 16 + lh + 8
    c.text(F_VALUE(), FX + 18, yy, figure, T["text_primary"])
    yy += F_VALUE().line_h + 4
    c.text(F_CAPTION(), FX + 18, yy, sub, T["text_secondary"])
    yy += lh
    if body:
        yy += 8
        for ln, warn in body:
            c.text(F_CAPTION(), FX + 18, yy, ln, T["status_warn"] if warn else T["text_muted"])
            yy += lh
    if chev:
        c.chevron(FX + FW - 22, y + h // 2, 6, T["accent_primary"], "right", 2)
    c.mark("insight.card.%d" % y, FX, y, FW, h)
    return y + h + 12


def unchanged(c, y, names, why=None):
    """The comparisons that found no change, in one line, not a card each."""
    c.rect(FX, y, FW, 1, T["surface_raised"])
    x = c.text(F_CAPTION(), FX, y + 12, "NO CHANGE  ", T["text_secondary"])
    yy = para(c, F_CAPTION(), x, y + 12, names, FW - (x - FX), T["text_primary"])
    if why:
        yy = para(c, F_CAPTION(), x, yy, why, FW - (x - FX), T["text_muted"])
    return yy + 14


def frame(hint):
    c = Canvas(W, H)
    header(c, hint)
    tabs(c, 0)
    return c


def bar(c):
    mark_strip(c)
    capture_bar(c, [("FILTER", "secondary"), ("HOLD", "secondary"), ("STOP", "secondary")])


# ---- 1: the way in from LIVE -------------------------------------------------------

def live_entry():
    c = live("capturing")
    c.rect(0, 980, W, 1232 - 980, T["bg"])   # LIVE's last panel and its bar, redrawn below
    y = 992
    panel(c, FX, y, FW, 64, None)
    c.text(F_CAPTION(), FX + 18, y + 12, "INSIGHT", T["accent_primary"])
    c.text(F_CAPTION(), FX + 104, y + 12, "MARK 1 · 1 CHANGE AFTER IT", T["text_primary"])
    c.text(F_CAPTION(), FX + 18, y + 34, "SEEING OWN + BROADCAST · EVERY FRAME SEEN", T["text_secondary"])
    c.chevron(FX + FW - 22, y + 32, 6, T["accent_primary"], "right", 2)
    c.mark("insight.row", FX, y, FW, 64)
    bar(c)
    return c


# ---- 2: WHEN, a change on good evidence (slow-dns) ------------------------------------

def when_slow():
    c = frame("CAPTURING · ETH0")
    face_head(c, 168)
    lenses(c, 236)
    c.text(F_CAPTION(), FX, 286, "03:20 INTO THE CAPTURE · 14:02:12", T["text_secondary"])
    y = seeing(c, 320, [], (("EVERY FRAME SEEN", False), ("ALL SEEN", False), ("ALL SEEN", False)))
    y = section(c, y + 22, "AFTER THE MARK · AGAINST THE MINUTE BEFORE")
    y = card(c, y, "DNS · ANSWER TIME", class_rgb(DNS), "SLOWER", "worse", "30 MS  →  420 MS",
             "MEDIAN · 60 | 60 ANSWERS TO THIS DEVICE",
             [("EVERY FRAME SEEN IN BOTH MINUTES", False)])
    y = unchanged(c, y, "DNS\u00a0ANSWERED (60\u00a0OF\u00a060 | 60\u00a0OF\u00a060) · DNS\u00a0FAILURES · "
                  "DHCP\u00a0DISCOVER · ARP\u00a0REQUESTS")
    y = section(c, y + 6, "BEFORE THE MARK · AGAINST THE 2 MINUTES BEFORE")
    unchanged(c, y, "ALL FIVE: DNS\u00a0ANSWER\u00a0TIME, ANSWERED, FAILURES · DHCP\u00a0DISCOVER · "
                  "ARP\u00a0REQUESTS")
    bar(c)
    return c


# ---- 3: WHEN, the loss could account for it (loss-healthy) -----------------------------

def when_loss():
    c = frame("CAPTURING · ETH0")
    face_head(c, 168)
    lenses(c, 236)
    c.text(F_CAPTION(), FX, 286, "03:20 INTO THE CAPTURE · 14:02:12", T["text_secondary"])
    y = seeing(c, 320, [(0, 60, "lost"), (-10, 58, "unread")],
               (("EVERY FRAME SEEN", False), ("! 0-56 LOST", True), ("! 0-56 LOST", True)), vantage="")
    y = section(c, y + 22, "AFTER THE MARK · AGAINST THE MINUTE BEFORE")
    y = card(c, y, "DNS · QUESTIONS ANSWERED", class_rgb(DNS), "CAN'T TELL", "cant", "40 OF 52  |  WAS 60 OF 60",
             "THIS DEVICE'S QUESTIONS · ANSWERED WITHIN 5 S",
             [("THE FRAMES LOST COULD ACCOUNT FOR IT. THE COUNTERS COULD NOT BE READ FROM 03:10 TO 04:18; "
               "THE READING AFTER TOLD 56 LOST, NOT WHEN.", False)])
    y = unchanged(c, y, "DNS\u00a0ANSWER\u00a0TIME (30\u00a0|\u00a030\u00a0MS, 60\u00a0|\u00a040\u00a0ANSWERS) · "
                  "FAILURES · DHCP\u00a0DISCOVER · ARP\u00a0REQUESTS",
                  "A LOST FRAME DOES NOT CHANGE THE TIME OF AN ANSWER THAT WAS SEEN.")
    y = section(c, y + 6, "BEFORE THE MARK · AGAINST THE 2 MINUTES BEFORE")
    unchanged(c, y, "ALL FIVE")
    bar(c)
    return c


# ---- 4: WHEN across HOLD (hold-recovery) -------------------------------------------------

def when_hold():
    c = frame("CAPTURING · ETH0")
    face_head(c, 168)
    lenses(c, 236)
    c.text(F_CAPTION(), FX, 286, "03:20 INTO THE CAPTURE · 14:02:12", T["text_secondary"])
    y = seeing(c, 320, [(5, 26, "held")], (("EVERY FRAME SEEN", False), ("ALL SEEN", False), ("ON HOLD 21 S", False)))
    y = section(c, y + 22, "BEFORE THE MARK · AGAINST THE 2 MINUTES BEFORE")
    y = card(c, y, "DNS · QUESTIONS ANSWERED", class_rgb(DNS), "FEWER", "worse", "30 OF 60  |  WAS 120 OF 120",
             "THIS DEVICE'S QUESTIONS · ANSWERED WITHIN 5 S", [("EVERY FRAME SEEN", False)])
    y = section(c, y + 4, "AFTER THE MARK · AGAINST THE MINUTE BEFORE")
    y = card(c, y, "DNS · QUESTIONS ANSWERED", class_rgb(DNS), "MORE", "better", "35 OF 35  |  WAS 30 OF 60",
             "ANSWERED AGAIN",
             [("WHEN IT CHANGED IS NOT KNOWN: 21 S ON HOLD. 5 QUESTIONS LEFT OUT: THEIR WAIT MET HOLD.", False)])
    y = card(c, y, "DHCP · DISCOVER", class_rgb(DHCP), "CAN'T TELL", "cant", "0  |  WAS 0",
             "IN 39 WATCHED SECONDS",
             [("TOO LITTLE WATCHED TO RULE OUT A RISE OF 6 A MINUTE.", False)], chev=False)
    unchanged(c, y, "DNS ANSWER TIME · DNS FAILURES · ARP REQUESTS")
    bar(c)
    return c


# ---- 5: WHEN with one-sided visibility (one-sided) -------------------------------------------

def when_oneway():
    c = frame("CAPTURING · ETH0")
    face_head(c, 168)
    lenses(c, 236)
    c.text(F_CAPTION(), FX, 286, "03:20 INTO THE CAPTURE · 14:02:12", T["text_secondary"])
    y = seeing(c, 320, [], (("EVERY FRAME SEEN", False), ("! ONE-SIDED", True), ("! ONE-SIDED", True)),
               vantage="SEEING OWN + BROADCAST")
    y = section(c, y + 22, "AFTER THE MARK · AGAINST THE MINUTE BEFORE")
    y = card(c, y, "DNS · THIS DEVICE'S QUESTIONS", class_rgb(DNS), "CAN'T TELL", "cant", "60 ASKED · 0 ANSWERS SEEN",
             "ANSWER TIME, ANSWERED AND FAILURES",
             [("ONE-SIDED: THIS DEVICE SENT 120 FRAMES IN EACH MINUTE AND NOTHING CAME BACK TO IT. A RESOLVER "
               "THAT STOPPED AND REPLIES TAKING ANOTHER PATH LOOK THE SAME FROM HERE.", False)])
    y = unchanged(c, y, "DHCP DISCOVER · ARP REQUESTS (12 | 6, 6 | 6)", "WHAT EVERY STATION RECEIVES IS STILL SEEN.")
    y = section(c, y + 6, "BEFORE THE MARK · AGAINST THE 2 MINUTES BEFORE")
    card(c, y, "DNS · THIS DEVICE'S QUESTIONS", class_rgb(DNS), "CAN'T TELL", "cant", "0 OF 60  |  WAS 120 OF 120",
         "ONE-SIDED FROM 02:20", [], chev=True)
    bar(c)
    return c


# ---- 6: a comparison opened, its packets expired (loss-unknown) --------------------------------

def evidence():
    c = frame("STOPPED · ETH0")
    c.text(F_ROW(), FX, 183, "DNS answer time", T["text_primary"])
    face(c, 436, 168, 112, 56, "BACK")
    c.text(F_CAPTION(), FX, 240, "AFTER THE MARK · AGAINST THE MINUTE BEFORE", T["text_secondary"])
    c.text(F_BUTTON(), FX, 270, "! SLOWER", T["status_warn"])
    c.text(F_VALUE(), FX, 300, "30 MS  →  420 MS", T["text_primary"])
    y = para(c, F_CAPTION(), FX, 334,
             "MEDIAN OF 60 ANSWERS BEFORE AND 60 AFTER, ALL TO THIS DEVICE'S OWN QUESTIONS. THE ANSWERS AFTER RANK "
             "SLOWER, AND BY MORE THAN 1.5 TIMES AND 20 MS.", FW, T["text_primary"])
    y += 18
    panel(c, FX, y, FW, 92, "SEEING")
    para(c, F_CAPTION(), FX + 18, y + 18,
         "THE COUNTERS COULD NOT BE READ FROM 04:10 UNTIL THE CAPTURE STOPPED: LOSSES NOT KNOWN. THE TIME OF AN "
         "ANSWER THAT WAS SEEN DOES NOT DEPEND ON THEM.", FW - 36, T["text_secondary"])
    y += 92 + 24
    panel(c, FX, y, FW, 260, "THE SLOWEST ANSWERS · TAP FOR THE PACKET")
    rows = (("PACKET 926", "! ITS PACKET HAS LEFT THE BUFFER", False),
            ("PACKET 861", "! ITS PACKET HAS LEFT THE BUFFER", False),
            ("PACKET 1 029", "ITS BYTES HAVE LEFT · THE SUMMARY REMAINS", True),
            ("PACKET 963", "! ITS PACKET HAS LEFT THE BUFFER", False))
    ry = y + 18
    for num, state, tap in rows:
        c.text(F_CAPTION(), FX + 18, ry + 8, "DNS", class_rgb(DNS))
        c.text(F_CAPTION(), FX + 62, ry + 8, num, T["text_primary"])
        c.text(F_DENSE(), FX + 62, ry + 30, state, T["text_secondary"] if tap else T["text_muted"])
        if tap:
            c.chevron(FX + FW - 22, ry + 26, 6, T["accent_primary"], "right", 2)
        c.rect(FX + 18, ry + 56, FW - 36, 1, T["surface_raised"])
        ry += 60
    y += 260 + 24
    panel(c, FX, y, FW, 112, "WHAT THIS CAN AND CANNOT TELL")
    para(c, F_CAPTION(), FX + 18, y + 18,
         "IT COMPARES THIS DEVICE'S OWN QUESTIONS; OTHER DEVICES' DNS DOES NOT PASS THIS PORT. IT SAYS THE ANSWERS "
         "ARE SLOWER, NOT WHY. THE NUMBERS LIVE WITH THE CAPTURE AND GO WITH IT.", FW - 36, T["text_secondary"])
    face(c, FX, STRIP_Y, FW, 56, "DNS PAIRS IN PACKETS")
    capture_bar(c, [("SAVE", "primary"), ("NEW CAPTURE", "secondary"), ("DISCARD", "secondary")])
    return c


# ---- 7: the wide shape (slow-dns) ------------------------------------------------------------------

def when_landscape():
    LW, LH = 1232, 568
    c = Canvas(LW, LH)
    c.rrect(50, 8, 72, 56, 6, T["surface"])
    c.chevron(86, 36, 9, T["accent_primary"], "left")
    f = F_BUTTON()
    for i, (n, x, w) in enumerate((("LIVE", 138, 120), ("PACKETS", 266, 150))):
        c.border(x, 8, w, 56, 6, T["line"], 1, T["surface"])
        tw = f.width(n)
        c.text(f, x + (w - tw) // 2, 8 + (56 - f.line_h) // 2, n, T["accent_primary"] if i == 0 else T["text_primary"])
        if i == 0:
            c.rect(x + (w - tw) // 2, 56, tw, 2, T["accent_primary"])
    w = chip(c, 432, 18, "CAPTURING", "active")
    c.text(F_CAPTION(), 432 + w + 12, 27, "ETH0 · 00:04:20", T["text_secondary"])
    face(c, 720, 8, 104, 56, "MARK")
    face(c, 832, 8, 104, 56, "1 MARK")
    face(c, 944, 8, 116, 56, "HOLD")
    face(c, 1068, 8, 114, 56, "STOP")
    c.rect(0, 71, LW, 1, T["line"])
    # left: the lens and its list
    lx, lw = 30, 560
    c.text(F_ROW(), lx, 84, "Mark 1 of 1", T["text_primary"])
    x = lx + 170
    x += chip(c, x, 82, "WHEN", "active", dot=False) + 8
    x += chip(c, x, 82, "WHO · LATER", "off", dot=False) + 8
    c.text(F_DENSE(), lx, 128, "03:20 INTO THE CAPTURE · EVERY FRAME SEEN · SEEING OWN + BROADCAST",
           T["text_secondary"])
    sy = 156
    c.text(F_CAPTION(), lx, sy, "AFTER THE MARK", T["accent_primary"])
    rows = (("DNS · ANSWER TIME", class_rgb(DNS), "! SLOWER", T["status_warn"], "30 → 420 MS · 60 | 60", True),
            ("DNS · ANSWERED", class_rgb(DNS), "NO CHANGE", T["text_secondary"], "60 OF 60 | 60 OF 60", False),
            ("DNS · FAILURES", class_rgb(DNS), "NO CHANGE", T["text_secondary"], "0 OF 60 | 0 OF 60", False),
            ("DHCP · DISCOVER", class_rgb(DHCP), "NO CHANGE", T["text_secondary"], "0 | 0 A MINUTE", False),
            ("ARP · REQUESTS", class_rgb(ARP), "NO CHANGE", T["text_secondary"], "6 | 6 A MINUTE", False))
    ry = sy + 26
    for name, hue, verdict, vcol, fig, sel in rows:
        if sel:
            c.border(lx, ry, lw, 50, 6, T["focus"], 2, T["surface"])
        else:
            c.rect(lx + 14, ry + 49, lw - 14, 1, T["surface_raised"])
        c.rect(lx + (3 if sel else 0), ry + 8, 4, 34, hue)
        c.text(F_CAPTION(), lx + 16, ry + 8, name, hue)
        c.text(F_DENSE(), lx + 16, ry + 28, fig, T["text_primary"])
        c.text_r(F_CAPTION(), lx + lw - 16, ry + 16, verdict, vcol)
        ry += 52
    c.text(F_CAPTION(), lx, ry + 10, "BEFORE THE MARK · NO CHANGE IN ALL FIVE", T["text_secondary"])
    c.mark("land.insight.list", lx, sy, lw, ry - sy)
    # right: the selected comparison
    c.rect(606, 84, 1, 464, T["line"])
    ix, iw = 622, 580
    c.text(F_CAPTION(), ix, 84, "DNS ANSWER TIME · AFTER THE MARK", class_rgb(DNS))
    c.text(F_BUTTON(), ix, 108, "! SLOWER", T["status_warn"])
    c.text(F_VALUE(), ix + 130, 104, "30 MS  →  420 MS", T["text_primary"])
    y = para(c, F_CAPTION(), ix, 142,
             "MEDIAN OF 60 ANSWERS BEFORE AND 60 AFTER, ALL TO THIS DEVICE'S OWN QUESTIONS; SLOWER BY RANK, AND BY "
             "MORE THAN 1.5 TIMES AND 20 MS. EVERY FRAME SEEN IN BOTH MINUTES.", iw, T["text_primary"])
    c.text(F_CAPTION(), ix, y + 12, "THE SLOWEST ANSWERS · TAP FOR THE PACKET", T["text_secondary"])
    ry = y + 38
    for num in ("PACKET 926", "PACKET 861", "PACKET 1 029", "PACKET 963"):
        c.text(F_CAPTION(), ix, ry + 6, "DNS", class_rgb(DNS))
        c.text(F_CAPTION(), ix + 44, ry + 6, num, T["text_primary"])
        c.text(F_DENSE(), ix + 200, ry + 7, "IN THE BUFFER", T["text_secondary"])
        c.chevron(ix + iw - 16, ry + 15, 5, T["accent_primary"], "right", 2)
        c.rect(ix, ry + 31, iw, 1, T["surface_raised"])
        ry += 34
    para(c, F_DENSE(), ix, ry + 10,
         "OTHER DEVICES' DNS DOES NOT PASS THIS PORT. IT SAYS SLOWER, NOT WHY.", iw, T["text_muted"])
    c.mark("land.insight.sheet", ix, 84, iw, 464)
    return c


def main():
    os.makedirs(OUT, exist_ok=True)
    shots = [
        ("i1-live-entry-portrait.png", live_entry()),
        ("i2-when-slower-portrait.png", when_slow()),
        ("i3-when-loss-portrait.png", when_loss()),
        ("i4-when-hold-portrait.png", when_hold()),
        ("i5-when-one-sided-portrait.png", when_oneway()),
        ("i6-evidence-expired-portrait.png", evidence()),
        ("i7-when-landscape.png", when_landscape()),
    ]
    for name, c in shots:
        c.save(os.path.join(OUT, name))
        print(name, c.w, "x", c.h)


if __name__ == "__main__":
    main()
