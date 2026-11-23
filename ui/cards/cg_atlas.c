/*
 * Card games: the composed image atlas and the shadow sprite. See cg_atlas.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_atlas.h"

#include <stdlib.h>
#include <string.h>

static void header(lv_image_dsc_t *d, lv_color_format_t cf, int w, int h, int stride, const uint8_t *data, size_t size)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)stride;
    d->data = data;
    d->data_size = (uint32_t)size;
}

/* ARGB8888 (bytes B, G, R, A) to RGB565A8: the colour rows, then the alpha
 * rows. */
static void pack(uint8_t *out, const lv_draw_buf_t *src, int w, int h)
{
    uint8_t *alpha = out + (size_t)w * h * 2;
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *in = src->data + (size_t)y * src->header.stride;
        uint8_t *rgb = out + (size_t)y * w * 2;

        for (x = 0; x < w; x++) {
            uint16_t c = (uint16_t)(((in[2] >> 3) << 11) | ((in[1] >> 2) << 5) | (in[0] >> 3));

            memcpy(rgb + x * 2, &c, 2);
            alpha[(size_t)y * w + x] = in[3];
            in += 4;
        }
    }
}

int cg_atlas_build(struct cg_atlas *a, lv_obj_t *scratch_parent, int w, int h, int count, cg_atlas_draw_cb draw,
                   void *user)
{
    size_t each = (size_t)w * h * 3;
    lv_draw_buf_t *buf;
    lv_obj_t *canvas;
    lv_area_t area = { 0, 0, w - 1, h - 1 };
    int i;

    if (!a) {
        return -1;
    }
    memset(a, 0, sizeof(*a));
    if (!scratch_parent || !draw || w <= 0 || h <= 0 || count <= 0) {
        return -1;
    }
    a->image = calloc((size_t)count, sizeof(*a->image));
    a->pixels = malloc(each * count);
    buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_ARGB8888, 0);
    canvas = lv_canvas_create(scratch_parent);
    if (!a->image || !a->pixels || !buf || !canvas) {
        if (canvas) {
            lv_obj_delete(canvas);
        }
        if (buf) {
            lv_draw_buf_destroy(buf);
        }
        cg_atlas_free(a);
        return -1;
    }
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(canvas, buf);
    for (i = 0; i < count; i++) {
        lv_layer_t layer;

        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
        lv_canvas_init_layer(canvas, &layer);
        draw(&layer, &area, i, user);
        lv_canvas_finish_layer(canvas, &layer);
        pack(a->pixels + each * i, buf, w, h);
        header(&a->image[i], LV_COLOR_FORMAT_RGB565A8, w, h, w * 2, a->pixels + each * i, each);
    }
    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
    a->w = w;
    a->h = h;
    a->count = count;
    a->bytes = each * count + (size_t)count * sizeof(*a->image);
    return 0;
}

const lv_image_dsc_t *cg_atlas_image(const struct cg_atlas *a, int index)
{
    return (a && a->image && index >= 0 && index < a->count) ? &a->image[index] : NULL;
}

void cg_atlas_free(struct cg_atlas *a)
{
    if (a) {
        free(a->image);
        free(a->pixels);
        memset(a, 0, sizeof(*a));
    }
}

/* Integer square root, rounded down. */
static int isqrt(int v)
{
    int r = 0;
    int bit = 1 << 14;

    while (bit) {
        int t = r | bit;

        if (t * t <= v) {
            r = t;
        }
        bit >>= 1;
    }
    return r;
}

int cg_shadow_build(lv_image_dsc_t *image, int w, int h, int radius, int margin, uint8_t peak)
{
    int sw = w + 2 * margin;
    int sh = h + 2 * margin;
    uint8_t *px;
    int x, y;

    if (!image || w <= 0 || h <= 0 || margin <= 0 || radius < 0) {
        return -1;
    }
    px = malloc((size_t)sw * sh);
    if (!px) {
        return -1;
    }
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            /* Distance outside the rounded rectangle, 0 inside it. */
            int ix = x - margin;
            int iy = y - margin;
            int qx = (ix < radius ? radius - ix : ix > w - 1 - radius ? ix - (w - 1 - radius) : 0);
            int qy = (iy < radius ? radius - iy : iy > h - 1 - radius ? iy - (h - 1 - radius) : 0);
            int d = isqrt(qx * qx + qy * qy) - radius;
            int v;

            if (d <= 0) {
                v = peak;
            } else if (d >= margin) {
                v = 0;
            } else {
                int left = margin - d;

                v = (peak * left * left) / (margin * margin);
            }
            px[(size_t)y * sw + x] = (uint8_t)v;
        }
    }
    header(image, LV_COLOR_FORMAT_A8, sw, sh, sw, px, (size_t)sw * sh);
    return 0;
}

void cg_shadow_free(lv_image_dsc_t *image)
{
    if (image) {
        free((void *)image->data);
        memset(image, 0, sizeof(*image));
    }
}
