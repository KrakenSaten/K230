/*
 * Hardware actions. See hw_actions.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "hw_actions.h"
#include "brightness.h"
#include "volume.h"

#include <stddef.h>
#include <string.h>

static const char *const names[HW_ACTION_COUNT] = {
    [HW_ACTION_NONE] = "none",
    [HW_ACTION_HOME] = "home",
    [HW_ACTION_BACK] = "back",
    [HW_ACTION_SETTINGS] = "settings",
    [HW_ACTION_TERMINAL] = "terminal",
    [HW_ACTION_WAVE] = "wave",
    [HW_ACTION_VISION] = "vision",
    [HW_ACTION_RIFT] = "rift",
    [HW_ACTION_SCREENSHOT] = "screenshot",
    [HW_ACTION_VOLUME_DOWN] = "volume_down",
    [HW_ACTION_VOLUME_UP] = "volume_up",
    [HW_ACTION_BRIGHTNESS_DOWN] = "brightness_down",
    [HW_ACTION_BRIGHTNESS_UP] = "brightness_up",
    [HW_ACTION_KBD_LIGHT_DOWN] = "keyboard_light_down",
    [HW_ACTION_KBD_LIGHT_UP] = "keyboard_light_up",
};

const char *hw_action_name(enum hw_action a)
{
    return (unsigned)a < HW_ACTION_COUNT ? names[a] : NULL;
}

int hw_action_parse(const char *name, enum hw_action *out)
{
    unsigned k;

    if (!name) {
        return -1;
    }
    for (k = 1; k < HW_ACTION_COUNT; k++) {
        if (strcmp(names[k], name) == 0) {
            if (out) {
                *out = (enum hw_action)k;
            }
            return 0;
        }
    }
    return -1;
}

const char *hw_result_name(enum hw_result r)
{
    switch (r) {
    case HW_RESULT_DONE: return "done";
    case HW_RESULT_NOOP: return "noop";
    case HW_RESULT_REFUSED: return "refused";
    case HW_RESULT_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}

/* The function row's matrix codes, F1 first. They are the vendor's
 * (tca8418_key_name, ui_hardware.c) and pos_keymap.c's; the row is wired
 * right to left, which is why the numbers run backwards from F4. */
static const uint8_t fkey_code[11] = { 50, 60, 59, 68, 67, 66, 65, 64, 63, 62, 61 };

/* F1..F11 in order. The vendor launcher's defaults (keyboard_hotkey table,
 * ui_hardware.c) with Doors' own apps: F9 is RIFT, where the vendor opened
 * its Meshtastic page. */
static const enum hw_action fkey_action[11] = {
    HW_ACTION_HOME,           HW_ACTION_SETTINGS,        HW_ACTION_KBD_LIGHT_DOWN, HW_ACTION_KBD_LIGHT_UP,
    HW_ACTION_VOLUME_DOWN,    HW_ACTION_VOLUME_UP,       HW_ACTION_SCREENSHOT,     HW_ACTION_TERMINAL,
    HW_ACTION_RIFT,           HW_ACTION_BRIGHTNESS_DOWN, HW_ACTION_BRIGHTNESS_UP,
};

#define CODE_LILYGO 8
#define CODE_MIC 11

int hw_action_fkey(uint8_t code)
{
    int k;

    for (k = 0; k < 11; k++) {
        if (fkey_code[k] == code) {
            return k + 1;
        }
    }
    return 0;
}

enum hw_action hw_action_for_key(uint8_t code)
{
    int f = hw_action_fkey(code);

    if (f) {
        return fkey_action[f - 1];
    }
    switch (code) {
    case CODE_MIC: return HW_ACTION_WAVE;
    case CODE_LILYGO: return HW_ACTION_TERMINAL;
    default: return HW_ACTION_NONE;
    }
}

int hw_step(int cur, int dir, int lo, int hi, int step)
{
    int next;

    if (step <= 0 || lo > hi) {
        return cur;
    }
    if (cur < lo) {
        return lo;
    }
    if (cur > hi) {
        return hi;
    }
    if (dir > 0) {
        /* The next grid point strictly above cur. */
        next = lo + ((cur - lo) / step + 1) * step;
        return next > hi ? hi : next;
    }
    if (dir < 0) {
        /* The next grid point strictly below cur. */
        next = lo + ((cur - lo + step - 1) / step - 1) * step;
        return next < lo ? lo : next;
    }
    return cur;
}

static const char *app_for(enum hw_action a)
{
    switch (a) {
    case HW_ACTION_SETTINGS: return "settings";
    case HW_ACTION_TERMINAL: return "terminal";
    case HW_ACTION_WAVE: return "wave";
    case HW_ACTION_VISION: return "vision";
    case HW_ACTION_RIFT: return "rift";
    default: return NULL;
    }
}

