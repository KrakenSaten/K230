/*
 * PG Blackjack deterministic pseudo-random source.
 *
 * xorshift32, as in PocketFleet, PocketRadar and PocketTimber, duplicated
 * for the reason apps/radar/engine/radar_rng.h gives: apps do not depend on
 * one another, and forty lines beside the engine are cheaper than a shared
 * first-party RNG. Its one use here is shuffling the shoe, so a session is a
 * function of its seed and the player's choices. NOT suitable for anything
 * security related - nor for real-money play, which this is not.
 *
 * The engine reads no clock and calls no library random source;
 * tests/bj_lint.sh keeps it that way by a text search, so prose here avoids
 * the names it looks for.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_RNG_H
#define PGBJ_RNG_H

#include <stdint.h>

#define BJ_RNG_ZERO_SEED 0x9E3779B9u

struct bj_rng {
    uint32_t state;
};

void bj_rng_seed(struct bj_rng *rng, uint32_t seed);
uint32_t bj_rng_next(struct bj_rng *rng);
/* Uniform in [0, limit), unbiased by rejection; 0 for limits 0 and 1
 * without consuming a draw. */
uint32_t bj_rng_below(struct bj_rng *rng, uint32_t limit);

#endif
