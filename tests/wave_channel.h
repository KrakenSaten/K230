/*
 * A simulated air path for Wave's host simulator (tests/wave_sim_test.c):
 * what can happen to a waveform between one board's speaker and another's
 * microphone, applied to mono S16 samples in memory.
 *
 * Deterministic: every impairment that is random draws from the channel's
 * own seeded generator, so a failing run is the same run next time.
 *
 * This models the path coarsely on purpose - gain, noise, a hum, clipping,
 * a DC offset, lost samples, a late start - enough to exercise the decoder
 * and everything after it without hardware. It does not model a room's
 * echo, the speaker's or the microphone's frequency response, or clock drift
 * between two boards; those stay hardware tests (docs/apps/WAVE.md).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WAVE_CHANNEL_H
#define WAVE_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

struct wave_channel {
    uint32_t seed;        /* the generator's state; nonzero */
    int gain_pct;         /* 100 = unchanged */
    int noise;            /* white noise amplitude, 0..32767 */
    int hum;              /* amplitude of a 50 Hz hum */
    int dc;               /* offset added to every sample */
    int clip;             /* |sample| limit after everything, 0 = 32767 */
    int drop_every;       /* zero one sample in this many, 0 = none */
};

void wave_channel_init(struct wave_channel *c, uint32_t seed);

/* Apply the channel to n samples from in, writing out (which may be in).
 * pos is the running sample position, so a hum stays continuous across
 * calls. */
void wave_channel_apply(struct wave_channel *c, const int16_t *in, int16_t *out, size_t n,
                        uint64_t pos);

/* The channel's own noise (and hum) with no signal: n samples of "a room". */
void wave_channel_room(struct wave_channel *c, int16_t *out, size_t n, uint64_t pos);

/* Largest absolute sample in buf. */
int wave_channel_peak(const int16_t *buf, size_t n);

#endif
