/*
 * Vision's layout. See vision_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_layout.h"

#include <string.h>

static struct vision_rect rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct vision_rect r = { x, y, w, h };

    return r;
}

/* The largest picture of shape fw:fh inside box_w x box_h, capped. */
static void fit(uint32_t fw, uint32_t fh, int32_t box_w, int32_t box_h, int32_t *pw, int32_t *ph)
{
    int64_t w = box_w;
    int64_t h = (w * fh) / fw;

    if (h > box_h) {
        h = box_h;
        w = (h * fw) / fh;
    }
    if (w > VISION_PICTURE_MAX) {
        w = VISION_PICTURE_MAX;
        h = (w * fh) / fw;
    }
    if (h > VISION_PICTURE_MAX) {
        h = VISION_PICTURE_MAX;
        w = (h * fw) / fh;
    }
    *pw = (int32_t)(w < 2 ? 2 : w);
    *ph = (int32_t)(h < 2 ? 2 : h);
}

int vision_layout_compute(struct vision_layout *l, int32_t w, int32_t h, int32_t inset_left,
                          int32_t inset_top, int32_t inset_right, int32_t inset_bottom,
                          uint32_t frame_w, uint32_t frame_h)
{
    int32_t x0 = inset_left;
    int32_t y0 = inset_top;
    int32_t iw = w - inset_left - inset_right;
    int32_t ih = h - inset_top - inset_bottom;
    int32_t pw;
    int32_t ph;

    memset(l, 0, sizeof(*l));
    if (frame_w == 0 || frame_h == 0 || iw < 200 || ih < 200) {
        return -1;
    }
    l->wide = iw > ih;
    if (!l->wide) {
        /* Tall: the picture across the width, the controls stacked under. */
        int32_t y;
        int32_t half;

        if (ih < VISION_CONTROLS_MIN + 100) {
            return -1;
        }
        fit(frame_w, frame_h, iw, ih - VISION_CONTROLS_MIN, &pw, &ph);
        l->picture = rect(x0 + (iw - pw) / 2, y0, pw, ph);
        y = y0 + ph + VISION_GAP;
        l->status = rect(x0, y, iw, VISION_STATUS_H);
        y += VISION_STATUS_H + VISION_GAP;
        half = (iw - VISION_GAP) / 2;
        l->count_a = rect(x0, y, half, VISION_COUNT_H);
        l->count_b = rect(x0 + half + VISION_GAP, y, half, VISION_COUNT_H);
        y += VISION_COUNT_H + VISION_GAP;
        l->line_btn = rect(x0, y, half, VISION_BTN_H);
        l->reset_btn = rect(x0 + half + VISION_GAP, y, half, VISION_BTN_H);
        return 0;
    }
    {
        /* Wide: the picture at the full height on the left, a column of
         * controls on the right. */
        int32_t side;
        int32_t sx;
        int32_t y;

        fit(frame_w, frame_h, iw - VISION_SIDE_MIN - VISION_GAP, ih, &pw, &ph);
        side = iw - pw - VISION_GAP;
        if (side < VISION_SIDE_MIN || ih < VISION_CONTROLS_MIN + VISION_BTN_H + VISION_GAP) {
            return -1;
        }
        l->picture = rect(x0, y0 + (ih - ph) / 2, pw, ph);
        sx = x0 + pw + VISION_GAP;
        y = y0;
        l->status = rect(sx, y, side, VISION_STATUS_H * 2);
        y += VISION_STATUS_H * 2 + VISION_GAP;
        l->count_a = rect(sx, y, side, VISION_COUNT_H);
        y += VISION_COUNT_H + VISION_GAP;
        l->count_b = rect(sx, y, side, VISION_COUNT_H);
        y += VISION_COUNT_H + VISION_GAP;
        l->line_btn = rect(sx, y, side, VISION_BTN_H);
        y += VISION_BTN_H + VISION_GAP;
        l->reset_btn = rect(sx, y, side, VISION_BTN_H);
        if (y + VISION_BTN_H > y0 + ih) {
            return -1;
        }
        return 0;
    }
}
