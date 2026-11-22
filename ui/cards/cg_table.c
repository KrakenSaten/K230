/*
 * Card games: the Lamplight table. See cg_table.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_table.h"

#include "cg_cloth.h"

#include <stdlib.h>
#include <string.h>

lv_color_t cg_table_ink(enum cg_table_ink ink)
{
    switch (ink) {
    case CG_INK_GOLD:
        return lv_color_hex(0xd9b66a);
    case CG_INK_DIM:
        return lv_color_hex(0xa89f8c);
    case CG_INK_DARK:
        return lv_color_hex(0x1a1714);
    case CG_INK_ROOM:
        return lv_color_hex(0x000000);
    case CG_INK_IVORY:
    default:
        return lv_color_hex(0xfaf4e4);
    }
}

/* ---- the cloth ------------------------------------------------------------------ */

static const uint32_t cloth_ink[CG_CLOTH_KINDS][2] = {
    { 0x7d2130, 0x3a0d16 },
    { 0x40377c, 0x1c183c },
    { 0x226347, 0x0e2f22 },
};

#define CLOTHS 3

struct cloth {
    int kind, w, h;
    uint16_t *px;
    lv_image_dsc_t image;
    uint32_t last;
};

static struct cloth cloths[CLOTHS];
static uint32_t cloth_clock;

static struct cloth *cloth_for(enum cg_cloth_kind kind, int w, int h)
{
    struct cloth *slot = &cloths[0];
    struct cg_cloth lamp;
    int i;

    for (i = 0; i < CLOTHS; i++) {
        if (cloths[i].px && cloths[i].kind == (int)kind && cloths[i].w == w && cloths[i].h == h) {
            cloths[i].last = ++cloth_clock;
            return &cloths[i];
        }
        if (!cloths[i].px || (slot->px && cloths[i].last < slot->last)) {
            slot = &cloths[i];
        }
    }
    free(slot->px);
    memset(slot, 0, sizeof(*slot));
    slot->px = malloc((size_t)w * h * 2);
    if (!slot->px) {
        return NULL;
    }
    lamp.lit = cloth_ink[kind][0];
    lamp.deep = cloth_ink[kind][1];
    lamp.cx = w / 2;
    lamp.cy = h * 45 / 100;
    lamp.rx = w * 88 / 100;
    lamp.ry = h * 78 / 100;
    lamp.grain = 3;
    cg_cloth_render(slot->px, w, h, w, &lamp);
    slot->kind = (int)kind;
    slot->w = w;
    slot->h = h;
    slot->last = ++cloth_clock;
    slot->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->image.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->image.header.w = (uint32_t)w;
    slot->image.header.h = (uint32_t)h;
    slot->image.header.stride = (uint32_t)w * 2;
    slot->image.data = (const uint8_t *)slot->px;
    slot->image.data_size = (uint32_t)w * h * 2;
    return slot;
}

int cg_table_cloth(lv_layer_t *layer, const lv_area_t *coords, enum cg_cloth_kind kind)
{
    int w = lv_area_get_width(coords);
    int h = lv_area_get_height(coords);
    lv_draw_image_dsc_t dsc;
    struct cloth *c;

    if (!layer || kind < 0 || kind >= CG_CLOTH_KINDS || w <= 0 || h <= 0) {
        return 0;
    }
    c = cloth_for(kind, w, h);
    if (!c) {
        return 0;
    }
    lv_draw_image_dsc_init(&dsc);
    dsc.src = &c->image;
    lv_draw_image(layer, &dsc, coords);
    return 1;
}

/* ---- chips ---------------------------------------------------------------------- */

static void ring(lv_layer_t *layer, int cx, int cy, int r, lv_color_t fill, lv_color_t line, int line_w, lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = fill;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_color = line;
    dsc.border_width = line_w;
    dsc.border_opa = opa;
    lv_draw_rect(layer, &dsc, &a);
}

