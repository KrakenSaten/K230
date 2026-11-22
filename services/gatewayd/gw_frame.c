/*
 * gatewayd: the bounded frame reader. See gw_frame.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_frame.h"

#include <stdlib.h>
#include <string.h>

int gw_frame_init(struct gw_frame *f, size_t max)
{
    f->buf = malloc(max + 4);
    f->len = 0;
    f->max = max;
    return f->buf != NULL ? 0 : -1;
}

void gw_frame_free(struct gw_frame *f)
{
    free(f->buf);
    f->buf = NULL;
    f->len = 0;
}

static size_t declared(const struct gw_frame *f)
{
    return ((size_t)f->buf[0] << 24) | ((size_t)f->buf[1] << 16) | ((size_t)f->buf[2] << 8) |
           (size_t)f->buf[3];
}

uint8_t *gw_frame_space(struct gw_frame *f, size_t *n)
{
    size_t want;

    if (f->len < 4) {
        want = 4;               /* the header first, and no further */
    } else {
        size_t body = declared(f);

        if (body == 0 || body > f->max) {
            *n = 0;             /* gw_frame_next() reports it */
            return f->buf + f->len;
        }
        want = 4 + body;
    }
    *n = want > f->len ? want - f->len : 0;
    return f->buf + f->len;
}

void gw_frame_added(struct gw_frame *f, size_t n)
{
    f->len += n;
}

int gw_frame_next(const struct gw_frame *f, const uint8_t **p, size_t *n)
{
    size_t body;

    if (f->len < 4) {
        return 0;
    }
    body = declared(f);
    if (body == 0 || body > f->max) {
        return -1;
    }
    if (f->len < 4 + body) {
        return 0;
    }
    *p = f->buf + 4;
    *n = body;
    return 1;
}

void gw_frame_consume(struct gw_frame *f)
{
    /* gw_frame_space() never lets more than one frame in, so a taken frame
     * is the whole buffer. */
    f->len = 0;
}
