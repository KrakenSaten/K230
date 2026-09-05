/*
 * PocketFleet opponent AI. See fleet_ai.h for the no-cheat contract: this
 * file may not include fleet_rules.h and may not name the hidden types.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_ai.h"

#include <string.h>

/* Placements covering an unresolved hit outweigh plain search cells by so
 * much that Admiral finishes a wounded ship before looking elsewhere. */
#define DENSITY_HIT_WEIGHT 100u

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

/* ---- observations ----------------------------------------------------- */

static int is_hit(const struct fleet_ai *ai, int idx)
{
    return ai->shot[idx] && (ai->result[idx] == FLEET_SHOT_HIT ||
                             ai->result[idx] == FLEET_SHOT_SUNK);
}

static int is_miss(const struct fleet_ai *ai, int idx)
{
    return ai->shot[idx] && ai->result[idx] == FLEET_SHOT_MISS;
}

/* A hit that has not yet been attributed to a sunk ship. */
static int is_open_hit(const struct fleet_ai *ai, int idx)
{
    return is_hit(ai, idx) && !ai->resolved[idx];
}

/* Length of the shortest ship still afloat, or 1 when none is left. */
static int shortest_afloat(const struct fleet_ai *ai)
{
    int shortest = 0;
    int ship;

    for (ship = 0; ship < FLEET_SHIP_COUNT; ship++) {
        int length = fleet_ship_length((enum fleet_ship)ship);

        if (!ai->sunk[ship] && (shortest == 0 || length < shortest)) {
            shortest = length;
        }
    }
    return shortest > 0 ? shortest : 1;
}

/* ---- candidate selection ---------------------------------------------- */

static int pick(const uint8_t *candidates, int count, struct fleet_rng *rng,
                int *row, int *col)
{
    int idx;

    if (count <= 0) {
        return -1;
    }
    idx = candidates[fleet_rng_below(rng, (uint32_t)count)];
    if (row) {
        *row = idx / FLEET_GRID;
    }
    if (col) {
        *col = idx % FLEET_GRID;
    }
    return 0;
}

/* Uniform over every cell not yet fired at. */
static int search_random(const struct fleet_ai *ai, struct fleet_rng *rng,
                         int *row, int *col)
{
    uint8_t candidates[FLEET_CELLS];
    int count = 0;
    int i;

    for (i = 0; i < FLEET_CELLS; i++) {
        if (!ai->shot[i]) {
            candidates[count++] = (uint8_t)i;
        }
    }
    return pick(candidates, count, rng, row, col);
}

/* No ship of length L can hide between cells spaced L apart on a diagonal
 * lattice, so searching only those cells covers the board with a fraction of
 * the shots. Falls back to a plain search once the lattice is exhausted. */
static int search_parity(const struct fleet_ai *ai, struct fleet_rng *rng,
                         int *row, int *col)
{
    uint8_t candidates[FLEET_CELLS];
    int step = shortest_afloat(ai);
    int count = 0;
    int i;

    for (i = 0; i < FLEET_CELLS; i++) {
        if (!ai->shot[i] && ((i / FLEET_GRID) + (i % FLEET_GRID)) % step == 0) {
            candidates[count++] = (uint8_t)i;
        }
    }
    if (count == 0) {
        return search_random(ai, rng, row, col);
    }
    return pick(candidates, count, rng, row, col);
}

/* Officer: the most recently queued candidate that is still unfired. */
static int hunt_queue(const struct fleet_ai *ai, int *row, int *col)
{
    int i;

    for (i = ai->queue_len - 1; i >= 0; i--) {
        if (!ai->shot[ai->queue[i]]) {
            if (row) {
                *row = ai->queue[i] / FLEET_GRID;
            }
            if (col) {
                *col = ai->queue[i] % FLEET_GRID;
            }
            return 0;
        }
    }
    return -1;
}

/* Commander: two adjacent unresolved hits fix the ship's axis, so fire at
 * the ends of that run before trying anything perpendicular. */
