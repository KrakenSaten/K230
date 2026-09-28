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

/* `n` buttons in rows of `cols` across `w` from (x, y); returns the height
 * used, gaps between rows included, none after the last. */
static int32_t grid(struct vision_layout *l, int32_t x, int32_t y, int32_t w, int cols, int n)
{
    int i;
    int rows = (n + cols - 1) / cols;

    for (i = 0; i < n; i++) {
        int r = i / cols;
        int c = i % cols;
        int in_row = n - r * cols < cols ? n - r * cols : cols;
        /* A short last row spreads its buttons over the width. */
        int32_t rw = (w - (in_row - 1) * VISION_GAP) / in_row;

        l->btn[i] = rect(x + c * (rw + VISION_GAP), y + r * (VISION_BTN_H + VISION_GAP), rw, VISION_BTN_H);
    }
    return rows * VISION_BTN_H + (rows - 1) * VISION_GAP;
}

int vision_layout_compute(struct vision_layout *l, int32_t w, int32_t h, int32_t inset_left,
                          int32_t inset_top, int32_t inset_right, int32_t inset_bottom,
                          uint32_t frame_w, uint32_t frame_h, int buttons, int status_lines)
{
    int32_t x0 = inset_left;
    int32_t y0 = inset_top;
    int32_t iw = w - inset_left - inset_right;
    int32_t ih = h - inset_top - inset_bottom;
    int32_t pw;
    int32_t ph;
    int32_t status_h;
    int32_t controls;
    int rows;

    memset(l, 0, sizeof(*l));
    if (frame_w == 0 || frame_h == 0 || iw < 200 || ih < 200 || buttons < 1 ||
        buttons > VISION_LAYOUT_BUTTONS || status_lines < 1 || status_lines > VISION_LAYOUT_STATUS_LINES_MAX) {
        return -1;
    }
    l->buttons = buttons;
    l->wide = iw > ih;
    if (!l->wide) {
        /* Tall: the picture across the width, the controls stacked under. */
        int32_t y;
        int32_t half;

        status_h = status_lines * VISION_STATUS_H;
        rows = (buttons + VISION_COLS_TALL - 1) / VISION_COLS_TALL;
        controls = status_h + VISION_GAP + VISION_COUNT_H + VISION_GAP + rows * (VISION_BTN_H + VISION_GAP);
        if (ih < controls + 100) {
            return -1;
        }
        fit(frame_w, frame_h, iw, ih - controls, &pw, &ph);
        l->picture = rect(x0 + (iw - pw) / 2, y0, pw, ph);
        y = y0 + ph + VISION_GAP;
        l->status = rect(x0, y, iw, status_h);
        y += status_h + VISION_GAP;
        half = (iw - VISION_GAP) / 2;
        l->count_a = rect(x0, y, half, VISION_COUNT_H);
        l->count_b = rect(x0 + half + VISION_GAP, y, half, VISION_COUNT_H);
        y += VISION_COUNT_H + VISION_GAP;
        grid(l, x0, y, iw, VISION_COLS_TALL, buttons);
        return 0;
    }
    {
        /* Wide: the picture at the full height on the left, a column of
         * controls on the right. The status gets a line more: the column
         * is narrower than the width. */
        int32_t side;
        int32_t sx;
        int32_t y;
        int32_t half;

        status_h = (status_lines + 1) * VISION_STATUS_H;
        rows = (buttons + VISION_COLS_WIDE - 1) / VISION_COLS_WIDE;
        controls = status_h + VISION_GAP + VISION_COUNT_H + VISION_GAP + rows * VISION_BTN_H +
                   (rows - 1) * VISION_GAP;
        fit(frame_w, frame_h, iw - VISION_SIDE_MIN - VISION_GAP, ih, &pw, &ph);
        side = iw - pw - VISION_GAP;
        if (side < VISION_SIDE_MIN || ih < controls) {
            return -1;
        }
        l->picture = rect(x0, y0 + (ih - ph) / 2, pw, ph);
        sx = x0 + pw + VISION_GAP;
        y = y0;
        l->status = rect(sx, y, side, status_h);
        y += status_h + VISION_GAP;
        half = (side - VISION_GAP) / 2;
        l->count_a = rect(sx, y, half, VISION_COUNT_H);
        l->count_b = rect(sx + half + VISION_GAP, y, half, VISION_COUNT_H);
        y += VISION_COUNT_H + VISION_GAP;
        y += grid(l, sx, y, side, VISION_COLS_WIDE, buttons);
        if (y > y0 + ih) {
            return -1;
        }
        return 0;
    }
}
