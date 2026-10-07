/*
 * Frame to picture geometry. See vision_geom.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_geom.h"

#include <errno.h>

static bool valid(const struct vision_view *v)
{
    return v && v->frame_w && v->frame_h && v->view_w && v->view_h &&
           v->frame_w <= VISION_MAX_COORD && v->frame_h <= VISION_MAX_COORD &&
           v->view_w <= VISION_MAX_COORD && v->view_h <= VISION_MAX_COORD &&
           (v->rotation == 0 || v->rotation == 90 || v->rotation == 180 || v->rotation == 270);
}

/* The part of the turned picture that is shown (c*) and where on the view
 * it is drawn (o*), exactly as pocketcam_to_rgb565() chooses them: cover
 * shows a centred part of the picture over the whole view, contain the
 * whole picture in a centred part of the view. */
struct fit {
    uint32_t cx;
    uint32_t cy;
    uint32_t cw;
    uint32_t ch;
    uint32_t ox;
    uint32_t oy;
    uint32_t ow;
    uint32_t oh;
};

static void fit_of(const struct vision_view *v, struct fit *f)
{
    bool quarter = v->rotation == 90 || v->rotation == 270;
    uint32_t tw = quarter ? v->frame_h : v->frame_w;
    uint32_t th = quarter ? v->frame_w : v->frame_h;
    bool wider = (uint64_t)tw * v->view_h > (uint64_t)v->view_w * th;

    f->cx = 0;
    f->cy = 0;
    f->cw = tw;
    f->ch = th;
    f->ox = 0;
    f->oy = 0;
    f->ow = v->view_w;
    f->oh = v->view_h;
    if (v->contain) {
        if (wider) {
            f->oh = (uint32_t)(((uint64_t)th * v->view_w) / tw);
            f->oh = f->oh ? f->oh : 1;
            f->oy = (v->view_h - f->oh) / 2;
        } else {
            f->ow = (uint32_t)(((uint64_t)tw * v->view_h) / th);
            f->ow = f->ow ? f->ow : 1;
            f->ox = (v->view_w - f->ow) / 2;
        }
    } else if (wider) {
        f->cw = (uint32_t)(((uint64_t)th * v->view_w) / v->view_h);
        f->cw = f->cw ? f->cw : 1;
        f->cx = (tw - f->cw) / 2;
    } else {
        f->ch = (uint32_t)(((uint64_t)tw * v->view_h) / v->view_w);
        f->ch = f->ch ? f->ch : 1;
        f->cy = (th - f->ch) / 2;
    }
}

/* The position of frame pixel (sx, sy) in a w x h frame turned clockwise by
 * rotation: the inverse of pocketcam_convert.c's source_of(). */
static void turn_point(int64_t w, int64_t h, int rotation, int64_t sx, int64_t sy, int64_t *tx, int64_t *ty)
{
    switch (rotation) {
    case 90:
        *tx = h - 1 - sy;
        *ty = sx;
        break;
    case 180:
        *tx = w - 1 - sx;
        *ty = h - 1 - sy;
        break;
    case 270:
        *tx = sy;
        *ty = w - 1 - sx;
        break;
    default:
        *tx = sx;
        *ty = sy;
        break;
    }
}

/* The turned position of frame pixel (sx, sy) on the preview. */
static void turned_of(const struct vision_view *v, int64_t sx, int64_t sy, int64_t *tx, int64_t *ty)
{
    turn_point(v->frame_w, v->frame_h, v->rotation, sx, sy, tx, ty);
}

/* The frame pixel a turned pixel (tx, ty) came from: turn_point undone. */
static void unturn_point(int64_t w, int64_t h, int rotation, int64_t tx, int64_t ty, int64_t *sx, int64_t *sy)
{
    switch (rotation) {
    case 90:
        *sx = ty;
        *sy = h - 1 - tx;
        break;
    case 180:
        *sx = w - 1 - tx;
        *sy = h - 1 - ty;
        break;
    case 270:
        *sx = w - 1 - ty;
        *sy = tx;
        break;
    default:
        *sx = tx;
        *sy = ty;
        break;
    }
}

