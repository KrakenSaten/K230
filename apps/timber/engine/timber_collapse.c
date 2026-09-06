/*
 * PocketTimber collapse choreography. See timber_collapse.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_collapse.h"

#include <string.h>

int32_t timber_collapse_floor(const struct timber_collapse *c, int32_t x, int32_t y)
{
    if (!c || !c->active) {
        return 0;
    }
    if (x >= 0 && x < TIMBER_BLOCK_LENGTH && y >= 0 && y < TIMBER_BLOCK_LENGTH) {
        return (int32_t)(c->hinge + 1) * TIMBER_BLOCK_HEIGHT;
    }
    return 0;
}

void timber_collapse_begin(struct timber_collapse *c, const struct timber_tower *t, int hinge,
                           int axis, int sign, struct timber_rng *rng)
{
    int id;

    if (!c) {
        return;
    }
    memset(c, 0, sizeof(*c));
    if (!t || hinge < 0 || hinge >= TIMBER_LAYERS_MAX) {
        return;
    }
    c->active = 1;
    c->hinge = (uint8_t)hinge;
    c->axis = axis == TIMBER_AXIS_Y ? TIMBER_AXIS_Y : TIMBER_AXIS_X;
    c->sign = (int8_t)(sign < 0 ? -1 : 1);

    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = &t->blocks[id];
        struct timber_fall *f = &c->blocks[id];
        struct timber_rect r;
        int up;
        int32_t along;
        int32_t across;

        f->rest_tick = TIMBER_REST_NEVER;
        if (!b->present || timber_block_rect(b->layer, b->slot, b->extraction, &r) != 0) {
            continue;
        }
        timber_rect_centre(&r, &f->x, &f->y);
        f->z = (int32_t)b->layer * TIMBER_BLOCK_HEIGHT;
        f->layer = b->layer;
        f->pose = (uint8_t)(timber_layer_axis(b->layer) == TIMBER_AXIS_X ? 0 : 3);
        if (b->layer <= hinge) {
            continue;
        }
        /* Above the hinge: it falls. Its fate is drawn now, four values in
         * a fixed order, and never again. */
        up = b->layer - hinge;
        along = c->sign * (TIMBER_BREAK_SPEED + TIMBER_BREAK_SPEED_PER_LAYER * up) +
                timber_rng_range(rng, -TIMBER_SCATTER, TIMBER_SCATTER);
        across = timber_rng_range(rng, -TIMBER_SCATTER, TIMBER_SCATTER);
        f->tumble = (uint8_t)timber_rng_range(rng, TIMBER_TUMBLE_MIN, TIMBER_TUMBLE_MAX);
        f->rest_pose = (uint8_t)timber_rng_below(rng, TIMBER_POSES);
        if (c->axis == TIMBER_AXIS_X) {
            f->vx = (int16_t)along;
            f->vy = (int16_t)across;
        } else {
            f->vx = (int16_t)across;
            f->vy = (int16_t)along;
        }
        f->vz = 0;
        f->falling = 1;
        c->falling++;
        c->fell++;
    }
}

static void rest(struct timber_collapse *c, struct timber_fall *f, int32_t floor)
{
    f->z = floor;
    f->vx = 0;
    f->vy = 0;
    f->vz = 0;
    f->falling = 0;
    f->pose = f->rest_pose;
    f->rest_tick = c->ticks;
    c->falling--;
}

int timber_collapse_tick(struct timber_collapse *c)
{
    int id;
    int landed = 0;

    if (!c || !c->active || c->falling == 0) {
        return 0;
    }
    c->ticks++;

    for (id = 0; id < TIMBER_BLOCKS; id++) {
        struct timber_fall *f = &c->blocks[id];
        int32_t floor;

        if (!f->falling) {
            continue;
        }
        if (c->ticks <= TIMBER_TIP_TICKS) {
            /* The tip: the stack leans as one, each layer a little further
             * than the one below, and settles a touch. */
            int up = f->layer - c->hinge;

            if (c->axis == TIMBER_AXIS_X) {
                f->x += c->sign * TIMBER_TIP_SHEAR * up;
            } else {
                f->y += c->sign * TIMBER_TIP_SHEAR * up;
            }
            f->z -= TIMBER_TIP_DROP;
            continue;
        }

        /* The break: every block on its own. */
        f->x += f->vx;
        f->y += f->vy;
        f->z += f->vz;
        f->vz = (int16_t)(f->vz - TIMBER_GRAVITY);
        if (f->tumble && c->ticks % f->tumble == 0) {
            f->pose = (uint8_t)((f->pose + 1) % TIMBER_POSES);
        }
        floor = timber_collapse_floor(c, f->x, f->y);
        if (f->z > floor) {
            continue;
        }
        if (!f->bounced && f->vz < -TIMBER_REST_SPEED) {
            f->z = floor;
            f->vz = (int16_t)(-f->vz * TIMBER_BOUNCE_PCT / 100);
            f->vx = (int16_t)(f->vx * TIMBER_GROUND_SPEED_PCT / 100);
            f->vy = (int16_t)(f->vy * TIMBER_GROUND_SPEED_PCT / 100);
            f->bounced = 1;
            continue;
        }
        rest(c, f, floor);
        landed++;
    }

    /* The ceiling: whatever is still moving is put down where it is. */
    if (c->falling > 0 && c->ticks >= TIMBER_COLLAPSE_TICKS_MAX) {
        for (id = 0; id < TIMBER_BLOCKS; id++) {
            struct timber_fall *f = &c->blocks[id];

            if (f->falling) {
                rest(c, f, timber_collapse_floor(c, f->x, f->y));
                landed++;
            }
        }
    }
    return landed;
}

int timber_collapse_done(const struct timber_collapse *c)
{
    return !c || !c->active || c->falling == 0;
}

const struct timber_fall *timber_collapse_block(const struct timber_collapse *c, int id)
{
    if (!c || !c->active || id < 0 || id >= TIMBER_BLOCKS) {
        return NULL;
    }
    return &c->blocks[id];
}
