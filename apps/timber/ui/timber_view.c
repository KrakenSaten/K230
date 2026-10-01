/*
 * PocketTimber view model. See timber_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_view.h"

#include <string.h>

void timber_view_init(struct timber_view *v, int width, int height, int motion)
{
    if (!v) {
        return;
    }
    memset(v, 0, sizeof(*v));
    v->width = width;
    v->height = height;
    v->scale = TIMBER_VIEW_SCALE;
    v->layer_px = TIMBER_VIEW_LAYER_PX;
    v->motion = (uint8_t)(motion != 0);
    timber_view_frame(v, TIMBER_LAYERS_BASE);
}

void timber_view_frame(struct timber_view *v, int layers)
{
    int top;

    if (!v) {
        return;
    }
    if (layers < 1) {
        layers = 1;
    }
    v->ox = v->width / 2;
    /* The near corner of the base sits 3 scale below the origin. */
    v->oy = v->height - TIMBER_VIEW_MARGIN - 3 * v->scale;
    top = v->oy - layers * v->layer_px;
    if (top < TIMBER_VIEW_MARGIN) {
        v->oy = TIMBER_VIEW_MARGIN + layers * v->layer_px;
    }
}

void timber_view_project(const struct timber_view *v, int32_t x, int32_t y, int32_t z, int *sx,
                         int *sy)
{
    if (!v || !sx || !sy) {
        return;
    }
    *sx = v->ox + (int)(((int64_t)(x - y) * v->scale) / 256);
    *sy = v->oy + (int)(((int64_t)(x + y) * v->scale) / 512) -
          (int)(((int64_t)z * v->layer_px) / TIMBER_BLOCK_HEIGHT);
}

/* Where a standing layer sits relative to its cell: the lean, and the sway
 * shared down the tower by the square of the height fraction. */
static void layer_offset(const struct timber_view *v, const struct timber_run *run, int layer,
                         int32_t *dx, int32_t *dy)
{
    int top = timber_tower_layers(&run->tower) - 1;

    *dx = (int32_t)(((int64_t)run->lean_x * layer) / 256);
    *dy = (int32_t)(((int64_t)run->lean_y * layer) / 256);
    if (v->motion && top > 0 && run->sway != 0) {
        int32_t s = (int32_t)(((int64_t)run->sway * layer * layer) / ((int64_t)top * top));

        if (run->disturb_axis == TIMBER_AXIS_X) {
            *dx += s;
        } else {
            *dy += s;
        }
    }
}

/* The three visible faces of a box from x0..x1, y0..y1, z0..z1, and the
 * corner a sprite is anchored by. */
static void box_shape(const struct timber_view *v, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      int32_t z0, int32_t z1, struct timber_shape *out)
{
    timber_view_project(v, x0, y0, z0, &out->origin_x, &out->origin_y);
    /* Top face, going round: far, right, near, left. */
    timber_view_project(v, x0, y0, z1, &out->top.x[0], &out->top.y[0]);
    timber_view_project(v, x1, y0, z1, &out->top.x[1], &out->top.y[1]);
    timber_view_project(v, x1, y1, z1, &out->top.x[2], &out->top.y[2]);
    timber_view_project(v, x0, y1, z1, &out->top.x[3], &out->top.y[3]);
    /* The +x face: top-far, top-near, bottom-near, bottom-far. */
    timber_view_project(v, x1, y0, z1, &out->right.x[0], &out->right.y[0]);
    timber_view_project(v, x1, y1, z1, &out->right.x[1], &out->right.y[1]);
    timber_view_project(v, x1, y1, z0, &out->right.x[2], &out->right.y[2]);
    timber_view_project(v, x1, y0, z0, &out->right.x[3], &out->right.y[3]);
    /* The +y face: top-near, top-far, bottom-far, bottom-near. */
    timber_view_project(v, x1, y1, z1, &out->left.x[0], &out->left.y[0]);
    timber_view_project(v, x0, y1, z1, &out->left.x[1], &out->left.y[1]);
    timber_view_project(v, x0, y1, z0, &out->left.x[2], &out->left.y[2]);
    timber_view_project(v, x1, y1, z0, &out->left.x[3], &out->left.y[3]);
}

/* Whether the choreography, not the cell, says where a block is. */
static const struct timber_fall *fall_of(const struct timber_run *run, int id)
{
    const struct timber_fall *f = timber_run_fall(run, id);

    if (f && (f->falling || f->rest_tick != TIMBER_REST_NEVER)) {
        return f;
    }
    return NULL;
}

int timber_view_block(const struct timber_view *v, const struct timber_run *run, int id,
                      struct timber_shape *out)
{
    const struct timber_block *b = run ? timber_tower_block(&run->tower, id) : NULL;
    const struct timber_fall *f;
    struct timber_rect r;
    int32_t z0;
    int32_t dx = 0;
    int32_t dy = 0;
    int along_x;

