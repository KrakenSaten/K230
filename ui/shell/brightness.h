/*
 * Display brightness: the panel's backlight-class device, as a percentage.
 *
 * The mechanism on the T-Display K230 (docs/hardware/DISPLAY_BRIGHTNESS.md):
 * the RM69A10 panel driver registers /sys/class/backlight/rm69a10, and a
 * write to its `brightness` attribute becomes one MIPI DCS 0x51 command with
 * an 8-bit level (max_brightness 255, 254 at boot). There is no PWM and no
 * GPIO in the path. Nothing here is K230-specific, though: any Linux
 * backlight-class device is driven the same way, and a board without one
 * (the HDMI variant, the simulator) is reported as unsupported rather than
 * guessed at.
 *
 * Why a floor: level 0 sends `0x51 00` and nothing else - the panel stays
 * powered and the shell keeps drawing, but the picture may be too dark to
 * find the control that brings it back. So a percentage below
 * BRIGHTNESS_MIN_PCT is never written, whatever asks for it. 10 % is raw 26
 * of 255, above the vendor launcher's own floor of 20; the lowest level that
 * is still readable on the AMOLED is unmeasured and the floor should only
 * move once it has been measured on the panel.
 *
 * Pure C, no LVGL, and the sysfs root is a parameter, so the whole of this
 * is unit-tested against a fake tree (tests/brightness_test.c). The shell
 * owns the one instance, because the shell owns the panel (ADR-002).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_BRIGHTNESS_H
#define POCKETOS_BRIGHTNESS_H

#include <stddef.h>

#define BRIGHTNESS_MIN_PCT 10
#define BRIGHTNESS_MAX_PCT 100
/* The step the Settings control moves by. */
#define BRIGHTNESS_STEP_PCT 10
/* A device with fewer levels than this is an on/off switch, not a
 * brightness control, and is reported as unsupported. */
#define BRIGHTNESS_MIN_LEVELS 10
/* Settings key (settings.conf), holding the percentage. */
#define BRIGHTNESS_SETTING "display_brightness"

struct brightness {
    int supported;
    int max_raw;
    char name[64];        /* device directory name, e.g. "rm69a10" */
    char path[384];       /* .../brightness */
};

/* Find the backlight device under <sysfs_root>/class/backlight. The first
 * entry in name order with a readable max_brightness of at least
 * BRIGHTNESS_MIN_LEVELS and a brightness attribute wins. Returns 0 when one
 * was found, 1 when there is none (b->supported 0). */
int brightness_probe(struct brightness *b, const char *sysfs_root);

/* The current level as a percentage, or -1 when unsupported or unreadable.
 * A level below the floor (set by something else, or 0) reads as what it
 * is, not as the floor: the UI shows the truth and the next step clamps. */
int brightness_get_percent(const struct brightness *b);

/* Clamp pct into [BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT] and write it.
 * Returns the percentage applied, or -1 (errno set) when unsupported or the
 * write failed. */
int brightness_set_percent(const struct brightness *b, int pct);

int brightness_clamp_percent(int pct);
/* max_raw > 0. pct is clamped first, and a non-zero percentage never maps
 * to raw 0. */
int brightness_percent_to_raw(int pct, int max_raw);
int brightness_raw_to_percent(int raw, int max_raw);

/* A stored setting: a plain decimal integer in [BRIGHTNESS_MIN_PCT,
 * BRIGHTNESS_MAX_PCT] and nothing else. Anything else - empty, signs,
 * spaces, trailing junk, out of range - is invalid (-1) and is not
 * "corrected" into range: a value this code never wrote is not trusted to
 * mean what it seems to. */
int brightness_parse_setting(const char *s, int *pct);

#endif
