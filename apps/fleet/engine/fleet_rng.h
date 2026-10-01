/*
 * PocketFleet deterministic pseudo-random source.
 *
 * xorshift32 (Marsaglia). Chosen for being small, auditable and trivially
 * serialisable: the whole state is one 32-bit word, so a saved game resumes
 * on exactly the same sequence. Quality is far more than a board game needs;
 * it is NOT suitable for anything security related.
 *
 * Every random decision in PocketFleet draws from one of these in a fixed
 * order, which is what makes a match reproducible from its seed.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_RNG_H
#define POCKETFLEET_RNG_H

#include <stdint.h>

/* Substituted for seed 0, which is the xorshift32 fixed point. */
#define FLEET_RNG_ZERO_SEED 0x9E3779B9u

struct fleet_rng {
    uint32_t state;
};

void fleet_rng_seed(struct fleet_rng *rng, uint32_t seed);
/* Next 32-bit value; never 0. */
uint32_t fleet_rng_next(struct fleet_rng *rng);
/* Uniform value in [0, limit), unbiased by rejection. Returns 0 when
 * limit is 0. */
uint32_t fleet_rng_below(struct fleet_rng *rng, uint32_t limit);

#endif
