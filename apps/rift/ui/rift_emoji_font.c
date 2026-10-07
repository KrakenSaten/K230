/*
 * RIFT colour emoji: an LVGL font that draws Noto Color Emoji images
 * (rift_emoji_font.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_emoji_font.h"

#include "rift_emoji_img.h"

#include <stdlib.h>
#include <string.h>

struct face {
    lv_font_t text;          /* the copy handed out: base, falling back to img */
    lv_font_t img;           /* the colour font, its dsc this face */
    const lv_font_t *base;
};

static struct face faces[RIFT_EMOJI_FACES];

/* One descriptor per image, made the first time it is drawn and kept: LVGL
 * draws a label after measuring it, so what a glyph points at must outlive
 * the lookup. calloc'd on first use; only the pages touched take memory. */
static lv_image_dsc_t *dscs;

static const struct rift_emoji_img *find(uint32_t key)
{
    unsigned lo = 0;
    unsigned hi = rift_emoji_img_count;

    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;

        if (rift_emoji_imgs[mid].key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < rift_emoji_img_count && rift_emoji_imgs[lo].key == key ? &rift_emoji_imgs[lo] : NULL;
}

const lv_image_dsc_t *rift_emoji_image(uint32_t key)
{
    const struct rift_emoji_img *e = find(key);
    lv_image_dsc_t *d;

    if (!e) {
        return NULL;
    }
    if (!dscs) {
        dscs = calloc(rift_emoji_img_count, sizeof(*dscs));
        if (!dscs) {
            return NULL;
        }
    }
    d = &dscs[e - rift_emoji_imgs];
    if (!d->data) {
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565A8;
        d->header.w = e->w;
        d->header.h = e->h;
        d->header.stride = (uint32_t)e->w * 2;
        d->data_size = (uint32_t)e->w * e->h * 3;
        d->data = rift_emoji_px + e->off;
    }
    return d;
}

static bool glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t letter, uint32_t next)
{
    const struct face *f = font->dsc;
    const lv_image_dsc_t *d;
    int32_t line;
    int32_t h;

    (void)next;
    if (letter == 0xFE0Eu || letter == 0xFE0Fu) {
        /* A variation selector says how the emoji before it is drawn and is
         * not drawn itself. Message text has them folded away
         * (rift_emoji_fold); a composer's field holds the text as it will
         * be sent, so there the selector is a glyph of no width rather
         * than LVGL's placeholder box after the heart of U+2764 U+FE0F. */
        memset(g, 0, sizeof(*g));
        g->format = LV_FONT_GLYPH_FORMAT_NONE;
        return true;
    }
    d = rift_emoji_image(letter);
    if (!d) {
        return false;
    }
    line = f->text.line_height;
    h = d->header.h;
    g->adv_w = (uint16_t)(d->header.w + 2);
    g->box_w = (uint16_t)d->header.w;
    g->box_h = (uint16_t)h;
    g->ofs_x = 1;
    /* LVGL puts a glyph's top at line - base_line - box_h - ofs_y below the
     * line's top; this puts the image in the middle of the line. */
    g->ofs_y = (int16_t)((line - f->text.base_line) - h - (line - h) / 2);
    g->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    g->is_placeholder = 0;
    g->gid.src = d;
    return true;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;
    return g->gid.src;
}

bool rift_emoji_font_is(const lv_font_t *font)
{
    int i;

    for (i = 0; i < RIFT_EMOJI_FACES; i++) {
        if (faces[i].base && font == &faces[i].text) {
            return true;
        }
    }
    return false;
}

const lv_font_t *rift_emoji_font(const lv_font_t *base)
{
    int i;

    if (!base || rift_emoji_font_is(base)) {
        return base;
    }
    for (i = 0; i < RIFT_EMOJI_FACES; i++) {
        if (faces[i].base == base) {
            return &faces[i].text;
        }
    }
    for (i = 0; i < RIFT_EMOJI_FACES; i++) {
        struct face *f = &faces[i];

        if (f->base) {
            continue;
        }
        f->text = *base;
        memset(&f->img, 0, sizeof(f->img));
        f->img.get_glyph_dsc = glyph_dsc;
        f->img.get_glyph_bitmap = glyph_bitmap;
        f->img.line_height = base->line_height;
        f->img.base_line = base->base_line;
        f->img.subpx = LV_FONT_SUBPX_NONE;
        f->img.dsc = f;
        f->img.fallback = base->fallback;
        f->text.fallback = &f->img;
        f->base = base;
        return &f->text;
    }
    return base;
}