    if (!v || !b || !b->present || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    f = fall_of(run, id);
    if (f) {
        /* Loose: a box around the choreography's centre, in the pose's
         * orientation, tilted by its pose. */
        along_x = f->pose < TIMBER_POSES / 2;
        if (along_x) {
            r.x0 = f->x - TIMBER_BLOCK_LENGTH / 2;
            r.x1 = f->x + TIMBER_BLOCK_LENGTH / 2;
            r.y0 = f->y - TIMBER_UNIT / 2;
            r.y1 = f->y + TIMBER_UNIT / 2;
        } else {
            r.x0 = f->x - TIMBER_UNIT / 2;
            r.x1 = f->x + TIMBER_UNIT / 2;
            r.y0 = f->y - TIMBER_BLOCK_LENGTH / 2;
            r.y1 = f->y + TIMBER_BLOCK_LENGTH / 2;
        }
        z0 = f->z;
        out->tilt = f->pose % (TIMBER_POSES / 2);
    } else {
        if (timber_tower_rect(&run->tower, id, &r) != 0) {
            return -1;
        }
        along_x = timber_layer_axis(b->layer) == TIMBER_AXIS_X;
        layer_offset(v, run, b->layer, &dx, &dy);
        if (b->tell) {
            /* A tell is a visible misalignment along the block's axis,
             * rounded up so the projection lands on whole pixels. */
            int32_t nudge = (TIMBER_VIEW_TELL_PX * 256 + v->scale - 1) / v->scale;

            if (along_x) {
                dx += nudge;
            } else {
                dy += nudge;
            }
        }
        z0 = (int32_t)b->layer * TIMBER_BLOCK_HEIGHT;
    }
    box_shape(v, r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy, z0, z0 + TIMBER_BLOCK_HEIGHT, out);
    out->end_is_right = along_x;
    out->along_x = along_x;
    if (along_x) {
        timber_view_project(v, r.x1 + dx, (r.y0 + r.y1) / 2 + dy, z0 + TIMBER_BLOCK_HEIGHT / 2,
                            &out->end_x, &out->end_y);
    } else {
        timber_view_project(v, (r.x0 + r.x1) / 2 + dx, r.y1 + dy, z0 + TIMBER_BLOCK_HEIGHT / 2,
                            &out->end_x, &out->end_y);
    }
    return 0;
}

int timber_view_ghost(const struct timber_view *v, const struct timber_run *run, int slot,
                      struct timber_shape *out)
{
    struct timber_rect r;
    int layer;
    int32_t dx;
    int32_t dy;
    int32_t z0;

    if (!v || !run || !out || timber_run_held(run) < 0 || !timber_tower_can_place(&run->tower, slot)) {
        return -1;
    }
    layer = timber_tower_place_layer(&run->tower);
    if (layer < 0 || timber_block_rect(layer, slot, 0, &r) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    layer_offset(v, run, layer, &dx, &dy);
    z0 = (int32_t)layer * TIMBER_BLOCK_HEIGHT;
    box_shape(v, r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy, z0, z0 + TIMBER_BLOCK_HEIGHT, out);
    out->end_is_right = timber_layer_axis(layer) == TIMBER_AXIS_X;
    out->along_x = out->end_is_right;
    return 0;
}

/* Painter's depth: x + y, plus height in the same units, so a nearer or a
 * higher block is drawn later. */
static int32_t depth_of(const struct timber_run *run, int id)
{
    const struct timber_block *b = &run->tower.blocks[id];
    const struct timber_fall *f = fall_of(run, id);
    struct timber_rect r;
    int32_t cx;
    int32_t cy;
    int32_t z;

    if (f) {
        cx = f->x;
        cy = f->y;
        z = f->z;
    } else {
        timber_tower_rect(&run->tower, id, &r);
        timber_rect_centre(&r, &cx, &cy);
        z = (int32_t)b->layer * TIMBER_BLOCK_HEIGHT;
    }
    return cx + cy + (int32_t)(((int64_t)z * TIMBER_UNIT) / TIMBER_BLOCK_HEIGHT);
}

int timber_view_order(const struct timber_run *run, int *ids)
{
    int32_t keys[TIMBER_BLOCKS];
    int n = 0;
    int id;
    int i;

    if (!run || !ids) {
        return 0;
    }
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        int32_t key;
        int j;

        if (!run->tower.blocks[id].present) {
            continue;
        }
        key = depth_of(run, id);
        /* Insertion keeps the order stable, so two blocks at one depth are
         * drawn in id order on every platform. */
        for (i = n; i > 0 && keys[i - 1] > key; i--) {
            keys[i] = keys[i - 1];
            ids[i] = ids[i - 1];
        }
        j = i;
        keys[j] = key;
        ids[j] = id;
        n++;
    }
    return n;
}

int timber_view_pick(const struct timber_view *v, const struct timber_run *run, int sx, int sy)
{
    int best = -1;
    int32_t best_d2 = (int32_t)TIMBER_VIEW_PICK_PX * TIMBER_VIEW_PICK_PX;
    int id;

    if (!v || !run) {
        return -1;
    }
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        struct timber_shape s;
        int32_t dx;
        int32_t dy;
        int32_t d2;

        if (!timber_tower_pullable(&run->tower, id) || timber_view_block(v, run, id, &s) != 0) {
            continue;
        }
        dx = s.end_x - sx;
        dy = s.end_y - sy;
        d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = id;
        }
    }
    return best;
}

int timber_view_track_sign(int layer)
{
    return timber_layer_axis(layer) == TIMBER_AXIS_X ? 1 : -1;
}

int32_t timber_view_travel(const struct timber_view *v, int layer, int dx_px)
{
    if (!v || v->scale <= 0) {
        return 0;
    }
    return timber_view_track_sign(layer) * (int32_t)(((int64_t)dx_px * 256) / v->scale);
}

int timber_view_slot_side(int layer, int slot)
{
    if (slot < 0 || slot >= TIMBER_SLOTS) {
        return 0;
    }
    /* Across an x layer the slots run along y, which goes left on screen;
     * across a y layer they run along x, which goes right. */
    return timber_layer_axis(layer) == TIMBER_AXIS_X ? 1 - slot : slot - 1;
}

int timber_view_slot_for_side(int layer, int side)
{
    if (side < -1 || side > 1) {
        return -1;
    }
    return timber_layer_axis(layer) == TIMBER_AXIS_X ? 1 - side : side + 1;
}
