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
 * LAYOUT. Every screen in two shapes, chosen from the box the app is given
 * and chosen again whenever that box changes size: tall, the portrait layout
 * it always had, and wide, side by side (see "the layout" below). The
 * orientation is the system's (DS section 21.2); nothing here asks for it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "clock_alert.h"
#include "clock_engine.h"
#include "clock_runtime.h"
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
    SCREEN_COUNT
};

/* Ten a second: the stopwatch shows hundredths, and anything slower makes
 * the last digit stutter. Nothing is written to storage on this timer. */
#define CLOCK_REFRESH_MS 100
#define TIMER_SET_MAX_MINUTES 99

/* The portrait body on the reference panel: 568 less the 20 px body padding
 * each side (DS §7). The wide shape is only chosen when two columns of this
 * and the gutter between them fit, so no column there is narrower than the
 * portrait screen it replaces. */
#define CLOCK_COLUMN_W 528
/* Cancel and Add beside the label field in the wide shape: two actions of
 * 140 px and the 8 px pair gap - the rail Notes uses (DS §23.1). */
#define CLOCK_RAIL_W 288

/* Hour, Minute and Repeat, stacked: three rows at the touch minimum and the
 * two gaps between them. The new-alarm time beside them is as tall. */
#define ADD_CONTROLS_H (3 * POCKETUI_TOUCH_MIN + 2 * POCKETUI_PAD)

struct clock_app {
    lv_obj_t *frame;      /* the app's own box in the body: the three screens */
    lv_area_t laid_out;   /* the frame's area when the shape was last chosen */
    bool wide;
    lv_obj_t *screen[SCREEN_COUNT];
    lv_obj_t *tab_btn[TAB_COUNT];
    lv_obj_t *pane[TAB_COUNT];
    /* Every pane but the clock face in two columns: one above the other when
     * the body is tall, side by side when it is wide. */
    lv_obj_t *column[TAB_COUNT][2];
    lv_timer_t *refresh;
    uint8_t tab;
    uint8_t screen_id;

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
    uint32_t alarm_sig;
    int confirm_index;
    lv_obj_t *confirm_panel;

    /* New alarm: the time and its controls, then the label and its actions */
    int add_hour;
    int add_minute;
    int add_repeat;
    lv_obj_t *add_when;
    lv_obj_t *add_hero;
    lv_obj_t *add_controls;
    lv_obj_t *add_what;
    lv_obj_t *add_actions;
    lv_obj_t *add_time;
    lv_obj_t *add_repeat_btn;
    lv_obj_t *add_label;

    /* Stopwatch */
    lv_obj_t *sw_hero;
    lv_obj_t *sw_value;
    lv_obj_t *sw_run_btn;
    lv_obj_t *sw_lap_btn;
    lv_obj_t *sw_laps;
    int sw_laps_drawn;

    /* Timer */
    int set_minutes;
    int set_seconds;
    lv_obj_t *tm_hero;
    lv_obj_t *tm_value;
    lv_obj_t *tm_setter;
    lv_obj_t *tm_min;
    lv_obj_t *tm_sec;
    lv_obj_t *tm_run_btn;
    lv_obj_t *tm_cancel_btn;
};

static void show_screen(struct clock_app *a, enum clock_screen which);
static void build_alarm_list(struct clock_app *a);
static void refresh_all(struct clock_app *a);
static void shape_face(struct clock_app *a);

/* The engine and the clock reading both belong to the shell now, so that an
 * alarm keeps its appointment with this app shut (clock_runtime.h). This
 * file configures and draws them; it owns neither. */
static struct clock_engine *eng(void)
{
    return clock_runtime_engine();
}

static const struct clock_now *tnow(void)
{
    return clock_runtime_now();
}

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

static void save(void)
{
    /* Only ever from a change the owner made, never from the refresh timer:
     * an alarm list is a setting, and settings are not written per tick. */
    if (clock_runtime_save() != 0) {
        pocketos_shell_set_status_hint("Alarms not saved");
    }
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
    if (a->face_grown != tnow()->wall.valid) {
        a->face_grown = tnow()->wall.valid;
        shape_face(a);
    }
    clock_format_wall(&tnow()->wall, buf, sizeof(buf));
    set_text(a->face_time, buf);
    clock_format_date(&tnow()->wall, buf, sizeof(buf));
    set_text(a->face_date, tnow()->wall.valid ? buf : "");
    set_hidden(a->face_date, !tnow()->wall.valid);
    set_hidden(a->unset_card, tnow()->wall.valid);
}

