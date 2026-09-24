/*
 * pocketcam's pixel conversion. See pocketcam_convert.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketcam_convert.h"

#include <errno.h>
#include <string.h>

static bool valid_rotation(int rotation)
{
    return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}

void pocketcam_turned_size(uint32_t w, uint32_t h, int rotation, uint32_t *tw, uint32_t *th)
{
    bool quarter = rotation == 90 || rotation == 270;

    *tw = quarter ? h : w;
    *th = quarter ? w : h;
}

int pocketcam_view_rotation(int mount_rotation, bool portrait)
{
    int r = mount_rotation + (portrait ? 90 : 0);

    r %= 360;
    return r < 0 ? r + 360 : r;
}

static inline uint8_t clip8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* BT.601 limited range to RGB, integer. */
static inline void yuv_rgb(int y, int u, int v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;

    if (c < 0) {
        c = 0;
    }
    *r = clip8((298 * c + 409 * e + 128) / 256);
    *g = clip8((298 * c - 100 * d - 208 * e + 128) / 256);
    *b = clip8((298 * c + 516 * d + 128) / 256);
}

/* Sensor pixel (sx, sy) as RGB. The frame has been checked. */
static inline void sample(const struct pocketcam_frame *f, uint32_t sx, uint32_t sy, uint8_t *r,
                          uint8_t *g, uint8_t *b)
{
    if (f->format == POCKETCAM_FMT_RGB565) {
        const uint8_t *p = f->data + (size_t)sy * f->stride + (size_t)sx * 2;
        uint16_t px = (uint16_t)(p[0] | (p[1] << 8));

        *r = (uint8_t)(((px >> 11) & 0x1f) << 3);
        *g = (uint8_t)(((px >> 5) & 0x3f) << 2);
        *b = (uint8_t)((px & 0x1f) << 3);
        return;
    }
    {
        const uint8_t *uv = f->data + (size_t)f->stride * f->height;
        uint32_t cy = f->format == POCKETCAM_FMT_NV12 ? sy / 2 : sy;
        size_t ci = (size_t)cy * f->stride + (sx & ~1u);

        yuv_rgb(f->data[(size_t)sy * f->stride + sx], uv[ci], uv[ci + 1], r, g, b);
    }
}

/* Where turned pixel (tx, ty) comes from in the sensor frame (W x H). A
 * clockwise quarter turn puts the sensor's bottom-left corner top-left. */
static inline void source_of(uint32_t tx, uint32_t ty, int rotation, uint32_t w, uint32_t h,
                             uint32_t *sx, uint32_t *sy)
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

static inline uint16_t pack565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

int pocketcam_to_rgb565(const struct pocketcam_frame *f, int rotation, bool mirror,
                        enum pocketcam_fit fit, uint16_t *dst, uint32_t dst_w, uint32_t dst_h,
                        uint32_t dst_stride)
{
    /* Turned coordinates per destination column and line. On the stack: the
     * helper converts every preview frame, and nothing here allocates. */
    uint32_t col_t[POCKETCAM_MAX_DIM];
    uint32_t row_t[POCKETCAM_MAX_DIM];
    uint32_t tw;
    uint32_t th;
    uint32_t cx = 0;
    uint32_t cy = 0;
    uint32_t cw;
    uint32_t ch;
    uint32_t ox = 0;
    uint32_t oy = 0;
    uint32_t ow = dst_w;
    uint32_t oh = dst_h;
    uint32_t x;
    uint32_t y;
    int r = pocketcam_frame_check(f);

    if (r != 0) {
        return r;
    }
    if (!dst || dst_w == 0 || dst_h == 0 || dst_w > POCKETCAM_MAX_DIM ||
        dst_h > POCKETCAM_MAX_DIM || dst_stride < dst_w || !valid_rotation(rotation)) {
        return -EINVAL;
    }
    pocketcam_turned_size(f->width, f->height, rotation, &tw, &th);
    cw = tw;
    ch = th;
    if (fit == POCKETCAM_FIT_COVER) {
        /* The largest centred part of the turned picture with the
         * destination's shape. */
        if ((uint64_t)tw * dst_h > (uint64_t)dst_w * th) {
            cw = (uint32_t)(((uint64_t)th * dst_w) / dst_h);
            cw = cw ? cw : 1;
            cx = (tw - cw) / 2;
        } else {
            ch = (uint32_t)(((uint64_t)tw * dst_h) / dst_w);
            ch = ch ? ch : 1;
            cy = (th - ch) / 2;
        }
    } else {
        /* The largest centred box of the picture's shape inside the
         * destination; black around it. */
        if ((uint64_t)tw * dst_h > (uint64_t)dst_w * th) {
            oh = (uint32_t)(((uint64_t)th * dst_w) / tw);
            oh = oh ? oh : 1;
            oy = (dst_h - oh) / 2;
        } else {
            ow = (uint32_t)(((uint64_t)tw * dst_h) / th);
            ow = ow ? ow : 1;
            ox = (dst_w - ow) / 2;
        }
        for (y = 0; y < dst_h; y++) {
            if (y < oy || y >= oy + oh) {
                memset(dst + (size_t)y * dst_stride, 0, dst_w * sizeof(uint16_t));
            } else {
                memset(dst + (size_t)y * dst_stride, 0, ox * sizeof(uint16_t));
                memset(dst + (size_t)y * dst_stride + ox + ow, 0,
                       (dst_w - ox - ow) * sizeof(uint16_t));
            }
        }
    }
    for (x = 0; x < ow; x++) {
        uint32_t dx = mirror ? ow - 1 - x : x;

        col_t[x] = cx + (uint32_t)(((uint64_t)dx * cw) / ow);
    }
    for (y = 0; y < oh; y++) {
        row_t[y] = cy + (uint32_t)(((uint64_t)y * ch) / oh);
    }
    for (y = 0; y < oh; y++) {
        uint16_t *out = dst + (size_t)(oy + y) * dst_stride + ox;

        for (x = 0; x < ow; x++) {
            uint32_t sx;
            uint32_t sy;
            uint8_t rr;
            uint8_t gg;
            uint8_t bb;

            source_of(col_t[x], row_t[y], rotation, f->width, f->height, &sx, &sy);
            sample(f, sx, sy, &rr, &gg, &bb);
            out[x] = pack565(rr, gg, bb);
        }
    }
    return 0;
}

int pocketcam_row_rgb888(const struct pocketcam_frame *f, int rotation, bool mirror, uint32_t y,
                         uint8_t *row)
{
    uint32_t tw;
    uint32_t th;
    uint32_t x;
    int r = pocketcam_frame_check(f);

    if (r != 0) {
        return r;
    }
    if (!row || !valid_rotation(rotation)) {
        return -EINVAL;
    }
    pocketcam_turned_size(f->width, f->height, rotation, &tw, &th);
    if (y >= th) {
        return -EINVAL;
    }
    for (x = 0; x < tw; x++) {
        uint32_t sx;
        uint32_t sy;

        source_of(mirror ? tw - 1 - x : x, y, rotation, f->width, f->height, &sx, &sy);
        sample(f, sx, sy, &row[x * 3], &row[x * 3 + 1], &row[x * 3 + 2]);
    }
    return 0;
}
