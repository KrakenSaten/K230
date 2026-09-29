/*
 * PG Solitaire deterministic pseudo-random source. See sol_rng.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "sol_rng.h"

void sol_rng_seed(struct sol_rng *rng, uint32_t seed)
{
    if (rng) {
        rng->state = seed ? seed : SOL_RNG_ZERO_SEED;
    }
}

uint32_t sol_rng_next(struct sol_rng *rng)
{
    uint32_t x;

    if (!rng) {
        return 0;
    }
    if (rng->state == 0) {
        rng->state = SOL_RNG_ZERO_SEED;
    }
    x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

uint32_t sol_rng_below(struct sol_rng *rng, uint32_t limit)
{
    uint32_t reject;
    uint32_t v;

    if (!rng || limit <= 1) {
        return 0;
    }
    reject = (uint32_t)(0x100000000ULL % (uint64_t)limit);
    do {
        v = sol_rng_next(rng);
    } while (v < reject);
    return v % limit;
}
