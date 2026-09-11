/*
 * PocketClock in the running app, driven by a real LVGL pointer device and a
 * real touch keyboard, against a real (temporary) store.
 *
 * Every alarm here is made the way a finger makes one: taps on the steppers,
 * taps on the keyboard, a tap on Add. What is then read back off the disk is
 * what the store actually kept.
 *
 * WHAT IS NOT HERE, and why. The ringing screen is reached by real elapsed
 * time on CLOCK_MONOTONIC, which this harness cannot fast-forward: pumping
 * LVGL ticks moves the UI, not the clock the engine reads. Sitting here for
 * real seconds to watch a countdown finish is the one thing the brief ruled
 * out, so the firing rules are tested in tests/clock_engine_test.c with both
 * clocks injected, and this file checks only that the screen is built.
 *
 * The app is hosted the way the shell hosts it, but the shell itself is not
 * here, so the three app.h keyboard entry points are implemented below
 * against the real pos_keyboard - which keeps the app honest: it can only
 * ask, and it never sees a keyboard.
 *
 * Needs LVGL, so it is built by ui/shell/CMakeLists.txt beside the shell
 * (host builds only) and run by tests/clock_shell_test.sh.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "clock_alert.h"
#include "clock_store.h"
#include "clock_time.h"
#include "pocketui.h"
#include "pos_keyboard.h"

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
    check("so is the fact that alarms need the app open",
          find_text_anywhere(app_body,
                             "Alarms ring while Clock is open. PocketOS has no "
                             "background apps yet.") != NULL);

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

    /* ---- 7. an alarm survives leaving the app -------------------------- */

    tap_obj(find_labelled(app_body, "Add alarm"));
    form = screen_of(SCREEN_ADD);
    set_form_time("06:30");
    snprintf(kept, sizeof(kept), "%s", text_of(hero_in(form)));
    tap_obj(find_labelled(app_body, "Add"));
    check("an alarm with no label can be added", label_present(app_body, kept));
    check("and needs no label to show its repeat", label_present(app_body, "Once"));

    app_stop();
    app_start();
    tap_obj(find_labelled(app_body, "Alarm"));
    check("it is still there after the app is closed and reopened",
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

    /* ---- 10. the ringing screen is built ------------------------------- */

    check("there is a Stop", find_text_anywhere(app_body, "Stop") != NULL);
    check("and a snooze of nine minutes",
          find_text_anywhere(app_body, "Snooze 9 min") != NULL);
    check("and it repeats what an alert can do on this board",
          find_text_anywhere(app_body, clock_alert_why()) != NULL);

    /* ---- 11. opening and closing repeatedly ---------------------------- */

    app_stop();
    app_start();
    app_stop();
    check("the app can be opened and closed without leaving a timer behind",
          lv_obj_get_child_count(g_content) == 0);

    wipe();
    printf("clock_app_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
