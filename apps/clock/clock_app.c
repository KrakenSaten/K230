/*
 * PocketClock: the time, an alarm, a stopwatch and a timer.
 *
 * Four panes behind four tabs, and three screens that take the whole body
 * when they are needed: a new-alarm form, a delete confirmation and the
 * ringing screen. No timing decision is made here - every one of them is in
 * clock_engine.c, which this file feeds one reading of both clocks and then
 * reads back. The views only draw.
 *
 * TWO THINGS THIS APP CANNOT DO, and says so on screen rather than letting
 * the owner find out:
 *
 *   1. The board has no battery-backed RTC, so after a power cycle it does
 *      not know the time until something sets it. Until then the clock face
 *      shows "--:--" and alarms do not ring. A believable wrong time would
 *      be worse than an obvious blank one.
 *   2. An alarm rings while PocketClock is open. The v0.1 app lifecycle has
 *      no background: an app is created when opened and destroyed when left
 *      (ADR-002), so there is nothing to run a clock behind the launcher.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "clock_alert.h"
#include "clock_engine.h"
#include "clock_store.h"
#include "clock_time.h"
#include "pocketui.h"

#include <stdio.h>
#include <string.h>

enum clock_tab {
    TAB_CLOCK = 0,
    TAB_ALARM,
    TAB_STOPWATCH,
    TAB_TIMER,
    TAB_COUNT
};

enum clock_screen {
    SCREEN_MAIN = 0,
    SCREEN_ADD,
    SCREEN_CONFIRM,
    SCREEN_RING,
    SCREEN_COUNT
};

/* Ten a second: the stopwatch shows hundredths, and anything slower makes
 * the last digit stutter. Nothing is written to storage on this timer. */
#define CLOCK_REFRESH_MS 100
#define TIMER_SET_MAX_MINUTES 99

struct clock_app {
    struct clock_engine engine;
    struct clock_now now;

    lv_obj_t *screen[SCREEN_COUNT];
    lv_obj_t *tab_btn[TAB_COUNT];
    lv_obj_t *pane[TAB_COUNT];
    lv_timer_t *refresh;
    uint8_t tab;
    uint8_t screen_id;
    uint8_t return_screen; /* where the ringing screen came from */

    /* Clock */
    lv_obj_t *face_card;
    bool face_grown;
    lv_obj_t *face_time;
    lv_obj_t *face_date;
    lv_obj_t *unset_card;

    /* Alarm */
    lv_obj_t *alarm_notice;
    lv_obj_t *alarm_list;
    lv_obj_t *alarm_add_btn;
    lv_obj_t *alarm_full;
    int confirm_index;

    /* New alarm */
    int add_hour;
    int add_minute;
    int add_repeat;
    lv_obj_t *add_time;
    lv_obj_t *add_repeat_btn;
    lv_obj_t *add_label;

    /* Stopwatch */
    lv_obj_t *sw_value;
    lv_obj_t *sw_run_btn;
    lv_obj_t *sw_lap_btn;
    lv_obj_t *sw_laps;
    int sw_laps_drawn;

    /* Timer */
    int set_minutes;
    int set_seconds;
    lv_obj_t *tm_value;
    lv_obj_t *tm_setter;
    lv_obj_t *tm_min;
    lv_obj_t *tm_sec;
    lv_obj_t *tm_run_btn;
    lv_obj_t *tm_cancel_btn;

    /* Ringing */
    lv_obj_t *ring_title;
    lv_obj_t *ring_detail;
    lv_obj_t *ring_snooze_btn;
};

static void show_screen(struct clock_app *a, enum clock_screen which);
static void build_alarm_list(struct clock_app *a);
static void refresh_all(struct clock_app *a);

/* ---- small helpers ----------------------------------------------------- */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* pocketui_button puts its label first and keeps it there. */
static void set_button_text(lv_obj_t *btn, const char *text)
{
    if (btn) {
        set_text(lv_obj_get_child(btn, 0), text);
    }
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Destructive and secondary buttons drop the accent: the bright fill is for
 * the safe choice (DS §17.5). */
static void make_secondary(lv_obj_t *btn)
{
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(btn, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED),
                        LV_STATE_PRESSED);
    pos_style_add(btn, POS_STYLE_BUTTON_SECONDARY, 0);
}

static lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56); /* paired buttons, DS §7 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *paired_button(lv_obj_t *row, const char *text,
                               lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = pocketui_button(row, text, cb, user);

    lv_obj_set_height(btn, 56);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return btn;
}

static void save(struct clock_app *a)
{
    /* Only ever from a change the owner made, never from the refresh timer:
     * an alarm list is a setting, and settings are not written per tick. */
    if (clock_store_save(&a->engine) != 0) {
        pocketos_shell_set_status_hint("Alarms not saved");
    }
}

/* ---- the ringing screen ------------------------------------------------ */

static void ring_enter(struct clock_app *a)
{
    char line[64];
    const struct clock_alarm *al;

    if (a->engine.ringing == CLOCK_RING_ALARM) {
        clock_alert_begin(CLOCK_ALERT_ALARM);
        set_text(a->ring_title, "Alarm");
        al = clock_alarm_at(&a->engine, a->engine.ringing_alarm);
        if (al && al->label[0]) {
            char hm[8];

            clock_format_hm(al->hour, al->minute, hm, sizeof(hm));
            snprintf(line, sizeof(line), "%s  %s", hm, al->label);
        } else if (al) {
            clock_format_hm(al->hour, al->minute, line, sizeof(line));
        } else {
            snprintf(line, sizeof(line), "Alarm");
        }
        set_hidden(a->ring_snooze_btn, false);
    } else {
        clock_alert_begin(CLOCK_ALERT_TIMER);
        set_text(a->ring_title, "Timer finished");
        clock_format_remaining(a->engine.timer.duration_ms, line, sizeof(line));
        set_hidden(a->ring_snooze_btn, true);
    }
    set_text(a->ring_detail, line);
    if (a->screen_id != SCREEN_RING) {
        a->return_screen = a->screen_id;
        /* A dialog over a field takes the keyboard away (DS §17.5). */
        pocketos_shell_keyboard_hide();
        show_screen(a, SCREEN_RING);
    }
}

static void ring_leave(struct clock_app *a)
{
    clock_alert_end();
    if (a->screen_id == SCREEN_RING) {
        show_screen(a, a->return_screen == SCREEN_RING ? SCREEN_MAIN
                                                       : a->return_screen);
    }
}

static void on_ring_stop(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (a->engine.ringing == CLOCK_RING_ALARM) {
        clock_alarm_acknowledge(&a->engine, &a->now);
        /* A one-shot alarm switched itself off doing that, so the list and
         * the file both have to follow. */
        build_alarm_list(a);
        save(a);
    } else {
        clock_timer_acknowledge(&a->engine);
    }
    ring_leave(a);
    refresh_all(a);
}

static void on_ring_snooze(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    clock_alarm_snooze(&a->engine, &a->now);
    ring_leave(a);
    refresh_all(a);
}

/* ---- the clock face ---------------------------------------------------- */

static void refresh_face(struct clock_app *a)
{
    char buf[32];

    /* With a real time to show, the face takes the room the pane has, so the
     * clock sits in the middle of the panel rather than clinging to the top
     * of it. Without one it shrinks back, because a card the height of the
     * screen holding nothing but "--:--" is a worse way of saying the same
     * thing than the explanation below it. */
    if (a->face_grown != a->now.wall.valid) {
        a->face_grown = a->now.wall.valid;
        lv_obj_set_flex_grow(a->face_card, a->face_grown ? 1 : 0);
    }
    clock_format_wall(&a->now.wall, buf, sizeof(buf));
    set_text(a->face_time, buf);
    clock_format_date(&a->now.wall, buf, sizeof(buf));
    set_text(a->face_date, a->now.wall.valid ? buf : "");
    set_hidden(a->face_date, !a->now.wall.valid);
    set_hidden(a->unset_card, a->now.wall.valid);
}

/* ---- the alarm list ---------------------------------------------------- */

static void on_alarm_row(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    int index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const struct clock_alarm *al = clock_alarm_at(&a->engine, index);

    if (!al) {
        return;
    }
    clock_alarm_set_enabled(&a->engine, index, !al->enabled);
    build_alarm_list(a);
    save(a);
}

static void on_alarm_delete(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    a->confirm_index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    /* Nothing is removed here; only the confirmation's Delete removes it
     * (DS §17.5). */
    show_screen(a, SCREEN_CONFIRM);
}