static enum hw_result open_app(const struct hw_action_host *h, const char *id)
{
    const char *cur = h->current_app ? h->current_app(h->ctx) : NULL;

    if (cur && strcmp(cur, id) == 0) {
        /* Already on screen. Opening it again would close it first
         * (app_open), losing what it holds: a Terminal's shell, Vision's
         * camera session. One instance, always. */
        return HW_RESULT_NOOP;
    }
    if (!h->open_app || h->open_app(h->ctx, id) < 0) {
        return HW_RESULT_UNAVAILABLE;
    }
    return HW_RESULT_DONE;
}

static enum hw_result level(int (*get)(void *), int (*set)(void *, int), void *ctx, int dir, int lo,
                            int hi, int step, int *value)
{
    int cur;
    int want;
    int got;

    if (!get || !set) {
        return HW_RESULT_UNAVAILABLE;
    }
    cur = get(ctx);
    if (cur < 0) {
        return HW_RESULT_UNAVAILABLE;
    }
    want = hw_step(cur, dir, lo, hi, step);
    if (value) {
        *value = cur;
    }
    if (want == cur) {
        return HW_RESULT_NOOP; /* at the bound: no wrap, no write */
    }
    got = set(ctx, want);
    if (got < 0) {
        return HW_RESULT_UNAVAILABLE;
    }
    if (value) {
        *value = want;
    }
    return HW_RESULT_DONE;
}

static bool navigates(enum hw_action a)
{
    switch (a) {
    case HW_ACTION_HOME:
    case HW_ACTION_BACK:
    case HW_ACTION_SETTINGS:
    case HW_ACTION_TERMINAL:
    case HW_ACTION_WAVE:
    case HW_ACTION_VISION:
    case HW_ACTION_RIFT:
        return true;
    default:
        return false;
    }
}

/* The volume setter takes only grid values and answers 0; the brightness
 * setter answers the level applied. Both are wrapped here to one shape. */
enum hw_result hw_action_run(const struct hw_action_host *h, enum hw_action a, int *value)
{
    const char *id;

    if (value) {
        *value = -1;
    }
    if (!h || a <= HW_ACTION_NONE || a >= HW_ACTION_COUNT) {
        return HW_RESULT_UNAVAILABLE;
    }
    /* The lock screen is not security (shell_lock.h), but it is what keeps a
     * device in a pocket from acting on what it is pressed against: a key
     * that would open an app or leave one waits until the lock is opened.
     * The levels keep working, as a phone's volume keys do. */
    if (navigates(a) && h->locked && h->locked(h->ctx)) {
        return HW_RESULT_REFUSED;
    }
    id = app_for(a);
    if (id) {
        return open_app(h, id);
    }
    switch (a) {
    case HW_ACTION_HOME:
        if (!h->home) {
            return HW_RESULT_UNAVAILABLE;
        }
        h->home(h->ctx);
        return HW_RESULT_DONE;
    case HW_ACTION_BACK:
        return h->back ? h->back(h->ctx) : HW_RESULT_UNAVAILABLE;
    case HW_ACTION_SCREENSHOT:
        if (!h->screenshot) {
            return HW_RESULT_UNAVAILABLE;
        }
        return h->screenshot(h->ctx) == 0 ? HW_RESULT_DONE : HW_RESULT_UNAVAILABLE;
    case HW_ACTION_VOLUME_DOWN:
    case HW_ACTION_VOLUME_UP:
        return level(h->volume_get, h->volume_set, h->ctx, a == HW_ACTION_VOLUME_UP ? 1 : -1,
                     VOLUME_MIN_PCT, VOLUME_MAX_PCT, VOLUME_STEP_PCT, value);
    case HW_ACTION_BRIGHTNESS_DOWN:
    case HW_ACTION_BRIGHTNESS_UP:
        return level(h->brightness_get, h->brightness_set, h->ctx, a == HW_ACTION_BRIGHTNESS_UP ? 1 : -1,
                     BRIGHTNESS_MIN_PCT, BRIGHTNESS_MAX_PCT, BRIGHTNESS_STEP_PCT, value);
    case HW_ACTION_KBD_LIGHT_DOWN:
    case HW_ACTION_KBD_LIGHT_UP:
        return level(h->kbd_light_get, h->kbd_light_set, h->ctx, a == HW_ACTION_KBD_LIGHT_UP ? 1 : -1,
                     HW_KBD_LIGHT_MIN_PCT, HW_KBD_LIGHT_MAX_PCT, HW_KBD_LIGHT_STEP_PCT, value);
    default:
        return HW_RESULT_UNAVAILABLE;
    }
}
