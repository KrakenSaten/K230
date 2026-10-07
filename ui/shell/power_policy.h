/*
 * Power & Sleep: when the screen goes off and when the lock comes down, from
 * how long nobody has touched the device. The decisions only; the shell acts
 * on them (shell.c) and Settings shows and changes them (apps/settings).
 *
 * Three different things, never one:
 *
 *   SCREEN OFF  the panel shows black and the first touch or key only wakes
 *               it. Everything keeps running: apps tick, services answer,
 *               the radio receives, alarms ring. On the AMOLED a black pixel
 *               is a pixel that is off. The backlight level is not touched
 *               (docs/hardware/DISPLAY_BRIGHTNESS.md: what level 0 shows on
 *               this panel is UNKNOWN).
 *   LOCK        the existing lock screen (shell_lock.h) covers the apps; a
 *               swipe opens it. Not security, as before.
 *   SLEEP       system suspend. NOT offered: the kernel lists freeze/mem,
 *               but which devices can wake the board from it is unverified
 *               and there is no RTC to wake on a timer (DS §52.4). Nothing
 *               here suspends anything.
 *
 * Both timeouts default to never, so a unit that has never been told
 * otherwise behaves as it always did. Stored in settings.conf as whole
 * seconds, one of the listed options and nothing else: a value this code
 * never wrote is not trusted to mean what it seems to (the brightness rule).
 *
 * Pure C, no LVGL: tests/power_policy_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_POWER_POLICY_H
#define DOORS_POWER_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* settings.conf keys, seconds; 0 (or absent) is never. */
#define POWER_SCREEN_SETTING "screen_off_s"
#define POWER_LOCK_SETTING "auto_lock_s"

enum power_timer {
    POWER_TIMER_SCREEN = 0, /* screen off after */
    POWER_TIMER_LOCK,       /* lock after */
    POWER_TIMER_COUNT
};

/* The options of a timer in the order a stepper walks them, never first. */
int power_option_count(enum power_timer t);
int power_option_at(enum power_timer t, int i);
/* The index of seconds among the options, or -1. */
int power_option_index(enum power_timer t, int seconds);
/* The option one step from seconds (dir < 0 shorter, > 0 longer, where
 * "never" is the longest), clamped at the ends; a value that is not an
 * option goes to the first option. */
int power_option_step(enum power_timer t, int seconds, int dir);
/* "Never", "30 s", "1 min", "30 min". */
void power_option_label(int seconds, char *out, size_t n);

/* A stored value: a plain decimal that is one of t's options. Returns 0 and
 * sets *seconds, or -1 (anything else, including empty). */
int power_parse_setting(enum power_timer t, const char *s, int *seconds);

struct power_policy {
    int screen_off_s; /* 0: never */
    int lock_s;       /* 0: never */
};

/* What is due now. */
#define POWER_DO_SCREEN_OFF 1u
#define POWER_DO_LOCK 2u

/* idle_ms: since the last touch or key. hold: something on screen keeps the
 * device awake (an alarm ringing, a video playing). Returns the actions due
 * that have not happened yet; nothing while held. */
unsigned power_policy_due(const struct power_policy *p, uint32_t idle_ms, bool hold, bool screen_off,
                          bool locked);

#endif
