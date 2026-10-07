/*
 * PG 2048 deterministic pseudo-random source.
 *
 * xorshift32 (Marsaglia), the generator PocketFleet, PocketRadar and
 * PocketTimber use, for the same reasons: small, auditable, one 32-bit word
 * of state that fits in a save file. It is NOT suitable for anything
 * security related.
 *
 * The duplication of apps/radar/engine/radar_rng.c is deliberate and follows
 * that file's own reasoning: apps do not depend on one another, and forty
 * lines of arithmetic beside the engine that uses it are cheaper than a
 * shared first-party RNG nobody has asked for yet.
 *
 * Every random decision in a game draws from one of these in a fixed order
 * (the cell first, then the value), which is what makes a game reproducible
 * from its seed and its moves. The engine reads no clock and calls no
 * library random source; tests/g2048_lint.sh keeps it that way by a text
 * search, so prose in these files avoids the names it looks for.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PG2048_RNG_H
#define PG2048_RNG_H

#include <stdint.h>

/* Substituted for seed 0, which is the xorshift32 fixed point. */
#define G2048_RNG_ZERO_SEED 0x9E3779B9u

struct g2048_rng {
    uint32_t state;
};

void g2048_rng_seed(struct g2048_rng *rng, uint32_t seed);
/* Next 32-bit value; never 0. */
uint32_t g2048_rng_next(struct g2048_rng *rng);
/* Uniform value in [0, limit), unbiased by rejection. Limits 0 and 1 return
 * 0 without consuming a draw. */
uint32_t g2048_rng_below(struct g2048_rng *rng, uint32_t limit);

#endif