static bool turn_ok(uint32_t w, uint32_t h, int rotation)
{
    return w > 0 && h > 0 && w <= VISION_MAX_COORD && h <= VISION_MAX_COORD &&
           (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270);
}

void vision_turned_size(uint32_t frame_w, uint32_t frame_h, int rotation, uint32_t *tw, uint32_t *th)
{
    bool quarter = rotation == 90 || rotation == 270;

    *tw = quarter ? frame_h : frame_w;
    *th = quarter ? frame_w : frame_h;
}

/* A box through a point mapping: its two corner pixels mapped, and the box
 * around them. */
static void box_through(uint32_t w, uint32_t h, int rotation, bool back, const struct vision_box *in,
                        struct vision_box *out)
{
    int64_t ax;
    int64_t ay;
    int64_t bx;
    int64_t by;
    int64_t x2 = (int64_t)in->x + in->w - 1;
    int64_t y2 = (int64_t)in->y + in->h - 1;

    if (back) {
        unturn_point(w, h, rotation, in->x, in->y, &ax, &ay);
        unturn_point(w, h, rotation, x2, y2, &bx, &by);
    } else {
        turn_point(w, h, rotation, in->x, in->y, &ax, &ay);
        turn_point(w, h, rotation, x2, y2, &bx, &by);
    }
    out->x = (int32_t)(ax < bx ? ax : bx);
    out->y = (int32_t)(ay < by ? ay : by);
    out->w = (int32_t)((ax < bx ? bx - ax : ax - bx) + 1);
    out->h = (int32_t)((ay < by ? by - ay : ay - by) + 1);
}

int vision_box_turn(uint32_t frame_w, uint32_t frame_h, int rotation, const struct vision_box *in,
                    struct vision_box *out)
{
    if (!in || !out || !turn_ok(frame_w, frame_h, rotation) || in->w <= 0 || in->h <= 0) {
        return -EINVAL;
    }
    box_through(frame_w, frame_h, rotation, false, in, out);
    return 0;
}

int vision_box_unturn(uint32_t frame_w, uint32_t frame_h, int rotation, const struct vision_box *in,
                      struct vision_box *out)
{
    if (!in || !out || !turn_ok(frame_w, frame_h, rotation) || in->w <= 0 || in->h <= 0) {
        return -EINVAL;
    }
    box_through(frame_w, frame_h, rotation, true, in, out);
    return 0;
}

int vision_turn_planes(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, uint32_t planes,
                       int rotation, uint8_t *dst)
{
    uint32_t tw;
    uint32_t th;
    uint32_t p;
    uint32_t x;
    uint32_t y;

    if (!src || !dst || !turn_ok(w, h, rotation) || stride < w || planes == 0) {
        return -EINVAL;
    }
    vision_turned_size(w, h, rotation, &tw, &th);
    for (p = 0; p < planes; p++) {
        const uint8_t *s = src + (size_t)p * stride * h;
        uint8_t *d = dst + (size_t)p * tw * th;

        for (y = 0; y < h; y++) {
            const uint8_t *row = s + (size_t)y * stride;

            switch (rotation) {
            case 90: /* (x, y) to (h - 1 - y, x) */
                for (x = 0; x < w; x++) {
                    d[(size_t)x * tw + (h - 1 - y)] = row[x];
                }
                break;
            case 180: /* to (w - 1 - x, h - 1 - y) */
                for (x = 0; x < w; x++) {
                    d[(size_t)(h - 1 - y) * tw + (w - 1 - x)] = row[x];
                }
                break;
            case 270: /* to (y, w - 1 - x) */
                for (x = 0; x < w; x++) {
                    d[(size_t)(w - 1 - x) * tw + y] = row[x];
                }
                break;
            default:
                for (x = 0; x < w; x++) {
                    d[(size_t)y * tw + x] = row[x];
                }
                break;
            }
        }
    }
    return 0;
}