/* ---- the alarm list ---------------------------------------------------- */

static void on_alarm_row(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);
    int index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const struct clock_alarm *al = clock_alarm_at(eng(), index);

    if (!al) {
        return;
    }
    /* The reading goes in because switching an alarm on is setting it: at or
     * after its time today, it means its next time (clock_engine.h). */
    clock_alarm_set_enabled(eng(), index, !al->enabled, tnow());
    build_alarm_list(a);
    save();
}

static void on_alarm_delete(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    a->confirm_index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    /* Nothing is removed here; only the confirmation's Delete removes it
     * (DS §17.5). */
    show_screen(a, SCREEN_CONFIRM);
}

/* Everything about the list that can change without this app doing it. The
 * shell acknowledges a ringing alarm, and a one-shot alarm switches itself
 * off when it does - so a list drawn before that is out of date, and the
 * owner is looking at a row that says On. */
static uint32_t alarm_signature(void)
{
    const struct clock_engine *e = eng();
    uint32_t sig = (uint32_t)e->alarm_count;
    int i;

    for (i = 0; i < e->alarm_count; i++) {
        sig = sig * 131u + e->alarms[i].hour;
        sig = sig * 131u + e->alarms[i].minute;
        sig = sig * 131u + e->alarms[i].repeat;
        sig = sig * 131u + (e->alarms[i].enabled ? 1u : 0u);
        sig = sig * 131u + (uint8_t)e->alarms[i].label[0];
    }
    return sig;
}

static void build_alarm_list(struct clock_app *a)
{
    int n = clock_alarm_count(eng());
    int i;

    lv_obj_clean(a->alarm_list);
    if (n == 0) {
        lv_obj_t *empty = pocketui_label(a->alarm_list, "No alarms yet",
                                         POS_STYLE_TEXT_SECONDARY);

        lv_obj_set_style_pad_ver(empty, 20, 0);
    }
    for (i = 0; i < n; i++) {
        const struct clock_alarm *al = clock_alarm_at(eng(), i);
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
    a->alarm_sig = alarm_signature();
}

static void refresh_alarm(struct clock_app *a)
{
    /* Redrawn only when it is actually different, so the ten-a-second
     * refresh does not rebuild eight rows for nothing - but redrawn without
     * this app being told, which is what keeps it honest about an alarm the
     * shell stopped while the list was on screen. */
    if (a->alarm_sig != alarm_signature()) {
        build_alarm_list(a);
    }
    set_hidden(a->alarm_notice, tnow()->wall.valid);
}

/* ---- the delete confirmation ------------------------------------------- */

static void on_confirm_cancel(lv_event_t *e)
{
    show_screen(lv_event_get_user_data(e), SCREEN_MAIN);
}

static void on_confirm_delete(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    clock_alarm_remove(eng(), a->confirm_index);
    build_alarm_list(a);
    save();
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
        /* The field and its caption head the form in the wide shape, so the
         * top is where the error is read - above a landscape keyboard too,
         * even when the time below was scrolled to (DS §17.1). Tall, the form
         * does not scroll. */
        lv_obj_scroll_to_y(a->screen[SCREEN_ADD], 0, LV_ANIM_OFF);
        return;
    }
    if (clock_alarm_add(eng(), a->add_hour, a->add_minute,
                        (enum clock_repeat)a->add_repeat,
                        *label ? label : NULL, tnow()) < 0) {
        pocketos_shell_set_status_hint("No room for another alarm");
        return;
    }
    pocketui_text_field_set_error(a->add_label, NULL);
    pocketos_shell_keyboard_hide();
    build_alarm_list(a);
    save();
    show_screen(a, SCREEN_MAIN);
}

