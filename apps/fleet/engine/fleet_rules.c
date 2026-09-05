/*
 * PocketFleet rules and match state. See fleet_rules.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_rules.h"

#include <string.h>

/* Random placement gives up after this many draws per ship and falls back to
 * a deterministic scan, so autoplace always terminates. */
#define AUTOPLACE_DRAWS 200

/* Derived from the seed so that the opponent stream does not shift when the
 * player uses auto-deploy. */
#define RNG_AI_SALT 0x9E3779B9u

static void cell_of(const struct fleet_ship_state *s, int n, int *row, int *col)
{
    *row = s->row + (s->orient == FLEET_VERTICAL ? n : 0);
    *col = s->col + (s->orient == FLEET_VERTICAL ? 0 : n);
}

void fleet_board_clear(struct fleet_board *board)
{
    if (!board) {
        return;
    }
    memset(board, 0, sizeof(*board));
    memset(board->ship_at, FLEET_NO_SHIP, sizeof(board->ship_at));
    board->ships_afloat = FLEET_SHIP_COUNT;
}

int fleet_board_can_place(const struct fleet_board *board, enum fleet_ship ship,
                          int row, int col, enum fleet_orient orient)
{
    uint8_t length = fleet_ship_length(ship);
    int n;

    if (!board || length == 0 || (unsigned)orient > FLEET_VERTICAL) {
        return 0;
    }
    for (n = 0; n < length; n++) {
        int r = row + (orient == FLEET_VERTICAL ? n : 0);
        int c = col + (orient == FLEET_VERTICAL ? 0 : n);
        int idx = fleet_index(r, c);
        uint8_t occupant;

        if (idx < 0) {
            return 0;
        }
        occupant = board->ship_at[idx];
        if (occupant != FLEET_NO_SHIP && occupant != (uint8_t)ship) {
            return 0;
        }
    }
    return 1;
}

int fleet_board_unplace(struct fleet_board *board, enum fleet_ship ship)
{
    struct fleet_ship_state *s;
    uint8_t length = fleet_ship_length(ship);
    int n;

    if (!board || length == 0) {
        return -1;
    }
    s = &board->ships[ship];
    if (!s->placed) {
        return -1;
    }
    for (n = 0; n < length; n++) {
        int r;
        int c;
        int idx;

        cell_of(s, n, &r, &c);
        idx = fleet_index(r, c);
        if (idx >= 0 && board->ship_at[idx] == (uint8_t)ship) {
            board->ship_at[idx] = FLEET_NO_SHIP;
        }
    }
    memset(s, 0, sizeof(*s));
    if (board->ships_placed > 0) {
        board->ships_placed--;
    }
    return 0;
}

int fleet_board_place(struct fleet_board *board, enum fleet_ship ship,
                      int row, int col, enum fleet_orient orient)
{
    struct fleet_ship_state previous;
    struct fleet_ship_state *s;
    uint8_t length = fleet_ship_length(ship);
    int had_ship;
    int n;

    if (!board || length == 0) {
        return -1;
    }
    s = &board->ships[ship];
    previous = *s;
    had_ship = s->placed;
    if (had_ship) {
        fleet_board_unplace(board, ship);
    }
    if (!fleet_board_can_place(board, ship, row, col, orient)) {
        if (had_ship) {
            /* Restore: the old cells were just freed, so this cannot fail. */
            *s = previous;
            board->ships_placed++;
            for (n = 0; n < length; n++) {
                int r;
                int c;

                cell_of(s, n, &r, &c);
                board->ship_at[fleet_index(r, c)] = (uint8_t)ship;
            }
        }
        return -1;
    }
    s->row = (uint8_t)row;
    s->col = (uint8_t)col;
    s->orient = (uint8_t)orient;
    s->hits = had_ship ? previous.hits : 0;
    s->placed = 1;
    board->ships_placed++;
    for (n = 0; n < length; n++) {
        int r;
        int c;

        cell_of(s, n, &r, &c);
        board->ship_at[fleet_index(r, c)] = (uint8_t)ship;
    }
    return 0;
}

