/*
 * Hardware actions test (ui/shell/hw_actions.c): the keyboard base's keys to
 * semantic actions, and every rule an action is run under - bounds without
 * wrap, one instance per app, a missing app, the lock, Back - against a fake
 * host that records what the shell was asked to do.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hw_actions.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* ---- a fake shell -------------------------------------------------------- */

struct fake {
    char current[16];     /* "" at home */
    const char *installed[8];
    int opens;            /* app_open() calls */
    char last_open[16];
    int homes;
    int backs;
    enum hw_result back_result;
    bool locked;
    int volume, brightness, light;
    int volume_sets, brightness_sets, light_sets;
    bool no_brightness;
    int shots;
    int shot_rc;
};

static const char *f_current(void *ctx)
{
    struct fake *f = ctx;

    return f->current[0] ? f->current : NULL;
}

static int f_open(void *ctx, const char *id)
{
    struct fake *f = ctx;
    int k;

    for (k = 0; k < 8 && f->installed[k]; k++) {
        if (strcmp(f->installed[k], id) == 0) {
            f->opens++;
            snprintf(f->last_open, sizeof(f->last_open), "%s", id);
            snprintf(f->current, sizeof(f->current), "%s", id);
            return 0;
        }
    }
    return -1;
}

static void f_home(void *ctx)
{
    struct fake *f = ctx;

    f->homes++;
    f->current[0] = '\0';
}

static enum hw_result f_back(void *ctx)
{
    struct fake *f = ctx;

    f->backs++;
    return f->back_result;
}

static bool f_locked(void *ctx)
{
    return ((struct fake *)ctx)->locked;
}

static int f_vol_get(void *ctx) { return ((struct fake *)ctx)->volume; }
static int f_vol_set(void *ctx, int p)
{
    struct fake *f = ctx;

    f->volume_sets++;
    f->volume = p;
    return p;
}
static int f_br_get(void *ctx)
{
    struct fake *f = ctx;

    return f->no_brightness ? -1 : f->brightness;
}
static int f_br_set(void *ctx, int p)
{
    struct fake *f = ctx;

    f->brightness_sets++;
    f->brightness = p;
    return p;
}
static int f_li_get(void *ctx) { return ((struct fake *)ctx)->light; }
static int f_li_set(void *ctx, int p)
{
    struct fake *f = ctx;

    f->light_sets++;
    f->light = p;
    return p;
}
static int f_shot(void *ctx)
{
    struct fake *f = ctx;

    f->shots++;
    return f->shot_rc;
}

static struct fake fk;
static struct hw_action_host host;

static void reset(void)
{
    static const char *const all[] = { "settings", "terminal", "wave", "vision", "rift", NULL };
    int k;

    memset(&fk, 0, sizeof(fk));
    for (k = 0; all[k]; k++) {
        fk.installed[k] = all[k];
    }
    fk.volume = 50;
    fk.brightness = 50;
    fk.light = 50;
    fk.back_result = HW_RESULT_DONE;
    host = (struct hw_action_host){ &fk, f_current, f_open, f_home, f_back, f_locked, f_vol_get, f_vol_set,
                                    f_br_get, f_br_set, f_li_get, f_li_set, f_shot };
}

/* The action a press of this matrix code runs, run. */
static enum hw_result press(uint8_t code)
{
    return hw_action_run(&host, hw_action_for_key(code), NULL);
}

/* Matrix codes, from pos_keymap.c's table and the vendor's. */
enum { F1 = 50, F2 = 60, F3 = 59, F4 = 68, F5 = 67, F6 = 66, F7 = 65, F8 = 64, F9 = 63, F10 = 62, F11 = 61,
       MIC = 11, LILYGO = 8 };

static void test_mapping(void)
{
    static const uint8_t f[11] = { F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11 };
    static const enum hw_action want[11] = {
        HW_ACTION_HOME,        HW_ACTION_SETTINGS,  HW_ACTION_KBD_LIGHT_DOWN, HW_ACTION_KBD_LIGHT_UP,
        HW_ACTION_VOLUME_DOWN, HW_ACTION_VOLUME_UP, HW_ACTION_SCREENSHOT,     HW_ACTION_TERMINAL,
        HW_ACTION_RIFT,        HW_ACTION_BRIGHTNESS_DOWN, HW_ACTION_BRIGHTNESS_UP,
    };
    char name[64];
    int k;
    int typing_mapped = 0;
    enum hw_action a;

    for (k = 0; k < 11; k++) {
        snprintf(name, sizeof(name), "F%d (code %d) is %s", k + 1, f[k], hw_action_name(want[k]));
        check(name, hw_action_for_key(f[k]) == want[k]);
        snprintf(name, sizeof(name), "code %d is function key %d", f[k], k + 1);
        check(name, hw_action_fkey(f[k]) == k + 1);
    }
    check("the orange microphone key opens Wave", hw_action_for_key(MIC) == HW_ACTION_WAVE);
    check("the LILYGO key opens Terminal", hw_action_for_key(LILYGO) == HW_ACTION_TERMINAL);
    /* Every other position types, or is a modifier: none of them may carry
     * an action, or ordinary typing would start opening apps. */
    for (k = 0; k <= 127; k++) {
        if (k == MIC || k == LILYGO || hw_action_fkey((uint8_t)k)) {
            continue;
        }
        typing_mapped += hw_action_for_key((uint8_t)k) != HW_ACTION_NONE;
    }
    check("no typing or modifier key carries an action", typing_mapped == 0);
    check("a letter is no function key", hw_action_fkey(29) == 0);

    /* The shell.action vocabulary round-trips. */
    for (k = 1; k < HW_ACTION_COUNT; k++) {
        snprintf(name, sizeof(name), "name %s parses back", hw_action_name((enum hw_action)k));
        check(name, hw_action_parse(hw_action_name((enum hw_action)k), &a) == 0 && a == (enum hw_action)k);
    }
    check("an unknown name is refused", hw_action_parse("reboot", &a) != 0);
    check("none is not an action to ask for", hw_action_parse("none", &a) != 0);
    check("NULL is refused", hw_action_parse(NULL, &a) != 0);
}