static void build_alarm_list(struct clock_app *a)
{
    int n = clock_alarm_count(&a->engine);
    int i;

    lv_obj_clean(a->alarm_list);
    if (n == 0) {
        lv_obj_t *empty = pocketui_label(a->alarm_list, "No alarms yet",
                                         POS_STYLE_TEXT_SECONDARY);

        lv_obj_set_style_pad_ver(empty, 20, 0);
    }
    for (i = 0; i < n; i++) {
        const struct clock_alarm *al = clock_alarm_at(&a->engine, i);
        lv_obj_t *row = lv_obj_create(a->alarm_list);
        lv_obj_t *text;
        lv_obj_t *time_lb;
        lv_obj_t *state;
        lv_obj_t *del;
        lv_obj_t *glyph;
        char buf[64];

        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), POCKETUI_ROW_H);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i + 1 < n) {
            pos_style_add(row, POS_STYLE_DIVIDER, 0);
        }

        /* The whole row toggles the alarm; the trailing button is the only
         * other hit area, and both are the full 64 px of DS §7. */
        text = lv_obj_create(row);
        lv_obj_remove_style_all(text);
        lv_obj_set_height(text, LV_PCT(100));
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(text, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        pos_style_add(text, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_user_data(text, (void *)(intptr_t)i);
        lv_obj_add_event_cb(text, on_alarm_row, LV_EVENT_CLICKED, a);

        clock_format_hm(al->hour, al->minute, buf, sizeof(buf));
        time_lb = pocketui_label(text, buf, POS_STYLE_VALUE);
        if (!al->enabled) {
            pos_style_add(time_lb, POS_STYLE_TEXT_MUTED, 0);
        }
        if (al->label[0]) {
            snprintf(buf, sizeof(buf), "%s  %s", clock_repeat_name(al->repeat),
                     al->label);
        } else {
            snprintf(buf, sizeof(buf), "%s", clock_repeat_name(al->repeat));
        }
        {
            lv_obj_t *caption = pocketui_label(text, buf, POS_STYLE_CAPTION);

            /* A label long enough to reach the state and the delete button
             * is cut with an ellipsis rather than drawn over them. */
            lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
            lv_obj_set_width(caption, LV_PCT(100));
        }

        /* On and Off in words, never a colour alone (DS §2). */
        state = pocketui_label(row, al->enabled ? "On" : "Off", POS_STYLE_CAPTION);
        pos_style_add(state, al->enabled ? POS_STYLE_STATUS_OK_TEXT
                                         : POS_STYLE_TEXT_MUTED, 0);

        /* No fill of its own: eight of these down a list would read as one
         * grey column rather than eight buttons. It is still the full
         * POCKETUI_TOUCH_MIN square, and pressing it shows the slab. */
        del = lv_button_create(row);
        lv_obj_remove_style_all(del);
        pos_style_add(del, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_size(del, POCKETUI_TOUCH_MIN, POCKETUI_TOUCH_MIN);
        lv_obj_clear_flag(del, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(del, (void *)(intptr_t)i);
        lv_obj_add_event_cb(del, on_alarm_delete, LV_EVENT_CLICKED, a);
        /* POS_STYLE_SYMBOL and nothing else: a text role added after it wins
         * on font as well as colour, and the body font has no trash can in
         * it - the glyph comes out as an empty box. */
        glyph = pocketui_label(del, LV_SYMBOL_TRASH, POS_STYLE_SYMBOL);
        lv_obj_center(glyph);
    }
    set_hidden(a->alarm_add_btn, n >= CLOCK_MAX_ALARMS);
    set_hidden(a->alarm_full, n < CLOCK_MAX_ALARMS);
}

static void refresh_alarm(struct clock_app *a)
{
    set_hidden(a->alarm_notice, a->now.wall.valid);
}

/* ---- the delete confirmation ------------------------------------------- */

static void on_confirm_cancel(lv_event_t *e)
{
    show_screen(lv_event_get_user_data(e), SCREEN_MAIN);
}

static void on_confirm_delete(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    clock_alarm_remove(&a->engine, a->confirm_index);
    build_alarm_list(a);
    save(a);
    show_screen(a, SCREEN_MAIN);
}

/* ---- the new-alarm form ------------------------------------------------ */

static void refresh_add(struct clock_app *a)
{
    char buf[32];

    clock_format_hm(a->add_hour, a->add_minute, buf, sizeof(buf));
    set_text(a->add_time, buf);
    snprintf(buf, sizeof(buf), "Repeat: %s",
             clock_repeat_name((enum clock_repeat)a->add_repeat));
    set_button_text(a->add_repeat_btn, buf);
}

static void on_hour_step(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    int delta = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    a->add_hour = ((a->add_hour + delta) % 24 + 24) % 24;
    refresh_add(a);
}

static void on_minute_step(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    int delta = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    a->add_minute = ((a->add_minute + delta) % 60 + 60) % 60;
    refresh_add(a);
}

static void on_repeat_cycle(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    a->add_repeat = (a->add_repeat + 1) % CLOCK_REPEAT_COUNT;
    refresh_add(a);
}

static void on_label_clicked(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, a);
    }
}

