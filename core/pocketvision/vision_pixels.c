/*
 * The pixel modes. See vision_pixels.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_pixels.h"

#include <errno.h>
#include <string.h>

#define GREEN 0x07e0u
#define MAGENTA 0xf81fu
#define YELLOW 0xffe0u

struct vision_rgb vision_rgb565_to_rgb(uint16_t v)
{
    struct vision_rgb c;
    unsigned r = (v >> 11) & 31;
    unsigned g = (v >> 5) & 63;
    unsigned b = v & 31;

    c.r = (uint8_t)((r << 3) | (r >> 2));
    c.g = (uint8_t)((g << 2) | (g >> 4));
    c.b = (uint8_t)((b << 3) | (b >> 2));
    return c;
}

uint16_t vision_rgb_to_rgb565(struct vision_rgb c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

uint8_t vision_luma(struct vision_rgb c)
{
    return (uint8_t)((c.r * 77u + c.g * 150u + c.b * 29u) >> 8);
}

static bool picture_ok(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride)
{
    return px && w >= 3 && h >= 3 && w <= VISION_PIXELS_MAX_W && stride >= w;
}

int vision_pixels_sample(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, int32_t x,
                         int32_t y, struct vision_rgb *out)
{
    int32_t x0;
    int32_t y0;
    int32_t i;
    int32_t j;
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t n = 0;

    if (!picture_ok(px, w, h, stride) || !out) {
        return -EINVAL;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= (int32_t)w) {
        x = (int32_t)w - 1;
    }
    if (y >= (int32_t)h) {
        y = (int32_t)h - 1;
    }
    x0 = x - 2 < 0 ? 0 : x - 2;
    y0 = y - 2 < 0 ? 0 : y - 2;
    for (j = y0; j <= y + 2 && j < (int32_t)h; j++) {
        for (i = x0; i <= x + 2 && i < (int32_t)w; i++) {
            struct vision_rgb c = vision_rgb565_to_rgb(px[(uint32_t)j * stride + (uint32_t)i]);

            r += c.r;
            g += c.g;
            b += c.b;
            n++;
        }
    }
    out->r = (uint8_t)(r / n);
    out->g = (uint8_t)(g / n);
    out->b = (uint8_t)(b / n);
    return 0;
}

int vision_pixels_color(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, struct vision_rgb target,
                        uint32_t tol, struct vision_color_result *out)
{
    uint32_t x;
    uint32_t y;
    uint64_t sx = 0;
    uint64_t sy = 0;
    uint32_t matched = 0;
    /* A green target would vanish under a green highlight. */
    uint16_t paint = target.g > target.r && target.g > target.b ? MAGENTA : GREEN;

    if (!picture_ok(px, w, h, stride) || !out) {
        return -EINVAL;
    }
    if (tol > VISION_COLOR_TOL_MAX) {
        tol = VISION_COLOR_TOL_MAX;
    }
    for (y = 0; y < h; y++) {
        uint16_t *row = px + (size_t)y * stride;

        for (x = 0; x < w; x++) {
            struct vision_rgb c = vision_rgb565_to_rgb(row[x]);
            uint32_t d = (uint32_t)(c.r > target.r ? c.r - target.r : target.r - c.r) +
                         (uint32_t)(c.g > target.g ? c.g - target.g : target.g - c.g) +
                         (uint32_t)(c.b > target.b ? c.b - target.b : target.b - c.b);

            if (d <= tol) {
                row[x] = paint;
                matched++;
                sx += x;
                sy += y;
            }
        }
    }
    out->matched = matched;
    out->total = w * h;
    out->cx = matched ? (int32_t)(sx / matched) : -1;
    out->cy = matched ? (int32_t)(sy / matched) : -1;
    return 0;
}

static void luma_row(const uint16_t *row, uint32_t w, uint8_t *out)
{
    uint32_t x;

    for (x = 0; x < w; x++) {
        out[x] = vision_luma(vision_rgb565_to_rgb(row[x]));
    }
}

int vision_pixels_edge(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t threshold,
                       struct vision_edge_result *out)
{
    /* Three luma rows: above, this, below. Row y is written only after
     * row y + 1's luma has been read, so the picture is edged in place. */
    static uint8_t luma[3][VISION_PIXELS_MAX_W];
    uint32_t y;
    uint32_t strong = 0;

