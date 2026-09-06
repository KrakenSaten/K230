/*
 * PocketTimber tower state. See timber_tower.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_tower.h"

#include <string.h>

static int valid_id(int id)
{
    return id >= 0 && id < TIMBER_BLOCKS;
}

/* The layer count follows the grid: the highest layer holding a block, plus
 * one. Kept as a field so the hot paths do not scan for it. */
static void settle_layers(struct timber_tower *t)
{
    int layer;
    int slot;

    for (layer = TIMBER_LAYERS_MAX - 1; layer >= 0; layer--) {
        for (slot = 0; slot < TIMBER_SLOTS; slot++) {
            if (t->grid[layer][slot] != TIMBER_NO_BLOCK) {
                t->layers = (uint8_t)(layer + 1);
                return;
            }
        }
    }
    t->layers = 0;
}

void timber_tower_build(struct timber_tower *t)
{
    int id;

    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
    memset(t->grid, TIMBER_NO_BLOCK, sizeof(t->grid));
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        struct timber_block *b = &t->blocks[id];

        b->present = 1;
        b->layer = (uint8_t)(id / TIMBER_SLOTS);
        b->slot = (uint8_t)(id % TIMBER_SLOTS);
        b->seat = TIMBER_SEAT_DEFAULT;
        b->mass = TIMBER_MASS_ONE;
        t->grid[b->layer][b->slot] = (uint8_t)id;
    }
    t->layers = TIMBER_LAYERS_BASE;
}

int timber_tower_layers(const struct timber_tower *t)
{
    return t ? t->layers : 0;
}

int timber_tower_layer_fill(const struct timber_tower *t, int layer)
{
    int slot;
    int n = 0;

    if (!t || layer < 0 || layer >= TIMBER_LAYERS_MAX) {
        return 0;
    }
    for (slot = 0; slot < TIMBER_SLOTS; slot++) {
        n += t->grid[layer][slot] != TIMBER_NO_BLOCK;
    }
    return n;
}

int timber_tower_top_complete(const struct timber_tower *t)
{
    int layer;

    if (!t) {
        return -1;
    }
    for (layer = t->layers - 1; layer >= 0; layer--) {
        if (timber_tower_layer_fill(t, layer) == TIMBER_SLOTS) {
            return layer;
        }
    }
    return -1;
}

int timber_tower_at(const struct timber_tower *t, int layer, int slot)
{
    uint8_t id;

    if (!t || layer < 0 || layer >= TIMBER_LAYERS_MAX || slot < 0 || slot >= TIMBER_SLOTS) {
        return -1;
    }
    id = t->grid[layer][slot];
    return id == TIMBER_NO_BLOCK ? -1 : (int)id;
}

const struct timber_block *timber_tower_block(const struct timber_tower *t, int id)
{
    if (!t || !valid_id(id)) {
        return NULL;
    }
    return &t->blocks[id];
}

int timber_tower_present_count(const struct timber_tower *t)
{
    int id;
    int n = 0;

    if (!t) {
        return 0;
    }
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        n += t->blocks[id].present != 0;
    }
    return n;
}

int timber_tower_pullable(const struct timber_tower *t, int id)
{
    const struct timber_block *b = timber_tower_block(t, id);
    int locked_from;

    if (!b || !b->present) {
        return 0;
    }
    locked_from = timber_tower_top_complete(t);
    if (locked_from < 0) {
        return 0;
    }
    return b->layer < locked_from;
}

int timber_tower_rect(const struct timber_tower *t, int id, struct timber_rect *out)
{
    const struct timber_block *b = timber_tower_block(t, id);

    if (!b || !b->present || !out) {
        return -1;
    }
    return timber_block_rect(b->layer, b->slot, b->extraction, out);
}

int timber_tower_gap(const struct timber_tower *t)
{
    int layer;

    if (!t) {
        return -1;
    }
    for (layer = 0; layer < t->layers - 1; layer++) {
        if (timber_tower_layer_fill(t, layer) == 0) {
            return layer;
        }
    }
    return -1;
}

/* ---- moving blocks --------------------------------------------------- */