static void on_add_cancel(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    pocketos_shell_keyboard_hide();
    show_screen(a, SCREEN_MAIN);
}

static void on_add_confirm(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    const char *label = lv_textarea_get_text(a->add_label);

    if (!clock_label_is_storable(label)) {
        pocketui_text_field_set_error(a->add_label,
                                      "That label is too long to store.");
        return;
    }
    if (clock_alarm_add(&a->engine, a->add_hour, a->add_minute,
                        (enum clock_repeat)a->add_repeat,
                        *label ? label : NULL, &a->now) < 0) {
        pocketos_shell_set_status_hint("No room for another alarm");
        return;
    }
    pocketui_text_field_set_error(a->add_label, NULL);
    pocketos_shell_keyboard_hide();
    build_alarm_list(a);
    save(a);
    show_screen(a, SCREEN_MAIN);
}

static void on_add_open(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    /* A new alarm opens at the current time when there is one, and at 07:00
     * when the board does not know what time it is. */
    if (a->now.wall.valid) {
        a->add_hour = a->now.wall.hour;
        a->add_minute = a->now.wall.minute;
    } else {
        a->add_hour = 7;
        a->add_minute = 0;
    }
    a->add_repeat = CLOCK_REPEAT_ONCE;
    lv_textarea_set_text(a->add_label, "");
    pocketui_text_field_set_error(a->add_label, NULL);
    refresh_add(a);
    show_screen(a, SCREEN_ADD);
}

/* ---- the stopwatch ----------------------------------------------------- */

static void on_sw_run(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (a->engine.sw.state == CLOCK_SW_RUNNING) {
        clock_sw_pause(&a->engine, &a->now);
    } else {
        clock_sw_start(&a->engine, &a->now);
    }
    refresh_all(a);
}

static void on_sw_lap(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (a->engine.sw.state == CLOCK_SW_RUNNING) {
        clock_sw_lap(&a->engine, &a->now);
    } else {
        clock_sw_reset(&a->engine);
    }
    refresh_all(a);
}

static void refresh_sw(struct clock_app *a)
{
    char buf[24];
    int running = a->engine.sw.state == CLOCK_SW_RUNNING;
    int i;

    clock_format_elapsed(clock_sw_elapsed_ms(&a->engine, &a->now), buf, sizeof(buf));
    set_text(a->sw_value, buf);
    set_button_text(a->sw_run_btn, running ? "Pause" : "Start");
    set_button_text(a->sw_lap_btn, running ? "Lap" : "Reset");

    /* An empty lap list is an empty box the height of the panel, so it is
     * not shown until there is a lap in it. */
    set_hidden(a->sw_laps, a->engine.sw.lap_count == 0);

    /* The list only ever grows or is cleared, so it is rebuilt on those two
     * events and left alone by the ten-a-second refresh. */
    if (a->sw_laps_drawn == a->engine.sw.lap_count) {
        return;
    }
    if (a->engine.sw.lap_count < a->sw_laps_drawn) {
        lv_obj_clean(a->sw_laps);
        a->sw_laps_drawn = 0;
    }
    for (i = a->sw_laps_drawn; i < a->engine.sw.lap_count; i++) {
        char n[16];

        snprintf(n, sizeof(n), "%d", i + 1);
        clock_format_elapsed(a->engine.sw.laps[i], buf, sizeof(buf));
        pocketui_kv_row(a->sw_laps, n, buf);
    }
    a->sw_laps_drawn = a->engine.sw.lap_count;
}

/* ---- the timer --------------------------------------------------------- */

