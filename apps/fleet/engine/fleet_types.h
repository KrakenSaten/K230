/*
 * PocketFleet rules vocabulary: board geometry, the fleet roster, shot
 * results, phases and difficulty levels.
 *
 * This header holds only what every part of the game may legitimately know.
 * The hidden state (struct fleet_board, struct fleet_game) lives in
 * fleet_rules.h, which the AI must never include: that is what makes it
 * structurally impossible for the AI to read the player's layout.
 *
 * Pure C, no LVGL, so the whole engine is unit-tested natively.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_TYPES_H
#define POCKETFLEET_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define FLEET_GRID 10
#define FLEET_CELLS (FLEET_GRID * FLEET_GRID)
#define FLEET_SHIP_COUNT 5
#define FLEET_HULL_CELLS 17    /* 5 + 4 + 3 + 3 + 2 */
#define FLEET_NO_SHIP 0xFF     /* empty cell in ship_at[] */
#define FLEET_SIDE_NONE 0xFF   /* no winner yet */
/* "D7" plus terminator; the longest cell name is "J10". */
#define FLEET_CELL_NAME_MAX 5

enum fleet_ship {
    FLEET_SHIP_CARRIER = 0,    /* 5 */
    FLEET_SHIP_BATTLESHIP,     /* 4 */
    FLEET_SHIP_CRUISER,        /* 3 */
    FLEET_SHIP_SUBMARINE,      /* 3 */
    FLEET_SHIP_DESTROYER       /* 2 */
};

enum fleet_orient {
    FLEET_HORIZONTAL = 0,      /* extends along increasing column */
    FLEET_VERTICAL = 1         /* extends along increasing row */
};

enum fleet_shot_result {
    FLEET_SHOT_INVALID = 0,    /* off grid, already fired at, or wrong phase */
    FLEET_SHOT_MISS,
    FLEET_SHOT_HIT,
    FLEET_SHOT_SUNK            /* a hit that completed a ship */
};

enum fleet_phase {
    FLEET_PHASE_DEPLOY = 0,
    FLEET_PHASE_PLAYER,
    FLEET_PHASE_OPPONENT,
    FLEET_PHASE_OVER
};

enum fleet_difficulty {
    FLEET_RECRUIT = 0,
    FLEET_OFFICER,
    FLEET_COMMANDER,
    FLEET_ADMIRAL,
    FLEET_DIFFICULTY_COUNT
};

enum fleet_side {
    FLEET_SIDE_PLAYER = 0,
    FLEET_SIDE_OPPONENT,
    FLEET_SIDE_COUNT
};

/* ---- roster ---------------------------------------------------------- */

/* Hull length in cells, or 0 for an out-of-range ship. */
uint8_t fleet_ship_length(enum fleet_ship ship);
/* Display name ("Carrier"), or "?" for an out-of-range ship. */
const char *fleet_ship_name(enum fleet_ship ship);
const char *fleet_shot_result_name(enum fleet_shot_result result);
const char *fleet_difficulty_name(enum fleet_difficulty difficulty);
/* Parse a difficulty name (case sensitive, as produced above). Returns 0. */
int fleet_difficulty_parse(const char *name, enum fleet_difficulty *out);

/* ---- geometry -------------------------------------------------------- */

int fleet_in_bounds(int row, int col);
/* Cell index, or -1 when out of bounds. */
int fleet_index(int row, int col);
/* Grid name of a cell: column letter A-J, then row number 1-10 ("D7").
 * Writes at most n bytes. Returns 0, or -1 when out of bounds or n is too
 * small (buf then holds "?" when n allows). */
int fleet_cell_name(int row, int col, char *buf, size_t n);

#endif
