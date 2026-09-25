/*
 * System volume: how loud the device plays, and whether it plays at all.
 *
 * The mechanism (docs/hardware/AUDIO_HARDWARE_MAP_2026-09-13.md §7): the
 * built-in speaker is a MAX98357A on the SoC's I2S pads, with no mixer volume
 * in its path - `PCM Playback Volume` belongs to the codec's headphone
 * output, which nothing plays to. So the volume is a digital gain applied by
 * pocketaudio to every sample it plays (options.volume_percent,
 * pocketaudio_volume_gain_q15): 100 % is the level validated on unit A
 * (-12 dBFS ceiling), 10 % is -27 dB below it. The only program that plays
 * sound is pos-wave, started by the Wave app, which passes the level on.
 * Muted, the Wave app does not start a send at all.
 *
 * The shell owns the setting, as it owns the brightness: two keys in
 * settings.conf, `audio_volume` (10..100, step 10, default 100) and
 * `audio_muted` (0|1, default 0). A stored value this code could not have
 * written is logged and replaced by the default in memory, never in the file.
 *
 * Whether there is anything to play to is a read of
 * <proc_root>/asound/cards: a board without a sound card shows the control
 * as unavailable rather than pretending.
 *
 * Pure C, no LVGL, no ALSA: unit-tested natively (tests/volume_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VOLUME_H
#define POCKETOS_VOLUME_H

#define VOLUME_MIN_PCT 10
#define VOLUME_MAX_PCT 100
#define VOLUME_STEP_PCT 10
#define VOLUME_DEFAULT_PCT 100
#define VOLUME_SETTING "audio_volume"
#define VOLUME_MUTED_SETTING "audio_muted"

struct volume_state {
    int percent; /* VOLUME_MIN_PCT..VOLUME_MAX_PCT */
    int muted;   /* 0 or 1 */
};

/* Exactly an integer VOLUME_MIN_PCT..VOLUME_MAX_PCT that is a multiple of
 * VOLUME_STEP_PCT. 0, or -1 for anything else. */
int volume_parse_percent(const char *s, int *out);
/* Exactly "0" or "1". */
int volume_parse_muted(const char *s, int *out);

/* The state from the two stored strings (either may be NULL for unset).
 * Returns a bit mask of the values that were present but unusable
 * (1: percent, 2: muted), which the caller logs; those fall back to the
 * defaults. */
int volume_load(struct volume_state *v, const char *stored_percent, const char *stored_muted);

/* A slider position to the value stored: rounded to the step, clamped into
 * VOLUME_MIN_PCT..VOLUME_MAX_PCT. */
int volume_round(int percent);

/* What playback uses: 0 while muted, else the percentage. */
int volume_effective(const struct volume_state *v);

/* 1 when the kernel lists at least one sound card under
 * <proc_root>/asound/cards ("/proc" in use), 0 when it lists none or the
 * file is absent. */
int volume_output_present(const char *proc_root);

#endif