static void on_timer_step(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    int delta = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    /* The minute row multiplies its step by 100 and the second row by 1, so
     * one callback serves both and reads which it was from the number. */
    int minutes = delta / 100;
    int seconds = delta % 100;

    if (minutes) {
        a->set_minutes += minutes;
        if (a->set_minutes < 0) {
            a->set_minutes = 0;
        }
        if (a->set_minutes > TIMER_SET_MAX_MINUTES) {
            a->set_minutes = TIMER_SET_MAX_MINUTES;
        }
    }
    if (seconds) {
        a->set_seconds += seconds;
        if (a->set_seconds < 0) {
            a->set_seconds = 0;
        }
        if (a->set_seconds > 59) {
            a->set_seconds = 59;
        }
    }
    refresh_all(a);
}

static void on_timer_run(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (a->engine.timer.state == CLOCK_TIMER_RUNNING) {
        clock_timer_pause(&a->engine, &a->now);
    } else {
        if (a->engine.timer.state == CLOCK_TIMER_IDLE &&
            !clock_timer_set(&a->engine, 0, a->set_minutes, a->set_seconds)) {
            pocketos_shell_set_status_hint("Set a duration first");
            return;
        }
        pocketos_shell_set_status_hint("");
        if (clock_timer_start(&a->engine, &a->now)) {
            /* The duration is a setting and survives a reboot; what is left
             * of a running countdown does not, and is not written. */
            save(a);
        }
    }
    refresh_all(a);
}

static void on_timer_cancel(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    clock_timer_cancel(&a->engine);
    refresh_all(a);
}

static void refresh_timer(struct clock_app *a)
{
    char buf[24];
    int idle = a->engine.timer.state == CLOCK_TIMER_IDLE;
    int running = a->engine.timer.state == CLOCK_TIMER_RUNNING;

    if (idle) {
        clock_format_remaining((int64_t)(a->set_minutes * 60 + a->set_seconds) * 1000,
                               buf, sizeof(buf));
    } else {
        clock_format_remaining(clock_timer_remaining_ms(&a->engine, &a->now), buf,
                               sizeof(buf));
    }
    set_text(a->tm_value, buf);
    set_hidden(a->tm_setter, !idle);
    set_hidden(a->tm_cancel_btn, idle);
    set_button_text(a->tm_run_btn, running ? "Pause" : "Start");
    snprintf(buf, sizeof(buf), "Minutes %02d", a->set_minutes);
    set_text(a->tm_min, buf);
    snprintf(buf, sizeof(buf), "Seconds %02d", a->set_seconds);
    set_text(a->tm_sec, buf);
}

/* ---- tabs and screens -------------------------------------------------- */

static void refresh_all(struct clock_app *a)
{
    refresh_face(a);
    refresh_alarm(a);
    refresh_sw(a);
    refresh_timer(a);
    refresh_add(a);
}

