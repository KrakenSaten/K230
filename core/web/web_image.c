/*
 * Image decoding for the Browser's helper. See web_image.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "web/web_image.h"

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BROWSER_HAVE_JPEG
#include <jpeglib.h>
#endif
#ifdef BROWSER_HAVE_PNG
#include <png.h>
#endif

#define FAKE_MAGIC "DOORS-FAKE-IMAGE "

const char *web_image_formats(void)
{
#if defined(BROWSER_HAVE_JPEG) && defined(BROWSER_HAVE_PNG)
    return "jpeg,png";
#elif defined(BROWSER_HAVE_JPEG)
    return "jpeg";
#elif defined(BROWSER_HAVE_PNG)
    return "png";
#else
    return "";
#endif
}

const char *web_image_err_text(enum web_image_err e)
{
    switch (e) {
    case WEB_IMAGE_OK:
        return "ok";
    case WEB_IMAGE_FORMAT:
        return "image format not supported";
    case WEB_IMAGE_BROKEN:
        return "damaged image";
    case WEB_IMAGE_TOO_LARGE:
        return "image too large";
    case WEB_IMAGE_MEMORY:
        return "out of memory";
    }
    return "image not shown";
}

void web_image_fit(int w0, int h0, int want_w, int max_w, int max_h, size_t max_pixels, int *w, int *h)
{
    double tw = w0;
    double th = h0;

    if (want_w > 0 && want_w < tw) {
        th = th * want_w / tw;
        tw = want_w;
    }
    if (max_w > 0 && tw > max_w) {
        th = th * max_w / tw;
        tw = max_w;
    }
    if (max_h > 0 && th > max_h) {
        tw = tw * max_h / th;
        th = max_h;
    }
    if (max_pixels > 0 && tw * th > (double)max_pixels) {
        double k = sqrt((double)max_pixels / (tw * th));

        tw *= k;
        th *= k;
    }
    *w = tw < 1 ? 1 : (int)tw;
    *h = th < 1 ? 1 : (int)th;
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* ---- a box filter, one source row at a time -------------------------------------- */

#if defined(BROWSER_HAVE_JPEG) || defined(BROWSER_HAVE_PNG)

struct scaler {
    int in_w;
    int in_h;
    int out_w;
    int out_h;
    uint32_t *acc;      /* out_w * 3 */
    uint32_t *cols;     /* how many source columns fall in each output column */
    int rows;           /* source rows in acc */
    int y;              /* next source row */
    uint16_t *out;
};

static int scaler_init(struct scaler *s, int in_w, int in_h, int out_w, int out_h)
{
    int x;

    memset(s, 0, sizeof(*s));
    /* Never larger than the source: every output row then gets a source row. */
    if (out_w > in_w) {
        out_w = in_w;
    }
    if (out_h > in_h) {
        out_h = in_h;
    }
    s->in_w = in_w;
    s->in_h = in_h;
    s->out_w = out_w;
    s->out_h = out_h;
    s->acc = calloc((size_t)out_w * 3, sizeof(uint32_t));
    s->cols = calloc((size_t)out_w, sizeof(uint32_t));
    s->out = malloc((size_t)out_w * (size_t)out_h * sizeof(uint16_t));
    if (!s->acc || !s->cols || !s->out) {
        return -1;
    }
    for (x = 0; x < in_w; x++) {
        s->cols[(int64_t)x * out_w / in_w]++;
    }
    return 0;
}

static void scaler_free(struct scaler *s)
{
    free(s->acc);
    free(s->cols);
    free(s->out);
    memset(s, 0, sizeof(*s));
}

