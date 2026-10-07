/*
 * Camera's layout. See camera_layout.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
    /* PHOTOS: right of the shutter, as the last photo is left of it. */
    {
        int32_t from = l->shutter.x + CAMERA_SHUTTER_W;
        int32_t room = x0 + bw - from;
        int32_t gw = room - CAMERA_GAP < CAMERA_PHOTOS_W ? room - CAMERA_GAP : CAMERA_PHOTOS_W;

        if (gw >= CAMERA_PHOTOS_MIN_W) {
            l->gallery = rect(from + (room - gw) / 2, row_y + (CAMERA_SHUTTER_H - CAMERA_BTN_H) / 2,
                              gw, CAMERA_BTN_H);
        }
    }
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
    /* PHOTOS under the shutter, unless the last photo had to go there. */
    if (l->last.y < l->shutter.y &&
        l->shutter.y + CAMERA_SHUTTER_H + CAMERA_GAP + CAMERA_BTN_H <= y0 + bh) {
        l->gallery = rect(l->shutter.x, l->shutter.y + CAMERA_SHUTTER_H + CAMERA_GAP,
                          CAMERA_SHUTTER_W, CAMERA_BTN_H);
    }
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

/* ---- the gallery ------------------------------------------------------------ */

static struct camera_rect capped(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t cw = w > CAMERA_PICTURE_MAX ? CAMERA_PICTURE_MAX : w;
    int32_t ch = h > CAMERA_PICTURE_MAX ? CAMERA_PICTURE_MAX : h;

    return rect(x + (w - cw) / 2, y + (h - ch) / 2, cw, ch);
}

/* As many cells of about GALLERY_CELL_TARGET as fit gw x gh, at most
 * GALLERY_CELLS_MAX. */
static int grid(struct camera_gallery_layout *l, int32_t gx, int32_t gy, int32_t gw, int32_t gh)
{
    int32_t cell;
    int32_t used_w;

    l->cols = (gw + GALLERY_GAP) / (GALLERY_CELL_TARGET + GALLERY_GAP);
    if (l->cols < 1) {
        l->cols = 1;
    }
    if (l->cols > GALLERY_CELLS_MAX) {
        l->cols = GALLERY_CELLS_MAX;
    }
    cell = (gw - (l->cols - 1) * GALLERY_GAP) / l->cols;
    if (cell > gh) {
        cell = gh;
    }
    if (cell < CAMERA_BTN_H) {
        return -1;
    }
    l->rows = (gh + GALLERY_GAP) / (cell + GALLERY_GAP);
    if (l->rows * l->cols > GALLERY_CELLS_MAX) {
        l->rows = GALLERY_CELLS_MAX / l->cols;
    }
    if (l->rows < 1) {
        return -1;
    }
    l->cell = cell;
    used_w = l->cols * cell + (l->cols - 1) * GALLERY_GAP;
    l->grid = rect(gx + (gw - used_w) / 2, gy, used_w,
                   l->rows * cell + (l->rows - 1) * GALLERY_GAP);
    return 0;
}

struct camera_rect camera_gallery_cell_rect(const struct camera_gallery_layout *l, int cell)
{
    int col = l->cols > 0 ? cell % l->cols : 0;
    int row = l->cols > 0 ? cell / l->cols : 0;

    return rect(l->grid.x + col * (l->cell + GALLERY_GAP), l->grid.y + row * (l->cell + GALLERY_GAP),
                l->cell, l->cell);
}

/* n buttons side by side across w from x. */
static void row_of(struct camera_rect *out[], int n, int32_t x, int32_t y, int32_t w)
{
    int32_t bw = (w - (n - 1) * CAMERA_GAP) / n;
    int i;

    for (i = 0; i < n; i++) {
        *out[i] = rect(x + i * (bw + CAMERA_GAP), y, bw, CAMERA_BTN_H);
    }
}

