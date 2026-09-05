/*
 * PocketFleet rules vocabulary. See fleet_types.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_types.h"

#include <stdio.h>
#include <string.h>

static const struct {
    uint8_t length;
    const char *name;
} roster[FLEET_SHIP_COUNT] = {
    { 5, "Carrier" },
    { 4, "Battleship" },
    { 3, "Cruiser" },
    { 3, "Submarine" },
    { 2, "Destroyer" }
};

static const char *const difficulty_names[FLEET_DIFFICULTY_COUNT] = {
    "Recruit", "Officer", "Commander", "Admiral"
};

uint8_t fleet_ship_length(enum fleet_ship ship)
{
    if ((unsigned)ship >= FLEET_SHIP_COUNT) {
        return 0;
    }
    return roster[ship].length;
}

const char *fleet_ship_name(enum fleet_ship ship)
{
    if ((unsigned)ship >= FLEET_SHIP_COUNT) {
        return "?";
    }
    return roster[ship].name;
}

const char *fleet_shot_result_name(enum fleet_shot_result result)
{
    switch (result) {
    case FLEET_SHOT_MISS:
        return "MISS";
    case FLEET_SHOT_HIT:
        return "HIT";
    case FLEET_SHOT_SUNK:
        return "SUNK";
    case FLEET_SHOT_INVALID:
    default:
        return "INVALID";
    }
}

const char *fleet_difficulty_name(enum fleet_difficulty difficulty)
{
    if ((unsigned)difficulty >= FLEET_DIFFICULTY_COUNT) {
        return "?";
    }
    return difficulty_names[difficulty];
}

int fleet_difficulty_parse(const char *name, enum fleet_difficulty *out)
{
    int i;

    if (!name || !out) {
        return -1;
    }
    for (i = 0; i < FLEET_DIFFICULTY_COUNT; i++) {
        if (strcmp(name, difficulty_names[i]) == 0) {
            *out = (enum fleet_difficulty)i;
            return 0;
        }
    }
    return -1;
}

int fleet_in_bounds(int row, int col)
{
    return row >= 0 && row < FLEET_GRID && col >= 0 && col < FLEET_GRID;
}

int fleet_index(int row, int col)
{
    if (!fleet_in_bounds(row, col)) {
        return -1;
    }
    return row * FLEET_GRID + col;
}

int fleet_cell_name(int row, int col, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return -1;
    }
    if (!fleet_in_bounds(row, col) || n < FLEET_CELL_NAME_MAX) {
        snprintf(buf, n, "?");
        return -1;
    }
    snprintf(buf, n, "%c%d", (char)('A' + col), row + 1);
    return 0;
}
