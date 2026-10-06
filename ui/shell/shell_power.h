/*
 * Power & Sleep in the running shell: the screen going off and the lock
 * coming down after the times power_policy.h decides from, and waking.
 *
 * The screen off is a black cover over everything (the system layer, above
 * the lock, the alerts and the debug overlay) that takes the first touch for
 * itself: the touch that wakes the screen does nothing else, and neither
 * does the key (shell_kbd's wake gate) or the keyboard action
 * (shell_power_gate) that wakes it. Under the cover nothing stops - this is
 * not sleep (power_policy.h).
 *
 * Idle time is LVGL's (lv_display_get_inactive_time): a touch or a key
 * delivered into the stream is activity, and so is every keyboard action
 * and every IPC request that acts like a person would (shell.c).
 *
 * The shell evaluates once a second (shell_power_tick). Nothing is due while
 * the shell's hold says something on screen keeps the device awake.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DOORS_SHELL_POWER_H
#define DOORS_SHELL_POWER_H

#include "lvgl.h"
#include "power_policy.h"

#include <stdbool.h>

struct shell_power_hooks {
    /* Something on screen keeps the device awake (an alarm ringing, an app
     * that plays or watches). */
    bool (*hold)(void);
    bool (*locked)(void);
    void (*lock)(const char *why);
    /* Something that must be seen now (an alarm ringing): it wakes the
     * screen even when the screen was put out by hand. May be NULL. */
    bool (*urgent)(void);
};

/* Read the stored timeouts (invalid values are logged and read as never)
 * and build the cover, hidden. Once, after the settings and the display. */
void shell_power_init(const struct shell_power_hooks *hooks);
/* Once a second. */
void shell_power_tick(void);

/* A person did something the shell saw outside the touch and key stream. */
void shell_power_activity(void);
/* For an input that must not act when it only woke the screen: true when
 * the screen was on (the input goes on), false when it was off - it is now
 * on, and the input stops here. Either way it counts as activity. */
bool shell_power_gate(void);
/* Wake now, if off. */
void shell_power_wake(const char *why);
/* Put the screen out now, by hand (the power key): the same cover, under
 * the same rules for waking. Not activity, and the lock is left to its own
 * timer (Lock after), which keeps counting under the cover. A hold already
 * in force when the screen went out by hand does not bring it straight
 * back - the owner turned it off over the video - but a hold that starts
 * afterwards does, and so does anything urgent. */
void shell_power_off_now(const char *why);
bool shell_power_screen_off(void);

/* The stored timeouts, seconds (0 never). set: one of the options
 * (power_policy.h) or -1 with nothing changed; persisted, applied at once. */
int shell_power_timeout(enum power_timer t);
int shell_power_set_timeout(enum power_timer t, int seconds);
uint32_t shell_power_idle_ms(void);
/* How many times the screen went off since start (shell.info, the tests). */
unsigned shell_power_off_count(void);

#endif