int fleet_board_complete(const struct fleet_board *board)
{
    return board && board->ships_placed == FLEET_SHIP_COUNT;
}

int fleet_board_autoplace(struct fleet_board *board, struct fleet_rng *rng)
{
    int ship;

    if (!board || !rng) {
        return -1;
    }
    for (ship = 0; ship < FLEET_SHIP_COUNT; ship++) {
        int placed = 0;
        int draw;
        int scan;
        uint32_t start;

        if (board->ships[ship].placed) {
            continue;
        }
        for (draw = 0; draw < AUTOPLACE_DRAWS && !placed; draw++) {
            int row = (int)fleet_rng_below(rng, FLEET_GRID);
            int col = (int)fleet_rng_below(rng, FLEET_GRID);
            enum fleet_orient orient = (enum fleet_orient)fleet_rng_below(rng, 2);

            placed = fleet_board_place(board, (enum fleet_ship)ship, row, col, orient) == 0;
        }
        if (placed) {
            continue;
        }
        /* Deterministic sweep from a random offset over every cell and
         * orientation, so placement cannot fail silently. */
        start = fleet_rng_below(rng, FLEET_CELLS * 2);
        for (scan = 0; scan < FLEET_CELLS * 2 && !placed; scan++) {
            int slot = (int)((start + (uint32_t)scan) % (FLEET_CELLS * 2));
            int cell = slot / 2;
            enum fleet_orient orient = (enum fleet_orient)(slot % 2);

            placed = fleet_board_place(board, (enum fleet_ship)ship, cell / FLEET_GRID,
                                       cell % FLEET_GRID, orient) == 0;
        }
        if (!placed) {
            return -1;
        }
    }
    return 0;
}

int fleet_board_ship_sunk(const struct fleet_board *board, enum fleet_ship ship)
{
    uint8_t length = fleet_ship_length(ship);

    if (!board || length == 0 || !board->ships[ship].placed) {
        return 0;
    }
    return board->ships[ship].hits >= length;
}

int fleet_board_ship_cell(const struct fleet_board *board, enum fleet_ship ship, int n,
                          int *row, int *col)
{
    uint8_t length = fleet_ship_length(ship);
    int r;
    int c;

    if (!board || length == 0 || n < 0 || n >= length || !board->ships[ship].placed) {
        return -1;
    }
    cell_of(&board->ships[ship], n, &r, &c);
    if (row) {
        *row = r;
    }
    if (col) {
        *col = c;
    }
    return 0;
}

enum fleet_shot_result fleet_board_fire(struct fleet_board *board, int row, int col,
                                        int *sunk_ship)
{
    int idx = fleet_index(row, col);
    uint8_t ship;

    if (sunk_ship) {
        *sunk_ship = -1;
    }
    if (!board || idx < 0 || board->shot[idx]) {
        return FLEET_SHOT_INVALID;
    }
    board->shot[idx] = 1;
    ship = board->ship_at[idx];
    if (ship == FLEET_NO_SHIP) {
        return FLEET_SHOT_MISS;
    }
    board->ships[ship].hits++;
    if (board->ships[ship].hits >= fleet_ship_length((enum fleet_ship)ship)) {
        if (board->ships_afloat > 0) {
            board->ships_afloat--;
        }
        if (sunk_ship) {
            *sunk_ship = (int)ship;
        }
        return FLEET_SHOT_SUNK;
    }
    return FLEET_SHOT_HIT;
}

/* ---- match ----------------------------------------------------------- */