static void on_add_open(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    /* A new alarm opens at the current time when there is one, and at 07:00
     * when the board does not know what time it is. */
    if (tnow()->wall.valid) {
        a->add_hour = tnow()->wall.hour;
        a->add_minute = tnow()->wall.minute;
    } else {
        a->add_hour = 7;
        a->add_minute = 0;
    }
    a->add_repeat = CLOCK_REPEAT_ONCE;
    lv_textarea_set_text(a->add_label, "");
    pocketui_text_field_set_error(a->add_label, NULL);
    refresh_add(a);
    /* A new form starts at its top, whatever the last one was scrolled to. */
    lv_obj_scroll_to_y(a->screen[SCREEN_ADD], 0, LV_ANIM_OFF);
    show_screen(a, SCREEN_ADD);
}

/* ---- the stopwatch ----------------------------------------------------- */

static void on_sw_run(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (eng()->sw.state == CLOCK_SW_RUNNING) {
        clock_sw_pause(eng(), tnow());
    } else {
        clock_sw_start(eng(), tnow());
    }
    refresh_all(a);
}

static void on_sw_lap(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    if (eng()->sw.state == CLOCK_SW_RUNNING) {
        clock_sw_lap(eng(), tnow());
    } else {
        clock_sw_reset(eng());
    }
    refresh_all(a);
}

static void refresh_sw(struct clock_app *a)
{
    char buf[24];
    int running = eng()->sw.state == CLOCK_SW_RUNNING;
    int i;

    clock_format_elapsed(clock_sw_elapsed_ms(eng(), tnow()), buf, sizeof(buf));
    set_text(a->sw_value, buf);
    set_button_text(a->sw_run_btn, running ? "Pause" : "Start");
    set_button_text(a->sw_lap_btn, running ? "Lap" : "Reset");

    /* An empty lap list is an empty box the height of the panel, so it is
     * not shown until there is a lap in it. */
    set_hidden(a->sw_laps, eng()->sw.lap_count == 0);

    /* The list only ever grows or is cleared, so it is rebuilt on those two
     * events and left alone by the ten-a-second refresh. */
    if (a->sw_laps_drawn == eng()->sw.lap_count) {
        return;
    }
    if (eng()->sw.lap_count < a->sw_laps_drawn) {
        lv_obj_clean(a->sw_laps);
        a->sw_laps_drawn = 0;
    }
    for (i = a->sw_laps_drawn; i < eng()->sw.lap_count; i++) {
        char n[16];

        snprintf(n, sizeof(n), "%d", i + 1);
        clock_format_elapsed(eng()->sw.laps[i], buf, sizeof(buf));
        pocketui_kv_row(a->sw_laps, n, buf);
    }
    a->sw_laps_drawn = eng()->sw.lap_count;
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

    if (eng()->timer.state == CLOCK_TIMER_RUNNING) {
        clock_timer_pause(eng(), tnow());
    } else {
        if (eng()->timer.state == CLOCK_TIMER_IDLE &&
            !clock_timer_set(eng(), 0, a->set_minutes, a->set_seconds)) {
            pocketos_shell_set_status_hint("Set a duration first");
            return;
        }
        pocketos_shell_set_status_hint("");
        if (clock_timer_start(eng(), tnow())) {
            /* The duration is a setting and survives a reboot; what is left
             * of a running countdown does not, and is not written. */
            save();
        }
    }
    refresh_all(a);
}

static void on_timer_cancel(lv_event_t *e)
{
    struct clock_app *a = lv_event_get_user_data(e);

    clock_timer_cancel(eng());
    refresh_all(a);
}

