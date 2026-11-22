/*
 * Card games: deterministic pseudo-random source.
 *
 * xorshift32, as the other games use. Its use here is shuffling, so a deal is
 * a function of its seed and the player's choices. NOT suitable for anything
 * security related - nor for real-money play, which none of the games is.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_RNG_H
#define CG_RNG_H

#include <stdint.h>

#define CG_RNG_ZERO_SEED 0x9E3779B9u

struct cg_rng {
    uint32_t state;
};

void cg_rng_seed(struct cg_rng *rng, uint32_t seed);
uint32_t cg_rng_next(struct cg_rng *rng);
/* Uniform in [0, limit), unbiased by rejection; 0 for limits 0 and 1
 * without consuming a draw. */
uint32_t cg_rng_below(struct cg_rng *rng, uint32_t limit);

#endif