static void select_tab(struct clock_app *a, enum clock_tab tab)
{
    int i;

    a->tab = (uint8_t)tab;
    for (i = 0; i < TAB_COUNT; i++) {
        set_hidden(a->pane[i], i != (int)tab);
        /* The selected tab carries the accent and is the only one that does;
         * the label says which it is either way (DS §2). It is a state, not a
         * style swapped in and out - tapping the tab you are already on would
         * otherwise add another copy of the same style every time. */
        if (i == (int)tab) {
            lv_obj_add_state(a->tab_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(a->tab_btn[i], LV_STATE_CHECKED);
        }
    }
}

static void on_tab(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    int which = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    select_tab(a, (enum clock_tab)which);
}

static void show_screen(struct clock_app *a, enum clock_screen which)
{
    int i;

    for (i = 0; i < SCREEN_COUNT; i++) {
        set_hidden(a->screen[i], i != (int)which);
    }
    a->screen_id = (uint8_t)which;
}

/* ---- the refresh ------------------------------------------------------- */

static void on_refresh(lv_timer_t *timer)
{
    struct clock_app *a = lv_timer_get_user_data(timer);

    clock_now_read(&a->now);
    clock_engine_step(&a->engine, &a->now);

    if (a->engine.ringing != CLOCK_RING_NONE) {
        ring_enter(a);
    } else if (a->screen_id == SCREEN_RING) {
        /* Something else stopped it - an alarm switched off, say. */
        ring_leave(a);
    }
    refresh_all(a);
}

/* ---- building ---------------------------------------------------------- */

static lv_obj_t *make_screen(lv_obj_t *parent)
{
    lv_obj_t *s = lv_obj_create(parent);

    lv_obj_remove_style_all(s);
    lv_obj_set_width(s, LV_PCT(100));
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s, POCKETUI_PAD, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *make_pane(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);

    lv_obj_remove_style_all(p);
    lv_obj_set_width(p, LV_PCT(100));
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, POCKETUI_PAD, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *wrapped(lv_obj_t *parent, const char *text,
                         enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

/* A stepper: [-b][-a][ value ][+a][+b], every cell a full-height target. */
static lv_obj_t *stepper(lv_obj_t *parent, int small, int large,
                         lv_event_cb_t cb, void *user, int encode)
{
    const int steps[4] = { -large, -small, small, large };
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *value = NULL;
    int i;

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < 5; i++) {
        int step;
        lv_obj_t *btn;
        char text[16];

        if (i == 2) {
            value = pocketui_label(row, "", POS_STYLE_ROW_TITLE);
            lv_obj_set_flex_grow(value, 1);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
            continue;
        }
        step = steps[i < 2 ? i : i - 1];
        snprintf(text, sizeof(text), "%+d", step);
        btn = pocketui_button(row, text, cb, user);
        make_secondary(btn);
        lv_obj_set_size(btn, 72, POCKETUI_TOUCH_MIN);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(btn, (void *)(intptr_t)(step * encode));
    }
    return value;
}

static void build_clock_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *card = pocketui_card(pane);
    lv_obj_t *title;

    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 40, 0);
    a->face_card = card;
    a->face_time = pocketui_label(card, "--:--", POS_STYLE_HERO_48);
    a->face_date = pocketui_label(card, "", POS_STYLE_TEXT_SECONDARY);

    a->unset_card = pocketui_card(pane);
    title = pocketui_label(a->unset_card, "Time not set", POS_STYLE_TITLE);
    pos_style_add(title, POS_STYLE_STATUS_WARN_TEXT, 0);
    lv_obj_set_style_pad_bottom(title, 12, 0);
    wrapped(a->unset_card,
            "This board has no battery-backed clock, so it forgets the time "
            "at every power-off. Alarms cannot ring until it is set. The "
            "stopwatch and the timer are unaffected: they measure elapsed "
            "time and do not need the date.",
            POS_STYLE_TEXT_SECONDARY);
}

static void build_alarm_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *notice_title;
    lv_obj_t *card;

    a->alarm_notice = pocketui_card(pane);
    notice_title = pocketui_label(a->alarm_notice, "Time not set",
                                  POS_STYLE_ROW_TITLE);
    pos_style_add(notice_title, POS_STYLE_STATUS_WARN_TEXT, 0);
    lv_obj_set_style_pad_bottom(notice_title, 8, 0);
    wrapped(a->alarm_notice,
            "Alarms will not ring until the clock is set. They are still "
            "saved.", POS_STYLE_TEXT_SECONDARY);

    card = pocketui_card(pane);
    lv_obj_set_style_pad_ver(card, 0, 0);
    a->alarm_list = card;

    a->alarm_full = wrapped(pane, "That is all eight alarms. Delete one to "
                                  "add another.", POS_STYLE_TEXT_SECONDARY);
    a->alarm_add_btn = pocketui_button(pane, "Add alarm", on_add_open, a);
    lv_obj_set_height(a->alarm_add_btn, POCKETUI_TOUCH_MIN);
    lv_obj_clear_flag(a->alarm_add_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    /* What an alert can actually do on this hardware, in the owner's words
     * rather than a silent surprise at 07:30 (clock_alert.h). */
    wrapped(pane, clock_alert_why(), POS_STYLE_CAPTION);
    wrapped(pane, "Alarms ring while Clock is open. PocketOS has no "
                  "background apps yet.", POS_STYLE_CAPTION);

    /* Last, because filling the list also decides whether the Add button and
     * the full notice are shown, and neither exists before now. */
    build_alarm_list(a);
}

