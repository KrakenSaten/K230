/*
 * PocketTimber stability model. See timber_stability.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_stability.h"

/* Q15 sine over a quarter turn in 64 steps, so a sixteen-bit phase needs
 * no arithmetic beyond a shift and a table look-up. Generated once; the
 * test checks the corners and the midpoint against their known values. */
static const int16_t quarter_sine[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

int32_t timber_stability_sin(uint16_t phase)
{
    unsigned index = phase >> 8;      /* 256 steps to the turn */
    unsigned quadrant = index >> 6;
    unsigned q = index & 63u;

    switch (quadrant) {
    case 0:
        return quarter_sine[q];
    case 1:
        return quarter_sine[64 - q];
    case 2:
        return -quarter_sine[q];
    default:
        return -quarter_sine[64 - q];
    }
}

/* ---- contacts ----------------------------------------------------------- */

/* The area over which a block meets the layer above it, and the bounding
 * box of that region. Returns the area; box is valid when it is not 0. */
static int64_t meeting(const struct timber_tower *t, const struct timber_block *b,
                       struct timber_rect *box)
{
    struct timber_rect mine;
    int64_t total = 0;
    int have = 0;
    int slot;

    if (timber_block_rect(b->layer, b->slot, b->extraction, &mine) != 0) {
        return 0;
    }
    for (slot = 0; slot < TIMBER_SLOTS; slot++) {
        int above = timber_tower_at(t, b->layer + 1, slot);
        struct timber_rect theirs;
        struct timber_rect cut;

        if (above < 0 || timber_tower_rect(t, above, &theirs) != 0) {
            continue;
        }
        if (!timber_rect_intersect(&mine, &theirs, &cut)) {
            continue;
        }
        total += timber_rect_area(&cut);
        if (!have) {
            *box = cut;
            have = 1;
        } else {
            if (cut.x0 < box->x0) {
                box->x0 = cut.x0;
            }
            if (cut.y0 < box->y0) {
                box->y0 = cut.y0;
            }
            if (cut.x1 > box->x1) {
                box->x1 = cut.x1;
            }
            if (cut.y1 > box->y1) {
                box->y1 = cut.y1;
            }
        }
    }
    return total;
}

int timber_stability_block_supports(const struct timber_tower *t, int id)
{
    const struct timber_block *b = timber_tower_block(t, id);
    struct timber_rect box;

    if (!b || !b->present || b->layer + 1 >= t->layers) {
        return 0;
    }
    return meeting(t, b, &box) >= TIMBER_SUPPORT_MIN;
}

int timber_stability_contact(const struct timber_tower *t, int layer, struct timber_contact *out)
{
    int slot;
    int have = 0;

    if (!t || !out || layer < 0 || layer + 1 >= t->layers) {
        return -1;
    }
    out->above = (uint8_t)timber_tower_layer_fill(t, layer + 1);
    if (out->above == 0) {
        return -1;
    }
    out->supported = 0;
    out->box.x0 = out->box.y0 = out->box.x1 = out->box.y1 = 0;
    for (slot = 0; slot < TIMBER_SLOTS; slot++) {
        int id = timber_tower_at(t, layer, slot);
        struct timber_rect box;

        if (id < 0 || meeting(t, &t->blocks[id], &box) < TIMBER_SUPPORT_MIN) {
            continue;
        }
        out->supported++;
        if (!have) {
            out->box = box;
            have = 1;
            continue;
        }
        if (box.x0 < out->box.x0) {
            out->box.x0 = box.x0;
        }
        if (box.y0 < out->box.y0) {
            out->box.y0 = box.y0;
        }
        if (box.x1 > out->box.x1) {
            out->box.x1 = box.x1;
        }
        if (box.y1 > out->box.y1) {
            out->box.y1 = box.y1;
        }
    }
    return 0;
}

/* ---- the stack above ---------------------------------------------------- */

int timber_stability_stack(const struct timber_tower *t, int layer, int32_t lean_x, int32_t lean_y,
                           struct timber_stack *out)
{
    int64_t mass = 0;
    int64_t mx = 0;
    int64_t my = 0;
    int n = 0;
    int id;

    if (!t || !out || layer < 0 || layer + 1 >= t->layers) {
        return -1;
    }
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = &t->blocks[id];
        struct timber_rect r;
        int32_t cx;
        int32_t cy;
        int32_t up;

        if (!b->present || b->layer <= layer) {
            continue;
        }
        if (timber_block_rect(b->layer, b->slot, b->extraction, &r) != 0) {
            continue;
        }
        timber_rect_centre(&r, &cx, &cy);
        /* The micro-offset is across the block's axis. */
        if (timber_layer_axis(b->layer) == TIMBER_AXIS_X) {
            cy += b->offset;
        } else {
            cx += b->offset;
        }
        /* The lean: a block up layers above this one sits lean * up over,
         * Q16.16 per layer down to Q8.8. */
        up = b->layer - layer;
        cx += (int32_t)(((int64_t)lean_x * up) / 256);
        cy += (int32_t)(((int64_t)lean_y * up) / 256);
        mass += b->mass;
        mx += (int64_t)b->mass * cx;
        my += (int64_t)b->mass * cy;
        n++;
    }
    if (mass == 0) {
        return -1;
    }
    out->mass = (int32_t)mass;
    out->cx = (int32_t)(mx / mass);
    out->cy = (int32_t)(my / mass);
    out->blocks = (uint8_t)n;
    return 0;
}

