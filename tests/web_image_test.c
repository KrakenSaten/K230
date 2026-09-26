/*
 * The Browser helper's pictures (core/web/web_image.h): the size a picture
 * is shown at, the fake network's pictures, and - in a build with the
 * decoders (BROWSER_IMAGES=1) - JPEG and PNG made here with libjpeg and
 * libpng, decoded and scaled, transparency over white, and damaged,
 * truncated, oversized and mutated files refused or survived without a
 * fault. Run under ASan/UBSan by `make browser-san-test`.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "web/web_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BROWSER_HAVE_JPEG
#include <jpeglib.h>
#endif
#ifdef BROWSER_HAVE_PNG
#include <png.h>
#endif

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static uint32_t rng = 99;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

#ifdef BROWSER_HAVE_PNG
static unsigned char *make_png(int w, int h, size_t *len)
{
    png_image im;
    unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
    unsigned char *out;
    png_alloc_size_t n = 0;
    int i;

    for (i = 0; i < w * h; i++) {
        rgba[i * 4] = (unsigned char)(i % w);
        rgba[i * 4 + 1] = 40;
        rgba[i * 4 + 2] = 200;
        rgba[i * 4 + 3] = (i % w) < w / 2 ? 0 : 255; /* the left half transparent */
    }
    memset(&im, 0, sizeof(im));
    im.version = PNG_IMAGE_VERSION;
    im.width = (png_uint_32)w;
    im.height = (png_uint_32)h;
    im.format = PNG_FORMAT_RGBA;
    png_image_write_to_memory(&im, NULL, &n, 0, rgba, 0, NULL);
    out = malloc(n);
    png_image_write_to_memory(&im, out, &n, 0, rgba, 0, NULL);
    free(rgba);
    *len = n;
    return out;
}
#endif

#ifdef BROWSER_HAVE_JPEG
static unsigned char *make_jpeg(int w, int h, size_t *len)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    unsigned char *out = NULL;
    unsigned long n = 0;
    unsigned char *row = malloc((size_t)w * 3);
    int x;

    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &n);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW rows[1] = { row };

        for (x = 0; x < w; x++) {
            row[x * 3] = 255;
            row[x * 3 + 1] = (unsigned char)(c.next_scanline & 0xff);
            row[x * 3 + 2] = 0;
        }
        jpeg_write_scanlines(&c, rows, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(row);
    *len = n;
    return out;
}
#endif

static int mutate_rounds(const unsigned char *src, size_t len, int rounds)
{
    unsigned char *buf = malloc(len);
    int bad = 0;
    int r;

    for (r = 0; r < rounds; r++) {
        struct web_pixels px;
        size_t cut = len;
        int k;
        enum web_image_err e;

        memcpy(buf, src, len);
        for (k = 0; k < 1 + (int)(rnd() % 8); k++) {
            buf[16 + rnd() % (len - 16)] = (unsigned char)rnd();
        }
        if (rnd() % 4 == 0) {
            cut = 16 + rnd() % (len - 16);
        }
        e = web_image_decode(buf, cut, 0, 528, 1200, 560 * 1024, &px);
        if (e == WEB_IMAGE_OK) {
            bad += px.w < 1 || px.h < 1 || px.w > 528 || px.h > 1200 || !px.px;
            free(px.px);
        } else {
            bad += px.px != NULL;
        }
    }
    free(buf);
    return bad;
}

