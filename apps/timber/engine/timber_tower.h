/*
 * PocketTimber tower state: the blocks, where each one is, and the
 * structural rules that say which of them may be touched.
 *
 * The tower is a grid of layers and slots holding block ids, and an array
 * of blocks each of which knows its own cell. The two are kept consistent
 * by the functions here and checked by timber_tower_validate(), which is
 * how the tests state "no impossible tower state" as an executable rule.
 *
 * The one structural rule of the game lives here: a block may be pulled
 * only from below the highest completed layer. That layer, and any
 * incomplete layer above it, is locked. Everything below is fair game
 * including the bottom layer; what makes a low block dangerous is the
 * stability model (timber_stability.c), not a rule.
 *
 * Nothing here decides whether the tower stands. The tower is geometry and
 * bookkeeping; the run (timber_rules.c) asks it questions and tells it what
 * changed.
 *
 * Pure C, no LVGL and no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_TOWER_H
#define POCKETTIMBER_TOWER_H

#include "timber_types.h"

/* The seat a block carries before the seed has spoken: the middle of the
 * range. Seeded generation overwrites it. */
#define TIMBER_SEAT_DEFAULT 128

struct timber_block {
    uint8_t present;        /* 1 while it is part of the tower, seated or part way out */
    uint8_t layer;
    uint8_t slot;
    /* Hidden looseness, 0 wedged tight to 255 free. Fixed at generation
     * and at each placement; the class the player is shown is derived from
     * it and from the load above (timber_pull.c). */
    uint8_t seat;
    uint8_t tell;           /* 1 when the block shows a visible misalignment */
    uint8_t tested;         /* 1 once a TEST has revealed its class this run */
    uint8_t variant;        /* wood grain, for the view only */
    uint8_t moves;          /* times it has been pulled and placed */
    int16_t extraction;     /* Q8.8 along its own axis, signed; 0 seated */
    int16_t offset;         /* Q8.8 micro-offset across its axis, small, signed */
    uint16_t mass;          /* Q8.8, TIMBER_MASS_ONE in v0.1 */
};

struct timber_tower {
    struct timber_block blocks[TIMBER_BLOCKS];
    uint8_t grid[TIMBER_LAYERS_MAX][TIMBER_SLOTS];  /* block id or TIMBER_NO_BLOCK */
    uint8_t layers;         /* highest layer holding a block, plus one */
};

/* Validation results. 0 is a tower the rules could have produced. */
enum timber_invalid {
    TIMBER_VALID = 0,
    TIMBER_INVALID_LAYERS = -1,      /* the layer count disagrees with the grid */
    TIMBER_INVALID_GRID = -2,        /* a cell names a block that is not there */
    TIMBER_INVALID_BLOCK = -3,       /* a block names a cell that does not hold it */
    TIMBER_INVALID_SLOT = -4,        /* a block's layer or slot is outside the tower */
    TIMBER_INVALID_EXTRACTION = -5,  /* out of range, two at once, or in a locked layer */
    TIMBER_INVALID_MASS = -6
};

/* The canonical tower: TIMBER_LAYERS_BASE complete layers, every block
 * seated with TIMBER_SEAT_DEFAULT, no tells, no offsets. Block id is
 * layer * TIMBER_SLOTS + slot. */
void timber_tower_build(struct timber_tower *t);

int timber_tower_layers(const struct timber_tower *t);
/* Blocks present in a layer, 0 for a layer outside the tower. */
int timber_tower_layer_fill(const struct timber_tower *t, int layer);
/* The highest layer with every slot filled, or -1. */
int timber_tower_top_complete(const struct timber_tower *t);
/* The block in a cell, or -1 when it is empty or outside the tower. */
int timber_tower_at(const struct timber_tower *t, int layer, int slot);
/* A block by id, present or not. NULL for an id outside the run. */
const struct timber_block *timber_tower_block(const struct timber_tower *t, int id);
int timber_tower_present_count(const struct timber_tower *t);
/* 1 when the block is in the tower below the highest completed layer. */
int timber_tower_pullable(const struct timber_tower *t, int id);
/* A block's footprint at its current extraction. Returns 0, or -1 when the
 * block is not in the tower. */
int timber_tower_rect(const struct timber_tower *t, int id, struct timber_rect *out);
/* The first layer strictly below the top that holds no block, or -1. A
 * played tower never has one: the layer above would have nothing to stand
 * on, and the stability model collapses it before the last block is out. */
int timber_tower_gap(const struct timber_tower *t);

/* ---- moving blocks --------------------------------------------------- */

/* The layer a block in hand goes to: the top layer while it is incomplete,
 * else the one above it. -1 when the tower is at TIMBER_LAYERS_MAX. */
int timber_tower_place_layer(const struct timber_tower *t);
int timber_tower_can_place(const struct timber_tower *t, int slot);
/* Take a present block out of the grid (into the player's hand). Its
 * extraction is cleared. Returns 0, or -1 for an absent block. The layer
 * count follows the grid. Whether the block was allowed to be pulled is the
 * run's decision, not the tower's. */
int timber_tower_remove(struct timber_tower *t, int id);
/* Put a block in hand into the placement layer at slot. Returns 0, or -1
 * when the block is present, the slot is taken or the tower is full. */
int timber_tower_place(struct timber_tower *t, int id, int slot);

/* TIMBER_VALID, or the first invariant found broken. */
int timber_tower_validate(const struct timber_tower *t);

#endif
