/*
 * PG 2048 rules. See g2048_rules.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_rules.h"

#include <string.h>

uint32_t g2048_value(uint8_t exp)
{
    if (exp == 0 || exp > G2048_EXP_MAX) {
        return 0;
    }
    return (uint32_t)1u << exp;
}

uint32_t g2048_slide_line(const uint8_t in[G2048_SIDE], uint8_t out[G2048_SIDE],
                          int8_t dest[G2048_SIDE], uint8_t merged[G2048_SIDE])
{
    uint8_t open[G2048_SIDE]; /* out[k] may still take a merge this move */
    int8_t source[G2048_SIDE]; /* which in[] first landed in out[k] */
    uint32_t gained = 0;
    int w = 0;
    int i;

    memset(open, 0, sizeof(open));
    memset(source, -1, sizeof(source));
    for (i = 0; i < G2048_SIDE; i++) {
        out[i] = 0;
        dest[i] = -1;
        merged[i] = 0;
    }
    for (i = 0; i < G2048_SIDE; i++) {
        if (in[i] == 0) {
            continue;
        }
        /* Only the tile placed last can meet this one: tiles slide up to one
         * another, so nothing lies between them. It merges once at most. */
        if (w > 0 && open[w - 1] && out[w - 1] == in[i] && in[i] < G2048_EXP_MAX) {
            out[w - 1] = (uint8_t)(in[i] + 1);
            open[w - 1] = 0;
            dest[i] = (int8_t)(w - 1);
            merged[i] = 1;
            merged[source[w - 1]] = 1;
            gained += g2048_value(out[w - 1]);
        } else {
            out[w] = in[i];
            open[w] = 1;
            source[w] = (int8_t)i;
            dest[i] = (int8_t)w;
            w++;
        }
    }
    return gained;
}

/* The cell index of position k (0 = the edge moved toward) in line n. */
static int line_cell(enum g2048_dir dir, int n, int k)
{
    switch (dir) {
    case G2048_UP:
        return k * G2048_SIDE + n;
    case G2048_DOWN:
        return (G2048_SIDE - 1 - k) * G2048_SIDE + n;
    case G2048_LEFT:
        return n * G2048_SIDE + k;
    case G2048_RIGHT:
    default:
        return n * G2048_SIDE + (G2048_SIDE - 1 - k);
    }
}

int g2048_empty_cells(const struct g2048_game *g)
{
    int n = 0;
    int i;

    for (i = 0; g && i < G2048_CELLS; i++) {
        n += g->cell[i] == 0;
    }
    return n;
}

int g2048_tile_count(const struct g2048_game *g)
{
    return g ? G2048_CELLS - g2048_empty_cells(g) : 0;
}

uint8_t g2048_max_exp(const struct g2048_game *g)
{
    uint8_t m = 0;
    int i;

    for (i = 0; g && i < G2048_CELLS; i++) {
        if (g->cell[i] > m) {
            m = g->cell[i];
        }
    }
    return m;
}

int g2048_spawn(struct g2048_game *g, uint8_t *exp_out)
{
    int empty;
    uint32_t pick;
    int i;

    if (!g) {
        return -1;
    }
    empty = g2048_empty_cells(g);
    if (empty == 0) {
        return -1;
    }
    /* The cell first, then the value: a fixed draw order, so a seed and a
     * list of moves always give the same game. */
    pick = g2048_rng_below(&g->rng, (uint32_t)empty);
    for (i = 0; i < G2048_CELLS; i++) {
        if (g->cell[i] != 0) {
            continue;
        }
        if (pick == 0) {
            uint8_t exp = g2048_rng_below(&g->rng, G2048_FOUR_IN) == 0 ? 2 : 1;

            g->cell[i] = exp;
            if (exp_out) {
                *exp_out = exp;
            }
            return i;
        }
        pick--;
    }
    return -1; /* not reached: pick < empty */
}

void g2048_new_game(struct g2048_game *g, uint32_t seed, uint32_t best)
{
    if (!g) {
        return;
    }
    memset(g, 0, sizeof(*g));
    g->best = best;
    g2048_rng_seed(&g->rng, seed);
    g2048_spawn(g, NULL);
    g2048_spawn(g, NULL);
}

/* Slide the whole board without touching the game. Returns whether anything
 * changed; fills turn's steps and gained when turn is not NULL. */