    if (!picture_ok(px, w, h, stride) || !out) {
        return -EINVAL;
    }
    luma_row(px, w, luma[0]);
    luma_row(px + stride, w, luma[1]);
    for (y = 0; y < h; y++) {
        const uint8_t *up = luma[(y + 2) % 3];   /* row y - 1 (row y + 2's slot, not yet filled) */
        const uint8_t *me = luma[y % 3];
        const uint8_t *dn = luma[(y + 1) % 3];
        uint16_t *row = px + (size_t)y * stride;
        uint32_t x;

        if (y + 1 < h && y > 0) {
            luma_row(px + (size_t)(y + 1) * stride, w, luma[(y + 1) % 3]);
        }
        if (y == 0) {
            up = me;
        }
        if (y + 1 >= h) {
            dn = me;
        }
        for (x = 0; x < w; x++) {
            uint32_t xl = x ? x - 1 : x;
            uint32_t xr = x + 1 < w ? x + 1 : x;
            int32_t gx = (int32_t)up[xr] + 2 * (int32_t)me[xr] + (int32_t)dn[xr] - (int32_t)up[xl] -
                         2 * (int32_t)me[xl] - (int32_t)dn[xl];
            int32_t gy = (int32_t)dn[xl] + 2 * (int32_t)dn[x] + (int32_t)dn[xr] - (int32_t)up[xl] -
                         2 * (int32_t)up[x] - (int32_t)up[xr];
            uint32_t mag = (uint32_t)((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) >> 3;
            uint32_t g;

            if (mag > 255) {
                mag = 255;
            }
            if (mag >= VISION_EDGE_STRONG) {
                strong++;
            }
            g = threshold ? (mag >= threshold ? 255u : 0u) : mag;
            row[x] = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        }
    }
    out->strong = strong;
    out->total = w * h;
    return 0;
}

int vision_pixels_trace(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, bool dark,
                        struct vision_trace_result *out)
{
    static int16_t centre[4096];  /* per row, -1 for none; the tallest picture a slot holds */
    uint32_t y;
    uint32_t rows = 0;
    uint32_t min_run = w / 50 < 2 ? 2 : w / 50;
    uint32_t max_run = (w * 6) / 10;
    int64_t sy = 0;
    int64_t sx = 0;
    int64_t syy = 0;
    int64_t sxy = 0;
    int64_t band_sum = 0;
    uint32_t band_n = 0;
    uint32_t band_from = (h * 3) / 4;

    if (!picture_ok(px, w, h, stride) || !out || h > 4096) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    for (y = 0; y < h; y++) {
        const uint16_t *row = px + (size_t)y * stride;
        uint32_t x;
        uint32_t n = 0;
        uint64_t sum = 0;

        for (x = 0; x < w; x++) {
            uint8_t l = vision_luma(vision_rgb565_to_rgb(row[x]));

            if (dark ? l < VISION_TRACE_DARK : l > VISION_TRACE_LIGHT) {
                n++;
                sum += x;
            }
        }
        if (n >= min_run && n <= max_run) {
            int32_t cx = (int32_t)(sum / n);

            centre[y] = (int16_t)cx;
            rows++;
            sy += y;
            sx += cx;
            syy += (int64_t)y * y;
            sxy += (int64_t)y * cx;
            if (y >= band_from) {
                band_sum += cx;
                band_n++;
            }
        } else {
            centre[y] = -1;
        }
    }
    out->rows = rows;
    if (rows < h / 8 || band_n == 0) {
        return 0;
    }
    {
        /* Least squares: cx = a + b * y; b in dx per 1000 rows. */
        int64_t n = rows;
        int64_t den = n * syy - sy * sy;
        int64_t b_pm = den ? ((n * sxy - sy * sx) * 1000) / den : 0;
        int64_t band_cx = band_sum / band_n;
        int64_t half = w / 2;

        out->found = true;
        out->slope_pm = (int32_t)(b_pm > 100000 ? 100000 : b_pm < -100000 ? -100000 : b_pm);
        out->offset_pm = (int32_t)(((band_cx - half) * 1000) / (half ? half : 1));
    }
    /* Mark the centroids: three bright pixels on every row that has one. */
    for (y = 0; y < h; y++) {
        if (centre[y] >= 0) {
            uint16_t *row = px + (size_t)y * stride;
            int32_t c = centre[y];
            int32_t i;

            for (i = c - 1; i <= c + 1; i++) {
                if (i >= 0 && i < (int32_t)w) {
                    row[i] = YELLOW;
                }
            }
        }
    }
    return 0;
}