int timber_tower_place_layer(const struct timber_tower *t)
{
    int top;

    if (!t) {
        return -1;
    }
    if (t->layers == 0) {
        return 0;
    }
    top = t->layers - 1;
    if (timber_tower_layer_fill(t, top) < TIMBER_SLOTS) {
        return top;
    }
    return top + 1 < TIMBER_LAYERS_MAX ? top + 1 : -1;
}

int timber_tower_can_place(const struct timber_tower *t, int slot)
{
    int layer = timber_tower_place_layer(t);

    if (layer < 0 || slot < 0 || slot >= TIMBER_SLOTS) {
        return 0;
    }
    return t->grid[layer][slot] == TIMBER_NO_BLOCK;
}

int timber_tower_remove(struct timber_tower *t, int id)
{
    struct timber_block *b;

    if (!t || !valid_id(id) || !t->blocks[id].present) {
        return -1;
    }
    b = &t->blocks[id];
    t->grid[b->layer][b->slot] = TIMBER_NO_BLOCK;
    b->present = 0;
    b->extraction = 0;
    settle_layers(t);
    return 0;
}

int timber_tower_place(struct timber_tower *t, int id, int slot)
{
    struct timber_block *b;
    int layer;

    if (!t || !valid_id(id) || t->blocks[id].present || !timber_tower_can_place(t, slot)) {
        return -1;
    }
    layer = timber_tower_place_layer(t);
    b = &t->blocks[id];
    b->present = 1;
    b->layer = (uint8_t)layer;
    b->slot = (uint8_t)slot;
    b->extraction = 0;
    if (b->moves < 0xFFu) {
        b->moves++;
    }
    t->grid[layer][slot] = (uint8_t)id;
    if (layer + 1 > t->layers) {
        t->layers = (uint8_t)(layer + 1);
    }
    return 0;
}

/* ---- validation ------------------------------------------------------ */

int timber_tower_validate(const struct timber_tower *t)
{
    struct timber_tower probe;
    int layer;
    int slot;
    int id;
    int extracted = 0;
    int locked_from;

    if (!t) {
        return TIMBER_INVALID_LAYERS;
    }
    if (t->layers > TIMBER_LAYERS_MAX) {
        return TIMBER_INVALID_LAYERS;
    }
    /* The layer count must be what the grid says it is. */
    probe = *t;
    settle_layers(&probe);
    if (probe.layers != t->layers) {
        return TIMBER_INVALID_LAYERS;
    }

    for (layer = 0; layer < TIMBER_LAYERS_MAX; layer++) {
        for (slot = 0; slot < TIMBER_SLOTS; slot++) {
            uint8_t cell = t->grid[layer][slot];

            if (cell == TIMBER_NO_BLOCK) {
                continue;
            }
            if (cell >= TIMBER_BLOCKS || !t->blocks[cell].present ||
                t->blocks[cell].layer != layer || t->blocks[cell].slot != slot) {
                return TIMBER_INVALID_GRID;
            }
        }
    }

    locked_from = timber_tower_top_complete(t);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_block *b = &t->blocks[id];

        if (b->mass == 0) {
            return TIMBER_INVALID_MASS;
        }
        if (!b->present) {
            if (b->extraction != 0) {
                return TIMBER_INVALID_EXTRACTION;
            }
            continue;
        }
        if (b->layer >= TIMBER_LAYERS_MAX || b->slot >= TIMBER_SLOTS) {
            return TIMBER_INVALID_SLOT;
        }
        if (t->grid[b->layer][b->slot] != id) {
            return TIMBER_INVALID_BLOCK;
        }
        if (b->extraction < -TIMBER_EXTRACTION_MAX || b->extraction > TIMBER_EXTRACTION_MAX) {
            return TIMBER_INVALID_EXTRACTION;
        }
        if (b->extraction != 0) {
            extracted++;
            if (locked_from < 0 || b->layer >= locked_from) {
                return TIMBER_INVALID_EXTRACTION;
            }
        }
    }
    if (extracted > 1) {
        return TIMBER_INVALID_EXTRACTION;
    }
    return TIMBER_VALID;
}
