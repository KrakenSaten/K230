/*
 * Frame to picture geometry. See vision_geom.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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

/* The turned picture's size and the part of it the cover fit shows, exactly
 * as pocketcam_to_rgb565() chooses them. */
static void crop(const struct vision_view *v, uint32_t *tw, uint32_t *th, uint32_t *cx,
                 uint32_t *cy, uint32_t *cw, uint32_t *ch)
{
    bool quarter = v->rotation == 90 || v->rotation == 270;

    *tw = quarter ? v->frame_h : v->frame_w;
    *th = quarter ? v->frame_w : v->frame_h;
    *cx = 0;
    *cy = 0;
    *cw = *tw;
    *ch = *th;
    if ((uint64_t)*tw * v->view_h > (uint64_t)v->view_w * *th) {
        *cw = (uint32_t)(((uint64_t)*th * v->view_w) / v->view_h);
        *cw = *cw ? *cw : 1;
        *cx = (*tw - *cw) / 2;
    } else {
        *ch = (uint32_t)(((uint64_t)*tw * v->view_h) / v->view_w);
        *ch = *ch ? *ch : 1;
        *cy = (*th - *ch) / 2;
    }
}

/* The turned position of frame pixel (sx, sy): the inverse of
 * pocketcam_convert.c's source_of(). */
static void turned_of(const struct vision_view *v, int64_t sx, int64_t sy, int64_t *tx, int64_t *ty)
{
    int64_t w = v->frame_w;
    int64_t h = v->frame_h;

    switch (v->rotation) {
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

int vision_map_point(const struct vision_view *v, int32_t sx, int32_t sy, int32_t *vx, int32_t *vy)
{
    uint32_t tw;
    uint32_t th;
    uint32_t cx;
    uint32_t cy;
    uint32_t cw;
    uint32_t ch;
    int64_t tx;
    int64_t ty;
    int64_t x;
    int64_t y;

    if (!valid(v) || !vx || !vy) {
        return -EINVAL;
    }
    crop(v, &tw, &th, &cx, &cy, &cw, &ch);
    turned_of(v, sx, sy, &tx, &ty);
    /* The converter takes destination column x from turned column
     * cx + x * cw / ow; so turned column t is shown at (t - cx) * ow / cw,
     * rounded to the nearest destination pixel. */
    x = ((tx - cx) * (int64_t)v->view_w + (int64_t)cw / 2) / (int64_t)cw;
    y = ((ty - cy) * (int64_t)v->view_h + (int64_t)ch / 2) / (int64_t)ch;
    if (v->mirror) {
        x = (int64_t)v->view_w - 1 - x;
    }
    *vx = (int32_t)x;
    *vy = (int32_t)y;
    return 0;
}

int vision_unmap_point(const struct vision_view *v, int32_t vx, int32_t vy, int32_t *sx, int32_t *sy)
{
    uint32_t tw;
    uint32_t th;
    uint32_t cx;
    uint32_t cy;
    uint32_t cw;
    uint32_t ch;
    uint64_t dx;
    uint64_t tx;
    uint64_t ty;

    if (!valid(v) || !sx || !sy || vx < 0 || vy < 0 || vx >= (int32_t)v->view_w ||
        vy >= (int32_t)v->view_h) {
        return -EINVAL;
    }
    crop(v, &tw, &th, &cx, &cy, &cw, &ch);
    /* pocketcam_to_rgb565(): col_t[x] = cx + x * cw / ow, mirrored first;
     * row_t[y] = cy + y * ch / oh; then source_of(). */
    dx = v->mirror ? (uint64_t)(v->view_w - 1 - (uint32_t)vx) : (uint64_t)vx;
    tx = cx + (dx * cw) / v->view_w;
    ty = cy + ((uint64_t)vy * ch) / v->view_h;
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