static void test_apps(void)
{
    reset();
    check("F1 at home: Home", press(F1) == HW_RESULT_DONE && fk.homes == 1);
    check("F2 opens Settings", press(F2) == HW_RESULT_DONE && strcmp(fk.last_open, "settings") == 0);
    check("F8 opens Terminal", press(F8) == HW_RESULT_DONE && strcmp(fk.current, "terminal") == 0);
    check("F9 opens RIFT", press(F9) == HW_RESULT_DONE && strcmp(fk.current, "rift") == 0);
    check("mic opens Wave", press(MIC) == HW_RESULT_DONE && strcmp(fk.current, "wave") == 0);
    check("LILYGO opens Terminal", press(LILYGO) == HW_RESULT_DONE && strcmp(fk.current, "terminal") == 0);
    check("Vision action opens Vision",
          hw_action_run(&host, HW_ACTION_VISION, NULL) == HW_RESULT_DONE && strcmp(fk.current, "vision") == 0);
    check("F1 from an app goes home", press(F1) == HW_RESULT_DONE && fk.current[0] == '\0' && fk.homes == 2);

    /* One instance: the same shortcut again does not reopen the app, which
     * would close it first and lose its shell or camera session. */
    reset();
    press(F8);
    press(F8);
    press(LILYGO);
    check("Terminal three times: opened once", fk.opens == 1);
    check("and the repeats are no-ops", press(F8) == HW_RESULT_NOOP);
    hw_action_run(&host, HW_ACTION_VISION, NULL);
    check("Vision from Terminal opens Vision", strcmp(fk.current, "vision") == 0 && fk.opens == 2);
    check("Vision in Vision: no second instance",
          hw_action_run(&host, HW_ACTION_VISION, NULL) == HW_RESULT_NOOP && fk.opens == 2);
    press(F8);
    check("Terminal again after Vision opens it", strcmp(fk.current, "terminal") == 0 && fk.opens == 3);

    /* An app this build does not have: nothing happens, nothing breaks. */
    reset();
    fk.installed[3] = NULL; /* no vision, no rift */
    fk.installed[4] = NULL;
    check("RIFT not installed: unavailable", press(F9) == HW_RESULT_UNAVAILABLE);
    check("Vision not installed: unavailable", hw_action_run(&host, HW_ACTION_VISION, NULL) ==
                                                   HW_RESULT_UNAVAILABLE);
    check("and the screen did not change", fk.current[0] == '\0' && fk.opens == 0 && fk.homes == 0);
    host.open_app = NULL;
    check("a host that cannot open apps: unavailable", press(F2) == HW_RESULT_UNAVAILABLE);
}

static void test_back(void)
{
    reset();
    fk.back_result = HW_RESULT_DONE;
    check("Back asks the shell's back path",
          hw_action_run(&host, HW_ACTION_BACK, NULL) == HW_RESULT_DONE && fk.backs == 1);
    fk.back_result = HW_RESULT_NOOP;
    check("Back with nothing to leave: no-op", hw_action_run(&host, HW_ACTION_BACK, NULL) == HW_RESULT_NOOP);
    check("Back never opens or homes by itself", fk.opens == 0 && fk.homes == 0);
    host.back = NULL;
    check("no back path: unavailable", hw_action_run(&host, HW_ACTION_BACK, NULL) == HW_RESULT_UNAVAILABLE);
}

static void test_lock(void)
{
    int v;

    reset();
    fk.locked = true;
    check("locked: F8 refused", press(F8) == HW_RESULT_REFUSED && fk.opens == 0);
    check("locked: mic refused", press(MIC) == HW_RESULT_REFUSED);
    check("locked: Home refused", press(F1) == HW_RESULT_REFUSED && fk.homes == 0);
    check("locked: Back refused", hw_action_run(&host, HW_ACTION_BACK, NULL) == HW_RESULT_REFUSED && fk.backs == 0);
    check("locked: Vision refused", hw_action_run(&host, HW_ACTION_VISION, NULL) == HW_RESULT_REFUSED);
    check("locked: volume still steps", hw_action_run(&host, HW_ACTION_VOLUME_UP, &v) == HW_RESULT_DONE && v == 60);
    check("locked: brightness still steps", press(F10) == HW_RESULT_DONE && fk.brightness == 40);
    check("locked: the keyboard light still steps", press(F4) == HW_RESULT_DONE && fk.light == 60);
    fk.locked = false;
    check("unlocked: F8 opens again", press(F8) == HW_RESULT_DONE && fk.opens == 1);
}

