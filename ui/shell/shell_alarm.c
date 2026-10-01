/*
 * The shell's alarm alert. See shell_alarm.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "shell_alarm.h"

#include "app.h"
#include "clock_alert.h"
#include "clock_engine.h"
#include "clock_runtime.h"
#include "clock_time.h"
#include "pocketui.h"
#include "pos_input.h"
#include "shell_kb_state.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *sheet;
static lv_obj_t *title;
static lv_obj_t *detail;
static lv_obj_t *snooze_btn;
static lv_obj_t *stop_btn;
static lv_obj_t *button_row;

/* The alert's own focus group (DS §18.8). The actions live here and never in
 * the one app group: this sheet exists for the whole life of the shell, and a
 * permanent member would be key-reachable from every screen (§17.2). The
 * group is handed to pos_input only while the alert is up, so the actions are
 * reachable exactly when they are on screen and at no other time. */
static lv_group_t *alert_group;

/* Whether this file currently holds the redirection and the suppression.
 * Every show and hide is an edge: shell_alarm_sync() runs on every tick and
 * on every ring change, and §18.6 forbids a repeated tick from doing the work
 * twice. One push per show, one pop per hide, and this is what guarantees
 * it. */
static bool isolating;

static void set_text(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    clock_runtime_stop_ringing();
    shell_alarm_sync();
}

static void on_snooze(lv_event_t *e)
{
    (void)e;
    clock_runtime_snooze();
    shell_alarm_sync();
}

