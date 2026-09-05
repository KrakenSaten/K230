/*
 * PocketFleet rules and match state: fleet deployment, firing, sinking,
 * turn order and the win condition.
 *
 * This header owns the hidden state. The AI MUST NOT include it: it is
 * handed observations only (fleet_ai.h), which is what makes cheating
 * structurally impossible rather than merely discouraged.
 *
 * Standard Milton Bradley rules: ships are axis aligned, may touch but not
 * overlap, and each side fires exactly one shot per turn whatever the
 * result. The player fires first.
 *
 * Determinism: a match is fully reproduced by (seed, difficulty, the ordered
 * list of player actions). Two independent streams are derived from the
 * seed so that the opponent's decisions do not shift when the player uses
 * auto-deploy: rng_setup drives fleet placement, rng_ai drives the opponent.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_RULES_H
#define POCKETFLEET_RULES_H

#include "fleet_ai.h"
#include "fleet_rng.h"
#include "fleet_types.h"

struct fleet_ship_state {
    uint8_t row;
    uint8_t col;
    uint8_t orient;   /* enum fleet_orient */
    uint8_t hits;
    uint8_t placed;
};

/* One side's own waters: where that side's ships are and where the opponent
 * has fired. */
struct fleet_board {
    uint8_t ship_at[FLEET_CELLS];   /* ship index or FLEET_NO_SHIP */
    uint8_t shot[FLEET_CELLS];      /* 1 once fired at */
    struct fleet_ship_state ships[FLEET_SHIP_COUNT];
    uint8_t ships_placed;
    uint8_t ships_afloat;
};

struct fleet_stats {
    uint16_t shots;
    uint16_t hits;
};

struct fleet_game {
    uint32_t seed;
    struct fleet_rng rng_setup;
    struct fleet_rng rng_ai;
    uint8_t difficulty;             /* enum fleet_difficulty */
    uint8_t phase;                  /* enum fleet_phase */
    uint8_t winner;                 /* enum fleet_side, or FLEET_SIDE_NONE */
    uint16_t turn;                  /* 1-based round number, 0 before start */
    /* board[side] holds that side's own ships; the other side fires into it. */
    struct fleet_board board[FLEET_SIDE_COUNT];
    struct fleet_stats stats[FLEET_SIDE_COUNT];
    /* The opponent's own record of its shots. It is deliberately separate
     * from board[FLEET_SIDE_PLAYER]: the AI is handed this and never the
     * board, so it cannot see where the player's ships are. */
    struct fleet_ai ai;
};

/* ---- board ----------------------------------------------------------- */

void fleet_board_clear(struct fleet_board *board);
/* Would ship fit at (row, col) with this orientation? The ship's own
 * current cells are ignored, so a placed ship can be moved onto itself.
 * Returns 1 when legal, 0 otherwise. */
int fleet_board_can_place(const struct fleet_board *board, enum fleet_ship ship,
                          int row, int col, enum fleet_orient orient);
/* Place or move a ship. Transactional: on an illegal target the previous
 * placement is restored. Returns 0, or -1 when the target is illegal. */
int fleet_board_place(struct fleet_board *board, enum fleet_ship ship,
                      int row, int col, enum fleet_orient orient);
/* Returns 0, or -1 when the ship was not placed. */
int fleet_board_unplace(struct fleet_board *board, enum fleet_ship ship);
/* 1 when all FLEET_SHIP_COUNT ships are placed. */
int fleet_board_complete(const struct fleet_board *board);
/* Place every unplaced ship at random. Returns 0, or -1 if no legal layout
 * was found (cannot happen for this roster on a 10x10 board). */
int fleet_board_autoplace(struct fleet_board *board, struct fleet_rng *rng);
/* Fire at a cell. sunk_ship (may be NULL) receives the ship index on
 * FLEET_SHOT_SUNK and -1 otherwise. A repeated or off-grid shot returns
 * FLEET_SHOT_INVALID and changes nothing. */
enum fleet_shot_result fleet_board_fire(struct fleet_board *board, int row, int col,
                                        int *sunk_ship);
/* Cell n (0-based) of a placed ship, for hull outlines. Returns 0, or -1
 * when the ship is not placed or n is out of range. */
int fleet_board_ship_cell(const struct fleet_board *board, enum fleet_ship ship, int n,
                          int *row, int *col);
/* 1 when the ship has taken as many hits as it is long. */
int fleet_board_ship_sunk(const struct fleet_board *board, enum fleet_ship ship);

/* ---- match ----------------------------------------------------------- */

/* Start a new match: clear both boards, deploy the opponent fleet from the
 * seed, and enter FLEET_PHASE_DEPLOY with the player's waters empty. */
void fleet_game_new(struct fleet_game *game, uint32_t seed, enum fleet_difficulty difficulty);
/* Leave deployment. Returns 0, or -1 when the player fleet is incomplete or
 * the match is not in FLEET_PHASE_DEPLOY. */
int fleet_game_start(struct fleet_game *game);
/* Fire for one side. Rejects a shot out of turn, off grid or repeated with
 * FLEET_SHOT_INVALID and leaves the match untouched. On a legal shot the
 * statistics, phase, turn counter and winner are updated. */
enum fleet_shot_result fleet_game_fire(struct fleet_game *game, enum fleet_side shooter,
                                       int row, int col, int *sunk_ship);
/* Play the opponent's turn: ask the AI for a cell, fire it, and feed the
 * announced result back. This is the only place where the hidden state and
 * the AI are both visible, and nothing but (cell, result, sunk ship) crosses
 * between them. row, col and sunk_ship (all optional) report the shot.
 * Returns FLEET_SHOT_INVALID when it is not the opponent's turn. */
enum fleet_shot_result fleet_game_opponent_turn(struct fleet_game *game, int *row,
                                                int *col, int *sunk_ship);
int fleet_game_is_over(const struct fleet_game *game);
/* The board the given side fires into. */
struct fleet_board *fleet_game_target(struct fleet_game *game, enum fleet_side shooter);

#endif
