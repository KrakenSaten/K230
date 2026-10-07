/*
 * Wave's presets: named combinations of the settings one exchange needs.
 *
 * A preset is what a person picks instead of a speed and a handful of
 * numbers. It fixes, for both directions at once:
 *
 *   - the transmit profile (one of ggwave's three audible protocols);
 *   - how many copies of a message a send plays, one after the other;
 *   - how long a listen runs before the microphone turns itself off;
 *   - how long a capture records before it is decoded;
 *   - how long an identical message heard again counts as the same one
 *     (a second copy folds into the first history entry instead of adding
 *     a new one).
 *
 * The receiver needs no choice to hear a sender: ggwave's decoder listens
 * for all three audible protocols at once (wave_modem.cpp), so a STANDARD
 * listener decodes a ROBUST or a QUICK sender too. What a preset changes on
 * the receiving side is how long the microphone stays on and how repeats are
 * folded, not what can be heard.
 *
 * The first-party set is small on purpose, and the table is the only place
 * it is defined: a later user-defined preset is one more entry that passes
 * wave_preset_valid(). The id is what is stored (wave_store.c); the label is
 * what is shown. Ids are never reused for a different meaning.
 *
 * Pure C, no LVGL: tests/wave_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETWAVE_PRESET_H
#define POCKETWAVE_PRESET_H

#include "wave_protocol.h"

/* Longest preset id, terminator included. */
#define WAVE_PRESET_ID_MAX 16
/* Copies one send may play. Each copy is its own helper run, so the
 * amplifier and route go through the validated open/close sequence every
 * time; three copies of the longest message on the slowest speed are about
 * 21 s of sound. */
#define WAVE_PRESET_MAX_COPIES 3
/* The longest fold window: a repeat heard later than this is a new entry. */
#define WAVE_PRESET_MAX_DEDUPE_MS 60000

struct wave_preset {
    const char *id;       /* stable, stored: [a-z0-9_], < WAVE_PRESET_ID_MAX */
    const char *label;    /* shown on the preset button, upper case */
    const char *summary;  /* one short line: what it is for */
    enum wave_profile profile;
    int copies;           /* 1..WAVE_PRESET_MAX_COPIES */
    int listen_seconds;   /* 1..WAVE_LISTEN_SECONDS */
    int capture_seconds;  /* 1..WAVE_CAPTURE_MAX_SECONDS */
    int dedupe_ms;        /* 0..WAVE_PRESET_MAX_DEDUPE_MS */
};

/* The built-in presets, in the order the preset button cycles through. */
enum {
    WAVE_PRESET_STANDARD = 0,
    WAVE_PRESET_ROBUST,
    WAVE_PRESET_QUICK,
    WAVE_PRESET_BUILTIN_COUNT
};

#define WAVE_PRESET_DEFAULT WAVE_PRESET_STANDARD

int wave_preset_count(void);
/* NULL outside 0..count-1. */
const struct wave_preset *wave_preset_get(int index);
/* The index of the preset with this id, or -1. */
int wave_preset_find(const char *id);
/* The index after this one, wrapping; the default for an invalid index. */
int wave_preset_next(int index);
/* 1 when every field is inside its bounds and the id is a stored word. */
int wave_preset_valid(const struct wave_preset *p);
/* The speed word of a preset's profile for people: "NORMAL", "FAST" or
 * "FASTEST". */
const char *wave_preset_speed_label(const struct wave_preset *p);

#endif