static int gallery_tall(struct camera_gallery_layout *l, int32_t x0, int32_t y0, int32_t bw,
                        int32_t bh)
{
    int32_t foot = y0 + bh - CAMERA_BTN_H;          /* the lower row of buttons */
    int32_t upper = foot - CAMERA_GAP - CAMERA_BTN_H;
    int32_t top = y0 + GALLERY_STATUS_H + CAMERA_GAP;
    struct camera_rect *two[2];
    struct camera_rect *three[3];
    int32_t info_y;

    l->status = rect(x0, y0, bw, GALLERY_STATUS_H);
    two[0] = &l->newer;
    two[1] = &l->older;
    row_of(two, 2, x0, upper, bw);
    two[0] = &l->left;
    two[1] = &l->middle;
    row_of(two, 2, x0, foot, bw);
    l->panel = rect(x0, top, bw, upper - CAMERA_GAP - top);
    if (grid(l, x0, top, bw, upper - CAMERA_GAP - top) != 0) {
        return -1;
    }

    two[0] = &l->p_newer;
    two[1] = &l->p_older;
    row_of(two, 2, x0, upper, bw);
    three[0] = &l->p_left;
    three[1] = &l->p_middle;
    three[2] = &l->p_right;
    row_of(three, 3, x0, foot, bw);
    info_y = upper - CAMERA_GAP - GALLERY_INFO_H;
    l->info = rect(x0, info_y, bw, GALLERY_INFO_H);
    if (info_y - CAMERA_GAP - top < CAMERA_BTN_H) {
        return -1;
    }
    l->photo = capped(x0, top, bw, info_y - CAMERA_GAP - top);
    return 0;
}

static int gallery_wide(struct camera_gallery_layout *l, int32_t x0, int32_t y0, int32_t bw,
                        int32_t bh)
{
    int32_t cx = x0 + bw - GALLERY_SIDE_W;
    int32_t mw = cx - CAMERA_GAP - x0;
    int32_t foot = y0 + bh - CAMERA_BTN_H;
    int32_t y;
    struct camera_rect *two[2];
    struct camera_rect *three[3];

    l->status = rect(cx, y0, GALLERY_SIDE_W, GALLERY_STATUS_H);
    y = y0 + GALLERY_STATUS_H + CAMERA_GAP;
    two[0] = &l->newer;
    two[1] = &l->older;
    row_of(two, 2, cx, y, GALLERY_SIDE_W);
    l->middle = rect(cx, y + CAMERA_BTN_H + CAMERA_GAP, GALLERY_SIDE_W, CAMERA_BTN_H);
    l->left = rect(cx, foot, GALLERY_SIDE_W, CAMERA_BTN_H);
    if (l->middle.y + CAMERA_BTN_H + CAMERA_GAP > foot) {
        return -1;
    }
    l->panel = rect(x0, y0, mw, bh);
    if (grid(l, x0, y0, mw, bh) != 0) {
        return -1;
    }

    l->info = rect(cx, y, GALLERY_SIDE_W, GALLERY_INFO_H);
    y += GALLERY_INFO_H + CAMERA_GAP;
    two[0] = &l->p_newer;
    two[1] = &l->p_older;
    row_of(two, 2, cx, y, GALLERY_SIDE_W);
    if (y + CAMERA_BTN_H + CAMERA_GAP > foot) {
        return -1;
    }
    three[0] = &l->p_left;
    three[1] = &l->p_middle;
    three[2] = &l->p_right;
    row_of(three, 3, cx, foot, GALLERY_SIDE_W);
    l->photo = capped(x0, y0, mw, bh);
    return 0;
}

int camera_gallery_layout_compute(struct camera_gallery_layout *l, int32_t w, int32_t h,
                                  int32_t inset_left, int32_t inset_top, int32_t inset_right,
                                  int32_t inset_bottom)
{
    int32_t x0 = inset_left;
    int32_t y0 = inset_top;
    int32_t bw = w - inset_left - inset_right;
    int32_t bh = h - inset_top - inset_bottom;
    int r;

    memset(l, 0, sizeof(*l));
    if (bw < 3 * CAMERA_BTN_H + 2 * CAMERA_GAP || bh < CAMERA_STATUS_H + CAMERA_GAP + CAMERA_BTN_H) {
        return -1;
    }
    l->wide = w > h;
    if (l->wide) {
        if (bw < GALLERY_SIDE_W + CAMERA_GAP + CAMERA_BTN_H) {
            return -1;
        }
        r = gallery_wide(l, x0, y0, bw, bh);
    } else {
        r = gallery_tall(l, x0, y0, bw, bh);
    }
    if (r != 0) {
        memset(l, 0, sizeof(*l));
        return -1;
    }
    /* The slideshow: everything but a status line at the foot. */
    l->show_status = rect(x0, y0 + bh - CAMERA_STATUS_H, bw, CAMERA_STATUS_H);
    l->show_touch = rect(x0, y0, bw, bh - CAMERA_STATUS_H - CAMERA_GAP);
    l->show = capped(x0, y0, bw, bh - CAMERA_STATUS_H - CAMERA_GAP);
    return 0;
}
