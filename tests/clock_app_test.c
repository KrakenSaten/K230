/*
 * PocketClock in the running app, driven by a real LVGL pointer device and a
 * real touch keyboard, against a real (temporary) store.
 *
 * Every alarm here is made the way a finger makes one: taps on the steppers,
 * taps on the keyboard, a tap on Add. What is then read back off the disk is
 * what the store actually kept.
 *
 * The shell is not here either, so this file plays it: the three app.h
 * keyboard entry points are implemented below against the real
 * pos_keyboard, and the one alarm runtime and the one alert sheet are
 * created the way ui/shell/shell.c creates them. That is what lets the last
 * sections test the thing the runtime exists for - an alarm ringing with
 * PocketClock shut - by stepping the runtime with an injected reading
 * instead of waiting for 07:30.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/clock_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "clock_alert.h"
#include "clock_runtime.h"
#include "clock_store.h"
#include "clock_time.h"
#include "pocketui.h"
#include "pos_keyboard.h"
#include "shell_alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PANEL_W 568
#define PANEL_H 1232
#define STATUS_H POCKETUI_STATUS_BAR_H

/* The app's own layout, as clock_app.c builds it. Reaching for a control by
 * position rather than by label is the only way to tell two stepper rows of
 * identical buttons apart. */
#define SCREEN_MAIN 0
#define SCREEN_ADD 1
#define PANE_CLOCK 0
#define PANE_ALARM 1
#define PANE_WATCH 2
#define PANE_TIMER 3

extern const struct pocketos_app app_clock;

static int failed;
static int checks;
static char root[128];

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

/* ---- the shell's side of app.h, as the shell implements it ------------- */

static lv_obj_t *g_keyboard;
static lv_obj_t *g_content;
static char g_hint[64];

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
    pos_keyboard_set_return(g_keyboard, ret == POCKETOS_KB_NEWLINE
                                            ? POS_KB_RETURN_NEWLINE
                                            : POS_KB_RETURN_DONE);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H - POS_KB_H);
    pos_keyboard_show(g_keyboard);
}

void pocketos_shell_keyboard_hide(void)
{
    pos_keyboard_hide(g_keyboard);
    lv_obj_set_height(g_content, PANEL_H - STATUS_H);
}

int pocketos_shell_keyboard_visible(void)
{
    return pos_keyboard_is_shown(g_keyboard);
}

/* ---- display and finger ------------------------------------------------ */

static uint8_t draw_buf[PANEL_W * 40 * 2];
static lv_indev_state_t finger_state = LV_INDEV_STATE_RELEASED;
static lv_point_t finger_point;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = finger_state;
    data->point = finger_point;
}

static void pump(int ms)
{
    int t;

    for (t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void drain(void)
{
    int t;

    for (t = 0; t < 300 && pos_input_queued() > 0; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
    pump(40);
}

static void tap_obj(lv_obj_t *obj)
{
    lv_area_t a;

    if (!obj) {
        printf("FAIL tap on a missing object\n");
        failed++;
        checks++;
        return;
    }
    lv_obj_update_layout(obj);
    lv_obj_get_coords(obj, &a);
    finger_point.x = a.x1 + lv_area_get_width(&a) / 2;
    finger_point.y = a.y1 + lv_area_get_height(&a) / 2;
    finger_state = LV_INDEV_STATE_PRESSED;
    pump(60);
    finger_state = LV_INDEV_STATE_RELEASED;
    pump(60);
    drain();
}

static void tap_key(const char *label)
{
    tap_obj(pos_keyboard_key(g_keyboard, label));
}

/* The alpha layer shows lower case, so a capital needs Shift first. */
static void type_text(const char *s)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            tap_key("SHIFT");
        }
        one[0] = *s;
        tap_key(one);
    }
}

/* ---- finding things in the app's tree ---------------------------------- */

static lv_obj_t *app_body;
static void *app_priv;

/* Depth-first search for a label with this text; returns its clickable
 * ancestor, which is what a finger would press. A hidden screen is still in
 * the tree, and a finger cannot press what is not shown, so neither may
 * this - which is also what keeps the four panes from matching each other. */