/* An RGB888 source row. */
static void scaler_row(struct scaler *s, const unsigned char *rgb)
{
    int oy = (int)((int64_t)s->y * s->out_h / s->in_h);
    int next = s->y + 1 < s->in_h ? (int)((int64_t)(s->y + 1) * s->out_h / s->in_h) : -1;
    int x;

    for (x = 0; x < s->in_w; x++) {
        uint32_t *a = s->acc + ((int64_t)x * s->out_w / s->in_w) * 3;

        a[0] += rgb[x * 3];
        a[1] += rgb[x * 3 + 1];
        a[2] += rgb[x * 3 + 2];
    }
    s->rows++;
    s->y++;
    if (next != oy && oy < s->out_h) {
        uint16_t *o = s->out + (size_t)oy * (size_t)s->out_w;

        /* The pinned Xuantie gcc 14 vectorizes this loop for RVV with a masked
         * division whose inactive lanes it then reports as "may be used
         * uninitialized" (-Werror stops the build). Scalar is plenty here. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC novector
#endif
        for (x = 0; x < s->out_w; x++) {
            uint32_t n = s->cols[x] * (uint32_t)s->rows;

            if (n == 0) {
                n = 1;
            }
            o[x] = rgb565(s->acc[x * 3] / n, s->acc[x * 3 + 1] / n, s->acc[x * 3 + 2] / n);
        }
        memset(s->acc, 0, (size_t)s->out_w * 3 * sizeof(uint32_t));
        s->rows = 0;
    }
}

static void take(struct scaler *s, struct web_pixels *out)
{
    out->px = s->out;
    out->w = s->out_w;
    out->h = s->out_h;
    s->out = NULL;
}

#endif

/* ---- the fake network's pictures --------------------------------------------------- */

static enum web_image_err fake(const unsigned char *data, size_t len, int want_w, int max_w, int max_h,
                               size_t max_pixels, struct web_pixels *out)
{
    char head[64];
    int w0;
    int h0;
    int w;
    int h;
    int x;
    int y;
    size_t n = len < sizeof(head) - 1 ? len : sizeof(head) - 1;

    memcpy(head, data, n);
    head[n] = '\0';
    if (sscanf(head + strlen(FAKE_MAGIC), "%d %d", &w0, &h0) != 2 || w0 < 1 || h0 < 1 ||
        w0 > WEB_IMAGE_SOURCE_SIDE_MAX || h0 > WEB_IMAGE_SOURCE_SIDE_MAX) {
        return WEB_IMAGE_BROKEN;
    }
    web_image_fit(w0, h0, want_w, max_w, max_h, max_pixels, &w, &h);
    out->px = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
    if (!out->px) {
        return WEB_IMAGE_MEMORY;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned r = (unsigned)(x * 255 / (w > 1 ? w - 1 : 1));
            unsigned b = (unsigned)(y * 255 / (h > 1 ? h - 1 : 1));
            unsigned g = ((x / 16 + y / 16) & 1) ? 200 : 90;

            out->px[(size_t)y * (size_t)w + (size_t)x] = rgb565(r, g, b);
        }
    }
    out->w = w;
    out->h = h;
    return WEB_IMAGE_OK;
}

/* ---- JPEG ---------------------------------------------------------------------------- */

#ifdef BROWSER_HAVE_JPEG
struct jerr {
    struct jpeg_error_mgr mgr;
    jmp_buf jump;
};

static void jpeg_fail(j_common_ptr c)
{
    longjmp(((struct jerr *)c->err)->jump, 1);
}

static void jpeg_quiet(j_common_ptr c, int level)
{
    (void)c;
    (void)level;
}

static enum web_image_err jpeg(const unsigned char *data, size_t len, int want_w, int max_w, int max_h,
                               size_t max_pixels, struct web_pixels *out)
{
    struct jpeg_decompress_struct c;
    struct jerr e;
    /* Assigned after setjmp and read after longjmp: volatile, and the
     * scaler on the heap. */
    struct scaler *volatile s = calloc(1, sizeof(struct scaler));
    unsigned char *volatile row = NULL;
    volatile enum web_image_err rc = WEB_IMAGE_BROKEN;
    int w;
    int h;

    if (!s) {
        return WEB_IMAGE_MEMORY;
    }
    c.err = jpeg_std_error(&e.mgr);
    e.mgr.error_exit = jpeg_fail;
    e.mgr.emit_message = jpeg_quiet;
    if (setjmp(e.jump)) {
        jpeg_destroy_decompress(&c);
        free(row);
        scaler_free(s);
        free(s);
        return rc;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, (unsigned char *)data, (unsigned long)len);
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
        longjmp(e.jump, 1);
    }
    if (c.image_width < 1 || c.image_height < 1 || c.image_width > WEB_IMAGE_SOURCE_SIDE_MAX ||
        c.image_height > WEB_IMAGE_SOURCE_SIDE_MAX ||
        (uint64_t)c.image_width * c.image_height > WEB_JPEG_PIXELS_MAX) {
        rc = WEB_IMAGE_TOO_LARGE;
        longjmp(e.jump, 1);
    }
    if (c.jpeg_color_space == JCS_CMYK || c.jpeg_color_space == JCS_YCCK) {
        rc = WEB_IMAGE_FORMAT;
        longjmp(e.jump, 1);
    }
    web_image_fit((int)c.image_width, (int)c.image_height, want_w, max_w, max_h, max_pixels, &w, &h);
    c.out_color_space = JCS_RGB;
    c.scale_num = 1;
    c.scale_denom = 1;
    /* Decode no larger than needed: the largest reduction still at least
     * as large as the result. */
    {
        unsigned d;

        for (d = 8; d > 1; d /= 2) {
            if ((c.image_width + d - 1) / d >= (unsigned)w && (c.image_height + d - 1) / d >= (unsigned)h) {
                c.scale_denom = d;
                break;
            }
        }
    }
    c.dct_method = JDCT_IFAST;
    c.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&c);
    if (c.output_components != 3 || c.output_width < 1 || c.output_height < 1) {
        longjmp(e.jump, 1);
    }
    row = malloc((size_t)c.output_width * 3);
    rc = WEB_IMAGE_MEMORY;
    if (!row || scaler_init(s, (int)c.output_width, (int)c.output_height, w, h) != 0) {
        longjmp(e.jump, 1);
    }
    rc = WEB_IMAGE_BROKEN;
    while (c.output_scanline < c.output_height) {
        JSAMPROW rows[1] = { row };

        if (jpeg_read_scanlines(&c, rows, 1) != 1) {
            longjmp(e.jump, 1);
        }
        scaler_row(s, row);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    free(row);
    take(s, out);
    scaler_free(s);
    free(s);
    return WEB_IMAGE_OK;
}
#endif

