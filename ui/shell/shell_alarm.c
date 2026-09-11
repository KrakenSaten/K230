/*
 * The shell's alarm alert. See shell_alarm.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_alarm.h"

#include "app.h"
#include "clock_alert.h"
#include "clock_engine.h"
#include "clock_runtime.h"
#include "clock_time.h"
#include "pocketui.h"

#include <stdio.h>
#include <string.h>

static lv_obj_t *sheet;
static lv_obj_t *title;
static lv_obj_t *detail;
static lv_obj_t *snooze_btn;
static lv_obj_t *button_row;

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

    /* Not added to the focus group: this sheet exists for the whole life of
     * the shell, and a hidden button sitting in the one group would be
     * reachable by key from every screen in PocketOS (DS §17.2). The alert
     * is touch-driven until there is a physical keyboard to reconsider it
     * for. */
    stop = pocketui_button(row, "Stop", on_stop, NULL);
    lv_obj_set_height(stop, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_grow(stop, 2); /* twice the width of Snooze */
    lv_obj_clear_flag(stop, LV_OBJ_FLAG_CLICK_FOCUSABLE);

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
    }
    set_text(detail, line);

    if (lv_obj_has_flag(sheet, LV_OBJ_FLAG_HIDDEN)) {
        /* A sheet over a field takes the keyboard away, the way a dialog
         * does (DS §17.5). It is not given back: the owner came here to
         * stop an alarm, not to carry on typing. */
        pocketos_shell_keyboard_hide();
        lv_obj_clear_flag(sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sheet);
    }
}
