/*
 * PG Solitaire deterministic pseudo-random source.
 *
 * xorshift32, as in PocketFleet, PocketRadar and PocketTimber, duplicated
 * for the reason apps/radar/engine/radar_rng.h gives: apps do not depend on
 * one another, and forty lines beside the engine are cheaper than a shared
 * first-party RNG. Its one use here is the shuffle, so a deal is a function
 * of its seed. NOT suitable for anything security related.
 *
 * The engine reads no clock and calls no library random source;
 * tests/sol_lint.sh keeps it that way by a text search, so prose here avoids
 * the names it looks for.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_RNG_H
#define PGSOL_RNG_H

#include <stdint.h>

#define SOL_RNG_ZERO_SEED 0x9E3779B9u

struct sol_rng {
    uint32_t state;
};

void sol_rng_seed(struct sol_rng *rng, uint32_t seed);
uint32_t sol_rng_next(struct sol_rng *rng);
/* Uniform in [0, limit), unbiased by rejection; 0 for limits 0 and 1
 * without consuming a draw. */
uint32_t sol_rng_below(struct sol_rng *rng, uint32_t limit);

#endif
