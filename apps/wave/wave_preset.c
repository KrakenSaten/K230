/*
 * Wave's presets. See wave_preset.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_preset.h"

#include <string.h>

/* The listen and capture lengths are the bounds a person can reason about:
 * 120 s is the privacy bound every listen already had; a capture is long
 * enough for the longest message the preset's sender can make, copies
 * included, with a few seconds to press the other device's button.
 *
 *   STANDARD  FAST, once.   64 bytes take 4.7 s; capture 10 s.
 *   ROBUST    NORMAL, twice. 64 bytes take 6.6 s a copy; capture 20 s.
 *   QUICK     FASTEST, once. 64 bytes take 2.7 s; capture 6 s. */
static const struct wave_preset builtin[WAVE_PRESET_BUILTIN_COUNT] = {
    [WAVE_PRESET_STANDARD] = {
        .id = "standard",
        .label = "STANDARD",
        .summary = "Fast speed, sent once",
        .profile = WAVE_PROFILE_FAST,
        .copies = 1,
        .listen_seconds = WAVE_LISTEN_SECONDS,
        .capture_seconds = 10,
        .dedupe_ms = 10000,
    },
    [WAVE_PRESET_ROBUST] = {
        .id = "robust",
        .label = "ROBUST",
        .summary = "Slowest speed, sent twice",
        .profile = WAVE_PROFILE_NORMAL,
        .copies = 2,
        .listen_seconds = WAVE_LISTEN_SECONDS,
        .capture_seconds = 20,
        .dedupe_ms = 20000,
    },
    [WAVE_PRESET_QUICK] = {
        .id = "quick",
        .label = "QUICK",
        .summary = "Fastest speed, close range",
        .profile = WAVE_PROFILE_FASTEST,
        .copies = 1,
        .listen_seconds = WAVE_LISTEN_SECONDS,
        .capture_seconds = 6,
        .dedupe_ms = 10000,
    },
};

static const char *const speed_labels[WAVE_PROFILE_COUNT] = WAVE_PROFILE_LABELS;

int wave_preset_count(void)
{
    return WAVE_PRESET_BUILTIN_COUNT;
}

const struct wave_preset *wave_preset_get(int index)
{
    return index >= 0 && index < wave_preset_count() ? &builtin[index] : NULL;
}

int wave_preset_find(const char *id)
{
    int i;

    for (i = 0; id && i < wave_preset_count(); i++) {
        if (strcmp(builtin[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int wave_preset_next(int index)
{
    if (index < 0 || index >= wave_preset_count()) {
        return WAVE_PRESET_DEFAULT;
    }
    return (index + 1) % wave_preset_count();
}

static int stored_word(const char *w)
{
    size_t n = 0;

    for (; w && *w; w++, n++) {
        if (!((*w >= 'a' && *w <= 'z') || (*w >= '0' && *w <= '9') || *w == '_')) {
            return 0;
        }
    }
    return w && n > 0 && n < WAVE_PRESET_ID_MAX;
}

int wave_preset_valid(const struct wave_preset *p)
{
    return p && stored_word(p->id) && p->label && *p->label && p->summary &&
           (unsigned)p->profile < WAVE_PROFILE_COUNT && p->copies >= 1 &&
           p->copies <= WAVE_PRESET_MAX_COPIES && p->listen_seconds >= 1 &&
           p->listen_seconds <= WAVE_LISTEN_SECONDS && p->capture_seconds >= 1 &&
           p->capture_seconds <= WAVE_CAPTURE_MAX_SECONDS && p->dedupe_ms >= 0 &&
           p->dedupe_ms <= WAVE_PRESET_MAX_DEDUPE_MS;
}

const char *wave_preset_speed_label(const struct wave_preset *p)
{
    return p && (unsigned)p->profile < WAVE_PROFILE_COUNT ? speed_labels[p->profile] : "";
}
