/*
 * PocketTimber pull model. See timber_pull.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_pull.h"

int32_t timber_pull_tightness(int seat, int load)
{
    int32_t tight;
    int32_t factor;

    if (seat < 0) {
        seat = 0;
    }
    if (seat > 255) {
        seat = 255;
    }
    if (load < 0) {
        load = 0;
    }
    if (load > TIMBER_LOAD_MAX) {
        load = TIMBER_LOAD_MAX;
    }
    /* (255 - seat) / 255 in Q8.8, rounded to nearest so seat 0 is exactly
     * 256, seat 255 exactly 0 and seat 128 exactly 128. */
    tight = ((255 - seat) * 256 + 128) / 255;
    /* 0.5 + 0.5 * load / max, Q8.8. */
    factor = 128 + (128 * load) / TIMBER_LOAD_MAX;
    return (tight * factor) >> 8;
}

enum timber_class timber_pull_class(int seat, int load)
{
    int32_t t = timber_pull_tightness(seat, load);

    if (t < TIMBER_TIGHT_EASY_FROM) {
        return TIMBER_CLASS_FREE;
    }
    if (t < TIMBER_TIGHT_FIRM_FROM) {
        return TIMBER_CLASS_EASY;
    }
    if (t < TIMBER_TIGHT_STUCK_FROM) {
        return TIMBER_CLASS_FIRM;
    }
    return TIMBER_CLASS_STUCK;
}

int32_t timber_pull_limit(enum timber_class cls)
{
    switch (cls) {
    case TIMBER_CLASS_FREE:
        return TIMBER_SPEED_FREE;
    case TIMBER_CLASS_EASY:
        return TIMBER_SPEED_EASY;
    case TIMBER_CLASS_FIRM:
        return TIMBER_SPEED_FIRM;
    case TIMBER_CLASS_STUCK:
        return TIMBER_SPEED_STUCK;
    default:
        /* An unknown class is treated as the tightest, never the loosest:
         * a bug here should cost the player a jolt, not hand out a free
         * pull. */
        return TIMBER_SPEED_STUCK;
    }
}

int32_t timber_pull_stiction(enum timber_class cls)
{
    switch (cls) {
    case TIMBER_CLASS_FIRM:
        return TIMBER_STICTION_FIRM;
    case TIMBER_CLASS_STUCK:
        return TIMBER_STICTION_STUCK;
    case TIMBER_CLASS_FREE:
    case TIMBER_CLASS_EASY:
        return 0;
    default:
        return TIMBER_STICTION_STUCK;
    }
}

int32_t timber_pull_jolt(int32_t excess, int32_t tightness)
{
    if (excess <= 0) {
        return 0;
    }
    if (tightness < 0) {
        tightness = 0;
    }
    if (tightness > 256) {
        tightness = 256;
    }
    /* excess * (0.25 + tightness), Q8.8 in and out. */
    return (int32_t)(((int64_t)excess * (64 + tightness)) >> 8);
}