static int slide_board(const uint8_t before[G2048_CELLS], enum g2048_dir dir,
                       uint8_t after[G2048_CELLS], struct g2048_turn *turn)
{
    int changed = 0;
    int n;

    for (n = 0; n < G2048_SIDE; n++) {
        uint8_t in[G2048_SIDE];
        uint8_t out[G2048_SIDE];
        int8_t dest[G2048_SIDE];
        uint8_t merged[G2048_SIDE];
        uint32_t gained;
        int k;

        for (k = 0; k < G2048_SIDE; k++) {
            in[k] = before[line_cell(dir, n, k)];
        }
        gained = g2048_slide_line(in, out, dest, merged);
        for (k = 0; k < G2048_SIDE; k++) {
            int cell = line_cell(dir, n, k);

            after[cell] = out[k];
            changed |= out[k] != in[k];
            if (turn && in[k] != 0 && turn->steps < G2048_CELLS) {
                struct g2048_step *s = &turn->step[turn->steps++];

                s->from = (uint8_t)cell;
                s->to = (uint8_t)line_cell(dir, n, dest[k]);
                s->exp = in[k];
                s->merged = merged[k];
            }
        }
        if (turn) {
            turn->gained += gained;
        }
    }
    return changed;
}

int g2048_can_move_dir(const struct g2048_game *g, enum g2048_dir dir)
{
    uint8_t after[G2048_CELLS];

    if (!g || (int)dir < 0 || dir >= G2048_DIR_COUNT) {
        return 0;
    }
    return slide_board(g->cell, dir, after, NULL);
}

int g2048_can_move(const struct g2048_game *g)
{
    int d;

    for (d = 0; d < G2048_DIR_COUNT; d++) {
        if (g2048_can_move_dir(g, (enum g2048_dir)d)) {
            return 1;
        }
    }
    return 0;
}

enum g2048_state g2048_state_of(const struct g2048_game *g)
{
    if (!g || g->over) {
        return G2048_OVER;
    }
    if (g->won && !g->keep_going) {
        return G2048_WON;
    }
    return G2048_PLAYING;
}

int g2048_move(struct g2048_game *g, enum g2048_dir dir, struct g2048_turn *turn)
{
    struct g2048_turn local;
    uint8_t after[G2048_CELLS];
    uint8_t had_won;

    if (!turn) {
        turn = &local;
    }
    memset(turn, 0, sizeof(*turn));
    turn->spawn = -1;
    if (!g || (int)dir < 0 || dir >= G2048_DIR_COUNT || g2048_state_of(g) != G2048_PLAYING) {
        return 0;
    }
    if (!slide_board(g->cell, dir, after, turn)) {
        /* Not a move: the steps describe tiles that stayed put, and nothing
         * else happens. */
        turn->gained = 0;
        return 0;
    }
    memcpy(g->cell, after, sizeof(g->cell));
    turn->moved = 1;
    g->moves++;
    g->score += turn->gained;
    if (g->score > g->best) {
        g->best = g->score;
    }
    had_won = g->won;
    if (g2048_max_exp(g) >= G2048_GOAL_EXP) {
        g->won = 1;
    }
    turn->reached_goal = (uint8_t)(g->won && !had_won);
    turn->spawn = (int8_t)g2048_spawn(g, &turn->spawn_exp);
    if (!g2048_can_move(g)) {
        g->over = 1;
        turn->ended = 1;
    }
    return 1;
}

void g2048_keep_going(struct g2048_game *g)
{
    if (g && g->won && !g->over) {
        g->keep_going = 1;
    }
}

int g2048_game_valid(const struct g2048_game *g)
{
    int i;

    if (!g) {
        return 0;
    }
    for (i = 0; i < G2048_CELLS; i++) {
        if (g->cell[i] > G2048_EXP_MAX) {
            return 0;
        }
    }
    /* A game starts with two tiles, and every move that merges tiles away
     * adds one back, so a board never holds fewer than two. */
    if (g2048_tile_count(g) < 2) {
        return 0;
    }
    /* Points come only from merges, each worth at least 4, and every merged
     * value is even. */
    if ((g->score & 1u) || (g->score > 0 && g->moves == 0) || g->best < g->score) {
        return 0;
    }
    if (g->rng.state == 0) {
        return 0;
    }
    if (g->won > 1 || g->keep_going > 1 || g->over > 1) {
        return 0;
    }
    /* Tiles never shrink, so a won game still shows the goal or more. */
    if (g->won && g2048_max_exp(g) < G2048_GOAL_EXP) {
        return 0;
    }
    if (g->keep_going && !g->won) {
        return 0;
    }
    if (g->over != !g2048_can_move(g)) {
        return 0;
    }
    return 1;
}