int vision_map_point(const struct vision_view *v, int32_t sx, int32_t sy, int32_t *vx, int32_t *vy)
{
    struct fit f;
    int64_t tx;
    int64_t ty;
    int64_t x;
    int64_t y;

    if (!valid(v) || !vx || !vy) {
        return -EINVAL;
    }
    fit_of(v, &f);
    turned_of(v, sx, sy, &tx, &ty);
    /* The converter takes destination column ox + x from turned column
     * cx + x * cw / ow; so turned column t is shown at (t - cx) * ow / cw,
     * rounded to the nearest destination pixel, from ox. */
    x = ((tx - f.cx) * (int64_t)f.ow + (int64_t)f.cw / 2) / (int64_t)f.cw;
    y = ((ty - f.cy) * (int64_t)f.oh + (int64_t)f.ch / 2) / (int64_t)f.ch;
    if (v->mirror) {
        x = (int64_t)f.ow - 1 - x;
    }
    *vx = (int32_t)(x + f.ox);
    *vy = (int32_t)(y + f.oy);
    return 0;
}

int vision_unmap_point(const struct vision_view *v, int32_t vx, int32_t vy, int32_t *sx, int32_t *sy)
{
    struct fit f;
    uint64_t dx;
    uint64_t tx;
    uint64_t ty;

    if (!valid(v) || !sx || !sy) {
        return -EINVAL;
    }
    fit_of(v, &f);
    if (vx < (int32_t)f.ox || vy < (int32_t)f.oy || vx >= (int32_t)(f.ox + f.ow) ||
        vy >= (int32_t)(f.oy + f.oh)) {
        return -EINVAL;
    }
    vx -= (int32_t)f.ox;
    vy -= (int32_t)f.oy;
    /* pocketcam_to_rgb565(): col_t[x] = cx + x * cw / ow, mirrored first;
     * row_t[y] = cy + y * ch / oh; then source_of(). */
    dx = v->mirror ? (uint64_t)(f.ow - 1 - (uint32_t)vx) : (uint64_t)vx;
    tx = f.cx + (dx * f.cw) / f.ow;
    ty = f.cy + ((uint64_t)vy * f.ch) / f.oh;
    switch (v->rotation) {
    case 90:
        *sx = (int32_t)ty;
        *sy = (int32_t)(v->frame_h - 1 - tx);
        break;
    case 180:
        *sx = (int32_t)(v->frame_w - 1 - tx);
        *sy = (int32_t)(v->frame_h - 1 - ty);
        break;
    case 270:
        *sx = (int32_t)(v->frame_w - 1 - ty);
        *sy = (int32_t)tx;
        break;
    default:
        *sx = (int32_t)tx;
        *sy = (int32_t)ty;
        break;
    }
    return 0;
}

int vision_map_box(const struct vision_view *v, const struct vision_box *in, struct vision_box *out)
{
    int32_t xs[4];
    int32_t ys[4];
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    int i;
    int r;

    if (!in || !out || in->w <= 0 || in->h <= 0) {
        return -EINVAL;
    }
    r = vision_map_point(v, in->x, in->y, &xs[0], &ys[0]);
    if (r != 0) {
        return r;
    }
    vision_map_point(v, in->x + in->w - 1, in->y, &xs[1], &ys[1]);
    vision_map_point(v, in->x, in->y + in->h - 1, &xs[2], &ys[2]);
    vision_map_point(v, in->x + in->w - 1, in->y + in->h - 1, &xs[3], &ys[3]);
    x1 = x2 = xs[0];
    y1 = y2 = ys[0];
    for (i = 1; i < 4; i++) {
        if (xs[i] < x1) {
            x1 = xs[i];
        }
        if (xs[i] > x2) {
            x2 = xs[i];
        }
        if (ys[i] < y1) {
            y1 = ys[i];
        }
        if (ys[i] > y2) {
            y2 = ys[i];
        }
    }
    if (x2 < 0 || y2 < 0 || x1 >= (int32_t)v->view_w || y1 >= (int32_t)v->view_h) {
        return 0;
    }
    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 >= (int32_t)v->view_w) {
        x2 = (int32_t)v->view_w - 1;
    }
    if (y2 >= (int32_t)v->view_h) {
        y2 = (int32_t)v->view_h - 1;
    }
    out->x = x1;
    out->y = y1;
    out->w = x2 - x1 + 1;
    out->h = y2 - y1 + 1;
    return 1;
}
