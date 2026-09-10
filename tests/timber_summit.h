/*
 * A run one block short of the summit, for tests.
 *
 * Every block is present except one in hand; the tower is TIMBER_LAYERS_MAX
 * layers tall with two of the top layer's three slots filled, every lower
 * layer thinned to its centre or to a side and its centre, no lean and no
 * micro-offsets, so it stands with margin to spare. It is built by hand on
 * the grid because the tower's own placement primitive completes a layer
 * before it starts the next, and because with the approved shift lean no
 * modelled player reaches the summit from a seed, which is the point.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_TEST_SUMMIT_H
#define POCKETTIMBER_TEST_SUMMIT_H

#include "timber_rules.h"

#include <string.h>

#define SUMMIT_SIDE_LAYERS 16

static void summit_put(struct timber_run *run, int id, int layer, int slot)
{
    struct timber_block *b = &run->tower.blocks[id];

    b->present = 1;
    b->layer = (uint8_t)layer;
    b->slot = (uint8_t)slot;
    b->extraction = 0;
    b->offset = 0;
    b->seat = TIMBER_SEAT_PLACED_MIN;
    run->tower.grid[layer][slot] = (uint8_t)id;
}

/* Build the run and return the id of the block in hand. */
static int summit_build(struct timber_run *run, uint32_t seed)
{
    int id = 0;
    int layer;

    timber_run_new(run, seed);
    timber_run_start(run);
    memset(run->tower.grid, TIMBER_NO_BLOCK, sizeof(run->tower.grid));
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        run->tower.blocks[id].present = 0;
        run->tower.blocks[id].extraction = 0;
    }
    id = 0;
    for (layer = 0; layer < TIMBER_LAYERS_MAX - 1; layer++) {
        summit_put(run, id++, layer, 1);
        if (layer < SUMMIT_SIDE_LAYERS) {
            summit_put(run, id++, layer, 0);
        }
    }
    summit_put(run, id++, TIMBER_LAYERS_MAX - 1, 0);
    summit_put(run, id++, TIMBER_LAYERS_MAX - 1, 2);
    run->tower.layers = TIMBER_LAYERS_MAX;
    /* The last block is in hand, as it would be after a slip. */
    run->held = (uint8_t)id;
    run->selected = (uint8_t)id;
    run->turn = TIMBER_TURN_PLACING;
    /* One tick measures the tower, as a live run would have. */
    timber_run_tick(run);
    timber_run_clear_events(run);
    return id;
}

#endif