int32_t timber_stability_sway_share(int layer, int top)
{
    int64_t num = 0;
    int64_t den;
    int j;

    if (top <= 0 || layer < 0 || layer >= top) {
        return 0;
    }
    for (j = layer + 1; j <= top; j++) {
        num += (int64_t)j * j - (int64_t)layer * layer;
    }
    den = (int64_t)(top - layer) * top * top;
    return (int32_t)((num * 256) / den);
}

/* ---- margins ------------------------------------------------------------ */

/* The least of four distances, and which edge it is to. Ties keep the
 * first, so the answer is the same on every platform. */
static int32_t least(int32_t lo_x, int32_t hi_x, int32_t lo_y, int32_t hi_y, uint8_t *axis, int8_t *sign)
{
    int32_t m = lo_x;

    *axis = TIMBER_AXIS_X;
    *sign = -1;
    if (hi_x < m) {
        m = hi_x;
        *sign = 1;
    }
    if (lo_y < m) {
        m = lo_y;
        *axis = TIMBER_AXIS_Y;
        *sign = -1;
    }
    if (hi_y < m) {
        m = hi_y;
        *axis = TIMBER_AXIS_Y;
        *sign = 1;
    }
    return m;
}

int timber_stability_margin(const struct timber_tower *t, int layer, int32_t lean_x, int32_t lean_y,
                            int32_t sway_x, int32_t sway_y, struct timber_margin *out)
{
    struct timber_contact c;
    struct timber_stack s;
    int32_t lo_x;
    int32_t hi_x;
    int32_t lo_y;
    int32_t hi_y;
    int32_t share;
    int32_t sx;
    int32_t sy;
    uint8_t axis;
    int8_t sign;

    if (!t || !out) {
        return -1;
    }
    if (timber_stability_contact(t, layer, &c) != 0 ||
        timber_stability_stack(t, layer, lean_x, lean_y, &s) != 0) {
        return -1;
    }
    out->supported = c.supported;
    if (c.supported == 0) {
        out->stat = TIMBER_MARGIN_NONE;
        out->eff = TIMBER_MARGIN_NONE;
        out->axis = TIMBER_AXIS_X;
        out->sign = 0;
        return 0;
    }
    lo_x = s.cx - c.box.x0;
    hi_x = c.box.x1 - s.cx;
    lo_y = s.cy - c.box.y0;
    hi_y = c.box.y1 - s.cy;
    out->stat = least(lo_x, hi_x, lo_y, hi_y, &axis, &sign);

    /* The sway displaces the stack's centre of mass by its share of the
     * displacement at the top. */
    share = timber_stability_sway_share(layer, t->layers - 1);
    sx = (int32_t)(((int64_t)sway_x * share) / 256);
    sy = (int32_t)(((int64_t)sway_y * share) / 256);
    out->eff = least(lo_x + sx, hi_x - sx, lo_y + sy, hi_y - sy, &axis, &sign);
    out->axis = axis;
    out->sign = sign;
    return 0;
}

int timber_stability_hinge(const struct timber_tower *t, int32_t lean_x, int32_t lean_y,
                           int32_t sway_x, int32_t sway_y, struct timber_margin *out)
{
    struct timber_margin m;
    int hinge = -1;
    int layer;

    if (!t || !out) {
        return -1;
    }
    for (layer = 0; layer + 1 < t->layers; layer++) {
        if (timber_stability_margin(t, layer, lean_x, lean_y, sway_x, sway_y, &m) != 0) {
            continue;
        }
        if (hinge < 0 || m.eff < out->eff || (m.eff == out->eff && m.supported < out->supported)) {
            hinge = layer;
            *out = m;
        }
    }
    return hinge;
}
