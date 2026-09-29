/*
 * Where the Video app's pieces go. See video_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "video_layout.h"

#include <string.h>

#define P VIDEO_LAYOUT_PAD
#define G VIDEO_LAYOUT_GAP

static struct video_rect rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct video_rect r = { x, y, w < 0 ? 0 : w, h < 0 ? 0 : h };

    return r;
}

static void list_layout(struct video_layout *l, int32_t w, int32_t h, int32_t il, int32_t it,
                        int32_t ir, int32_t ib)
{
    int32_t top = it + G;
    int32_t left = il + P;
    int32_t right = w - ir - P;

    if (l->landscape) {
        l->back = rect(left, top, VIDEO_LAYOUT_BACK_W, VIDEO_LAYOUT_BTN_H);
        left += VIDEO_LAYOUT_BACK_W + 16;
    }
    l->rescan = rect(right - VIDEO_LAYOUT_RESCAN_W, top, VIDEO_LAYOUT_RESCAN_W, VIDEO_LAYOUT_BTN_H);
    l->title = rect(left, top, l->rescan.x - G - left, VIDEO_LAYOUT_BTN_H);
    l->list = rect(il + P, top + VIDEO_LAYOUT_BTN_H + G, w - il - ir - 2 * P,
                   h - ib - P - (top + VIDEO_LAYOUT_BTN_H + G));
}

int video_layout_compute(struct video_layout *l, int32_t w, int32_t h, int32_t il, int32_t it,
                         int32_t ir, int32_t ib, bool landscape, bool fullscreen)
{
    int32_t x0;
    int32_t y0;
    int32_t iw;
    int32_t bottom;

    memset(l, 0, sizeof(*l));
    l->landscape = landscape;
    l->fullscreen = fullscreen;
    if (w <= 0 || h <= 0) {
        return -1;
    }
    list_layout(l, w, h, il, it, ir, ib);
    if (fullscreen) {
        l->box = rect(0, 0, w, h);
        return 0;
    }
    x0 = il + P;
    y0 = it + P;
    bottom = h - ib - P;
    if (landscape) {
        int32_t side_x = w - ir - P - VIDEO_LAYOUT_SIDE_W;
        int32_t y = y0;
        int32_t box_h;

        l->btn_back = rect(side_x, y, VIDEO_LAYOUT_SIDE_W, VIDEO_LAYOUT_BTN_H);
        y += VIDEO_LAYOUT_BTN_H + G;
        l->btn_play = rect(side_x, y, VIDEO_LAYOUT_SIDE_W, VIDEO_LAYOUT_BTN_H);
        y += VIDEO_LAYOUT_BTN_H + G;
        l->btn_stop = rect(side_x, y, VIDEO_LAYOUT_SIDE_W, VIDEO_LAYOUT_BTN_H);
        y += VIDEO_LAYOUT_BTN_H + G;
        l->btn_full = rect(side_x, y, VIDEO_LAYOUT_SIDE_W, VIDEO_LAYOUT_BTN_H);
        y += VIDEO_LAYOUT_BTN_H + G;
        l->status = rect(side_x, y, VIDEO_LAYOUT_SIDE_W, bottom - y);
        iw = side_x - P - x0;
        box_h = bottom - y0 - (G + VIDEO_LAYOUT_SLIDER_H + VIDEO_LAYOUT_TIME_H);
        l->box = rect(x0, y0, iw, box_h);
        l->slider = rect(x0, y0 + box_h + G, iw, VIDEO_LAYOUT_SLIDER_H);
        l->elapsed = rect(x0, l->slider.y + VIDEO_LAYOUT_SLIDER_H, iw / 2, VIDEO_LAYOUT_TIME_H);
        l->duration = rect(x0 + iw / 2, l->elapsed.y, iw - iw / 2, VIDEO_LAYOUT_TIME_H);
        return l->box.w >= VIDEO_LAYOUT_MIN_BOX && l->box.h >= VIDEO_LAYOUT_MIN_BOX &&
                       l->status.h >= 0 && y <= bottom
                   ? 0
                   : -1;
    }
    iw = w - il - ir - 2 * P;
    {
        int32_t bw = (iw - 3 * G) / 4;
        int32_t by = bottom - VIDEO_LAYOUT_BTN_H;
        int32_t ty = by - G - VIDEO_LAYOUT_TIME_H;
        int32_t sy = ty - VIDEO_LAYOUT_SLIDER_H;
        int32_t sty = sy - G - VIDEO_LAYOUT_STATUS_H;

        l->btn_back = rect(x0, by, bw, VIDEO_LAYOUT_BTN_H);
        l->btn_stop = rect(x0 + (bw + G), by, bw, VIDEO_LAYOUT_BTN_H);
        l->btn_play = rect(x0 + 2 * (bw + G), by, bw, VIDEO_LAYOUT_BTN_H);
        l->btn_full = rect(x0 + 3 * (bw + G), by, iw - 3 * (bw + G), VIDEO_LAYOUT_BTN_H);
        l->elapsed = rect(x0, ty, iw / 2, VIDEO_LAYOUT_TIME_H);
        l->duration = rect(x0 + iw / 2, ty, iw - iw / 2, VIDEO_LAYOUT_TIME_H);
        l->slider = rect(x0, sy, iw, VIDEO_LAYOUT_SLIDER_H);
        l->status = rect(x0, sty, iw, VIDEO_LAYOUT_STATUS_H);
        l->box = rect(x0, y0, iw, sty - G - y0);
    }
    return l->box.w >= VIDEO_LAYOUT_MIN_BOX && l->box.h >= VIDEO_LAYOUT_MIN_BOX ? 0 : -1;
}