static int hunt_line(const struct fleet_ai *ai, struct fleet_rng *rng, int *row, int *col)
{
    uint8_t candidates[FLEET_CELLS];
    int count = 0;
    int axis;
    int i;

    for (axis = 0; axis < 2 && count == 0; axis++) {
        int dr = axis == 0 ? 0 : 1;   /* horizontal first, then vertical */
        int dc = axis == 0 ? 1 : 0;

        for (i = 0; i < FLEET_CELLS && count == 0; i++) {
            int r = i / FLEET_GRID;
            int c = i % FLEET_GRID;
            int next = fleet_index(r + dr, c + dc);
            int end;

            if (!is_open_hit(ai, i) || next < 0 || !is_open_hit(ai, next)) {
                continue;
            }
            /* Walk to both ends of the run of open hits and take the cell
             * just beyond each, when it has not been fired at. */
            for (end = 0; end < 2; end++) {
                int sr = end == 0 ? -dr : dr;
                int sc = end == 0 ? -dc : dc;
                int rr = r;
                int cc = c;
                int probe = -1;

                if (end == 1) {
                    rr = next / FLEET_GRID;
                    cc = next % FLEET_GRID;
                }
                for (;;) {
                    probe = fleet_index(rr + sr, cc + sc);
                    if (probe < 0 || !is_open_hit(ai, probe)) {
                        break;
                    }
                    rr = probe / FLEET_GRID;
                    cc = probe % FLEET_GRID;
                }
                if (probe >= 0 && !ai->shot[probe]) {
                    candidates[count++] = (uint8_t)probe;
                }
            }
        }
    }
    return pick(candidates, count, rng, row, col);
}

/* Commander: unfired orthogonal neighbours of any unresolved hit. */
static int hunt_neighbours(const struct fleet_ai *ai, struct fleet_rng *rng,
                           int *row, int *col)
{
    uint8_t candidates[FLEET_CELLS];
    int count = 0;
    int i;
    int d;

    for (i = 0; i < FLEET_CELLS; i++) {
        if (!is_open_hit(ai, i)) {
            continue;
        }
        for (d = 0; d < 4; d++) {
            int probe = fleet_index(i / FLEET_GRID + neighbour_row[d],
                                    i % FLEET_GRID + neighbour_col[d]);
            int k;
            int seen = 0;

            if (probe < 0 || ai->shot[probe]) {
                continue;
            }
            for (k = 0; k < count; k++) {
                seen |= candidates[k] == (uint8_t)probe;
            }
            if (!seen) {
                candidates[count++] = (uint8_t)probe;
            }
        }
    }
    return pick(candidates, count, rng, row, col);
}

/* Admiral: count every placement of every ship still afloat that agrees with
 * the announcements, and fire where the most of them overlap. */