static void build_stopwatch_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *card = pocketui_card(pane);
    lv_obj_t *row;

    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 32, 0);
    a->sw_value = pocketui_label(card, "00:00.00", POS_STYLE_HERO_48);

    row = button_row(pane);
    a->sw_run_btn = paired_button(row, "Start", on_sw_run, a);
    a->sw_lap_btn = paired_button(row, "Reset", on_sw_lap, a);
    make_secondary(a->sw_lap_btn);

    a->sw_laps = pocketui_card(pane);
    lv_obj_set_style_pad_ver(a->sw_laps, 0, 0);
    lv_obj_set_flex_grow(a->sw_laps, 1);
    lv_obj_add_flag(a->sw_laps, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_timer_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *card = pocketui_card(pane);
    lv_obj_t *row;

    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 32, 0);
    a->tm_value = pocketui_label(card, "00:00", POS_STYLE_HERO_48);

    a->tm_setter = lv_obj_create(pane);
    lv_obj_remove_style_all(a->tm_setter);
    lv_obj_set_width(a->tm_setter, LV_PCT(100));
    lv_obj_set_height(a->tm_setter, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(a->tm_setter, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(a->tm_setter, 8, 0);
    lv_obj_clear_flag(a->tm_setter, LV_OBJ_FLAG_SCROLLABLE);
    /* Minutes are encoded as 100 per unit and seconds as 1, so one callback
     * reads both from the button it was given. */
    a->tm_min = stepper(a->tm_setter, 1, 10, on_timer_step, a, 100);
    a->tm_sec = stepper(a->tm_setter, 1, 10, on_timer_step, a, 1);

    row = button_row(pane);
    a->tm_run_btn = paired_button(row, "Start", on_timer_run, a);
    a->tm_cancel_btn = paired_button(row, "Cancel", on_timer_cancel, a);
    make_secondary(a->tm_cancel_btn);
}

static void build_main(struct clock_app *a)
{
    static const char *const names[TAB_COUNT] = { "Clock", "Alarm", "Watch",
                                                  "Timer" };
    lv_obj_t *screen = a->screen[SCREEN_MAIN];
    lv_obj_t *tabs = lv_obj_create(screen);
    int i;

    lv_obj_remove_style_all(tabs);
    lv_obj_set_width(tabs, LV_PCT(100));
    lv_obj_set_height(tabs, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 8, 0);
    lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);

    for (i = 0; i < TAB_COUNT; i++) {
        a->tab_btn[i] = pocketui_button(tabs, names[i], on_tab, a);
        make_secondary(a->tab_btn[i]);
        pos_style_add(a->tab_btn[i], POS_STYLE_BUTTON_PRIMARY, LV_STATE_CHECKED);
        lv_obj_set_height(a->tab_btn[i], POCKETUI_TOUCH_MIN);
        lv_obj_set_flex_grow(a->tab_btn[i], 1);
        lv_obj_clear_flag(a->tab_btn[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(a->tab_btn[i], (void *)(intptr_t)i);
        a->pane[i] = make_pane(screen);
    }
    build_clock_pane(a, a->pane[TAB_CLOCK]);
    build_alarm_pane(a, a->pane[TAB_ALARM]);
    build_stopwatch_pane(a, a->pane[TAB_STOPWATCH]);
    build_timer_pane(a, a->pane[TAB_TIMER]);
}

static void build_add(struct clock_app *a)
{
    lv_obj_t *screen = a->screen[SCREEN_ADD];
    lv_obj_t *card = pocketui_card(screen);
    lv_obj_t *hour;
    lv_obj_t *minute;
    lv_obj_t *row;
    lv_obj_t *cancel;

    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 24, 0);
    a->add_time = pocketui_label(card, "07:00", POS_STYLE_HERO_48);

    /* The time itself is the hero above, so these two rows only have to say
     * which of its halves they move. */
    hour = stepper(screen, 1, 5, on_hour_step, a, 1);
    set_text(hour, "Hour");
    minute = stepper(screen, 1, 5, on_minute_step, a, 1);
    set_text(minute, "Minute");

    a->add_repeat_btn = pocketui_button(screen, "Repeat: Once", on_repeat_cycle, a);
    lv_obj_set_height(a->add_repeat_btn, POCKETUI_TOUCH_MIN);
    lv_obj_clear_flag(a->add_repeat_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    make_secondary(a->add_repeat_btn);

    a->add_label = pocketui_text_field(screen, "Label (optional)", true);
    lv_textarea_set_max_length(a->add_label, CLOCK_LABEL_MAX - 1);
    lv_obj_add_event_cb(a->add_label, on_label_clicked, LV_EVENT_CLICKED, a);

    row = button_row(screen);
    cancel = paired_button(row, "Cancel", on_add_cancel, a);
    make_secondary(cancel);
    paired_button(row, "Add", on_add_confirm, a);
}

static void build_confirm(struct clock_app *a)
{
    lv_obj_t *panel = pocketui_card(a->screen[SCREEN_CONFIRM]);
    lv_obj_t *body;
    lv_obj_t *row;
    lv_obj_t *cancel;
    lv_obj_t *confirm;

    pocketui_label(panel, "Delete this alarm?", POS_STYLE_TITLE);
    body = wrapped(panel, "The alarm is removed from this device. There is "
                          "no undo.", POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 20, 0);

    row = button_row(panel);
    /* Cancel first and accented: the safe choice gets the bright treatment
     * (DS §17.5). */
    cancel = paired_button(row, "Cancel", on_confirm_cancel, a);
    confirm = paired_button(row, "Delete", on_confirm_delete, a);
    make_secondary(confirm);
    pos_input_add_obj(cancel);
    pos_input_add_obj(confirm);
}

static void build_ring(struct clock_app *a)
{
    lv_obj_t *panel = pocketui_card(a->screen[SCREEN_RING]);
    lv_obj_t *why;
    lv_obj_t *row;
    lv_obj_t *stop;

    /* This is the whole alert. There is no sound to fall back on, so it
     * takes the panel rather than sitting in a card at the top of it. */
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    a->ring_title = pocketui_label(panel, "Alarm", POS_STYLE_HERO_40);
    lv_obj_set_style_pad_bottom(a->ring_title, 12, 0);
    a->ring_detail = pocketui_label(panel, "", POS_STYLE_ROW_TITLE);
    lv_obj_set_style_pad_bottom(a->ring_detail, 24, 0);
    why = wrapped(panel, clock_alert_why(), POS_STYLE_CAPTION);
    /* The gap before the buttons goes on the caption above them: a pad on a
     * fixed-height row squeezes its buttons instead of moving it down. */
    lv_obj_set_style_pad_bottom(why, 24, 0);

    row = button_row(panel);
    a->ring_snooze_btn = paired_button(row, "Snooze 9 min", on_ring_snooze, a);
    make_secondary(a->ring_snooze_btn);
    stop = paired_button(row, "Stop", on_ring_stop, a);
    pos_input_add_obj(stop);
}

/* ---- the app ----------------------------------------------------------- */

static void *clock_create(lv_obj_t *root)
{
    struct clock_app *a = lv_malloc_zeroed(sizeof(*a));
    int i;

    if (!a) {
        return NULL;
    }
    clock_engine_init(&a->engine);
    if (clock_store_load(&a->engine) < 0) {
        pocketos_shell_set_status_hint("Alarms unreadable");
    }
    a->set_minutes = (int)(a->engine.timer.duration_ms / 60000);
    a->set_seconds = (int)((a->engine.timer.duration_ms / 1000) % 60);
    if (a->set_minutes > TIMER_SET_MAX_MINUTES) {
        a->set_minutes = TIMER_SET_MAX_MINUTES;
    }
    a->confirm_index = -1;
    a->add_hour = 7;
    a->return_screen = SCREEN_MAIN;

    /* The first reading happens before anything is drawn, so no view ever
     * paints a time the engine has not been stepped to. */
    clock_now_read(&a->now);
    clock_engine_step(&a->engine, &a->now);

    for (i = 0; i < SCREEN_COUNT; i++) {
        a->screen[i] = make_screen(root);
    }
    build_main(a);
    build_add(a);
    build_confirm(a);
    build_ring(a);

    select_tab(a, TAB_CLOCK);
    show_screen(a, SCREEN_MAIN);
    refresh_all(a);

    a->refresh = lv_timer_create(on_refresh, CLOCK_REFRESH_MS, a);
    return a;
}

static void clock_destroy(void *priv)
{
    struct clock_app *a = priv;

    if (!a) {
        return;
    }
    /* The shell deletes the objects under root, but the timer is ours. */
    if (a->refresh) {
        lv_timer_delete(a->refresh);
    }
    clock_alert_end();
    pocketos_shell_keyboard_hide();
    /* Leaving the app ends the stopwatch and the countdown: v0.1 has no
     * background, and a number that carried on counting while nothing was
     * running would be a fiction (ADR-002). The alarms are already on disk;
     * they were written when they changed. */
    lv_free(a);
}

const struct pocketos_app app_clock = {
    .id = "clock",
    .name = "Clock",
    .icon = LV_SYMBOL_BELL,
    .create = clock_create,
    .tick = NULL,
    .destroy = clock_destroy,
};
