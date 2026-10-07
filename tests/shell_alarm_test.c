/*
 * The system alert's keyboard isolation and focus ownership (DS v0.1 §18.5,
 * §18.6, §18.8).
 *
 * The claim under test is the one §18.8 makes a shipping gate: while an alert
 * holds the panel, nothing the owner types reaches the app underneath, the
 * alert's own actions are reachable by key, and acknowledging gives the app
 * back exactly what it had. A text field stands in for the app, because a
 * field is the one thing that would silently swallow a printable key.
 *
 * Which action a key reached is tested by what it did - an alarm that ends up
 * snoozed was reached by Snooze, one that ends up acknowledged by Stop -
 * rather than by reading focus out of a private group. That keeps the test
 * honest about behaviour and adds no API to see inside the alert.
 *
 * The keyboard suppression rule is the real one: this links
 * ui/shell/shell_kb_state.c and asks the same predicate the shell asks, so
 * passing here cannot mean a copy of the rule passed.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/alert_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app.h"
#include "clock_engine.h"
#include "clock_runtime.h"
#include "pocketui.h"
#include "pos_input.h"
#include "pos_keyboard.h"
#include "shell_alarm.h"
#include "shell_kb_state.h"

#include <stdio.h>
#include <string.h>

#define PANEL_W 568
#define PANEL_H 1232
/* The content area's top: the top edge, since no chrome reserves a row
 * there any more (DS §36, ui/shell/chrome.h). */
#define STATUS_H 0

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
               want);
    }
}

/* ---- a display that draws nowhere ------------------------------------- */

static uint8_t buf[PANEL_W * 40 * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

/* ---- the shell services the alert uses -------------------------------- */

static lv_obj_t *g_keyboard;
static lv_obj_t *g_content;
static char g_hint[64];
static int g_show_calls;

void pocketos_shell_set_status_hint(const char *text)
{
    snprintf(g_hint, sizeof(g_hint), "%s", text ? text : "");
}

void pocketos_shell_go_home(void) { }
int pocketos_shell_reduced_motion(void) { return 0; }
const char *pocketos_shell_radio_state(void) { return NULL; }

void pocketos_shell_keyboard_show(enum pocketos_kb_return ret,
                                  void (*on_done)(void *user), void *user)
{
    (void)on_done;
    (void)user;
    g_show_calls++;
    /* The shell's own rule, asked here rather than reimplemented. */
    if (!g_keyboard || !pocketos_shell_keyboard_may_show()) {
        return;
    }
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE
                                            ? POS_KB_RETURN_NEWLINE
                                            : POS_KB_RETURN_DONE);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H - POS_KB_H);
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    if (!g_keyboard) {
        return;
    }
    pos_keyboard_hide(g_keyboard);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H);
}

int pocketos_shell_keyboard_visible(void)
{
    return g_keyboard && pos_keyboard_is_shown(g_keyboard);
}

/* ---- driving LVGL ----------------------------------------------------- */