static int search_density(const struct fleet_ai *ai, struct fleet_rng *rng,
                          int *row, int *col)
{
    uint32_t heat[FLEET_CELLS];
    uint8_t candidates[FLEET_CELLS];
    uint32_t best = 0;
    int count = 0;
    int need_cover;
    int pass;
    int i;

    /* While hits are unresolved only placements that explain them count; if
     * no placement can (the announcements and the model disagree), fall back
     * to counting every placement. */
    need_cover = 0;
    for (i = 0; i < FLEET_CELLS; i++) {
        need_cover |= is_open_hit(ai, i);
    }
    for (pass = 0; pass < 2; pass++) {
        int ship;

        memset(heat, 0, sizeof(heat));
        for (ship = 0; ship < FLEET_SHIP_COUNT; ship++) {
            int length = fleet_ship_length((enum fleet_ship)ship);
            int orient;

            if (ai->sunk[ship]) {
                continue;
            }
            for (orient = 0; orient < 2; orient++) {
                int r;

                for (r = 0; r < FLEET_GRID; r++) {
                    int c;

                    for (c = 0; c < FLEET_GRID; c++) {
                        int cells[FLEET_GRID];
                        int covered = 0;
                        int fits = 1;
                        uint32_t weight;
                        int n;

                        for (n = 0; n < length && fits; n++) {
                            int rr = r + (orient ? n : 0);
                            int cc = c + (orient ? 0 : n);
                            int idx = fleet_index(rr, cc);

                            if (idx < 0 || is_miss(ai, idx) || ai->resolved[idx]) {
                                fits = 0;
                                break;
                            }
                            cells[n] = idx;
                            covered += is_open_hit(ai, idx);
                        }
                        if (!fits || (need_cover && covered == 0)) {
                            continue;
                        }
                        weight = covered ? (uint32_t)covered * DENSITY_HIT_WEIGHT : 1u;
                        for (n = 0; n < length; n++) {
                            if (!ai->shot[cells[n]]) {
                                heat[cells[n]] += weight;
                            }
                        }
                    }
                }
            }
        }
        for (i = 0; i < FLEET_CELLS; i++) {
            if (heat[i] > best) {
                best = heat[i];
            }
        }
        if (best > 0) {
            break;
        }
        need_cover = 0; /* second pass: drop the requirement */
    }
    if (best == 0) {
        return search_parity(ai, rng, row, col);
    }
    for (i = 0; i < FLEET_CELLS; i++) {
        if (heat[i] == best) {
            candidates[count++] = (uint8_t)i;
        }
    }
    return pick(candidates, count, rng, row, col);
}

int fleet_ai_next_shot(const struct fleet_ai *ai, struct fleet_rng *rng,
                       int *row, int *col)
{
    if (!ai || !rng || fleet_ai_remaining(ai) == 0) {
        return -1;
    }
    switch (ai->difficulty) {
    case FLEET_ADMIRAL:
        return search_density(ai, rng, row, col);
    case FLEET_COMMANDER:
        if (hunt_line(ai, rng, row, col) == 0) {
            return 0;
        }
        if (hunt_neighbours(ai, rng, row, col) == 0) {
            return 0;
        }
        return search_parity(ai, rng, row, col);
    case FLEET_OFFICER:
        if (hunt_queue(ai, row, col) == 0) {
            return 0;
        }
        return search_random(ai, rng, row, col);
    case FLEET_RECRUIT:
    default:
        return search_random(ai, rng, row, col);
    }
}

/* ---- feedback --------------------------------------------------------- */

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

/* A ship of a known length has just sunk at (row, col). Find the run of that
 * many unresolved hits, in line and including the sinking cell, and mark it
 * as that ship's hull so it stops attracting fire.
 *
 * When two ships lie end to end the run of hits can be longer than the ship
 * and the wrong window is chosen. That is an inference the AI is entitled to
 * get wrong: it is drawn from the announcements alone, it never makes a shot
 * illegal, and the density model recovers from it. */
static void resolve_hull(struct fleet_ai *ai, int row, int col, int length)
{
    int axis;

    for (axis = 0; axis < 2; axis++) {
        int dr = axis == 0 ? 0 : 1;
        int dc = axis == 0 ? 1 : 0;
        int offset;

        for (offset = 0; offset < length; offset++) {
            int cells[FLEET_GRID];
            int fits = 1;
            int n;

            for (n = 0; n < length; n++) {
                int idx = fleet_index(row + (n - offset) * dr, col + (n - offset) * dc);

                if (idx < 0 || !is_open_hit(ai, idx)) {
                    fits = 0;
                    break;
                }
                cells[n] = idx;
            }
            if (!fits) {
                continue;
            }
            for (n = 0; n < length; n++) {
                ai->resolved[cells[n]] = 1;
            }
            return;
        }
    }
    /* Nothing consistent found: at least stop chasing the sinking cell. */
    ai->resolved[fleet_index(row, col)] = 1;
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
        if (ai->difficulty == FLEET_COMMANDER || ai->difficulty == FLEET_ADMIRAL) {
            resolve_hull(ai, row, col, fleet_ship_length((enum fleet_ship)sunk_ship));
        }
    }
    if (ai->difficulty != FLEET_OFFICER) {
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