static void refresh_timer(struct clock_app *a)
{
    char buf[24];
    int idle = eng()->timer.state == CLOCK_TIMER_IDLE;
    int running = eng()->timer.state == CLOCK_TIMER_RUNNING;

    if (idle) {
        clock_format_remaining((int64_t)(a->set_minutes * 60 + a->set_seconds) * 1000,
                               buf, sizeof(buf));
    } else {
        clock_format_remaining(clock_timer_remaining_ms(eng(), tnow()), buf,
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

    /* A fresh reading, and nothing more. The engine is advanced once a
     * second by the shell and by nobody else (clock_runtime.h); this timer
     * exists so the stopwatch can show hundredths, not so the app can
     * decide that an alarm has gone off. */
    clock_runtime_read();
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
    lv_obj_set_style_pad_column(p, POCKETUI_PAD, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* A box of items one under the other, with the gap the screen puts between
 * them, as tall as what is in it. It draws nothing, so in the tall shape the
 * items sit exactly where they would without it. It is shaped by the layout,
 * which also decides whether it scrolls. */
static lv_obj_t *make_group(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);

    lv_obj_remove_style_all(g);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g, POCKETUI_PAD, 0);
    lv_obj_set_style_pad_column(g, POCKETUI_PAD, 0);
    lv_obj_set_scroll_dir(g, LV_DIR_VER);
    /* A tap on the gap between two controls never takes the focus from the
     * label field; whether it is pressed at all is the layout's. */
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
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

/* The notice and the list in the first column; what can be done about them,
 * and what an alarm can and cannot do, in the second. */
static void build_alarm_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *list_col = make_group(pane);
    lv_obj_t *info_col = make_group(pane);
    lv_obj_t *notice_title;
    lv_obj_t *card;

    a->column[TAB_ALARM][0] = list_col;
    a->column[TAB_ALARM][1] = info_col;
    a->alarm_notice = pocketui_card(list_col);
    notice_title = pocketui_label(a->alarm_notice, "Time not set",
                                  POS_STYLE_ROW_TITLE);
    pos_style_add(notice_title, POS_STYLE_STATUS_WARN_TEXT, 0);
    lv_obj_set_style_pad_bottom(notice_title, 8, 0);
    wrapped(a->alarm_notice,
            "Alarms will not ring until the clock is set. They are still "
            "saved.", POS_STYLE_TEXT_SECONDARY);

    card = pocketui_card(list_col);
    lv_obj_set_style_pad_ver(card, 0, 0);
    a->alarm_list = card;

    a->alarm_full = wrapped(info_col, "That is all eight alarms. Delete one to "
                                      "add another.", POS_STYLE_TEXT_SECONDARY);
    a->alarm_add_btn = pocketui_button(info_col, "Add alarm", on_add_open, a);
    lv_obj_set_height(a->alarm_add_btn, POCKETUI_TOUCH_MIN);
    lv_obj_clear_flag(a->alarm_add_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    /* What an alert can actually do on this hardware, in the owner's words
     * rather than a silent surprise at 07:30 (clock_alert.h). */
    wrapped(info_col, clock_alert_why(), POS_STYLE_CAPTION);
    wrapped(info_col, "Alarms ring with Clock closed. They do not ring with the "
                      "device switched off.", POS_STYLE_CAPTION);

    /* Last, because filling the list also decides whether the Add button and
     * the full notice are shown, and neither exists before now. */
    build_alarm_list(a);
}

/* The running time in the first column; its buttons and the laps in the
 * second, which takes the rest of the pane so the laps can grow into it. */
static void build_stopwatch_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *hero_col = make_group(pane);
    lv_obj_t *run_col = make_group(pane);
    lv_obj_t *card = pocketui_card(hero_col);
    lv_obj_t *row;

    a->column[TAB_STOPWATCH][0] = hero_col;
    a->column[TAB_STOPWATCH][1] = run_col;
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 32, 0);
    a->sw_hero = card;
    a->sw_value = pocketui_label(card, "00:00.00", POS_STYLE_HERO_48);

    row = button_row(run_col);
    a->sw_run_btn = paired_button(row, "Start", on_sw_run, a);
    a->sw_lap_btn = paired_button(row, "Reset", on_sw_lap, a);
    make_secondary(a->sw_lap_btn);

    a->sw_laps = pocketui_card(run_col);
    lv_obj_set_style_pad_ver(a->sw_laps, 0, 0);
    lv_obj_set_flex_grow(a->sw_laps, 1);
    lv_obj_add_flag(a->sw_laps, LV_OBJ_FLAG_SCROLLABLE);
}

/* The countdown in the first column; setting it, starting and stopping it in
 * the second. */
static void build_timer_pane(struct clock_app *a, lv_obj_t *pane)
{
    lv_obj_t *hero_col = make_group(pane);
    lv_obj_t *set_col = make_group(pane);
    lv_obj_t *card = pocketui_card(hero_col);
    lv_obj_t *row;

    a->column[TAB_TIMER][0] = hero_col;
    a->column[TAB_TIMER][1] = set_col;
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 32, 0);
    a->tm_hero = card;
    a->tm_value = pocketui_label(card, "00:00", POS_STYLE_HERO_48);

    a->tm_setter = lv_obj_create(set_col);
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

    row = button_row(set_col);
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

/* In two parts, in portrait order: when - the time, its steppers and the
 * repeat - and what - the label and the actions that finish the form. */
static void build_add(struct clock_app *a)
{
    lv_obj_t *screen = a->screen[SCREEN_ADD];
    lv_obj_t *card;
    lv_obj_t *hour;
    lv_obj_t *minute;
    lv_obj_t *row;
    lv_obj_t *cancel;

    lv_obj_set_scroll_dir(screen, LV_DIR_VER);
    a->add_when = make_group(screen);
    a->add_what = make_group(screen);

    card = pocketui_card(a->add_when);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(card, 24, 0);
    a->add_hero = card;
    a->add_time = pocketui_label(card, "07:00", POS_STYLE_HERO_48);

    /* The time itself is the hero above, so these two rows only have to say
     * which of its halves they move. */
    a->add_controls = make_group(a->add_when);
    hour = stepper(a->add_controls, 1, 5, on_hour_step, a, 1);
    set_text(hour, "Hour");
    minute = stepper(a->add_controls, 1, 5, on_minute_step, a, 1);
    set_text(minute, "Minute");

    a->add_repeat_btn = pocketui_button(a->add_controls, "Repeat: Once", on_repeat_cycle, a);
    lv_obj_set_height(a->add_repeat_btn, POCKETUI_TOUCH_MIN);
    lv_obj_clear_flag(a->add_repeat_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    make_secondary(a->add_repeat_btn);

    a->add_label = pocketui_text_field(a->add_what, "Label (optional)", true);
    lv_textarea_set_max_length(a->add_label, CLOCK_LABEL_MAX - 1);
    lv_obj_add_event_cb(a->add_label, on_label_clicked, LV_EVENT_CLICKED, a);

    row = button_row(a->add_what);
    a->add_actions = row;
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

    a->confirm_panel = panel;
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

/* ---- the layout -------------------------------------------------------- *
 *
 * The screens sit in one frame that is exactly the body's content box - the
 * whole of the room the shell gives the app - and each is shaped from the
 * size of that box alone:
 *
 *   TALL (portrait: 528 x 1060 on the reference panel, 528 x 764 with the
 *   keyboard up). The layout Clock always had: the tabs over one pane, the
 *   new-alarm form down the body, the confirmation across its top.
 *
 *   WIDE (landscape: 1192 x 396, and 1192 x 100 with the keyboard up).
 *   Height is what landscape lacks and width is what it has, so what a
 *   portrait pane stacks goes side by side instead, in two halves of the
 *   width with the 20 px gutter between them. The tabs keep the top. The big
 *   number - the stopwatch, the countdown - fills the first half, and what
 *   is done to it is in the second: its buttons and the laps, its steppers
 *   and buttons. The alarms are listed in the first half, and Add alarm and
 *   what an alarm can do are in the second. The clock face, with a time to
 *   show, takes both halves; without one, the explanation takes the second.
 *   Each half scrolls on its own, so a long list never moves the button
 *   beside it. The new-alarm form puts the label and its actions across the
 *   top - the field, then Cancel and Add in a rail beside it - because above
 *   a landscape keyboard the body is 100 px, and those are what a keyboard
 *   is up for; the time and its steppers follow below, side by side, and the
 *   form scrolls. The confirmation keeps its portrait width, centred.
 *   Chosen only when both halves keep CLOCK_COLUMN_W, so nothing is narrower
 *   than in portrait. No control's size is shared out of the height, so no
 *   body can bring one under 64 px.
 *
 * The objects are built once and only shaped here - flow, sizes, and which
 * box scrolls - so the time on show, a running stopwatch, the alarm being
 * set, the typed label, the focus and the keyboard are never touched by a
 * change of shape. The alarm rows are rebuilt from the engine as they always
 * were; they fill their column whatever its width.
 *
 * Whatever the shape, the content clears the panel's unsafe area (DS §21.1,
 * §22.2): the clock face, the laps and a scrolling column reach the foot of
 * the body, and would reach 10 px into the 30 px corner squares of the
 * reference panel, so the frame pads its foot by however far a corner square
 * reaches into the body (pos_display_rect_insets, the rule Calculator and
 * Notes use). With the keyboard up the foot is far from the corners and the
 * pad is 0; on a panel with square corners it is always 0. */

/* Tall: as tall as what is in it, or as tall as the pane leaves it when it
 * holds something that grows; never scrolled, and a finger passes through it
 * to what is behind, as it did before there was a box here at all. Wide: half
 * the width, the full height, scrolling itself - and pressable, because LVGL
 * finds a scroller only under a finger that lands on something clickable,
 * and a drag can start in the gap between two controls. */
static void shape_column(lv_obj_t *col, bool wide, bool grows_tall)
{
    lv_obj_set_flex_grow(col, wide || grows_tall ? 1 : 0);
    lv_obj_set_height(col, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
    if (wide) {
        lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_scroll_to_y(col, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* A big number in a card: as tall as it needs above what it counts, the
 * whole of its column beside it. */
static void shape_hero(lv_obj_t *card, bool wide)
{
    lv_obj_set_height(card, wide ? LV_PCT(100) : LV_SIZE_CONTENT);
}

/* The face and, when there is no time, the explanation. Tall: the face grows
 * down the pane when it has a time to show (refresh_face says why) and the
 * explanation is under it. Wide: the face takes the width and the height
 * with a time to show; without one, the face and the explanation share the
 * width, each as tall as it needs. */
static void shape_face(struct clock_app *a)
{
    bool valid = a->face_grown;

    lv_obj_set_flex_flow(a->pane[TAB_CLOCK], a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(a->face_card, a->wide || valid ? 1 : 0);
    lv_obj_set_height(a->face_card, a->wide && valid ? LV_PCT(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(a->unset_card, a->wide ? 1 : 0);
}

static void shape_panes(struct clock_app *a)
{
    int t;

    shape_face(a);
    for (t = TAB_ALARM; t < TAB_COUNT; t++) {
        lv_obj_set_flex_flow(a->pane[t], a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
        shape_column(a->column[t][0], a->wide, false);
        /* The laps grow into what the stopwatch's buttons leave of the pane. */
        shape_column(a->column[t][1], a->wide, t == TAB_STOPWATCH);
    }
    shape_hero(a->sw_hero, a->wide);
    shape_hero(a->tm_hero, a->wide);
}

/* Tall: the time, Hour, Minute and Repeat, the field, Cancel and Add, one
 * under the other. Wide: the flow runs bottom to top, so the label part - last
 * in the tree - heads the form: the field, and Cancel and Add in the rail
 * beside it at their Notes size. Under it the time part, the time beside
 * Hour, Minute and Repeat and as tall as they are. The form scrolls, which
 * above the keyboard is how the time is reached; its top is always the
 * field. */
static void shape_add(struct clock_app *a)
{
    lv_obj_t *s = a->screen[SCREEN_ADD];
    lv_flex_flow_t across = a->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN;

    lv_obj_set_flex_flow(s, a->wide ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_COLUMN);
    if (a->wide) {
        lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_scroll_to_y(s, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_set_flex_flow(a->add_when, across);
    lv_obj_set_flex_grow(a->add_hero, a->wide ? 1 : 0);
    lv_obj_set_height(a->add_hero, a->wide ? ADD_CONTROLS_H : LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(a->add_controls, a->wide ? 1 : 0);

    lv_obj_set_flex_flow(a->add_what, across);
    lv_obj_set_flex_grow(lv_obj_get_parent(a->add_label), a->wide ? 1 : 0);
    lv_obj_set_width(a->add_actions, a->wide ? CLOCK_RAIL_W : LV_PCT(100));
}

/* The panel across the top of the body; in the wide shape at its portrait
 * width, centred - the track as well as the panel in it, or a column flow
 * leaves a narrower panel at the left. */
static void shape_confirm(struct clock_app *a)
{
    lv_flex_align_t across = a->wide ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START;

    lv_obj_set_flex_align(a->screen[SCREEN_CONFIRM], LV_FLEX_ALIGN_START, across, across);
    lv_obj_set_width(a->confirm_panel, a->wide ? CLOCK_COLUMN_W : LV_PCT(100));
}

static void layout(struct clock_app *a)
{
    lv_area_t box;
    struct pos_insets in;
    int32_t w;
    int32_t h;

    lv_obj_get_coords(a->frame, &box);
    if (lv_area_get_width(&box) <= 0 || lv_area_get_height(&box) <= 0 ||
        memcmp(&box, &a->laid_out, sizeof(box)) == 0) {
        return;
    }
    a->laid_out = box;
    in = pos_display_rect_insets(pocketui_display_geometry(), box.x1, box.y1, box.x2, box.y2);
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    w = lv_area_get_width(&box) - in.left - in.right;
    h = lv_area_get_height(&box) - in.top - in.bottom;
    a->wide = w > h && w >= 2 * CLOCK_COLUMN_W + POCKETUI_PAD;
    shape_panes(a);
    shape_add(a);
    shape_confirm(a);
}

/* The frame is the body's content box, so this is the body changing size:
 * the keyboard came up or went down, the display turned, or this is the
 * first layout pass after the app was built. */
static void on_frame_size(lv_event_t *e)
{
    layout(lv_event_get_user_data(e));
}

static void build_frame(struct clock_app *a, lv_obj_t *root)
{
    lv_obj_t *frame = lv_obj_create(root);

    lv_obj_remove_style_all(frame);
    /* Exactly the body's content box, whatever is in it, so the shape is
     * always chosen from the room the shell gives and never from the size of
     * what the shape itself put there. */
    lv_obj_set_size(frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(frame, LV_FLEX_FLOW_COLUMN);
    /* No gap between the screens, only one of which is ever shown: LVGL 9.5
     * takes a gap from a growing screen that is not the first child, shown
     * or not (DS §23.4). */
    lv_obj_set_style_pad_row(frame, 0, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    a->frame = frame;
}

/* ---- the app ----------------------------------------------------------- */

static void *clock_create(lv_obj_t *root)
{
    struct clock_app *a = lv_malloc_zeroed(sizeof(*a));
    int i;

    if (!a) {
        return NULL;
    }
    /* The engine is already there, with whatever the shell loaded at boot
     * and whatever has happened to it since: an alarm that rang while this
     * app was shut, a countdown still running, a stopwatch still going. The
     * app picks it up where it is (clock_runtime.h). */
    a->set_minutes = (int)(eng()->timer.duration_ms / 60000);
    a->set_seconds = (int)((eng()->timer.duration_ms / 1000) % 60);
    if (a->set_minutes > TIMER_SET_MAX_MINUTES) {
        a->set_minutes = TIMER_SET_MAX_MINUTES;
    }
    a->confirm_index = -1;
    a->add_hour = 7;

    /* A fresh reading before anything is drawn, so no view paints a stale
     * one for a tick. */
    clock_runtime_read();

    build_frame(a, root);
    for (i = 0; i < SCREEN_COUNT; i++) {
        a->screen[i] = make_screen(a->frame);
    }
    build_main(a);
    build_add(a);
    build_confirm(a);

    select_tab(a, TAB_CLOCK);
    show_screen(a, SCREEN_MAIN);
    refresh_all(a);

    /* Only now: building lays objects out as it goes, and the layout step
     * shapes objects that must all exist. */
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);

    a->refresh = lv_timer_create(on_refresh, CLOCK_REFRESH_MS, a);
    return a;
}

static void clock_destroy(void *priv)
{
    struct clock_app *a = priv;

    if (!a) {
        return;
    }
    /* The shell deletes the objects under root, but the timer is ours - and
     * the frame outlives this by a moment, in which nothing may call back
     * into a freed app. */
    if (a->refresh) {
        lv_timer_delete(a->refresh);
    }
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    pocketos_shell_keyboard_hide();
    /* Nothing else is torn down. The alarms, the countdown and the
     * stopwatch belong to the shell and go on without this app: that is the
     * whole point of moving them there. The alarms are already on disk, and
     * were written when they changed. */
    lv_free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_clock);

const struct pocketos_app app_clock = {
    .id = "clock",
    .name = "Clock",
    .icon = LV_SYMBOL_BELL,
    .icon_mask = &pos_app_icon_clock,
    .create = clock_create,
    .tick = NULL,
    .destroy = clock_destroy,
};
