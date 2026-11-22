/*
 * Card games: deterministic pseudo-random source. See cg_rng.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_rng.h"

void cg_rng_seed(struct cg_rng *rng, uint32_t seed)
{
    if (rng) {
        rng->state = seed ? seed : CG_RNG_ZERO_SEED;
    }
}

uint32_t cg_rng_next(struct cg_rng *rng)
{
    uint32_t x;

    if (!rng) {
        return 0;
    }
    if (rng->state == 0) {
        rng->state = CG_RNG_ZERO_SEED;
    }
    x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

uint32_t cg_rng_below(struct cg_rng *rng, uint32_t limit)
{
    uint32_t reject;
    uint32_t v;

    if (!rng || limit <= 1) {
        return 0;
    }
    reject = (uint32_t)(0x100000000ULL % (uint64_t)limit);
    do {
        v = cg_rng_next(rng);
    } while (v < reject);
    return v % limit;
}
