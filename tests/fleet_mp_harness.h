/*
 * Test support for PocketFleet multiplayer: a node (a match, its saved blob
 * and an automatic player), and the two ways a node survives a restart.
 * Header-only, shared by tests/fleet_match_test.c and tests/fleet_mp_sim_test.c.
 *
 * The player is PocketFleet's own AI, handed only its own shots and the
 * answers to them (the no-cheat contract, docs/apps/POCKETFLEET.md). Its
 * random stream for a decision is derived from (seed, ply), so a node that
 * restarts and has to choose again chooses the same square: network faults
 * change when a match happens, never what happens in it. That is the oracle
 * the simulator holds every faulty run to.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef FLEET_MP_HARNESS_H
#define FLEET_MP_HARNESS_H

#include "fleet_ai.h"
#include "fleet_match.h"
#include "fleet_match_save.h"

#include <stdio.h>
#include <string.h>

struct mp_node {
    struct fleet_match m;
    uint8_t key[FLEET_KEY_BYTES];
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];
    int has_blob;
    uint32_t seed;
    int difficulty;
    unsigned saves;
    unsigned reloads;
    int skip_save;          /* crash injection: the next save does not happen */
};

static void mp_key(uint8_t key[FLEET_KEY_BYTES], uint8_t first)
{
    int i;

    for (i = 0; i < FLEET_KEY_BYTES; i++) {
        key[i] = (uint8_t)(first + i * 7);
    }
    key[0] = first;
}

static void mp_node_init(struct mp_node *n, uint8_t first, uint32_t seed, int difficulty)
{
    memset(n, 0, sizeof(*n));
    mp_key(n->key, first);
    n->seed = seed;
    n->difficulty = difficulty;
    fleet_match_init(&n->m, n->key, seed ^ 0x5EED);
}

/* Save if the match asks for it: what the app does after every call. */
static void mp_save(struct mp_node *n)
{
    if (!fleet_match_dirty(&n->m)) {
        return;
    }
    if (n->skip_save) {
        return;
    }
    if (fleet_match_save_encode(&n->m, n->blob, sizeof(n->blob)) == FLEET_MATCH_SAVE_SIZE) {
        n->has_blob = 1;
        n->saves++;
        fleet_match_saved(&n->m);
    }
}

/* The process died: everything not saved is gone. Returns the restore
 * result (0, or -1 when the saved match was refused). */
static int mp_reload(struct mp_node *n, int64_t now)
{
    fleet_match_init(&n->m, n->key, n->seed ^ 0x5EED ^ (uint32_t)(++n->reloads * 7919));
    n->skip_save = 0;
    if (!n->has_blob) {
        return 0;
    }
    if (fleet_match_save_decode(&n->m, n->blob, sizeof(n->blob)) != 0) {
        return -1;
    }
    return fleet_match_restore(&n->m, now);
}

/* The next square this node's player would fire at, from the answers alone. */
static int mp_choose(const struct mp_node *n, int *row, int *col)
{
    struct fleet_ai ai;
    struct fleet_rng rng;
    int k;

    fleet_ai_init(&ai, (enum fleet_difficulty)n->difficulty);
    for (k = 1; k <= n->m.resolved; k++) {
        uint8_t res;

        if (fleet_match_shooter(k) != n->m.role) {
            continue;
        }
        res = n->m.log_res[k];
        fleet_ai_observe(&ai, n->m.log_cell[k] / FLEET_GRID, n->m.log_cell[k] % FLEET_GRID,
                         (enum fleet_shot_result)fleet_res_outcome(res), fleet_res_ship(res));
    }
    fleet_rng_seed(&rng, n->seed * 2654435761u + (uint32_t)n->m.resolved * 40503u + 1);
    return fleet_ai_next_shot(&ai, &rng, row, col);
}

static int mp_deploy(struct mp_node *n, int64_t now)
{
    struct fleet_board b;
    struct fleet_rng rng;
    uint8_t salt[FLEET_SALT_BYTES];
    int i;

    fleet_board_clear(&b);
    fleet_rng_seed(&rng, n->seed ^ 0xB0A7D);
    fleet_board_autoplace(&b, &rng);
    for (i = 0; i < FLEET_SALT_BYTES; i++) {
        salt[i] = (uint8_t)fleet_rng_next(&rng);
    }
    return fleet_match_deploy(&n->m, &b, salt, now);
}

/* Play whatever this node's player would do now. Returns 1 when it acted. */
static int mp_play(struct mp_node *n, int64_t now, int auto_accept)
{
    int row;
    int col;

    if (auto_accept && n->m.phase == FLEET_MP_INVITED) {
        return fleet_match_accept(&n->m, now) == 0;
    }
    if (n->m.phase == FLEET_MP_DEPLOY && !n->m.committed) {
        return mp_deploy(n, now) == 0;
    }
    if (fleet_match_my_turn(&n->m) && mp_choose(n, &row, &col) == 0) {
        return fleet_match_fire(&n->m, row, col, now) == 0;
    }
    return 0;
}

#endif
