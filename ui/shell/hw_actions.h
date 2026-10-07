/*
 * Hardware actions: what the keyboard base's own keys mean to Doors.
 *
 * The function row, the orange microphone key and the LILYGO key are real
 * keys that type nothing (pos_keymap.h reports them as reserved). This layer
 * gives each a semantic action - Home, Settings, a volume step, Terminal -
 * and runs it through a small host interface the shell fills in with the
 * paths it already has: app_open() for an app, the back slab's path for
 * Back, the shell's own brightness and volume setters. Nothing here opens a
 * window, a device or a file, so the whole mapping and every rule about
 * bounds, duplicates, a missing app and the lock is tested on the host
 * against a fake host (tests/hw_actions_test.c).
 *
 * Two layers, deliberately:
 *   physical event -> action   hw_action_for_key(): a table, nothing else
 *   action -> effect           hw_action_run(): the rules, through the host
 * so the key driver never launches anything, and an action has one meaning
 * whether a key, the bench (shell.action) or a later source asked for it.
 *
 * The default mapping follows the vendor launcher's own hotkey table
 * (docs/hardware/HARDWARE_CONTROLS.md §2), with Doors' apps where the
 * vendor had its own: F9 opens RIFT where the vendor opened Meshtastic.
 *
 * Pure C, no LVGL. Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_HW_ACTIONS_H
#define POCKETOS_HW_ACTIONS_H

#include <stdbool.h>
#include <stdint.h>

enum hw_action {
    HW_ACTION_NONE = 0,
    HW_ACTION_HOME,
    HW_ACTION_BACK,
    HW_ACTION_SETTINGS,
    HW_ACTION_TERMINAL,
    HW_ACTION_WAVE,
    HW_ACTION_VISION,
    HW_ACTION_RIFT,
    HW_ACTION_SCREENSHOT,
    HW_ACTION_VOLUME_DOWN,
    HW_ACTION_VOLUME_UP,
    HW_ACTION_BRIGHTNESS_DOWN,
    HW_ACTION_BRIGHTNESS_UP,
    HW_ACTION_KBD_LIGHT_DOWN,
    HW_ACTION_KBD_LIGHT_UP,
    HW_ACTION_COUNT
};

/* What running an action came to. */
enum hw_result {
    HW_RESULT_DONE = 0,     /* it happened */
    HW_RESULT_NOOP,         /* nothing to do: already there, or at a bound */
    HW_RESULT_REFUSED,      /* not while the lock screen is up */
    HW_RESULT_UNAVAILABLE   /* this build or board has no such thing */
};

/* The keyboard light, in settings.conf as `keyboard_backlight`: 0 (off) to
 * 100, steps of 10. Off is a level here, unlike the display, whose floor
 * keeps the panel readable. */
#define HW_KBD_LIGHT_MIN_PCT 0
#define HW_KBD_LIGHT_MAX_PCT 100
#define HW_KBD_LIGHT_STEP_PCT 10
#define HW_KBD_LIGHT_SETTING "keyboard_backlight"

/* The action's name ("home", "volume_up", ...), or NULL out of range. The
 * names are the shell.action vocabulary (docs/api/shell.md). */
const char *hw_action_name(enum hw_action a);
/* The action a name stands for. 0, or -1 for a name that is none. */
int hw_action_parse(const char *name, enum hw_action *out);
const char *hw_result_name(enum hw_result r);

/* The action a keyboard-base matrix code carries on its press, or
 * HW_ACTION_NONE for a key that types. */
enum hw_action hw_action_for_key(uint8_t code);
/* 1..11 when the code is F1..F11, else 0. */
int hw_action_fkey(uint8_t code);

/* The shell's side. Every member may be NULL, which the action it serves
 * reports as unavailable. */
struct hw_action_host {
    void *ctx;
    /* The app on screen, or NULL at the launcher. */
    const char *(*current_app)(void *ctx);
    /* Open an installed app through the normal launch path. -1 when there is
     * no such app in this build. */
    int (*open_app)(void *ctx, const char *id);
    /* The launcher's own page: whatever is open closes, as the back slab and
     * shell.home do. */
    void (*home)(void *ctx);
    /* One level back (the shell's Back rules, shell.c). HW_RESULT_NOOP when
     * there was nothing to go back from. */
    enum hw_result (*back)(void *ctx);
    /* True while the lock screen covers the device. */
    bool (*locked)(void *ctx);
    /* Levels, in percent; get returns -1 when the control does not exist
     * here, set returns the level applied or -1. */
    int (*volume_get)(void *ctx);
    int (*volume_set)(void *ctx, int pct);
    int (*brightness_get)(void *ctx);
    int (*brightness_set)(void *ctx, int pct);
    int (*kbd_light_get)(void *ctx);
    int (*kbd_light_set)(void *ctx, int pct);
    /* Start a screenshot; 0 when one was started. */
    int (*screenshot)(void *ctx);
};

/* The next level from cur in direction dir (-1 or +1) on a grid of step
 * between lo and hi. Never leaves [lo, hi] and never wraps; a level off the
 * grid (a panel booted at 99 %) moves to the next grid point in that
 * direction. Returns cur itself when it is already at the bound. */
int hw_step(int cur, int dir, int lo, int hi, int step);

/* Run one action. *value, when given, receives the level after a level
 * action (else -1), for the on-screen confirmation. */
enum hw_result hw_action_run(const struct hw_action_host *h, enum hw_action a, int *value);

#endif