static lv_obj_t *find_labelled(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return NULL;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            lv_obj_t *p = obj;

            while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) {
                p = lv_obj_get_parent(p);
            }
            return p ? p : obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_labelled(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

/* The same search, hidden subtrees included and the label itself returned:
 * for asking whether something was built, not whether it can be pressed. */
static lv_obj_t *find_text_anywhere(lv_obj_t *obj, const char *text)
{
    uint32_t i;

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *t = lv_label_get_text(obj);

        if (t && strcmp(t, text) == 0) {
            return obj;
        }
    }
    for (i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *hit = find_text_anywhere(lv_obj_get_child(obj, i), text);

        if (hit) {
            return hit;
        }
    }
    return NULL;
}

static int label_present(lv_obj_t *obj, const char *text)
{
    return find_labelled(obj, text) != NULL;
}

static const char *text_of(lv_obj_t *label)
{
    return label ? lv_label_get_text(label) : "(missing)";
}

static lv_obj_t *screen_of(int which)
{
    return lv_obj_get_child(app_body, which);
}

static lv_obj_t *pane_of(int which)
{
    return lv_obj_get_child(screen_of(SCREEN_MAIN), which + 1); /* 0 is the tabs */
}

/* Every pane and the new-alarm form open with a card whose first child is
 * the big number. */
static lv_obj_t *hero_in(lv_obj_t *parent)
{
    return lv_obj_get_child(lv_obj_get_child(parent, 0), 0);
}

/* A stepper row is [-big][-small][value][+small][+big]. */
#define STEP_MINUS_BIG 0
#define STEP_MINUS 1
#define STEP_VALUE 2
#define STEP_PLUS 3
#define STEP_PLUS_BIG 4

static lv_obj_t *stepper_cell(lv_obj_t *parent, int row, int cell)
{
    return lv_obj_get_child(lv_obj_get_child(parent, row), cell);
}

/* The trash button on the alarm row showing this time. */
static lv_obj_t *alarm_row_delete(const char *hm)
{
    lv_obj_t *hit = find_labelled(app_body, hm);

    if (!hit) {
        return NULL;
    }
    return lv_obj_get_child(lv_obj_get_parent(hit), 2);
}

/* ---- the app, hosted the way the shell hosts it ------------------------ */

