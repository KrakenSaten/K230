/*
 * Power & Sleep in the running shell. See shell_power.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "shell_power.h"

#include "pocketlog/pocketlog.h"
#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct {
    struct shell_power_hooks hooks;
    struct power_policy policy;
    lv_obj_t *cover;  /* the black screen, on the system layer; hidden while on */
    bool off;
    bool held;        /* the hold as the last tick saw it, for the log */
    unsigned offs;    /* times the screen went off, for shell.info and the tests */
} pw;

static const char *setting_key(enum power_timer t)
{
    return t == POWER_TIMER_LOCK ? POWER_LOCK_SETTING : POWER_SCREEN_SETTING;
}

static int load(enum power_timer t)
{
    const char *stored = settings_get(setting_key(t), NULL);
    int s = 0;

    if (stored && power_parse_setting(t, stored, &s) < 0) {
        LOG_WARN("power: stored %s=%s is not one of the options, read as never", setting_key(t), stored);
        s = 0;
    }
    return s;
}

/* The touch that lands on the cover is the one that woke the screen: the
 * cover goes at the press, and LVGL keeps the rest of the gesture - the
 * release, the click - on the object that was pressed, so nothing under it
 * sees any of it. */
static void on_cover_pressed(lv_event_t *e)
{
    (void)e;
    shell_power_wake("touch");
}

void shell_power_init(const struct shell_power_hooks *hooks)
{
    char a[16];
    char b[16];

    memset(&pw, 0, sizeof(pw));
    if (hooks) {
        pw.hooks = *hooks;
    }
    pw.policy.screen_off_s = load(POWER_TIMER_SCREEN);
    pw.policy.lock_s = load(POWER_TIMER_LOCK);

    pw.cover = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(pw.cover);
    lv_obj_set_size(pw.cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(pw.cover, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pw.cover, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pw.cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pw.cover, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(pw.cover, on_cover_pressed, LV_EVENT_PRESSED, NULL);

    power_option_label(pw.policy.screen_off_s, a, sizeof(a));
    power_option_label(pw.policy.lock_s, b, sizeof(b));
    LOG_INFO("power: screen off after %s, lock after %s", a, b);
}

static void screen_off(void)
{
    if (pw.off || !pw.cover) {
        return;
    }
    pw.off = true;
    pw.offs++;
    lv_obj_remove_flag(pw.cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pw.cover);
    LOG_INFO("power: screen off after %u s idle", (unsigned)(shell_power_idle_ms() / 1000u));
}

void shell_power_wake(const char *why)
{
    if (!pw.off) {
        return;
    }
    pw.off = false;
    lv_obj_add_flag(pw.cover, LV_OBJ_FLAG_HIDDEN);
    lv_display_trigger_activity(NULL);
    LOG_INFO("power: screen on (%s)", why ? why : "wake");
}

bool shell_power_screen_off(void)
{
    return pw.off;
}

void shell_power_activity(void)
{
    lv_display_trigger_activity(NULL);
}

bool shell_power_gate(void)
{
    if (pw.off) {
        shell_power_wake("key");
        return false;
    }
    lv_display_trigger_activity(NULL);
    return true;
}

uint32_t shell_power_idle_ms(void)
{
    return lv_display_get_inactive_time(NULL);
}

unsigned shell_power_off_count(void)
{
    return pw.offs;
}

void shell_power_tick(void)
{
    bool hold = pw.hooks.hold && pw.hooks.hold();
    bool locked = pw.hooks.locked && pw.hooks.locked();
    unsigned due;

    if (hold != pw.held) {
        pw.held = hold;
        LOG_INFO("power: %s", hold ? "held awake" : "no longer held awake");
    }
    if (hold) {
        /* What holds the device awake also brings the screen back - an alarm
         * that starts ringing under a dark screen - and the time it held
         * counts as time in use. */
        shell_power_wake("held awake");
        lv_display_trigger_activity(NULL);
        return;
    }
    due = power_policy_due(&pw.policy, shell_power_idle_ms(), false, pw.off, locked);
    if ((due & POWER_DO_LOCK) && pw.hooks.lock) {
        pw.hooks.lock("idle");
    }
    if (due & POWER_DO_SCREEN_OFF) {
        screen_off();
    }
}

int shell_power_timeout(enum power_timer t)
{
    return t == POWER_TIMER_LOCK ? pw.policy.lock_s : pw.policy.screen_off_s;
}

int shell_power_set_timeout(enum power_timer t, int seconds)
{
    char value[12];
    char label[16];

    if (power_option_index(t, seconds) < 0) {
        return -1;
    }
    snprintf(value, sizeof(value), "%d", seconds);
    if (settings_set(setting_key(t), value) < 0) {
        LOG_WARN("power: %s=%d not persisted to %s: %s", setting_key(t), seconds, settings_path(),
                 strerror(errno));
        return -1;
    }
    if (t == POWER_TIMER_LOCK) {
        pw.policy.lock_s = seconds;
    } else {
        pw.policy.screen_off_s = seconds;
    }
    /* A new time counts from now, not from the last touch: the tap that set
     * it is the last touch anyway, and an IPC set should not blank a screen
     * somebody is looking at the moment it lands. */
    lv_display_trigger_activity(NULL);
    power_option_label(seconds, label, sizeof(label));
    LOG_INFO("power: %s after %s", t == POWER_TIMER_LOCK ? "lock" : "screen off", label);
    return 0;
}