void fleet_game_new(struct fleet_game *game, uint32_t seed, enum fleet_difficulty difficulty)
{
    int side;

    if (!game) {
        return;
    }
    memset(game, 0, sizeof(*game));
    game->seed = seed;
    fleet_rng_seed(&game->rng_setup, seed);
    fleet_rng_seed(&game->rng_ai, seed ^ RNG_AI_SALT);
    game->difficulty = (uint8_t)((unsigned)difficulty < FLEET_DIFFICULTY_COUNT
                                 ? difficulty : FLEET_OFFICER);
    game->phase = FLEET_PHASE_DEPLOY;
    game->winner = FLEET_SIDE_NONE;
    game->turn = 0;
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        fleet_board_clear(&game->board[side]);
    }
    fleet_ai_init(&game->ai, (enum fleet_difficulty)game->difficulty);
    fleet_board_autoplace(&game->board[FLEET_SIDE_OPPONENT], &game->rng_setup);
}

int fleet_game_start(struct fleet_game *game)
{
    if (!game || game->phase != FLEET_PHASE_DEPLOY) {
        return -1;
    }
    if (!fleet_board_complete(&game->board[FLEET_SIDE_PLAYER])) {
        return -1;
    }
    game->phase = FLEET_PHASE_PLAYER;
    game->turn = 1;
    return 0;
}

struct fleet_board *fleet_game_target(struct fleet_game *game, enum fleet_side shooter)
{
    if (!game || (unsigned)shooter >= FLEET_SIDE_COUNT) {
        return NULL;
    }
    return &game->board[shooter == FLEET_SIDE_PLAYER ? FLEET_SIDE_OPPONENT
                                                     : FLEET_SIDE_PLAYER];
}

int fleet_game_is_over(const struct fleet_game *game)
{
    return game && game->phase == FLEET_PHASE_OVER;
}

enum fleet_shot_result fleet_game_opponent_turn(struct fleet_game *game, int *row,
                                                int *col, int *sunk_ship)
{
    enum fleet_shot_result result;
    int r = 0;
    int c = 0;
    int sunk = -1;

    if (sunk_ship) {
        *sunk_ship = -1;
    }
    if (!game || game->phase != FLEET_PHASE_OPPONENT) {
        return FLEET_SHOT_INVALID;
    }
    if (fleet_ai_next_shot(&game->ai, &game->rng_ai, &r, &c) != 0) {
        return FLEET_SHOT_INVALID;
    }
    result = fleet_game_fire(game, FLEET_SIDE_OPPONENT, r, c, &sunk);
    if (result == FLEET_SHOT_INVALID) {
        return result;
    }
    fleet_ai_observe(&game->ai, r, c, result, sunk);
    if (row) {
        *row = r;
    }
    if (col) {
        *col = c;
    }
    if (sunk_ship) {
        *sunk_ship = sunk;
    }
    return result;
}

enum fleet_shot_result fleet_game_fire(struct fleet_game *game, enum fleet_side shooter,
                                       int row, int col, int *sunk_ship)
{
    struct fleet_board *target;
    enum fleet_shot_result result;
    uint8_t expected;

    if (sunk_ship) {
        *sunk_ship = -1;
    }
    if (!game || (unsigned)shooter >= FLEET_SIDE_COUNT) {
        return FLEET_SHOT_INVALID;
    }
    expected = (uint8_t)(shooter == FLEET_SIDE_PLAYER ? FLEET_PHASE_PLAYER
                                                      : FLEET_PHASE_OPPONENT);
    if (game->phase != expected) {
        return FLEET_SHOT_INVALID;
    }
    target = fleet_game_target(game, shooter);
    result = fleet_board_fire(target, row, col, sunk_ship);
    if (result == FLEET_SHOT_INVALID) {
        return result;
    }
    game->stats[shooter].shots++;
    if (result != FLEET_SHOT_MISS) {
        game->stats[shooter].hits++;
    }
    if (target->ships_afloat == 0) {
        game->phase = FLEET_PHASE_OVER;
        game->winner = (uint8_t)shooter;
        return result;
    }
    if (shooter == FLEET_SIDE_PLAYER) {
        game->phase = FLEET_PHASE_OPPONENT;
    } else {
        game->phase = FLEET_PHASE_PLAYER;
        game->turn++;
    }
    return result;
}