static void app_start(void)
{
    app_body = lv_obj_create(g_content);
    lv_obj_remove_style_all(app_body);
    lv_obj_set_size(app_body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(app_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(app_body, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_row(app_body, POCKETUI_PAD, 0);
    lv_obj_add_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(app_body, LV_DIR_VER);
    app_priv = app_clock.create(app_body);
    pump(60);
}

static void app_stop(void)
{
    pocketos_shell_keyboard_hide();
    app_clock.destroy(app_priv);
    app_priv = NULL;
    lv_obj_delete(app_body);
    app_body = NULL;
    pump(60);
}

static void wipe(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) {
        /* nothing there yet */
    }
}

/* What is actually on disk, read back through the store. */
static int stored(struct clock_engine *out)
{
    clock_engine_init(out);
    return clock_store_load(out);
}

/* A clock reading the shell tick might have taken. The alert is reached by
 * injecting one rather than waiting for 07:30 to come round. */
static struct clock_now at(int64_t day, int hour, int minute, int wday,
                           int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.wall.valid = true;
    n.wall.day = day;
    n.wall.hour = hour;
    n.wall.minute = minute;
    n.wall.wday = wday;
    n.wall.epoch = CLOCK_WALL_VALID_FROM;
    n.mono_ms = mono_ms;
    return n;
}

/* A board that does not know the time - which is what a countdown has to
 * work through, because that is how this one boots. */
static struct clock_now unset_now(int64_t mono_ms)
{
    struct clock_now n;

    memset(&n, 0, sizeof(n));
    n.mono_ms = mono_ms;
    return n;
}

/* Everything the shell would lose at a power cut, lost: the runtime starts
 * again and has only the store to go on. */
static void reboot_runtime(void)
{
    clock_runtime_deinit();
    clock_runtime_init(shell_alarm_sync);
}

/* The alert lives on the screen, not under the app, so it is looked for
 * there - which is the point of it. */
static lv_obj_t *on_screen(const char *text)
{
    return find_labelled(lv_screen_active(), text);
}

/* Step the new-alarm form to a given time. Both steppers wrap, so this
 * terminates whatever time the form opened at. */
static void set_form_time(const char *want)
{
    lv_obj_t *form = screen_of(SCREEN_ADD);
    lv_obj_t *hero = hero_in(form);
    int guard;

    for (guard = 0; guard < 24 && strncmp(text_of(hero), want, 2) != 0; guard++) {
        tap_obj(stepper_cell(form, 1, STEP_PLUS));
    }
    for (guard = 0; guard < 60 && strcmp(text_of(hero), want) != 0; guard++) {
        tap_obj(stepper_cell(form, 2, STEP_PLUS));
    }
}

int main(void)
{
    lv_display_t *disp;
    lv_indev_t *finger;
    struct clock_engine disk;
    struct clock_now now;
    lv_obj_t *form;
    lv_obj_t *timer_pane;
    lv_obj_t *field;
    char before[24];
    char kept[16];

    snprintf(root, sizeof(root), "/tmp/pocketclock-app-%u", (unsigned)getpid());
    setenv("POCKETOS_STATE_DIR", root, 1);
    wipe();

    lv_init();
    disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    finger = lv_indev_create();
    lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(finger, read_cb);

    pos_input_init();
    pocketui_init();
    pocketui_style_screen(lv_screen_active());

    g_content = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_content);
    lv_obj_set_size(g_content, PANEL_W, PANEL_H - STATUS_H);
    lv_obj_set_pos(g_content, 0, STATUS_H);
    g_keyboard = pos_keyboard_create(lv_screen_active());

    /* What the shell does: the one alert sheet, then the one runtime, with
     * the sheet listening for a ring. */
    shell_alarm_create(lv_screen_active());
    clock_runtime_init(shell_alarm_sync);
    pump(60);

    /* ---- 1. the four tabs ---------------------------------------------- */

    app_start();
    check("the Clock tab is there", label_present(app_body, "Clock"));
    check("the Alarm tab", label_present(app_body, "Alarm"));
    check("the stopwatch tab", label_present(app_body, "Watch"));
    check("the Timer tab", label_present(app_body, "Timer"));
    check("the clock pane is the one showing", !lv_obj_has_flag(pane_of(PANE_CLOCK),
                                                                LV_OBJ_FLAG_HIDDEN));
    check("and the others are not", lv_obj_has_flag(pane_of(PANE_TIMER),
                                                    LV_OBJ_FLAG_HIDDEN));
    check("the keyboard is not up", !pocketos_shell_keyboard_visible());

    /* ---- 2. the clock face tells the truth about the clock ------------- */

    clock_now_read(&now);
    if (now.wall.valid) {
        char expect[16];

        clock_format_wall(&now.wall, expect, sizeof(expect));
        check_str("a board that knows the time shows it",
                  text_of(hero_in(pane_of(PANE_CLOCK))), expect);
        check("and does not show the unset state",
              !label_present(app_body, "Time not set"));
    } else {
        check_str("a board that does not know the time shows none",
                  text_of(hero_in(pane_of(PANE_CLOCK))), "--:--");
        check("and says so in words", label_present(app_body, "Time not set"));
    }

    /* ---- 3. the alarm tab is honest about what an alert can do --------- */

    tap_obj(find_labelled(app_body, "Alarm"));
    check("the alarm list starts empty", label_present(app_body, "No alarms yet"));
    check("and offers to add one", label_present(app_body, "Add alarm"));
    check("the alert capability is stated on screen",
          find_text_anywhere(app_body, clock_alert_why()) != NULL);
    check("so is what an alarm does and does not survive",
          find_text_anywhere(app_body,
                             "Alarms ring with Clock closed. They do not ring "
                             "with the device switched off.") != NULL);

    /* ---- 4. add one, tapping every control ----------------------------- */

    tap_obj(find_labelled(app_body, "Add alarm"));
    form = screen_of(SCREEN_ADD);
    check("the form opens", label_present(app_body, "Repeat: Once"));
    check("with an hour row", strcmp(text_of(stepper_cell(form, 1, STEP_VALUE)),
                                     "Hour") == 0);
    check("and a minute row", strcmp(text_of(stepper_cell(form, 2, STEP_VALUE)),
                                     "Minute") == 0);

    set_form_time("08:15");
    check_str("the steppers reach the time that was wanted",
              text_of(hero_in(form)), "08:15");
    tap_obj(stepper_cell(form, 1, STEP_MINUS));
    check_str("and step back down", text_of(hero_in(form)), "07:15");
    tap_obj(stepper_cell(form, 1, STEP_PLUS));

    tap_obj(find_labelled(app_body, "Repeat: Once"));
    check("the repeat cycles", label_present(app_body, "Repeat: Daily"));

    field = lv_obj_get_child(lv_obj_get_child(form, 4), 0);
    check("the form has a label field",
          lv_obj_check_type(field, &lv_textarea_class));
    tap_obj(field);
    check("tapping it brings the keyboard up", pocketos_shell_keyboard_visible());
    check("and focuses the field", pos_input_focused() == field);
    type_text("Tea");
    check_str("what is typed reaches the label", lv_textarea_get_text(field), "Tea");

    tap_obj(find_labelled(app_body, "Add"));
    check("adding puts the keyboard away", !pocketos_shell_keyboard_visible());
    check("the alarm is in the list", label_present(app_body, "08:15"));
    check("with its repeat and label", label_present(app_body, "Daily  Tea"));
    check("and it is on", label_present(app_body, "On"));
    check("the empty state is gone", !label_present(app_body, "No alarms yet"));

    check("it was written to the store", stored(&disk) == 0);
    check("as one alarm", clock_alarm_count(&disk) == 1);
    check("at the time that was set", clock_alarm_at(&disk, 0)->hour == 8 &&
                                      clock_alarm_at(&disk, 0)->minute == 15);
    check_str("with the label that was typed", clock_alarm_at(&disk, 0)->label, "Tea");
    check("and daily", clock_alarm_at(&disk, 0)->repeat == CLOCK_REPEAT_DAILY);

    /* ---- 5. tapping the row switches it off, and that is saved too ----- */

    tap_obj(find_labelled(app_body, "08:15"));
    check("tapping the row switches it off", label_present(app_body, "Off"));
    check("in words, not by a colour alone", !label_present(app_body, "On"));
    stored(&disk);
    check("and the store followed", !clock_alarm_at(&disk, 0)->enabled);
    tap_obj(find_labelled(app_body, "08:15"));
    check("and back on again", label_present(app_body, "On"));

    /* ---- 6. deleting asks first (DS 17.5) ------------------------------ */

    tap_obj(alarm_row_delete("08:15"));
    check("the confirmation opens", label_present(app_body, "Delete this alarm?"));
    check("the list is not shown behind it", !label_present(app_body, "Add alarm"));
    tap_obj(find_labelled(app_body, "Cancel"));
    check("cancelling leaves the alarm alone", label_present(app_body, "08:15"));
    stored(&disk);
    check("and the store untouched", clock_alarm_count(&disk) == 1);

    tap_obj(alarm_row_delete("08:15"));
    tap_obj(find_labelled(app_body, "Delete"));
    check("confirming removes it", !label_present(app_body, "08:15"));
    check("and the empty state is back", label_present(app_body, "No alarms yet"));
    stored(&disk);
    check("and it is gone from the store", clock_alarm_count(&disk) == 0);

    /* ---- 7. an alarm survives leaving the app, and a power cut --------- */

    tap_obj(find_labelled(app_body, "Add alarm"));
    form = screen_of(SCREEN_ADD);
    set_form_time("06:30");
    snprintf(kept, sizeof(kept), "%s", text_of(hero_in(form)));
    tap_obj(find_labelled(app_body, "Add"));
    check("an alarm with no label can be added", label_present(app_body, kept));
    check("and needs no label to show its repeat", label_present(app_body, "Once"));

    /* Closing the app leaves the runtime holding it. */
    app_stop();
    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("it is still there after the app is closed and reopened",
          label_present(app_body, kept));

    /* And a power cut takes the runtime with it, so this time the alarm has
     * to come back off the disk. */
    app_stop();
    reboot_runtime();
    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("and after the shell itself has restarted",
          label_present(app_body, kept));
    stored(&disk);
    check("one alarm on disk", clock_alarm_count(&disk) == 1);
    tap_obj(alarm_row_delete(kept));
    tap_obj(find_labelled(app_body, "Delete"));
    check("and can be deleted again afterwards",
          label_present(app_body, "No alarms yet"));

    /* ---- 8. the stopwatch ---------------------------------------------- */

    tap_obj(find_labelled(app_body, "Watch"));
    check_str("the stopwatch starts at zero",
              text_of(hero_in(pane_of(PANE_WATCH))), "00:00.00");
    check("and offers Start", label_present(app_body, "Start"));
    check("and Reset", label_present(app_body, "Reset"));

    tap_obj(find_labelled(app_body, "Start"));
    check("starting turns it into Pause", label_present(app_body, "Pause"));
    check("and Reset into Lap", label_present(app_body, "Lap"));
    /* The stopwatch reads the real monotonic clock, which pumping LVGL ticks
     * does not move: a thousand pumps can still be under a hundredth of a
     * second of real time. So this waits for three real hundredths to have
     * passed - thirty milliseconds, not the seconds the brief ruled out -
     * and then asks what the display says. */
    {
        struct clock_now t0;
        struct clock_now t1;

        clock_now_read(&t0);
        do {
            pump(20);
            clock_now_read(&t1);
        } while (t1.mono_ms - t0.mono_ms < 30);
        snprintf(before, sizeof(before), "%s",
                 text_of(hero_in(pane_of(PANE_WATCH))));
    }
    check("a running stopwatch is no longer at zero",
          strcmp(before, "00:00.00") != 0);

    tap_obj(find_labelled(app_body, "Lap"));
    check("a lap is listed", label_present(app_body, "1"));
    tap_obj(find_labelled(app_body, "Pause"));
    check("pausing offers Start again", label_present(app_body, "Start"));
    check("and Reset", label_present(app_body, "Reset"));
    snprintf(before, sizeof(before), "%s", text_of(hero_in(pane_of(PANE_WATCH))));
    pump(400);
    check_str("and a paused stopwatch stays where it stopped",
              text_of(hero_in(pane_of(PANE_WATCH))), before);
    tap_obj(find_labelled(app_body, "Reset"));
    check_str("reset returns it to zero",
              text_of(hero_in(pane_of(PANE_WATCH))), "00:00.00");
    check("and clears the laps", !label_present(app_body, "1"));

    /* ---- 9. the timer -------------------------------------------------- */

    tap_obj(find_labelled(app_body, "Timer"));
    timer_pane = pane_of(PANE_TIMER);
    check_str("the minute row starts at zero",
              text_of(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_VALUE)),
              "Minutes 00");
    check_str("and the second row",
              text_of(stepper_cell(lv_obj_get_child(timer_pane, 1), 1, STEP_VALUE)),
              "Seconds 00");

    tap_obj(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_PLUS));
    check_str("a minute is added",
              text_of(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_VALUE)),
              "Minutes 01");
    tap_obj(stepper_cell(lv_obj_get_child(timer_pane, 1), 1, STEP_PLUS_BIG));
    check_str("and ten seconds",
              text_of(stepper_cell(lv_obj_get_child(timer_pane, 1), 1, STEP_VALUE)),
              "Seconds 10");
    check_str("the countdown shows what was set",
              text_of(hero_in(timer_pane)), "01:10");
    tap_obj(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_MINUS_BIG));
    check_str("stepping below zero clamps rather than wrapping round",
              text_of(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_VALUE)),
              "Minutes 00");
    tap_obj(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_PLUS));

    tap_obj(find_labelled(app_body, "Start"));
    check("starting hides the setter",
          lv_obj_has_flag(lv_obj_get_child(timer_pane, 1), LV_OBJ_FLAG_HIDDEN));
    check("and offers Pause", label_present(app_body, "Pause"));
    check("and Cancel", label_present(app_body, "Cancel"));
    stored(&disk);
    check("the duration is a setting and was saved",
          disk.timer.duration_ms == 70000);
    check("but what is left of the countdown is not",
          disk.timer.remaining_ms == disk.timer.duration_ms);

    tap_obj(find_labelled(app_body, "Pause"));
    check("pausing offers Start again", label_present(app_body, "Start"));
    tap_obj(find_labelled(app_body, "Cancel"));
    check("cancelling brings the setter back",
          !lv_obj_has_flag(lv_obj_get_child(timer_pane, 1), LV_OBJ_FLAG_HIDDEN));
    check_str("with the duration that was set",
              text_of(stepper_cell(lv_obj_get_child(timer_pane, 1), 0, STEP_VALUE)),
              "Minutes 01");

    /* ---- 10. the alert is the shell's, and rings with the app shut ----- */

    /* This is the thing the whole runtime exists for, so it is tested with
     * PocketClock closed: no app, no app timer, nothing of the Clock UI on
     * screen at all. The clock is injected, because 07:30 is not worth
     * waiting for. */
    app_stop();
    check("no app is running", lv_obj_get_child_count(g_content) == 0);
    check("and nothing is alerting", !shell_alarm_visible());

    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now;

        clock_alarm_add(e, 7, 30, CLOCK_REPEAT_DAILY, "Wake up", NULL);
        clock_runtime_save();

        now = at(20260911, 7, 0, 5, 1000);
        clock_runtime_step_at(&now);
        pump(60);
        check("still quiet before its time", !shell_alarm_visible());

        now = at(20260911, 7, 30, 5, 1800000);
        clock_runtime_step_at(&now);
        pump(60);
        check("the alarm rings with PocketClock closed", shell_alarm_visible());
        check("and says what it is",
              find_text_anywhere(lv_screen_active(), "Alarm") != NULL);
        check("and which alarm it was",
              find_text_anywhere(lv_screen_active(), "07:30  Wake up") != NULL);
        check("with Stop on it", on_screen("Stop") != NULL);
        check("and a snooze", on_screen("Snooze 9 min") != NULL);
        check("and what an alert can do on this board",
              find_text_anywhere(lv_screen_active(), clock_alert_why()) != NULL);

        /* Ticking through the same minute must not produce a second alert. */
        clock_runtime_step_at(&now);
        clock_runtime_step_at(&now);
        pump(60);
        check("and it is still one alert", shell_alarm_visible());

        tap_obj(on_screen("Stop"));
        check("Stop puts it away", !shell_alarm_visible());
        check("and acknowledges it", e->ringing == CLOCK_RING_NONE);

        now = at(20260911, 7, 31, 5, 1860000);
        clock_runtime_step_at(&now);
        pump(60);
        check("an acknowledged alarm does not come back",
              !shell_alarm_visible());
    }

    /* ---- 11. and it interrupts whatever is on screen -------------------- */

    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("PocketClock is up", label_present(app_body, "Add alarm"));
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now = at(20260912, 7, 30, 6, 90000000);

        clock_runtime_step_at(&now);
        pump(60);
        check("the alert covers the app it is over", shell_alarm_visible());
        check("and the app is not the one drawing it",
              find_text_anywhere(app_body, "Snooze 9 min") == NULL);
        check("PocketClock has no ringing screen of its own",
              find_text_anywhere(app_body, "Stop") == NULL);

        tap_obj(on_screen("Stop"));
        check("Stop from over the app works too", !shell_alarm_visible());
        check("and acknowledged it", e->ringing == CLOCK_RING_NONE);
        check("PocketClock is where it was", label_present(app_body, "Add alarm"));
    }

    /* A one-shot alarm switches itself off when the shell acknowledges it.
     * The list on screen was drawn before that happened, and it has to
     * notice - otherwise the owner is looking at a row that says On for an
     * alarm that will never ring again. */
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now;

        clock_alarm_add(e, 9, 0, CLOCK_REPEAT_ONCE, "once only", NULL);
        pump(200);
        check("the new alarm appears without the app being told",
              label_present(app_body, "09:00"));
        check("switched on", label_present(app_body, "On"));
        check("and none are off yet", !label_present(app_body, "Off"));

        now = at(20260912, 9, 0, 6, 95000000);
        clock_runtime_step_at(&now);
        pump(60);
        check("it rings over the app", shell_alarm_visible());
        tap_obj(on_screen("Stop"));
        pump(200);
        check("and the list on screen notices it switched itself off",
              label_present(app_body, "Off"));
        check("without anyone having touched the app",
              !shell_alarm_visible());
    }

    /* ---- 12. the countdown finishes in the background too -------------- */

    app_stop();
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now now = unset_now(500000);

        check("a countdown is set", clock_timer_set(e, 0, 1, 0));
        check("and started", clock_timer_start(e, &now));

        now = unset_now(500000 + 59000);
        clock_runtime_step_at(&now);
        pump(60);
        check("it has not finished", !shell_alarm_visible());

        now = unset_now(500000 + 60000);
        clock_runtime_step_at(&now);
        pump(60);
        check("the timer rings with PocketClock closed", shell_alarm_visible());
        check("and says so",
              find_text_anywhere(lv_screen_active(), "Timer finished") != NULL);
        check("there is nothing to snooze on a countdown",
              on_screen("Snooze 9 min") == NULL);
        check("but there is a Stop", on_screen("Stop") != NULL);
        check("none of which needed a wall clock",
              !clock_runtime_now()->wall.valid);

        tap_obj(on_screen("Stop"));
        check("Stop dismisses it", !shell_alarm_visible());
        check("and returns the timer to idle", e->timer.state == CLOCK_TIMER_IDLE);
    }

    /* ---- 13. switching an alarm back on after its time ------------------ */

    /* An alarm switched off before its time and on again after it rang the
     * moment its row was tapped (P1-3 of the v0.0.8 review). The app reads
     * the real clock on its refresh, so the alarm sits at the real current
     * minute - which counts as gone by - and the runtime is stepped with a
     * real reading, the way the shell's tick steps it. */
    {
        struct clock_engine *e = clock_runtime_engine();
        struct clock_now real;
        char hm[8];

        clock_now_read(&real);
        if (!real.wall.valid) {
            printf("note: the host clock is not set, so the re-enable check cannot run\n");
        } else {
            while (clock_alarm_count(e) > 0) {
                clock_alarm_remove(e, 0);
            }
            /* The clock has been real since before this alarm was set, as it
             * is on a board whose time was set at boot. */
            clock_runtime_step_at(&real);
            clock_alarm_add(e, real.wall.hour, real.wall.minute, CLOCK_REPEAT_DAILY,
                            "back on", NULL);
            clock_format_hm(real.wall.hour, real.wall.minute, hm, sizeof(hm));

            app_start();
            tap_obj(find_labelled(app_body, "Alarm"));
            tap_obj(find_labelled(app_body, hm));
            check("a tap switches it off", label_present(app_body, "Off"));
            tap_obj(find_labelled(app_body, hm));
            check("and a second tap on again, after its time",
                  label_present(app_body, "On"));
            clock_now_read(&real);
            clock_runtime_step_at(&real);
            pump(60);
            check("switched on after its time, it does not ring on the spot",
                  !shell_alarm_visible());
            check("and nothing is ringing", e->ringing == CLOCK_RING_NONE);
            check("it stays on, for its next time",
                  clock_alarm_at(e, 0) && clock_alarm_at(e, 0)->enabled);
            app_stop();
            while (clock_alarm_count(e) > 0) {
                clock_alarm_remove(e, 0);
            }
            clock_runtime_save();
        }
    }

    /* ---- 14. opening and closing repeatedly ---------------------------- */

    app_start();
    app_stop();
    check("the app can be opened and closed without leaving a timer behind",
          lv_obj_get_child_count(g_content) == 0);

    wipe();
    printf("clock_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
