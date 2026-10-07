# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""A small SVG subset rasterizer for the Doors icon tools. Standard library only.

It draws what the Doors and DOORS B icon sources are made of, and nothing
else: <path> (M L H V Q C A Z, absolute and relative), <rect> (with rx),
<circle>, <ellipse>, <g transform="translate(..) scale(..)">, solid fills,
two-point linear gradients in objectBoundingBox units, and strokes. Strokes
are drawn with round caps and joins (a union of capsules), which is what the
B glyphs ask for; the older Doors icons ask for square caps and are drawn
round on purpose, so both families share one line language inside the B
portal frame.

Coverage is sampled on an SS x SS grid per pixel; colour is taken at the
pixel centre. Layers are composited "over" in premultiplied floating point,
so the result is deterministic and exact to the rounding at the end.
"""
import math
import re

SS = 4                                   # subsamples per pixel, per axis

_TOKEN = re.compile(r"[MmLlHhVvQqCcAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")


def parse_color(text):
    text = text.strip()
    if text.startswith("#") and len(text) == 7:
        return tuple(int(text[i:i + 2], 16) for i in (1, 3, 5))
    if text.startswith("#") and len(text) == 4:
        return tuple(int(c * 2, 16) for c in text[1:])
    raise ValueError("colour %r not handled" % text)


def _flatten_quad(p0, p1, p2, steps=16):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        a, b, c = (1 - t) ** 2, 2 * (1 - t) * t, t * t
        out.append((a * p0[0] + b * p1[0] + c * p2[0], a * p0[1] + b * p1[1] + c * p2[1]))
    return out


def _flatten_cubic(p0, p1, p2, p3, steps=20):
    out = []
    for i in range(1, steps + 1):
        t = i / steps
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0],
                    a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


def _flatten_arc(p0, rx, ry, phi_deg, large, sweep, p1):
    """SVG endpoint arc to points (implementation notes F.6.5)."""
    if rx == 0 or ry == 0:
        return [p1]
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = (x1p ** 2) / (rx ** 2) + (y1p ** 2) / (ry ** 2)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        coef = -coef
    cxp, cyp = coef * rx * y1p / ry, -coef * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (p0[0] + p1[0]) / 2
    cy = sp * cxp + cp * cyp + (p0[1] + p1[1]) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    steps = max(8, int(abs(dt) / (math.pi / 32)))
    out = []
    for i in range(1, steps + 1):
        t = t1 + dt * i / steps
        x, y = rx * math.cos(t), ry * math.sin(t)
        out.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    out[-1] = p1
    return out


def parse_path(d):
    """Returns a list of (points, closed) subpaths in user units."""
    tokens = _TOKEN.findall(d)
    subpaths = []
    pts = []
    cur = (0.0, 0.0)
    start = cur
    cmd = None
    i = 0

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    def flush(closed):
        nonlocal pts
        if len(pts) > 1:
            subpaths.append((pts, closed))
        pts = []

    while i < len(tokens):
        if tokens[i].isalpha():
            cmd = tokens[i]
            i += 1
            if cmd in "Zz":
                flush(True)
                cur = start
                continue
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = cur if rel else (0.0, 0.0)
        if c == "M":
            flush(False)
            cur = (ox + num(), oy + num())
            start = cur
            pts = [cur]
            cmd = "l" if rel else "L"        # implicit lineto after moveto
        elif c == "L":
            cur = (ox + num(), oy + num())
            pts.append(cur)
        elif c == "H":
            cur = ((cur[0] if rel else 0.0) + num(), cur[1])
            pts.append(cur)
        elif c == "V":
            cur = (cur[0], (cur[1] if rel else 0.0) + num())
            pts.append(cur)
        elif c == "Q":
            p1 = (ox + num(), oy + num())
            p2 = (ox + num(), oy + num())
            pts.extend(_flatten_quad(cur, p1, p2))
            cur = p2
        elif c == "C":
            p1 = (ox + num(), oy + num())
            p2 = (ox + num(), oy + num())
            p3 = (ox + num(), oy + num())
            pts.extend(_flatten_cubic(cur, p1, p2, p3))
            cur = p3
        elif c == "A":
            rx, ry, phi, large, sweep = num(), num(), num(), num(), num()
            p1 = (ox + num(), oy + num())
            pts.extend(_flatten_arc(cur, rx, ry, phi, int(large), int(sweep), p1))
            cur = p1
        else:
            raise ValueError("path command %r not handled" % cmd)
        if not pts:
            pts = [cur]
    flush(False)
    return subpaths


def ellipse_path(cx, cy, rx, ry, steps=72):
    return [([(cx + rx * math.cos(2 * math.pi * k / steps), cy + ry * math.sin(2 * math.pi * k / steps))
              for k in range(steps)], True)]


def rect_path(x, y, w, h, r=0.0):
    if r <= 0:
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
    r = min(r, w / 2, h / 2)
    pts = []
    for cx, cy, a0 in ((x + w - r, y + r, -90), (x + w - r, y + h - r, 0), (x + r, y + h - r, 90), (x + r, y + r, 180)):
        for k in range(9):
            a = math.radians(a0 + 90 * k / 8)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return [(pts, True)]


def transform(subpaths, sx, tx, ty):
    return [([(x * sx + tx, y * sx + ty) for x, y in pts], closed) for pts, closed in subpaths]


class Canvas:
    """Premultiplied RGBA, floats 0..1, row-major."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def _composite(self, cover, color_at, opacity):
        for idx, c in cover.items():
            if c <= 0:
                continue
            x, y = idx % self.w, idx // self.w
            r, g, b = color_at(x + 0.5, y + 0.5)
            a = c * opacity
            p = self.px[idx]
            p[0] = r / 255 * a + p[0] * (1 - a)
            p[1] = g / 255 * a + p[1] * (1 - a)
            p[2] = b / 255 * a + p[2] * (1 - a)
            p[3] = a + p[3] * (1 - a)

    def fill(self, subpaths, color_at, opacity=1.0):
        """Even-odd fill of every subpath together (each is closed implicitly)."""
        edges = []
        xs, ys = [], []
        for pts, _ in subpaths:
            for k in range(len(pts)):
                a, b = pts[k], pts[(k + 1) % len(pts)]
                if a[1] != b[1]:
                    edges.append((a, b))
                xs.append(a[0])
                ys.append(a[1])
        if not edges:
            return
        x0, x1 = max(0, int(math.floor(min(xs)))), min(self.w - 1, int(math.ceil(max(xs))))
        y0, y1 = max(0, int(math.floor(min(ys)))), min(self.h - 1, int(math.ceil(max(ys))))
        cover = {}
        n = SS * SS
        for py in range(y0, y1 + 1):
            for sy in range(SS):
                yy = py + (sy + 0.5) / SS
                crossings = []
                for (ax, ay), (bx, by) in edges:
                    if (ay <= yy < by) or (by <= yy < ay):
                        crossings.append(ax + (yy - ay) * (bx - ax) / (by - ay))
                crossings.sort()
                for k in range(0, len(crossings) - 1, 2):
                    xa, xb = crossings[k], crossings[k + 1]
                    for px_ in range(max(x0, int(math.floor(xa))), min(x1, int(math.ceil(xb))) + 1):
                        cnt = 0
                        for sx in range(SS):
                            xx = px_ + (sx + 0.5) / SS
                            if xa <= xx < xb:
                                cnt += 1
                        if cnt:
                            idx = py * self.w + px_
                            cover[idx] = cover.get(idx, 0) + cnt / n
        self._composite(cover, color_at, opacity)

    def stroke(self, subpaths, width, color_at, opacity=1.0):
        """Round-capped, round-joined stroke: every sample within width/2 of a segment."""
        hw = width / 2
        W, H = self.w * SS, self.h * SS
        hit = bytearray(W * H)
        for pts, closed in subpaths:
            segs = list(zip(pts, pts[1:]))
            if closed:
                segs.append((pts[-1], pts[0]))
            if len(pts) == 1:
                segs = [(pts[0], pts[0])]
            for (ax, ay), (bx, by) in segs:
                dx, dy = bx - ax, by - ay
                ll = dx * dx + dy * dy
                sx0 = max(0, int(math.floor((min(ax, bx) - hw) * SS)))
                sx1 = min(W - 1, int(math.ceil((max(ax, bx) + hw) * SS)))
                sy0 = max(0, int(math.floor((min(ay, by) - hw) * SS)))
                sy1 = min(H - 1, int(math.ceil((max(ay, by) + hw) * SS)))
                hw2 = hw * hw
                for sy in range(sy0, sy1 + 1):
                    yy = (sy + 0.5) / SS
                    row = sy * W
                    for sx in range(sx0, sx1 + 1):
                        if hit[row + sx]:
                            continue
                        xx = (sx + 0.5) / SS
                        if ll:
                            t = ((xx - ax) * dx + (yy - ay) * dy) / ll
                            t = 0.0 if t < 0 else 1.0 if t > 1 else t
                            qx, qy = ax + t * dx - xx, ay + t * dy - yy
                        else:
                            qx, qy = ax - xx, ay - yy
                        if qx * qx + qy * qy <= hw2:
                            hit[row + sx] = 1
        cover = {}
        n = SS * SS
        for sy in range(H):
            row = sy * W
            py = sy // SS
            for sx in range(W):
                if hit[row + sx]:
                    idx = py * self.w + sx // SS
                    cover[idx] = cover.get(idx, 0) + 1 / n
        self._composite(cover, color_at, opacity)

    def rgba(self):
        """Straight (not premultiplied) 8-bit RGBA tuples, row-major."""
        out = []
        for r, g, b, a in self.px:
            if a <= 0:
                out.append((0, 0, 0, 0))
                continue
            out.append((min(255, int(r / a * 255 + 0.5)), min(255, int(g / a * 255 + 0.5)),
                        min(255, int(b / a * 255 + 0.5)), min(255, int(a * 255 + 0.5))))
        return out


def solid(rgb):
    return lambda x, y: rgb


def gradient(stops, x0, y0, x1, y1, bbox=None):
    """Linear gradient; with bbox=(bx, by, bw, bh) the vector is in objectBoundingBox units."""
    def at(x, y):
        if bbox:
            bx, by, bw, bh = bbox
            u, v = (x - bx) / bw, (y - by) / bh
        else:
            u, v = x, y
        vx, vy = x1 - x0, y1 - y0
        t = ((u - x0) * vx + (v - y0) * vy) / (vx * vx + vy * vy)
        t = 0.0 if t < 0 else 1.0 if t > 1 else t
        for k in range(len(stops) - 1):
            (o0, c0), (o1, c1) = stops[k], stops[k + 1]
            if t <= o1:
                f = 0.0 if o1 == o0 else (t - o0) / (o1 - o0)
                return tuple(c0[i] + (c1[i] - c0[i]) * f for i in range(3))
        return stops[-1][1]
    return at