int main(void)
{
    struct web_pixels px;
    int w;
    int h;
    enum web_image_err e;

    /* ---- the size a picture is shown at ------------------------------------------------ */
    web_image_fit(1600, 1200, 0, 528, 1200, 0, &w, &h);
    check("a wide photo fits the width", w == 528 && h == 396);
    web_image_fit(100, 50, 0, 528, 1200, 0, &w, &h);
    check("a small picture is never enlarged", w == 100 && h == 50);
    web_image_fit(1000, 1000, 200, 528, 1200, 0, &w, &h);
    check("the page's width wins when smaller", w == 200 && h == 200);
    web_image_fit(500, 5000, 0, 528, 1200, 0, &w, &h);
    check("a tall one is held to the height", h == 1200 && w == 120);
    web_image_fit(4000, 4000, 0, 1600, 1600, 100 * 100, &w, &h);
    check("and all of it to the pixel budget", w * h <= 100 * 100 && w >= 99);
    web_image_fit(1, 100000, 0, 528, 1200, 0, &w, &h);
    check("never below one pixel", w == 1 && h == 1200);

    /* ---- the fake network's pictures, any build ------------------------------------------ */
    e = web_image_decode((const unsigned char *)"DOORS-FAKE-IMAGE 480 270\n", 25, 480, 528, 1200, 560 * 1024, &px);
    check("a fake picture is drawn at its size", e == WEB_IMAGE_OK && px.w == 480 && px.h == 270 && px.px);
    free(px.px);
    e = web_image_decode((const unsigned char *)"DOORS-FAKE-IMAGE 99999 5\n", 25, 0, 528, 1200, 560 * 1024, &px);
    check("a fake picture too large is refused", e == WEB_IMAGE_BROKEN && !px.px);
    e = web_image_decode((const unsigned char *)"GIF89a\x01\x00\x01\x00\x00\x00\x00", 13, 0, 528, 1200, 1000, &px);
    check("GIF is a format Browser does not read", e == WEB_IMAGE_FORMAT);
    e = web_image_decode((const unsigned char *)"RIFF\x10\x00\x00\x00WEBPVP8 ", 16, 0, 528, 1200, 1000, &px);
    check("WebP too", e == WEB_IMAGE_FORMAT);
    e = web_image_decode((const unsigned char *)"<svg xmlns=", 11, 0, 528, 1200, 1000, &px);
    check("SVG too", e == WEB_IMAGE_FORMAT);
    e = web_image_decode((const unsigned char *)"x", 1, 0, 528, 1200, 1000, &px);
    check("a byte is not a picture", e == WEB_IMAGE_BROKEN);

#ifdef BROWSER_HAVE_PNG
    {
        size_t n;
        unsigned char *png = make_png(800, 600, &n);
        unsigned char hdr[64];

        e = web_image_decode(png, n, 0, 400, 1200, 560 * 1024, &px);
        check("PNG 800x600 at 400 wide: 400x300", e == WEB_IMAGE_OK && px.w == 400 && px.h == 300);
        check("its transparent half is laid over white", px.px && px.px[10] == 0xffff);
        check("its opaque half keeps its colour", px.px && px.px[390] != 0xffff);
        free(px.px);
        e = web_image_decode(png, n / 2, 0, 400, 1200, 560 * 1024, &px);
        check("a truncated PNG is refused", e == WEB_IMAGE_BROKEN && !px.px);
        /* An IHDR claiming 100000 x 100000, with a CRC that does not matter:
         * the size is refused before anything is allocated for it. */
        memcpy(hdr, png, 33);
        hdr[16] = 0x00;
        hdr[17] = 0x01;
        hdr[18] = 0x86;
        hdr[19] = 0xa0;
        memcpy(hdr + 20, hdr + 16, 4);
        e = web_image_decode(hdr, 33, 0, 400, 1200, 560 * 1024, &px);
        check("a PNG claiming 100000 x 100000 is refused", e != WEB_IMAGE_OK && !px.px);
        check("2000 mutated PNGs: none faults, every result in bounds", mutate_rounds(png, n, 2000) == 0);
        free(png);
        png = make_png(2100, 2100, &n);
        e = web_image_decode(png, n, 0, 528, 1200, 560 * 1024, &px);
        check("a PNG past 4 megapixels is refused as too large", e == WEB_IMAGE_TOO_LARGE);
        free(png);
    }
#else
    printf("SKIP PNG: this build has no decoders (BROWSER_IMAGES=1)\n");
#endif
#ifdef BROWSER_HAVE_JPEG
    {
        size_t n;
        unsigned char *jpg = make_jpeg(1600, 1200, &n);

        e = web_image_decode(jpg, n, 0, 528, 1200, 560 * 1024, &px);
        check("JPEG 1600x1200 at 528 wide: 528x396", e == WEB_IMAGE_OK && px.w == 528 && px.h == 396);
        check("its colour comes through (red)", px.px && (px.px[1000] >> 11) > 28);
        free(px.px);
        e = web_image_decode(jpg, n, 100, 528, 1200, 560 * 1024, &px);
        check("the page's 100 wide is kept", e == WEB_IMAGE_OK && px.w == 100 && px.h == 75);
        free(px.px);
        e = web_image_decode(jpg, 200, 0, 528, 1200, 560 * 1024, &px);
        check("a JPEG cut after its header does not fault", (e == WEB_IMAGE_OK) == (px.px != NULL));
        free(px.px);
        check("2000 mutated JPEGs: none faults, every result in bounds", mutate_rounds(jpg, n, 2000) == 0);
        free(jpg);
    }
#else
    printf("SKIP JPEG: this build has no decoders (BROWSER_IMAGES=1)\n");
#endif
    printf("formats: \"%s\"\n", web_image_formats());
    printf("web_image_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
