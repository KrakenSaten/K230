/*
 * PocketTimber deterministic pseudo-random source.
 *
 * xorshift32 (Marsaglia), the same generator PocketFleet and PocketRadar
 * use and for the same reasons: small, auditable, one 32-bit word of state.
 * Quality is far more than a seat table needs; it is NOT suitable for
 * anything security related.
 *
 * The duplication of apps/radar/engine/radar_rng.c is deliberate. Apps do
 * not depend on one another and PocketOS has no shared first-party RNG in
 * core/; forty lines of self-contained arithmetic beside the engine that
 * uses it is cheaper than a platform change (ADR-002 fixed point 1: public
 * APIs are IPC contracts, not C headers between components).
 *
 * The stream is consumed in exactly two places: when a tower is built from
 * its seed, and when a collapse begins. Nothing draws from it during a pull,
 * a test or a placement, so what the player does never moves the stream and
 * no outcome is ever a roll of the dice. The engine reads no clock and calls
 * no library random source; tests/timber_lint.sh keeps it that way, by a
 * text search over the engine, so prose in these files avoids the names it
 * looks for.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETTIMBER_RNG_H
#define POCKETTIMBER_RNG_H

#include <stdint.h>

/* Substituted for seed 0, which is the xorshift32 fixed point. */
#define TIMBER_RNG_ZERO_SEED 0x9E3779B9u

struct timber_rng {
    uint32_t state;
};

void timber_rng_seed(struct timber_rng *rng, uint32_t seed);
/* Next 32-bit value; never 0. */
uint32_t timber_rng_next(struct timber_rng *rng);
/* Uniform value in [0, limit), unbiased by rejection. Returns 0 when limit
 * is 0. Limits of 0 and 1 are answered without consuming a draw, so adding
 * a degenerate choice to the engine cannot shift the stream. */
uint32_t timber_rng_below(struct timber_rng *rng, uint32_t limit);
/* Uniform value in [lo, hi] inclusive. Returns lo when hi < lo, and clamps
 * a span wider than 32 bits. */
int32_t timber_rng_range(struct timber_rng *rng, int32_t lo, int32_t hi);

#endif
