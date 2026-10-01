/*
 * The keyboard base's backlight, as a percentage.
 *
 * The mechanism (vendor launcher, k230_phone_ui ui_hardware.c, DOCUMENTED):
 * GPIO52 muxed to PWM4 drives the keys' light. PWM4 is channel 1 of the
 * pwmchip whose device-tree node is `pwm3_5` (pwmchip3 on the Doors image,
 * VERIFIED present on units A and B). The vendor writes, in this order:
 * enable 0, duty 0, period 20000 ns (50 kHz), polarity "inversed" (the
 * driver refuses "normal"), duty = period * (100 - pct) / 100, enable 1.
 * With the inverted polarity a duty of the whole period is dark and 0 is
 * full light.
 *
 * The pin mux is not here: it lives in the one file that maps the iomux
 * block (kbd_bus_k230_light_mux()). This file is pure sysfs, and the sysfs
 * root is a parameter, so tests/kbd_light_test.c runs it against a fake tree.
 *
 * Levels are 0..100 in steps of 10 (hw_actions.h HW_KBD_LIGHT_*), 0 is off,
 * and the shell keeps the level in settings.conf as `keyboard_backlight`.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_KBD_LIGHT_H
#define POCKETOS_KBD_LIGHT_H

#define KBD_LIGHT_PERIOD_NS 20000
#define KBD_LIGHT_OF_NODE "pwm3_5"
#define KBD_LIGHT_CHANNEL 1

struct kbd_light {
    int supported;
    int percent;    /* the level last applied, -1 before any */
    char dir[320];  /* .../pwmchipN/pwm1 */
    char chip[256]; /* .../pwmchipN */
};

/* Find the PWM under <sysfs_root>/class/pwm. 0 when found, -1 when this
 * board has none (the simulator, a board without the base's light). */
int kbd_light_probe(struct kbd_light *l, const char *sysfs_root);

/* Apply a level (clamped to 0..100, rounded to the step). Returns the level
 * applied, or -1 with errno when a write failed or there is no PWM. */
int kbd_light_set(struct kbd_light *l, int percent);

/* The stored value: an integer 0..100 that is a multiple of 10. 0, or -1. */
int kbd_light_parse(const char *s, int *out);

#endif
