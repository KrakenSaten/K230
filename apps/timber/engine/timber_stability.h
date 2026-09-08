/*
 * PocketTimber stability model: what holds the tower up, and by how much.
 *
 * This is a stability model, not a physics engine. For every layer k with
 * something above it:
 *
 *   contact   the rectangles where the blocks of layer k that still carry
 *             load meet the blocks of layer k+1, and their bounding box. A
 *             block carries load while at least TIMBER_SUPPORT_MIN_PCT of
 *             its footprint meets the layer above, so a block three
 *             quarters of the way out has already let go of the stack.
 *   stack     the mass and centre of mass of every block above k, at its
 *             extracted position, with its micro-offset, and displaced by
 *             the lean: a block j layers up sits lean * j further over.
 *   margin    the smallest distance from that centre of mass to an edge of
 *             the contact box, on either axis. A complete layer under a
 *             centred stack has 1.5 widths of it; a layer with only its
 *             centre block has 0.5 both ways; a layer with one side block
 *             has none, and the stack tips.
 *
 * The effective margin also carries the sway: the disturbance the player
 * has put into the tower, rendered as a displacement of the top, shared
 * down the tower by the mean of a cantilever mode shape (the stack above a
 * low layer sways further than the stack above a high one). The hinge is
 * the layer with the least effective margin; when it goes below zero the
 * tower falls there (timber_rules.c decides when to look).
 *
 * The bounding box overstates the support only in the corner case of two
 * diagonally opposite contacts, which a played tower does not produce.
 *
 * Integers throughout, Q8.8 widths, 64-bit sums. Pure C, no LVGL, no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_STABILITY_H
#define POCKETTIMBER_STABILITY_H

#include "timber_tower.h"

#define TIMBER_NO_LAYER 0xFFu

/* A block still carries load while this much of its footprint meets the
 * layer above. A quarter: a block let go of the stack at 75 % out, and
 * slips free at 80 % (TIMBER_SLIP_AT), so the shift is a moment of its
 * own, just before the block is out. */
#define TIMBER_SUPPORT_MIN_PCT 25
#define TIMBER_SUPPORT_MIN \
    ((int64_t)TIMBER_BLOCK_LENGTH * TIMBER_UNIT * TIMBER_SUPPORT_MIN_PCT / 100)

/* The margin a complete layer gives a centred stack: half the footprint,
 * and the value the stability meter reads as full. */
#define TIMBER_MARGIN_FULL (3 * TIMBER_UNIT / 2)
/* The margin of a layer whose stack has nothing to stand on. */
#define TIMBER_MARGIN_NONE (-(1 << 20))
/* Below this static margin the tower creaks: a quarter of a width. */
#define TIMBER_CREAK_MARGIN (TIMBER_UNIT / 4)

/* What the stack does when a block lets go of it: it settles onto the
 * blocks that are left, with a disturbance that grows with the load the
 * block was carrying, from 0.25 to 1.0. */
#define TIMBER_SHIFT_IMPULSE_MIN 64
#define TIMBER_SHIFT_IMPULSE_MAX 256
/* A shift also leaves the tower leaning a hair: the stack follows the
 * block along its axis as it lets go, and settles across it toward the
 * neighbour that happens to be lower, which the block's micro-offset
 * stands in for. Q16.16 widths per layer per full-load shift, scaled by
 * the shift's impulse. This is the ramp: without it a careful player is
 * never brought down (docs/apps/POCKETTIMBER.md, D3, approved by the
 * product owner 2026-09-08 as a deterministic pacing mechanism). Both are
 * tuning constants: these values are the simulator's, not hardware-tuned,
 * and 0 restores the reviewed model. Everything here is derived from the
 * seed and the player's actions; nothing draws from the generator. */
#define TIMBER_SHIFT_LEAN 393           /* 0.006, along the pull */
#define TIMBER_SHIFT_LEAN_ACROSS 262    /* 0.004, toward the lower neighbour */

struct timber_contact {
    struct timber_rect box;     /* bounding box of the contact region */
    uint8_t supported;          /* blocks of the layer that carry load */
    uint8_t above;              /* blocks in the layer above */
};

struct timber_stack {
    int32_t mass;               /* Q8.8 */
    int32_t cx;                 /* Q8.8, in the layer's own frame */
    int32_t cy;
    uint8_t blocks;
};

struct timber_margin {
    int32_t stat;               /* without the sway, Q8.8 */
    int32_t eff;                /* with the sway */
    uint8_t axis;               /* enum timber_axis of the effective minimum */
    int8_t sign;                /* +1 toward the high edge, -1 toward the low */
    uint8_t supported;          /* blocks of the layer that carry the stack */
};

/* 1 when the block is in the tower and still carries load from the layer
 * above it. A block with nothing above it carries nothing. */
int timber_stability_block_supports(const struct timber_tower *t, int id);
/* The contact between a layer and the one above. Returns 0, or -1 when
 * the layer has nothing directly above it, in which case it carries
 * nothing and has no margin to speak of. */
int timber_stability_contact(const struct timber_tower *t, int layer, struct timber_contact *out);
/* The stack above a layer, leaned by lean_x and lean_y (Q16.16 widths per
 * layer). Returns 0, or -1 when nothing is above. */
int timber_stability_stack(const struct timber_tower *t, int layer, int32_t lean_x, int32_t lean_y,
                           struct timber_stack *out);
/* How much of a sway at the top the stack above a layer feels, Q8.8: the
 * mean of the mode shape (h / top)^2 over the layers above, less the
 * layer's own. top is the highest layer. 0 when nothing is above. */
int32_t timber_stability_sway_share(int layer, int top);
/* The margin of one layer. sway_x and sway_y are the displacement of the
 * top of the tower, Q8.8. Returns 0, or -1 when nothing is above. */
int timber_stability_margin(const struct timber_tower *t, int layer, int32_t lean_x, int32_t lean_y,
                            int32_t sway_x, int32_t sway_y, struct timber_margin *out);
/* The layer with the least effective margin, its margin in out. A thinned
 * layer and the layer under it share a margin, so a tie goes to the layer
 * with fewer blocks carrying the stack, and then to the lower one. Returns
 * the layer, or -1 when no layer carries anything. */
int timber_stability_hinge(const struct timber_tower *t, int32_t lean_x, int32_t lean_y,
                           int32_t sway_x, int32_t sway_y, struct timber_margin *out);
/* Sine of a phase in 1/65536 turn, Q15, from a quarter-wave table. */
int32_t timber_stability_sin(uint16_t phase);

#endif
