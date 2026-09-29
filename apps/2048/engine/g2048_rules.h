/*
 * PG 2048 rules: a 4 x 4 board, slides, merges, new tiles, score, the goal
 * and the end of a game.
 *
 * Pure C. No LVGL, no I/O, no floating point, no clock and no platform
 * entropy (tests/g2048_lint.sh). Nothing here knows about pixels, screens,
 * fingers or keys: the board is sixteen logical cells, and a direction is a
 * direction on that board. The view decides where a cell is drawn and which
 * key or swipe means which direction, so turning the panel changes the view
 * and never these rules.
 *
 * COORDINATES. Cell index = row * 4 + col. Row 0 is the top row and column
 * 0 the left column *of the board*, not of any display. UP moves tiles
 * toward row 0, LEFT toward column 0.
 *
 * TILES. A cell holds an exponent: 0 is empty, 1 is the tile 2, 2 is 4, and
 * so on. On a 4 x 4 board with 2s and 4s the largest reachable tile is
 * 2^17 = 131072, so an exponent always fits in a byte and G2048_EXP_MAX
 * bounds a valid board.
 *
 * RULES (the standard game):
 *  - A move slides every tile as far as it goes toward one edge.
 *  - Two tiles of the same value that meet merge into one of twice the
 *    value, and the merged value is added to the score.
 *  - A tile merges at most once per move: 2 2 2 2 -> 4 4, never 8, and
 *    4 4 8 -> 8 8, never 16. Where three equal tiles meet, the two nearest
 *    the edge being moved toward merge.
 *  - A move that changes nothing is not a move: no tile appears, the move
 *    counter does not advance.
 *  - After every real move one tile appears in a uniformly chosen empty
 *    cell: a 2 nine times in ten, a 4 one time in ten. A new game starts
 *    with two such tiles.
 *  - Making the 2048 tile (G2048_GOAL_EXP) wins; the player may keep going.
 *  - The game is over when no move in any direction would change the board.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PG2048_RULES_H
#define PG2048_RULES_H

#include "g2048_rng.h"

#include <stdint.h>

#define G2048_SIDE 4
#define G2048_CELLS (G2048_SIDE * G2048_SIDE)
/* 2^11 = 2048. */
#define G2048_GOAL_EXP 11
/* 2^17 = 131072, the largest tile a 4 x 4 board can hold. */
#define G2048_EXP_MAX 17
/* A new tile is a 4 (exponent 2) when a draw below 10 lands on 0. */
#define G2048_FOUR_IN 10

enum g2048_dir {
    G2048_UP = 0,
    G2048_DOWN,
    G2048_LEFT,
    G2048_RIGHT,
    G2048_DIR_COUNT
};

/* Where the game stands, as the view needs to know it. */
enum g2048_state {
    G2048_PLAYING = 0, /* moves are accepted */
    G2048_WON,         /* the goal was just made and not yet acknowledged */
    G2048_OVER         /* no move remains */
};

struct g2048_game {
    uint8_t cell[G2048_CELLS]; /* exponents, 0 = empty */
    uint32_t score;
    uint32_t best;       /* carried across games by the caller */
    uint32_t moves;      /* moves that changed the board */
    struct g2048_rng rng;
    uint8_t won;         /* the goal tile has been made in this game */
    uint8_t keep_going;  /* the player chose to continue past it */
    uint8_t over;        /* no move remains */
};

/* What one tile did during a move, for the view's animation: it started in
 * `from` with exponent `exp` and ended in `to`. `merged` is 1 for both tiles
 * that became one; the tile left in `to` then has exponent exp + 1. A tile
 * that did not move has from == to. Empty cells have no step. */
struct g2048_step {
    uint8_t from;
    uint8_t to;
    uint8_t exp;
    uint8_t merged;
};

struct g2048_turn {
    uint8_t moved;        /* 0: nothing changed, nothing appeared */
    uint8_t steps;        /* entries used in step[], one per tile before the move */
    struct g2048_step step[G2048_CELLS];
    uint32_t gained;      /* score added by this move */
    int8_t spawn;         /* cell of the new tile, or -1 */
    uint8_t spawn_exp;
    uint8_t reached_goal; /* this move made the goal for the first time */
    uint8_t ended;        /* after this move no move remains */
};

/* One line of four cells, ordered so that index 0 is the edge the tiles move
 * toward. out[] receives the line after the slide, dest[i] the index in out
 * that in[i] ended in (-1 when in[i] is empty), merged[i] whether in[i]
 * became half of a merge. Returns the score the line gained. This is the
 * whole of the slide-and-merge rule; the board applies it four times. */
uint32_t g2048_slide_line(const uint8_t in[G2048_SIDE], uint8_t out[G2048_SIDE],
                          int8_t dest[G2048_SIDE], uint8_t merged[G2048_SIDE]);

/* A new game from a seed: an empty board with two tiles, score 0, `best`
 * kept as given. */
void g2048_new_game(struct g2048_game *g, uint32_t seed, uint32_t best);

/* Apply a move. Returns 1 when the board changed (a tile then appeared),
 * 0 when it did not or the game does not accept moves (over, or won and not
 * yet acknowledged). turn may be NULL. */
int g2048_move(struct g2048_game *g, enum g2048_dir dir, struct g2048_turn *turn);

/* Whether a move in dir would change the board. */
int g2048_can_move_dir(const struct g2048_game *g, enum g2048_dir dir);
/* Whether any move would. */
int g2048_can_move(const struct g2048_game *g);

/* Place one random tile in an empty cell. Returns the cell, or -1 when the
 * board is full. exp_out may be NULL. Used by new games and moves; exposed
 * so the placement rule is tested on its own. */
int g2048_spawn(struct g2048_game *g, uint8_t *exp_out);

/* Acknowledge the goal and continue. No effect unless the game is won. */
void g2048_keep_going(struct g2048_game *g);

enum g2048_state g2048_state_of(const struct g2048_game *g);
int g2048_empty_cells(const struct g2048_game *g);
int g2048_tile_count(const struct g2048_game *g);
uint8_t g2048_max_exp(const struct g2048_game *g);
/* The tile's face value, 2^exp; 0 for an empty cell or an exponent out of
 * range. */
uint32_t g2048_value(uint8_t exp);

/* Could the rules have produced this game? Used to refuse a stored game
 * that decodes but describes something impossible. 1 valid, 0 not. */
int g2048_game_valid(const struct g2048_game *g);

#endif
