/*
 * The shared picture's encoder (vision_share.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_share.h"

#include <errno.h>
#include <string.h>

uint32_t vision_share_divisor(uint32_t w, uint32_t h, uint32_t max_side)
{
    uint32_t longer = w > h ? w : h;
    uint32_t div;

    if (w == 0 || h == 0 || max_side < VISION_SHARE_MIN_SIDE || max_side > VISION_SHARE_MAX_SIDE) {
        return 0;
    }
    div = (longer + max_side - 1) / max_side;
    if (div == 0) {
        div = 1;
    }
    /* Nothing is left of a picture narrower than its divisor. */
    if (w / div == 0 || h / div == 0) {
        return 0;
    }
    return div;
}

#ifdef POCKETCAM_HAVE_JPEG

#include <setjmp.h>
#include <stdio.h>

#include <jpeglib.h>

bool vision_share_supported(void)
{
    return true;
}

/* A destination that is the caller's buffer and ends there: libjpeg asks for
 * more room by calling empty_output_buffer, and there is none. */
struct fixed_dest {
    struct jpeg_destination_mgr pub;
    uint8_t *out;
    size_t cap;
    volatile bool full; /* written before a longjmp, read after it */
};

struct share_err {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void on_error(j_common_ptr cinfo)
{
    struct share_err *e = (struct share_err *)cinfo->err;

    longjmp(e->jump, 1);
}

static void on_message(j_common_ptr cinfo)
{
    (void)cinfo; /* nothing is written to stderr: it is the helper's event channel's neighbour */
}

static void dest_init(j_compress_ptr cinfo)
{
    struct fixed_dest *d = (struct fixed_dest *)cinfo->dest;

    d->pub.next_output_byte = d->out;
    d->pub.free_in_buffer = d->cap;
}

static boolean dest_empty(j_compress_ptr cinfo)
{
    struct fixed_dest *d = (struct fixed_dest *)cinfo->dest;
    struct share_err *e = (struct share_err *)cinfo->err;

    d->full = true;
    longjmp(e->jump, 1);
    return FALSE;
}

static void dest_term(j_compress_ptr cinfo)
{
    (void)cinfo;
}

/* One reduced row as RGB888: each output pixel the mean of its div x div
 * block. */
static void reduce_row(const uint16_t *px, uint32_t stride, uint32_t div, uint32_t ow, uint32_t oy, uint8_t *row)
{
    const uint32_t n = div * div;
    uint32_t ox;

    for (ox = 0; ox < ow; ox++) {
        uint32_t r = 0;
        uint32_t g = 0;
        uint32_t b = 0;
        uint32_t dy;

        for (dy = 0; dy < div; dy++) {
            const uint16_t *p = px + (size_t)(oy * div + dy) * stride + (size_t)ox * div;
            uint32_t dx;

            for (dx = 0; dx < div; dx++) {
                uint16_t v = p[dx];

                r += (v >> 11) & 0x1f;
                g += (v >> 5) & 0x3f;
                b += v & 0x1f;
            }
        }
        r /= n;
        g /= n;
        b /= n;
        row[ox * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        row[ox * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        row[ox * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
    }
}

int vision_share_encode(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t div, int quality,
                        uint8_t *out, size_t cap, uint32_t *ow, uint32_t *oh)
{
    static uint8_t row[VISION_SHARE_MAX_SIDE * 3];
    struct jpeg_compress_struct cinfo;
    struct share_err err;
    struct fixed_dest dest;
    uint32_t rw;
    uint32_t rh;

    if (!px || !out || !ow || !oh || div == 0 || w == 0 || h == 0 || stride < w || cap == 0) {
        return -EINVAL;
    }
    rw = w / div;
    rh = h / div;
    if (rw == 0 || rh == 0 || rw > VISION_SHARE_MAX_SIDE || rh > VISION_SHARE_MAX_SIDE ||
        quality < 1 || quality > 100) {
        return -EINVAL;
    }
    memset(&dest, 0, sizeof(dest));
    dest.out = out;
    dest.cap = cap;
    dest.pub.init_destination = dest_init;
    dest.pub.empty_output_buffer = dest_empty;
    dest.pub.term_destination = dest_term;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_message;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        return dest.full ? -ENOSPC : -EIO;
    }
    jpeg_create_compress(&cinfo);
    cinfo.dest = &dest.pub;
    cinfo.image_width = rw;
    cinfo.image_height = rh;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    /* The fast integer transform: the picture is a preview, and the hart is
     * the detector's first. */
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW rows[1];

        reduce_row(px, stride, div, rw, cinfo.next_scanline, row);
        rows[0] = row;
        jpeg_write_scanlines(&cinfo, rows, 1);
    }
    jpeg_finish_compress(&cinfo);
    {
        size_t n = cap - dest.pub.free_in_buffer;

        jpeg_destroy_compress(&cinfo);
        *ow = rw;
        *oh = rh;
        return (int)n;
    }
}

#else

bool vision_share_supported(void)
{
    return false;
}

int vision_share_encode(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t div, int quality,
                        uint8_t *out, size_t cap, uint32_t *ow, uint32_t *oh)
{
    (void)px;
    (void)w;
    (void)h;
    (void)stride;
    (void)div;
    (void)quality;
    (void)out;
    (void)cap;
    (void)ow;
    (void)oh;
    return -ENOTSUP;
}

#endif