static void settle(void)
{
    int i;

    for (i = 0; i < 40; i++) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void type(uint32_t key)
{
    pos_input_push_key(key);
    settle();
}

/* The invariant behind the suppression fix: the keyboard is only ever held
 * down while the alert actually owns the keys. Suppression standing with the
 * stream un-redirected would mean a keyboard refused for the rest of the
 * session with no alert to show for it, which is exactly what raising it
 * before the push could produce. Sampled at every state change below. */
static void check_suppression_coupled(const char *where)
{
    checks++;
    if (!pocketos_shell_keyboard_may_show() && !pos_input_group_redirected()) {
        failed++;
        printf("FAIL suppression without isolation (%s)\n", where);
    }
}

/* ---- making something ring, without waiting for a clock ---------------- */

static int64_t g_day = 20260912;
static int64_t g_mono = 1000;

static struct clock_now at(int hour, int minute)
{
    struct clock_now now;

    memset(&now, 0, sizeof(now));
    now.wall.valid = true;
    now.wall.epoch = 1789000000LL; /* far above CLOCK_WALL_VALID_FROM */
    now.wall.day = g_day;
    now.wall.hour = hour;
    now.wall.minute = minute;
    now.wall.wday = 6;
    g_mono += 1000;
    now.mono_ms = g_mono;
    return now;
}

/* One daily alarm, rung again by moving to the next day: an alarm fires once
 * per day, so repetition needs a new day rather than a new alarm. */
static void ring_alarm(void)
{
    struct clock_now now;

    g_day++;
    now = at(7, 29);
    clock_runtime_step_at(&now);
    now = at(7, 30);
    clock_runtime_step_at(&now);
    settle();
}

static void ring_timer(void)
{
    struct clock_engine *e = clock_runtime_engine();
    struct clock_now now;

    g_day++;
    now = at(9, 0);
    clock_runtime_step_at(&now);
    clock_timer_set(e, 0, 0, 1);
    clock_timer_start(e, &now);
    now = at(9, 1);
    g_mono += 5000; /* past the one-second countdown */
    now.mono_ms = g_mono;
    clock_runtime_step_at(&now);
    settle();
}

static const struct clock_alarm *the_alarm(void)
{
    return clock_alarm_at(clock_runtime_engine(), 0);
}

int main(void)
{
    lv_display_t *disp;
    lv_obj_t *screen;
    lv_obj_t *field;
    lv_obj_t *other;
    struct clock_now now;
    int i;

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    pos_input_init();
    pocketui_init();
    screen = lv_screen_active();
    pocketui_style_screen(screen);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    g_content = lv_obj_create(screen);
    lv_obj_remove_style_all(g_content);
    lv_obj_set_size(g_content, LV_PCT(100), PANEL_H - STATUS_H);
    g_keyboard = pos_keyboard_create(screen);

    field = pocketui_text_field(g_content, "Note", false);
    lv_textarea_set_text(field, "note");
    pos_input_focus(field);
    settle();

    clock_runtime_init(shell_alarm_sync);
    shell_alarm_create(screen);
    now = at(7, 0);
    clock_runtime_step_at(&now);
    clock_alarm_add(clock_runtime_engine(), 7, 30, CLOCK_REPEAT_DAILY, "Wake",
                    &now);
    settle();

    check("the field is focused before any alert", pos_input_focused() == field);
    check("no redirection before any alert", !pos_input_group_redirected());
    check("the keyboard may be shown before any alert",
          pocketos_shell_keyboard_may_show());
    check_suppression_coupled("before any alert");

    /* ---- 1. LVGL's editing mode, measured rather than assumed ---------- *
     *
     * A focused text area may put its group into editing mode, where LVGL
     * treats NEXT as caret movement instead of advancing focus. Whatever it
     * does, the isolation below must hold; what it does is recorded so the
     * next reader does not have to guess. */
    printf("note: text field focused, group editing=%d\n",
           (int)lv_group_get_editing(pos_input_group()));
    type(LV_KEY_NEXT);
    printf("note: after NEXT the focus %s the field\n",
           pos_input_focused() == field ? "stayed on" : "left");
    pos_input_focus(field);
    settle();

    /* ---- 2. the alert takes the panel --------------------------------- */

    pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, NULL);
    settle();
    check("the keyboard is up before the alert",
          pocketos_shell_keyboard_visible());

    ring_alarm();
    check("the alarm is ringing", shell_alarm_visible());
    check("the alert redirected the stream", pos_input_group_redirected());
    check("the keyboard was dismissed (DS 18.5)",
          !pocketos_shell_keyboard_visible());
    check("and may not be shown again (DS 18.8)",
          !pocketos_shell_keyboard_may_show());
    check_suppression_coupled("while the alarm alert is up");

    g_show_calls = 0;
    pocketos_shell_keyboard_show(POCKETOS_KB_NEWLINE, NULL, NULL);
    settle();
    check("an app asking for the keyboard is refused, not obeyed",
          g_show_calls == 1 && !pocketos_shell_keyboard_visible());

    /* ---- 3. nothing reaches the app underneath ------------------------ */

    type('x');
    check_str("a printable key does not reach the hidden field",
              lv_textarea_get_text(field), "note");
    type(LV_KEY_BACKSPACE);
    check_str("nor does Backspace", lv_textarea_get_text(field), "note");
    type(LV_KEY_NEXT);
    type(LV_KEY_PREV);
    type('y');
    check_str("nor does anything after Tab and Prev",
              lv_textarea_get_text(field), "note");
    check("the stream is still redirected", pos_input_group_redirected());

    /* ---- 4. Enter acts on Stop, the dominant action (DS 18.4) ---------- */

    type(LV_KEY_ENTER);
    check("Enter stopped the alarm", !shell_alarm_visible());
    check("it was Stop, not Snooze: no snooze was set",
          the_alarm() && the_alarm()->snooze_until == 0);
    check("the redirection was popped", !pos_input_group_redirected());
    check("the keyboard may be shown again",
          pocketos_shell_keyboard_may_show());
    check("but it was not put back (DS 18.5)",
          !pocketos_shell_keyboard_visible());
    check("focus came back to the field", pos_input_focused() == field);
    check_str("with its text intact", lv_textarea_get_text(field), "note");
    check_suppression_coupled("after Stop");

    /* ---- 5. Snooze is reachable only when it is shown ------------------ */

    ring_alarm();
    check("the alarm is ringing again", shell_alarm_visible());
    type(LV_KEY_NEXT);
    type(LV_KEY_ENTER);
    check("NEXT then Enter reached Snooze", !shell_alarm_visible());
    check("and the alarm is snoozed rather than acknowledged",
          the_alarm() && the_alarm()->snooze_until != 0);
    check("focus came back to the field after Snooze",
          pos_input_focused() == field);
    check_suppression_coupled("after Snooze");

    /* Clear the snooze so it cannot ring during the timer case. */
    clock_runtime_stop_ringing();
    shell_alarm_sync();

    ring_timer();
    check("a timer alert is up", shell_alarm_visible());
    check("the timer alert redirected the stream too",
          pos_input_group_redirected());
    type(LV_KEY_NEXT);
    type(LV_KEY_ENTER);
    check("with Snooze hidden, NEXT then Enter still reaches Stop",
          !shell_alarm_visible());
    check_str("and the field is still untouched",
              lv_textarea_get_text(field), "note");
    check_suppression_coupled("after the timer alert");

    /* ---- 6. repetition, and ticks that change nothing ------------------ */

    for (i = 0; i < 3; i++) {
        ring_alarm();
        check("open: redirected", pos_input_group_redirected());
        shell_alarm_sync();
        shell_alarm_sync();
        check("a repeated tick does not push again",
              pos_input_group_redirected());
        type(LV_KEY_ENTER);
        check("close: not redirected", !pos_input_group_redirected());
        shell_alarm_sync();
        check("a repeated tick does not pop again",
              !pos_input_group_redirected());
        check("focus restored", pos_input_focused() == field);
        check_suppression_coupled("through a repeated open and close");
    }
    check("no suppression left behind", pocketos_shell_keyboard_may_show());
    check_suppression_coupled("after three cycles");
    check_str("and the field survived all of it",
              lv_textarea_get_text(field), "note");

    /* ---- 7. an alert over something that is not a text field ----------- */

    other = pocketui_button(g_content, "Open", NULL, NULL);
    pos_input_add_obj(other);
    pos_input_focus(other);
    settle();
    check("the button is focused", pos_input_focused() == other);
    ring_alarm();
    type(LV_KEY_ENTER);
    check("focus returns to a non-field object too",
          pos_input_focused() == other);

    /* ---- 8. the app underneath is destroyed while the alert is up ------ */

    ring_alarm();
    check("an alert is up over the doomed object", shell_alarm_visible());
    lv_obj_delete(other);
    settle();
    type(LV_KEY_ENTER);
    check("acknowledging after the object died does not crash",
          !shell_alarm_visible());
    check("and no stale pointer was focused", pos_input_focused() != other);
    check("the stream is not left redirected", !pos_input_group_redirected());
    check("nor the keyboard left suppressed",
          pocketos_shell_keyboard_may_show());

    printf("shell_alarm_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
