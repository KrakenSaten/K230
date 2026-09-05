/*
 * PocketFleet opponent AI.
 *
 * NO-CHEAT CONTRACT. This header and fleet_ai.c MUST NOT include
 * fleet_rules.h, and MUST NOT name struct fleet_board or struct fleet_game.
 * The AI is handed nothing but its own shots and the results that were
 * announced for them, exactly what a human opponent across the table would
 * know. Because the hidden types are not even visible here, reading the
 * player's layout is impossible rather than merely discouraged.
 * tests/fleet_lint.sh enforces this.
 *
 * Choosing a shot is a pure function of (state, rng): fleet_ai_next_shot()
 * takes the state as const and every decision draws from the supplied
 * generator, so a match replays identically from its seed.
 *
 * Difficulty levels:
 *   Recruit   uniform random search; results are recorded but never used.
 *   Officer   random search plus hunt/target: a hit queues its orthogonal
 *             neighbours, and the queue is drained before searching again.
 *   Commander implemented in a later step; plays as Officer until then.
 *   Admiral   implemented in a later step; plays as Officer until then.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_AI_H
#define POCKETFLEET_AI_H

#include "fleet_rng.h"
#include "fleet_types.h"

#define FLEET_AI_QUEUE_MAX FLEET_CELLS

struct fleet_ai {
    uint8_t difficulty;                /* enum fleet_difficulty */
    uint16_t shots;                    /* shots taken */
    uint8_t shot[FLEET_CELLS];         /* 1 once fired at */
    uint8_t result[FLEET_CELLS];       /* enum fleet_shot_result, own shots only */
    uint8_t sunk[FLEET_SHIP_COUNT];    /* 1 when that ship has been announced sunk */
    uint8_t queue[FLEET_AI_QUEUE_MAX]; /* candidate cells around unresolved hits */
    uint8_t queue_len;
};

void fleet_ai_init(struct fleet_ai *ai, enum fleet_difficulty difficulty);
/* Choose the next cell to fire at. Returns 0 and fills row/col, or -1 when
 * every cell has already been fired at. Does not change the AI state. */
int fleet_ai_next_shot(const struct fleet_ai *ai, struct fleet_rng *rng,
                       int *row, int *col);
/* Record the announced result of a shot. sunk_ship is the ship index on
 * FLEET_SHOT_SUNK and is ignored otherwise. Invalid or repeated cells are
 * ignored. */
void fleet_ai_observe(struct fleet_ai *ai, int row, int col,
                      enum fleet_shot_result result, int sunk_ship);
/* Number of cells not yet fired at. */
int fleet_ai_remaining(const struct fleet_ai *ai);

#endif
