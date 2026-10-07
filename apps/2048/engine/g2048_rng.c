/*
 * PG 2048 deterministic pseudo-random source. See g2048_rng.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "g2048_rng.h"

void g2048_rng_seed(struct g2048_rng *rng, uint32_t seed)
{
    if (!rng) {
        return;
    }
    rng->state = seed ? seed : G2048_RNG_ZERO_SEED;
}

uint32_t g2048_rng_next(struct g2048_rng *rng)
{
    uint32_t x;

    if (!rng) {
        return 0;
    }
    if (rng->state == 0) {
        rng->state = G2048_RNG_ZERO_SEED; /* a zeroed struct can carry 0 */
    }
    x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

uint32_t g2048_rng_below(struct g2048_rng *rng, uint32_t limit)
{
    uint32_t reject;
    uint32_t v;

    if (!rng || limit <= 1) {
        return 0;
    }
    /* Values below this would make the modulo favour the low residues. */
    reject = (uint32_t)(0x100000000ULL % (uint64_t)limit);
    do {
        v = g2048_rng_next(rng);
    } while (v < reject);
    return v % limit;
}
