/*
 * PocketRadar deterministic pseudo-random source. See radar_rng.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "radar_rng.h"

void radar_rng_seed(struct radar_rng *rng, uint32_t seed)
{
    if (!rng) {
        return;
    }
    rng->state = seed ? seed : RADAR_RNG_ZERO_SEED;
}

uint32_t radar_rng_next(struct radar_rng *rng)
{
    uint32_t x;

    if (!rng) {
        return 0;
    }
    if (rng->state == 0) {
        rng->state = RADAR_RNG_ZERO_SEED; /* a zeroed struct can carry 0 */
    }
    x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

uint32_t radar_rng_below(struct radar_rng *rng, uint32_t limit)
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
        v = radar_rng_next(rng);
    } while (v < reject);
    return v % limit;
}

int32_t radar_rng_range(struct radar_rng *rng, int32_t lo, int32_t hi)
{
    uint64_t span;

    if (hi < lo) {
        return lo;
    }
    span = (uint64_t)((int64_t)hi - (int64_t)lo) + 1u;
    if (span > 0xFFFFFFFFu) {
        span = 0xFFFFFFFFu;
    }
    return (int32_t)((int64_t)lo + (int64_t)radar_rng_below(rng, (uint32_t)span));
}
