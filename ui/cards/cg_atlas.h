/*
 * Card games: a set of same-sized images composed once into RAM, so that
 * drawing one afterwards is a single blit (docs/design/card-games, section 8).
 *
 * The caller draws each image with ordinary LVGL draw calls; the atlas keeps
 * the result as RGB565A8, the format the device blends cheapest with alpha.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_ATLAS_H
#define CG_ATLAS_H

#include "lvgl.h"

#include <stddef.h>
#include <stdint.h>

/* Draw image `index` into `area` of `layer`. The layer starts transparent. */
typedef void (*cg_atlas_draw_cb)(lv_layer_t *layer, const lv_area_t *area, int index, void *user);

struct cg_atlas {
    int w, h, count;
    lv_image_dsc_t *image;   /* count descriptors */
    uint8_t *pixels;         /* count * w * h * 3 bytes */
    size_t bytes;            /* everything the atlas holds */
};

/* Compose `count` images of w x h. `scratch_parent` hosts a hidden canvas for
 * the duration of the call. Returns 0, or -1 with nothing held. */
int cg_atlas_build(struct cg_atlas *a, lv_obj_t *scratch_parent, int w, int h, int count, cg_atlas_draw_cb draw,
                   void *user);
const lv_image_dsc_t *cg_atlas_image(const struct cg_atlas *a, int index);
void cg_atlas_free(struct cg_atlas *a);

/* A soft shadow for a w x h rounded rectangle as one A8 image, `margin`
 * pixels larger on every side; draw it with the shadow colour as recolor.
 * `peak` is the opacity under the card's edge. Returns 0 or -1. */
int cg_shadow_build(lv_image_dsc_t *image, int w, int h, int radius, int margin, uint8_t peak);
void cg_shadow_free(lv_image_dsc_t *image);

#endif
