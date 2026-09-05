/*
 * PocketFleet opponent AI. See fleet_ai.h for the no-cheat contract: this
 * file may not include fleet_rules.h and may not name the hidden types.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_ai.h"

#include <string.h>

static const int neighbour_row[4] = { -1, 1, 0, 0 };
static const int neighbour_col[4] = { 0, 0, -1, 1 };

void fleet_ai_init(struct fleet_ai *ai, enum fleet_difficulty difficulty)
{
    if (!ai) {
        return;
    }
    memset(ai, 0, sizeof(*ai));
    ai->difficulty = (uint8_t)((unsigned)difficulty < FLEET_DIFFICULTY_COUNT
                               ? difficulty : FLEET_OFFICER);
}

int fleet_ai_remaining(const struct fleet_ai *ai)
{
    int i;
    int n = 0;

    if (!ai) {
        return 0;
    }
    for (i = 0; i < FLEET_CELLS; i++) {
        n += !ai->shot[i];
    }
    return n;
}

/* Recruit fires blind: it records results but never consults them. */
static int uses_hunt(const struct fleet_ai *ai)
{
    return ai->difficulty != FLEET_RECRUIT;
}

static void queue_push(struct fleet_ai *ai, int row, int col)
{
    int idx = fleet_index(row, col);
    int i;

    if (idx < 0 || ai->shot[idx] || ai->queue_len >= FLEET_AI_QUEUE_MAX) {
        return;
    }
    for (i = 0; i < ai->queue_len; i++) {
        if (ai->queue[i] == (uint8_t)idx) {
            return;
        }
    }
    ai->queue[ai->queue_len++] = (uint8_t)idx;
}

/* Most recently queued candidate that has not been fired at yet, or -1.
 * Entries whose cell was fired at meanwhile are simply skipped; observe()
 * compacts the queue. */
static int queue_peek(const struct fleet_ai *ai)
{
    int i;

    for (i = ai->queue_len - 1; i >= 0; i--) {
        if (!ai->shot[ai->queue[i]]) {
            return ai->queue[i];
        }
    }
    return -1;
}

static void queue_compact(struct fleet_ai *ai)
{
    uint8_t kept = 0;
    int i;

    for (i = 0; i < ai->queue_len; i++) {
        if (!ai->shot[ai->queue[i]]) {
            ai->queue[kept++] = ai->queue[i];
        }
    }
    ai->queue_len = kept;
}

int fleet_ai_next_shot(const struct fleet_ai *ai, struct fleet_rng *rng,
                       int *row, int *col)
{
    int candidate;
    int remaining;
    int pick;
    int i;

    if (!ai || !rng) {
        return -1;
    }
    if (uses_hunt(ai)) {
        candidate = queue_peek(ai);
        if (candidate >= 0) {
            if (row) {
                *row = candidate / FLEET_GRID;
            }
            if (col) {
                *col = candidate % FLEET_GRID;
            }
            return 0;
        }
    }
    remaining = fleet_ai_remaining(ai);
    if (remaining == 0) {
        return -1;
    }
    pick = (int)fleet_rng_below(rng, (uint32_t)remaining);
    for (i = 0; i < FLEET_CELLS; i++) {
        if (ai->shot[i]) {
            continue;
        }
        if (pick == 0) {
            if (row) {
                *row = i / FLEET_GRID;
            }
            if (col) {
                *col = i % FLEET_GRID;
            }
            return 0;
        }
        pick--;
    }
    return -1;
}

void fleet_ai_observe(struct fleet_ai *ai, int row, int col,
                      enum fleet_shot_result result, int sunk_ship)
{
    int idx = fleet_index(row, col);
    int i;

    if (!ai || idx < 0 || ai->shot[idx] || result == FLEET_SHOT_INVALID) {
        return;
    }
    ai->shot[idx] = 1;
    ai->result[idx] = (uint8_t)result;
    ai->shots++;
    if (result == FLEET_SHOT_SUNK && sunk_ship >= 0 && sunk_ship < FLEET_SHIP_COUNT) {
        ai->sunk[sunk_ship] = 1;
    }
    if (!uses_hunt(ai)) {
        return;
    }
    if (result == FLEET_SHOT_HIT) {
        for (i = 0; i < 4; i++) {
            queue_push(ai, row + neighbour_row[i], col + neighbour_col[i]);
        }
    } else if (result == FLEET_SHOT_SUNK) {
        /* Officer resolves one ship at a time: sinking ends the hunt and it
         * returns to searching. Hits belonging to a second ship that happens
         * to sit alongside are forgotten, which is the deliberate weakness
         * that separates Officer from Commander. */
        ai->queue_len = 0;
    }
    queue_compact(ai);
}
