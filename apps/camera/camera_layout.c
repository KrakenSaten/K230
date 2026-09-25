/*
 * Camera's layout. See camera_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_layout.h"

#include <string.h>

static struct camera_rect rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct camera_rect r = { x, y, w, h };

    return r;
}

/* The largest box of the photo's shape inside max_w x max_h, capped at what a
 * slot of the shared memory holds. */
static void fit(uint32_t photo_w, uint32_t photo_h, int32_t max_w, int32_t max_h, int32_t *w,
                int32_t *h)
{
    int64_t fw = max_w;
    int64_t fh;

    if (fw > CAMERA_PICTURE_MAX) {
        fw = CAMERA_PICTURE_MAX;
    }
    fh = fw * photo_h / photo_w;
    if (fh > max_h || fh > CAMERA_PICTURE_MAX) {
        fh = max_h < CAMERA_PICTURE_MAX ? max_h : CAMERA_PICTURE_MAX;
        fw = fh * photo_w / photo_h;
    }
    *w = (int32_t)fw;
    *h = (int32_t)fh;
}

static int tall(struct camera_layout *l, int32_t x0, int32_t y0, int32_t bw, int32_t bh,
                uint32_t pw, uint32_t ph)
{
    int32_t w;
    int32_t h;
    int32_t cy;
    int32_t ch;
    int32_t row_y;
    int32_t half;

    fit(pw, ph, bw, bh - CAMERA_GAP - CAMERA_CONTROLS_MIN, &w, &h);
    if (w < 1 || h < 1) {
        return -1;
    }
    l->picture = rect(x0 + (bw - w) / 2, y0, w, h);
    cy = y0 + h + CAMERA_GAP;
    ch = y0 + bh - cy;
    l->status = rect(x0, cy, bw, CAMERA_STATUS_H);
    /* The shutter's row: centred in what is left under the status line. */
    row_y = cy + CAMERA_STATUS_H + (ch - CAMERA_STATUS_H - CAMERA_SHUTTER_H) / 2;
    l->shutter = rect(x0 + (bw - CAMERA_SHUTTER_W) / 2, row_y, CAMERA_SHUTTER_W, CAMERA_SHUTTER_H);
    l->last = rect(x0 + (l->shutter.x - x0 - CAMERA_THUMB) / 2,
                   row_y + (CAMERA_SHUTTER_H - CAMERA_THUMB) / 2, CAMERA_THUMB, CAMERA_THUMB);
    half = (bw - CAMERA_GAP) / 2;
    l->btn_a = rect(x0, row_y + (CAMERA_SHUTTER_H - CAMERA_BTN_H) / 2, half, CAMERA_BTN_H);
    l->btn_b = rect(x0 + bw - half, l->btn_a.y, half, CAMERA_BTN_H);
    return 0;
}

static int wide(struct camera_layout *l, int32_t x0, int32_t y0, int32_t bw, int32_t bh,
                uint32_t pw, uint32_t ph)
{
    int32_t w;
    int32_t h;
    int32_t cx;
    int32_t cw;
    int32_t mid;
    int32_t btn_w;

    fit(pw, ph, bw - CAMERA_GAP - CAMERA_SIDE_MIN, bh, &w, &h);
    if (w < 1 || h < 1) {
        return -1;
    }
    l->picture = rect(x0, y0 + (bh - h) / 2, w, h);
    cx = x0 + w + CAMERA_GAP;
    cw = x0 + bw - cx;
    l->status = rect(cx, y0, cw, CAMERA_STATUS_H);
    mid = y0 + bh / 2;
    l->shutter = rect(cx + (cw - CAMERA_SHUTTER_W) / 2, mid - CAMERA_SHUTTER_H / 2,
                      CAMERA_SHUTTER_W, CAMERA_SHUTTER_H);
    /* The last photo between the status line and the shutter. */
    l->last = rect(cx + (cw - CAMERA_THUMB) / 2,
                   y0 + CAMERA_STATUS_H +
                       (l->shutter.y - (y0 + CAMERA_STATUS_H) - CAMERA_THUMB) / 2,
                   CAMERA_THUMB, CAMERA_THUMB);
    if (l->last.y < y0 + CAMERA_STATUS_H) {
        /* Too short for it above: under the shutter instead. */
        l->last.y = l->shutter.y + CAMERA_SHUTTER_H + CAMERA_GAP;
    }
    btn_w = cw < CAMERA_SIDE_MIN + 60 ? cw : CAMERA_SIDE_MIN + 60;
    l->btn_a = rect(cx + (cw - btn_w) / 2, mid - CAMERA_GAP / 2 - CAMERA_BTN_H, btn_w,
                    CAMERA_BTN_H);
    l->btn_b = rect(l->btn_a.x, mid + CAMERA_GAP / 2, btn_w, CAMERA_BTN_H);
    return 0;
}

int camera_layout_compute(struct camera_layout *l, int32_t w, int32_t h, int32_t inset_left,
                          int32_t inset_top, int32_t inset_right, int32_t inset_bottom,
                          uint32_t photo_w, uint32_t photo_h)
{
    int32_t bw = w - inset_left - inset_right;
    int32_t bh = h - inset_top - inset_bottom;

    memset(l, 0, sizeof(*l));
    if (photo_w == 0 || photo_h == 0 || bw < CAMERA_SIDE_MIN || bh < CAMERA_CONTROLS_MIN) {
        return -1;
    }
    l->wide = w > h;
    if (l->wide) {
        if (bw < CAMERA_SIDE_MIN + CAMERA_GAP + 1 || bh < 2 * CAMERA_BTN_H + CAMERA_GAP) {
            return -1;
        }
        return wide(l, inset_left, inset_top, bw, bh, photo_w, photo_h);
    }
    if (bh < CAMERA_CONTROLS_MIN + CAMERA_GAP + 1) {
        return -1;
    }
    return tall(l, inset_left, inset_top, bw, bh, photo_w, photo_h);
}