/* ---- PNG ----------------------------------------------------------------------------- */

#ifdef BROWSER_HAVE_PNG
static enum web_image_err png(const unsigned char *data, size_t len, int want_w, int max_w, int max_h,
                              size_t max_pixels, struct web_pixels *out)
{
    png_image im;
    png_color white = { 255, 255, 255 };
    unsigned char *buf;
    struct scaler s;
    int w;
    int h;
    int y;

    memset(&im, 0, sizeof(im));
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&im, data, len)) {
        return WEB_IMAGE_BROKEN;
    }
    if (im.width < 1 || im.height < 1 || im.width > WEB_IMAGE_SOURCE_SIDE_MAX ||
        im.height > WEB_IMAGE_SOURCE_SIDE_MAX || (uint64_t)im.width * im.height > WEB_PNG_PIXELS_MAX) {
        png_image_free(&im);
        return WEB_IMAGE_TOO_LARGE;
    }
    im.format = PNG_FORMAT_RGB;
    buf = malloc(PNG_IMAGE_SIZE(im));
    if (!buf) {
        png_image_free(&im);
        return WEB_IMAGE_MEMORY;
    }
    if (!png_image_finish_read(&im, &white, buf, 0, NULL)) {
        png_image_free(&im);
        free(buf);
        return WEB_IMAGE_BROKEN;
    }
    web_image_fit((int)im.width, (int)im.height, want_w, max_w, max_h, max_pixels, &w, &h);
    if (scaler_init(&s, (int)im.width, (int)im.height, w, h) != 0) {
        scaler_free(&s);
        free(buf);
        return WEB_IMAGE_MEMORY;
    }
    for (y = 0; y < (int)im.height; y++) {
        scaler_row(&s, buf + (size_t)y * im.width * 3);
    }
    free(buf);
    take(&s, out);
    scaler_free(&s);
    return WEB_IMAGE_OK;
}
#endif

enum web_image_err web_image_decode(const unsigned char *data, size_t len, int want_w, int max_w,
                                    int max_h, size_t max_pixels, struct web_pixels *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || len < 8) {
        return WEB_IMAGE_BROKEN;
    }
    if (len > strlen(FAKE_MAGIC) && memcmp(data, FAKE_MAGIC, strlen(FAKE_MAGIC)) == 0) {
        return fake(data, len, want_w, max_w, max_h, max_pixels, out);
    }
    if (data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
#ifdef BROWSER_HAVE_JPEG
        return jpeg(data, len, want_w, max_w, max_h, max_pixels, out);
#else
        return WEB_IMAGE_FORMAT;
#endif
    }
    if (memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
#ifdef BROWSER_HAVE_PNG
        return png(data, len, want_w, max_w, max_h, max_pixels, out);
#else
        return WEB_IMAGE_FORMAT;
#endif
    }
    return WEB_IMAGE_FORMAT;
}
