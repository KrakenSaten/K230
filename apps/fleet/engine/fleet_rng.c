/*
 * PocketFleet deterministic pseudo-random source. See fleet_rng.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_rng.h"

void fleet_rng_seed(struct fleet_rng *rng, uint32_t seed)
{
    if (!rng) {
        return;
    }
    rng->state = seed ? seed : FLEET_RNG_ZERO_SEED;
}

uint32_t fleet_rng_next(struct fleet_rng *rng)
{
    uint32_t x;

    if (!rng) {
        return 0;
    }
    if (rng->state == 0) {
        rng->state = FLEET_RNG_ZERO_SEED; /* a restored save can carry 0 */
    }
    x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

uint32_t fleet_rng_below(struct fleet_rng *rng, uint32_t limit)
{
    uint32_t reject;
    uint32_t v;

    if (!rng || limit == 0) {
        return 0;
    }
    if (limit == 1) {
        return 0;
    }
    /* Values below this would make the modulo favour the low residues. */
    reject = (uint32_t)(0x100000000ULL % (uint64_t)limit);
    do {
        v = fleet_rng_next(rng);
    } while (v < reject);
    return v % limit;
}