void shell_alarm_create(lv_obj_t *parent)
{
    lv_obj_t *panel;
    lv_obj_t *why;
    lv_obj_t *row;
    lv_obj_t *stop;

    if (sheet) {
        return;
    }
    /* The whole panel, on the screen and not in the content area: an alarm
     * has to cover the launcher and any app, including the status bar's
     * app, without either knowing it exists. */
    sheet = lv_obj_create(parent);
    lv_obj_remove_style_all(sheet);
    pos_style_add(sheet, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(sheet, POCKETUI_PAD, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    /* Clickable so a stray tap anywhere behind the buttons is swallowed
     * rather than reaching the app underneath. */
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);

    panel = pocketui_card(sheet);
    lv_obj_set_height(panel, LV_PCT(100));
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    title = pocketui_label(panel, "Alarm", POS_STYLE_HERO_40);
    lv_obj_set_style_pad_bottom(title, 12, 0);
    detail = pocketui_label(panel, "", POS_STYLE_ROW_TITLE);
    lv_obj_set_style_pad_bottom(detail, 24, 0);

    /* What an alert can actually do on this board, in the owner's words
     * rather than a silence they have to work out for themselves
     * (clock_alert.h). */
    why = pocketui_label(panel, clock_alert_why(), POS_STYLE_CAPTION);
    lv_label_set_long_mode(why, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(why, LV_PCT(100));
    /* The gap before the buttons goes on the caption above them: a pad on a
     * fixed-height row squeezes its buttons instead of moving it down. */
    lv_obj_set_style_pad_bottom(why, 24, 0);

    row = lv_obj_create(panel);
    button_row = row;
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Stop is the accented one and it comes second, under the thumb: this is
     * not a destructive choice to be talked out of, it is the thing the
     * owner reached for the device to do. */
    snooze_btn = pocketui_button(row, "Snooze 9 min", on_snooze, NULL);
    lv_obj_set_height(snooze_btn, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_grow(snooze_btn, 1);
    lv_obj_clear_flag(snooze_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_style(snooze_btn, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(snooze_btn, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED),
                        LV_STATE_PRESSED);
    pos_style_add(snooze_btn, POS_STYLE_BUTTON_SECONDARY, 0);

    stop = pocketui_button(row, "Stop", on_stop, NULL);
    stop_btn = stop;
    lv_obj_set_height(stop, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_grow(stop, 2); /* twice the width of Snooze */
    lv_obj_clear_flag(stop, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    /* The actions go in the alert's own group, not the one app group. Both
     * keep CLICK_FOCUSABLE cleared: a tap must not move focus (DS §17.2),
     * while a key must be able to. Stop is added first, so it is what NEXT
     * starts from and what §18.4 calls the dominant action; Snooze joins and
     * leaves with its visibility in shell_alarm_sync(). */
    alert_group = lv_group_create();
    if (alert_group) {
        lv_group_add_obj(alert_group, stop_btn);
    }

    shell_alarm_sync();
}

int shell_alarm_visible(void)
{
    return sheet && !lv_obj_has_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

void shell_alarm_sync(void)
{
    const struct clock_engine *e;
    char line[64];

    if (!sheet) {
        return;
    }
    e = clock_runtime_engine();
    if (e->ringing == CLOCK_RING_NONE) {
        clock_alert_end();
        lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
        /* The hide edge, and only the edge: a tick that finds nothing
         * ringing must not pop a redirection it never pushed (§18.6). The
         * keyboard is not put back - acknowledging an alert is not resuming
         * the task it interrupted (§18.5). */
        if (isolating) {
            isolating = false;
            pos_input_pop_group();
            pocketos_shell_keyboard_set_suppressed(0);
        }
        return;
    }

    if (e->ringing == CLOCK_RING_ALARM) {
        const struct clock_alarm *al = clock_alarm_at(e, e->ringing_alarm);

        clock_alert_begin(CLOCK_ALERT_ALARM);
        set_text(title, "Alarm");
        if (al && al->label[0]) {
            char hm[8];

            clock_format_hm(al->hour, al->minute, hm, sizeof(hm));
            snprintf(line, sizeof(line), "%s  %s", hm, al->label);
        } else if (al) {
            clock_format_hm(al->hour, al->minute, line, sizeof(line));
        } else {
            snprintf(line, sizeof(line), "Alarm");
        }
        lv_obj_clear_flag(snooze_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_column(button_row, 8, 0);
        /* Membership follows visibility, in the same breath as the flag: a
         * control the owner can see is one NEXT can reach, and one they
         * cannot see must not be (§18.4). */
        if (alert_group && lv_obj_get_group(snooze_btn) != alert_group) {
            lv_group_add_obj(alert_group, snooze_btn);
        }
    } else {
        clock_alert_begin(CLOCK_ALERT_TIMER);
        set_text(title, "Timer finished");
        clock_format_remaining(e->timer.duration_ms, line, sizeof(line));
        /* Nothing to snooze: a countdown that is over is over. Stop takes
         * the whole row, and the gap between the two goes with it - LVGL
         * still counts it, and it would leave the one button short of the
         * edge for no reason anyone could see. */
        lv_obj_add_flag(snooze_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_column(button_row, 0, 0);
        /* Nothing to snooze, so nothing to focus: it leaves the group with
         * the same call that hides it, or NEXT would reach a control the
         * owner cannot see. */
        if (alert_group && lv_obj_get_group(snooze_btn) == alert_group) {
            lv_group_remove_obj(snooze_btn);
        }
    }
    set_text(detail, line);

    if (lv_obj_has_flag(sheet, LV_OBJ_FLAG_HIDDEN)) {
        /* A sheet over a field takes the keyboard away, the way a dialog
         * does (DS §17.5). It is not given back: the owner came here to
         * stop an alarm, not to carry on typing. */
        pocketos_shell_keyboard_hide();

        /* The show edge, once. Focus lands on Stop, the dominant action
         * (§18.4), so the first key the owner presses acts on it; it is set
         * after the push because the push saves the *app* group's focus, not
         * this one's, so the order between them does not matter. While the
         * redirection holds, the app underneath keeps its focus and its text
         * and receives nothing: printable keys, Enter and Backspace alike go
         * to a group that contains only these actions.
         *
         * Suppression follows the redirection and never precedes it. The two
         * are one transition: hiding the keyboard is only worth making stick
         * while the alert actually owns the keys, and a suppression raised
         * before a push that then failed would refuse the keyboard for the
         * rest of the session with nothing to show for it. */
        if (!isolating && pos_input_push_group(alert_group)) {
            isolating = true;
            if (alert_group) {
                lv_group_focus_obj(stop_btn);
            }
            pocketos_shell_keyboard_set_suppressed(1);
        }
        lv_obj_clear_flag(sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sheet);
    }
}
