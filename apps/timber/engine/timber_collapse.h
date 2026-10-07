/*
 * PocketTimber collapse choreography: what every block does from the tick
 * the tower goes to the tick the last one rests.
 *
 * This is not a physics engine either. The stump, every layer up to and
 * including the hinge, stays where it is. Every block above the hinge
 * first shears with the stack (the tip: each layer slides a little further
 * than the one below it along the axis the hinge gave way on, and the whole
 * stack drops a touch), then breaks into blocks that each fly on their own:
 * a horizontal speed along the failing direction that grows with height
 * above the hinge, a scatter across it, gravity, and a tumble through the
 * poses the view can draw. A block lands on the felt, or on the stump's top
 * if it is still over the footprint; it bounces once, softly, and rests in
 * a pose drawn for it. Blocks pass through one another; the pile is a
 * matter of drawing order.
 *
 * Everything a block does is decided when the collapse begins: the scatter,
 * the tumble rate and the rest pose are drawn from the generator then, one
 * fixed set per falling block in id order, so a collapse is a function of
 * the tower's state and the generator's state and nothing else, and the
 * same state falls the same way twice. The tower itself is frozen at the
 * moment it went; the choreography is an overlay the view reads.
 *
 * Positions are Q8.8 widths on x and y and Q8.8 on z with one layer
 * TIMBER_BLOCK_HEIGHT tall; z is the block's underside, so a block in
 * layer j starts at z = j * TIMBER_BLOCK_HEIGHT and rests at z = 0 on the
 * felt. Pure C, no LVGL, no I/O.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETTIMBER_COLLAPSE_H
#define POCKETTIMBER_COLLAPSE_H

#include "timber_rng.h"
#include "timber_tower.h"

/* Poses the view can draw a loose block in: the two orientations flat,
 * each with two tilts. Which is which is the view's business. */
#define TIMBER_POSES 6

/* The tip: how far each layer above the hinge slides per tick, per layer
 * of height above it, and how far the stack drops per tick. Q8.8. */
#define TIMBER_TIP_SHEAR 3
#define TIMBER_TIP_DROP 2
/* The break: speed along the failing direction for the layer just above
 * the hinge, plus this much per layer of height, Q8.8 per tick. */
#define TIMBER_BREAK_SPEED 24
#define TIMBER_BREAK_SPEED_PER_LAYER 2
/* Scatter either way on both axes, Q8.8 per tick. */
#define TIMBER_SCATTER 25
/* A landing keeps this much of its downward speed, upward, once; and this
 * much of its horizontal speed. */
#define TIMBER_BOUNCE_PCT 20
#define TIMBER_GROUND_SPEED_PCT 50
/* Below this downward speed a landing is a rest, not a bounce. */
#define TIMBER_REST_SPEED 12
/* Ticks between pose steps while tumbling. */
#define TIMBER_TUMBLE_MIN 2
#define TIMBER_TUMBLE_MAX 4
/* Draws from the generator per falling block, in this order: scatter
 * along, scatter across, tumble rate, rest pose. */
#define TIMBER_COLLAPSE_DRAWS 4

#define TIMBER_REST_NEVER 0xFFFFu

struct timber_fall {
    int32_t x;              /* centre, Q8.8 widths */
    int32_t y;
    int32_t z;              /* underside, Q8.8 layers of TIMBER_BLOCK_HEIGHT */
    int16_t vx;             /* per tick */
    int16_t vy;
    int16_t vz;
    uint8_t layer;          /* the layer it fell from, for the tip and for drawing order */
    uint8_t falling;        /* 1 while in motion */
    uint8_t bounced;
    uint8_t pose;           /* 0 .. TIMBER_POSES-1 */
    uint8_t tumble;         /* ticks per pose step */
    uint8_t rest_pose;
    uint16_t rest_tick;     /* the tick it came to rest, or TIMBER_REST_NEVER */
};

struct timber_collapse {
    uint8_t active;
    uint8_t hinge;
    uint8_t axis;           /* enum timber_axis it gave way on */
    int8_t sign;
    uint8_t falling;        /* blocks still in motion */
    uint8_t fell;           /* blocks that were above the hinge */
    uint16_t ticks;
    struct timber_fall blocks[TIMBER_BLOCKS];
};

/* Begin: every present block above the hinge starts falling from its cell;
 * the stump and any block in hand stay put. Draws
 * TIMBER_COLLAPSE_DRAWS values per falling block from rng in id order. */
void timber_collapse_begin(struct timber_collapse *c, const struct timber_tower *t, int hinge,
                           int axis, int sign, struct timber_rng *rng);
/* One tick of TIMBER_TICK_MS. Returns how many blocks came to rest this
 * tick. At TIMBER_COLLAPSE_TICKS_MAX everything still moving is put down
 * where it is, so a collapse always ends. */
int timber_collapse_tick(struct timber_collapse *c);
/* 1 once nothing is moving any more (or nothing ever was). */
int timber_collapse_done(const struct timber_collapse *c);
/* Where a block is, or NULL for an id outside the run or no collapse. A
 * block that never fell reports its cell. */
const struct timber_fall *timber_collapse_block(const struct timber_collapse *c, int id);
/* The height a block would land on at this position: the stump's top over
 * the footprint, the felt elsewhere. */
int32_t timber_collapse_floor(const struct timber_collapse *c, int32_t x, int32_t y);

#endif
