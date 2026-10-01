/*
 * The DOORS lock screen (DS §31.4).
 *
 * What it is for: the device's identity at rest, the time at a glance, and
 * keeping a pocket from opening apps. What it is not: security. There is no
 * code, and nothing here claims there is - anyone holding the device can
 * open it with one swipe. Every service keeps running underneath; the lock
 * only covers the screen and takes the input.
 *
 * Engaged at every cold start of the shell and on request (the launcher's
 * Lock action, Controls, shell.lock over IPC). Not engaged when the shell
 * restarts itself to apply a rotation: that is the same session carrying on.
 *
 * Opening: a swipe up (the content follows the finger, and a short drag
 * springs back), Enter or Space on a keyboard, or shell.unlock over IPC.
 * A tap alone only brightens the hint. Opening plays the package's door
 * sequence - the closed door gives way to the open one, which gives way to
 * whatever is underneath - in about 0.8 s, or at once with reduced motion.
 *
 * The lock sits above the apps, the launcher and the touch keyboard and
 * below the status bar and the alarm alert, so the bar stays readable and an
 * alarm can still be stopped without opening the device.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_SHELL_LOCK_H
#define DOORS_SHELL_LOCK_H

#include "lvgl.h"

#include <stdbool.h>

struct shell_lock_hooks {
    /* Called when the lock is fully engaged, and when it has fully opened
     * (after the transition). Either may be NULL. */
    void (*engaged)(void);
    void (*opened)(void);
    /* Called once per opening, when what is under the lock starts to show
     * through it: the open door beginning to fade, or at once when there is
     * no open door to show. The lock is still locked. May be NULL. */
    void (*revealing)(void);
};

/* Build the (hidden) lock over screen, for this run's orientation. */
void shell_lock_create(lv_obj_t *screen, bool landscape, const struct shell_lock_hooks *hooks);

/* Cover the screen now. why is for the log. A lock already engaged stays. */
void shell_lock_engage(const char *why);

/* Open. animate=false skips the door sequence (IPC, tests, reduced motion
 * does it anyway). A lock already open, or opening, stays as it is. */
void shell_lock_open(bool animate, const char *why);

/* Engaged or still opening: the device is not in the owner's hands yet. */
bool shell_lock_is_locked(void);
/* The door sequence is running. */
bool shell_lock_is_opening(void);
/* Opening, and far enough along that what is under the lock shows through. */
bool shell_lock_is_revealing(void);

/* The time and date the lock shows (the shell's tick). */
void shell_lock_set_time(const char *hm, const char *date);

/* How far a drag must travel upward before letting go opens the lock. */
int32_t shell_lock_open_distance(void);

/* Simulator tests only: park the door sequence on the open door, so the
 * transition's middle can be captured. The shell calls it for
 * POCKETOS_TEST_LOCK_HOLD=door in builds with test hooks. */
void shell_lock_test_hold_at_door(bool hold);

/* Counters for shell.info and the tests. */
unsigned shell_lock_engage_count(void);
unsigned shell_lock_open_count(void);

#endif
