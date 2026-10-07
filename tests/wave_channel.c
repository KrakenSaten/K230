/*
 * The simulated air path. See wave_channel.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_channel.h"

#include <math.h>
#include <string.h>

#define RATE 48000.0

void wave_channel_init(struct wave_channel *c, uint32_t seed)
{
    memset(c, 0, sizeof(*c));
    c->seed = seed ? seed : 1;
    c->gain_pct = 100;
}

static uint32_t next(struct wave_channel *c)
{
    c->seed ^= c->seed << 13;
    c->seed ^= c->seed >> 17;
    c->seed ^= c->seed << 5;
    return c->seed;
}

static int16_t one(struct wave_channel *c, int32_t signal, uint64_t pos)
{
    int32_t limit = c->clip > 0 && c->clip < 32767 ? c->clip : 32767;
    int32_t v = signal * c->gain_pct / 100;

    if (c->noise > 0) {
        v += (int32_t)(next(c) % (uint32_t)(2 * c->noise + 1)) - c->noise;
    }
    if (c->hum > 0) {
        v += (int32_t)lrint(c->hum * sin(2.0 * M_PI * 50.0 * (double)pos / RATE));
    }
    v += c->dc;
    if (c->drop_every > 0 && next(c) % (uint32_t)c->drop_every == 0) {
        v = 0;
    }
    if (v > limit) {
        v = limit;
    }
    if (v < -limit) {
        v = -limit;
    }
    return (int16_t)v;
}

void wave_channel_apply(struct wave_channel *c, const int16_t *in, int16_t *out, size_t n,
                        uint64_t pos)
{
    size_t i;

    for (i = 0; i < n; i++) {
        out[i] = one(c, in[i], pos + i);
    }
}

void wave_channel_room(struct wave_channel *c, int16_t *out, size_t n, uint64_t pos)
{
    size_t i;

    for (i = 0; i < n; i++) {
        out[i] = one(c, 0, pos + i);
    }
}

int wave_channel_peak(const int16_t *buf, size_t n)
{
    int p = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        int v = buf[i] < 0 ? -(int)buf[i] : buf[i];

        if (v > p) {
            p = v;
        }
    }
    return p;
}
