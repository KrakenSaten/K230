/*
 * Images for the Browser: JPEG and PNG bytes from the network into RGB565
 * pixels at the size they will be shown, in the helper, so the shell never
 * decodes anything a page sent.
 *
 * JPEG with libjpeg (BROWSER_HAVE_JPEG) and PNG with libpng's simplified API
 * (BROWSER_HAVE_PNG); both libraries are in the K230 image already
 * (docs/apps/BROWSER.md). GIF, WebP, SVG, AVIF and everything else are
 * refused as a format, never guessed at.
 *
 * BOUNDED. A JPEG is decoded at 1/2, 1/4 or 1/8 of its size when it will be
 * shown smaller, and one row at a time; a PNG is read whole but only up to
 * WEB_PNG_PIXELS_MAX source pixels. Either is refused past
 * WEB_IMAGE_SOURCE_SIDE_MAX per side. The result is at most max_w wide,
 * max_h high and max_pixels in all. Transparency is laid over white, as a
 * browser shows it on a page with no background of its own.
 *
 * The fake network's images ("DOORS-FAKE-IMAGE <w> <h>") are drawn here too,
 * in any build, so the tests and the simulator's demo need no decoder.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_WEB_IMAGE_H
#define POCKETOS_WEB_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_IMAGE_SOURCE_SIDE_MAX 16384
#define WEB_JPEG_PIXELS_MAX (64u * 1024u * 1024u)
#define WEB_PNG_PIXELS_MAX (4u * 1024u * 1024u)

enum web_image_err {
    WEB_IMAGE_OK = 0,
    WEB_IMAGE_FORMAT,       /* not JPEG or PNG, or this build cannot read it */
    WEB_IMAGE_BROKEN,
    WEB_IMAGE_TOO_LARGE,
    WEB_IMAGE_MEMORY
};

struct web_pixels {
    uint16_t *px;           /* w * h RGB565, malloc'd */
    int w;
    int h;
};

/* want_w: the width the page asked for, 0 when it did not say. */
enum web_image_err web_image_decode(const unsigned char *data, size_t len, int want_w, int max_w,
                                    int max_h, size_t max_pixels, struct web_pixels *out);

/* "jpeg,png", "jpeg", ... or "" for what this build can read. */
const char *web_image_formats(void);

const char *web_image_err_text(enum web_image_err e);

/* The size to show a w0 x h0 image at: never larger than it is, want_w
 * when the page says so, then within max_w, max_h and max_pixels. */
void web_image_fit(int w0, int h0, int want_w, int max_w, int max_h, size_t max_pixels, int *w, int *h);

#endif