static void chip(lv_layer_t *layer, int cx, int cy, int d, uint32_t body, uint32_t rim)
{
    int r = d / 2;

    /* Its shadow on the chip or cloth below, then the chip. */
    ring(layer, cx, cy + 2, r, lv_color_black(), lv_color_black(), 0, LV_OPA_TRANSP);
    ring(layer, cx, cy, r, lv_color_hex(body), lv_color_hex(rim), d >= 28 ? 3 : 2, LV_OPA_COVER);
    ring(layer, cx, cy, r * 58 / 100, lv_color_hex(body), lv_color_hex(rim), 1, LV_OPA_70);
}

int cg_table_chips(lv_layer_t *layer, int cx, int base_y, uint32_t amount, int d)
{
    static const struct {
        uint32_t value, body, rim;
    } kinds[] = { { 500, 0x6a3fa0, 0xe9dcf7 }, { 100, 0x1c1a18, 0xd9b66a }, { 50, 0x2e7d55, 0xeaf3ea },
                  { 10, 0x3f5f8f, 0xe6edf7 } };
    int count[4];
    int stacks = 0;
    int lift = d / 6 < 3 ? 3 : d / 6;
    int gap = d / 8;
    int width, x, i, k;

    if (!layer || d < 12 || amount == 0) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        count[i] = (int)(amount / kinds[i].value);
        amount -= (uint32_t)count[i] * kinds[i].value;
        /* A tall stack reads as "many": eight is enough to say it. */
        if (count[i] > 8) {
            count[i] = 8;
        }
        stacks += count[i] > 0;
    }
    if (stacks == 0) {
        return 0;
    }
    width = stacks * d + (stacks - 1) * gap;
    x = cx - width / 2 + d / 2;
    for (i = 0; i < 4; i++) {
        if (count[i] == 0) {
            continue;
        }
        for (k = 0; k < count[i]; k++) {
            chip(layer, x, base_y - d / 2 - k * lift, d, kinds[i].body, kinds[i].rim);
        }
        x += d + gap;
    }
    return width;
}

/* ---- the controls --------------------------------------------------------------- */

void cg_table_style_room(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_bg_color(obj, cg_table_ink(CG_INK_ROOM), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
}

void cg_table_style_text(lv_obj_t *label, enum cg_table_ink ink)
{
    if (label) {
        lv_obj_set_style_text_color(label, cg_table_ink(ink), 0);
    }
}

void cg_table_style_hud(lv_obj_t *panel, lv_obj_t *label, lv_obj_t *value)
{
    if (panel) {
        lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_opa(panel, LV_OPA_TRANSP, 0);
    }
    cg_table_style_text(label, CG_INK_DIM);
    cg_table_style_text(value, CG_INK_IVORY);
}

void cg_table_style_button(lv_obj_t *button, int primary, int enabled)
{
    lv_color_t ivory = cg_table_ink(CG_INK_IVORY);
    lv_obj_t *label;

    if (!button) {
        return;
    }
    label = lv_obj_get_child(button, 0);
    /* Over the DS roles in both states they style. */
    {
        static const lv_style_selector_t states[] = { 0, LV_STATE_DISABLED };
        size_t i;

        for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
            lv_style_selector_t sel = states[i];

            lv_obj_set_style_radius(button, 16, sel);
            lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, sel);
            lv_obj_set_style_bg_color(button, primary ? ivory : cg_table_ink(CG_INK_ROOM), sel);
            lv_obj_set_style_bg_opa(button, primary ? LV_OPA_COVER : LV_OPA_TRANSP, sel);
            lv_obj_set_style_border_color(button, ivory, sel);
            lv_obj_set_style_border_width(button, 2, sel);
            lv_obj_set_style_border_opa(button, primary ? LV_OPA_TRANSP : LV_OPA_70, sel);
            lv_obj_set_style_opa(button, enabled ? LV_OPA_COVER : LV_OPA_40, sel);
        }
    }
    cg_table_style_text(label, primary ? CG_INK_DARK : CG_INK_IVORY);
}