static void test_levels(void)
{
    int v;
    int k;

    reset();
    /* Volume: 10..100 in 10s, the Controls tile's own range. */
    fk.volume = 90;
    check("F6 raises the volume", press(F6) == HW_RESULT_DONE && fk.volume == 100);
    fk.volume_sets = 0;
    check("F6 at 100: no-op, no wrap", press(F6) == HW_RESULT_NOOP && fk.volume == 100 && fk.volume_sets == 0);
    for (k = 0; k < 20; k++) {
        press(F5);
    }
    check("F5 x20 stops at 10", fk.volume == 10);
    check("F5 at 10: no-op", press(F5) == HW_RESULT_NOOP && fk.volume == 10);
    check("and reports the level it is at", hw_action_run(&host, HW_ACTION_VOLUME_DOWN, &v) == HW_RESULT_NOOP &&
                                                v == 10);

    /* Brightness: 10..100, and a level off the grid moves to the grid. */
    fk.brightness = 99;
    check("F11 from 99 goes to 100", press(F11) == HW_RESULT_DONE && fk.brightness == 100);
    fk.brightness = 99;
    check("F10 from 99 goes to 90", press(F10) == HW_RESULT_DONE && fk.brightness == 90);
    for (k = 0; k < 20; k++) {
        press(F10);
    }
    check("F10 x20 stops at the floor, 10", fk.brightness == 10);
    for (k = 0; k < 20; k++) {
        press(F11);
    }
    check("F11 x20 stops at 100", fk.brightness == 100);
    fk.no_brightness = true;
    check("no backlight device: unavailable", press(F11) == HW_RESULT_UNAVAILABLE);

    /* The keyboard light: 0 (off) .. 100. */
    fk.light = 10;
    check("F3 from 10 turns it off", press(F3) == HW_RESULT_DONE && fk.light == 0);
    fk.light_sets = 0;
    check("F3 at off: no-op, no wrap to 100", press(F3) == HW_RESULT_NOOP && fk.light == 0 && fk.light_sets == 0);
    for (k = 0; k < 15; k++) {
        press(F4);
    }
    check("F4 x15 stops at 100", fk.light == 100 && fk.light_sets == 10);
    check("F4 at 100: no-op", press(F4) == HW_RESULT_NOOP);
    host.kbd_light_get = NULL;
    check("no keyboard light: unavailable", press(F4) == HW_RESULT_UNAVAILABLE);

    /* hw_step itself. */
    check("step up on grid", hw_step(50, 1, 10, 100, 10) == 60);
    check("step down on grid", hw_step(50, -1, 10, 100, 10) == 40);
    check("step up off grid", hw_step(55, 1, 10, 100, 10) == 60);
    check("step down off grid", hw_step(55, -1, 10, 100, 10) == 50);
    check("below the range comes into it", hw_step(3, -1, 10, 100, 10) == 10);
    check("above the range comes into it", hw_step(130, 1, 10, 100, 10) == 100);
    check("no step is no change", hw_step(50, 1, 10, 100, 0) == 50);
}

static void test_screenshot(void)
{
    reset();
    check("F7 starts a screenshot", press(F7) == HW_RESULT_DONE && fk.shots == 1);
    fk.shot_rc = -1;
    check("a screenshot that cannot start: unavailable", press(F7) == HW_RESULT_UNAVAILABLE);
    fk.shot_rc = 0;
    fk.locked = true;
    check("locked: F7 still works (it navigates nowhere)", press(F7) == HW_RESULT_DONE);
}

static void test_robustness(void)
{
    int v = 7;

    reset();
    check("NONE runs nothing", hw_action_run(&host, HW_ACTION_NONE, &v) == HW_RESULT_UNAVAILABLE && v == -1);
    check("out of range runs nothing", hw_action_run(&host, HW_ACTION_COUNT, NULL) == HW_RESULT_UNAVAILABLE);
    check("no host runs nothing", hw_action_run(NULL, HW_ACTION_HOME, NULL) == HW_RESULT_UNAVAILABLE);
    check("result names", strcmp(hw_result_name(HW_RESULT_REFUSED), "refused") == 0 &&
                              strcmp(hw_result_name(HW_RESULT_NOOP), "noop") == 0);
    check("nothing was touched", fk.opens == 0 && fk.homes == 0 && fk.backs == 0);
}

int main(void)
{
    test_mapping();
    test_apps();
    test_back();
    test_lock();
    test_levels();
    test_screenshot();
    test_robustness();
    printf("hw_actions_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
