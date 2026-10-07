/*
 * PocketRadar deterministic pseudo-random source.
 *
 * xorshift32 (Marsaglia), the same generator PocketFleet uses and for the
 * same reasons: small, auditable, and one 32-bit word of state. Quality is
 * far more than a target generator needs; it is NOT suitable for anything
 * security related.
 *
 * The duplication of apps/fleet/engine/fleet_rng.c is deliberate. Apps do
 * not depend on one another and PocketOS has no shared first-party RNG in
 * core/; forty lines of self-contained arithmetic beside the engine that
 * uses it is cheaper than a platform change (ADR-002 fixed point 1: public
 * APIs are IPC contracts, not C headers between components).
 *
 * Every random decision in a PocketRadar run draws from one of these in a
 * fixed order, which is what makes a run reproducible from its seed and its
 * recorded player actions. The engine reads no clock and calls no library
 * random source; tests/radar_lint.sh keeps it that way, by a text search
 * over the engine, so prose in these files avoids the names it looks for.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETRADAR_RNG_H
#define POCKETRADAR_RNG_H

#include <stdint.h>

/* Substituted for seed 0, which is the xorshift32 fixed point. */
#define RADAR_RNG_ZERO_SEED 0x9E3779B9u

struct radar_rng {
    uint32_t state;
};

void radar_rng_seed(struct radar_rng *rng, uint32_t seed);
/* Next 32-bit value; never 0. */
uint32_t radar_rng_next(struct radar_rng *rng);
/* Uniform value in [0, limit), unbiased by rejection. Returns 0 when limit
 * is 0. Limits of 0 and 1 are answered without consuming a draw, so adding
 * a degenerate choice to the engine cannot shift the stream. */
uint32_t radar_rng_below(struct radar_rng *rng, uint32_t limit);
/* Uniform value in [lo, hi] inclusive. Returns lo when hi < lo, and clamps
 * a span wider than 32 bits. */
int32_t radar_rng_range(struct radar_rng *rng, int32_t lo, int32_t hi);

#endif
