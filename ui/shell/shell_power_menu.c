/*
 * The power menu. See shell_power_menu.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "shell_power_menu.h"

#include "lvgl.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "pos_styles.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <string.h>

#define MENU_BTN_H 56      /* DS §17.5's dialog buttons */
#define MENU_GAP 8
#define MENU_MAX_W 480
#define MENU_SCRIM_OPA LV_OPA_70

static struct {
    lv_obj_t *scrim;
    lv_obj_t *buttons;
    lv_obj_t *note;
    bool busy; /* sysd took the request: the menu only reports now */
    char note_text[96];
    unsigned opens;
} pm;

static void set_note(const char *text, enum pos_style_role role)
{
    snprintf(pm.note_text, sizeof(pm.note_text), "%s", text);
    if (!pm.note) {
        return;
    }
    lv_label_set_text(pm.note, pm.note_text);
    lv_obj_remove_style(pm.note, pos_style(POS_STYLE_STATUS_ERROR_TEXT), 0);
    lv_obj_remove_style(pm.note, pos_style(POS_STYLE_TEXT_SECONDARY), 0);
    pos_style_add(pm.note, role, 0);
    if (text[0]) {
        lv_obj_remove_flag(pm.note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pm.note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_cancel(lv_event_t *e)
{
    (void)e;
    shell_power_menu_close("cancel");
}

/* A tap on the scrim, outside the panel, is Cancel (DS §17.5: dismissal by
 * any other route is Cancel). The panel itself is clickable, so a tap
 * between its buttons never gets here. */
static void on_scrim(lv_event_t *e)
{
    if (lv_event_get_target(e) == pm.scrim) {
        shell_power_menu_close("tap outside");
    }
}

/* The one place this menu reaches a method that stops the machine, and only
 * from a press on Restart or Power off. The call is System's (bounded,
 * SHELL_IPC_UI_TIMEOUT_MS); a refusal - sysd away, a storage expansion
 * running - stays on the panel and the menu stays usable. */
static void request(const char *method, const char *doing)
{
    char err[96];
    cJSON *result;

    if (pm.busy) {
        return;
    }
    err[0] = '\0';
    LOG_INFO("power menu: %s", method);
    result = shell_ipc_call_timeout("sysd", method, NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    if (result) {
        cJSON_Delete(result);
        pm.busy = true;
        lv_obj_add_flag(pm.buttons, LV_OBJ_FLAG_HIDDEN);
        set_note(doing, POS_STYLE_TEXT_SECONDARY);
        return;
    }
    LOG_WARN("power menu: %s refused: %s", method, err[0] ? err : "no answer");
    set_note(err[0] ? err : "sysd did not answer", POS_STYLE_STATUS_ERROR_TEXT);
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    request("system.reboot", "Restarting...");
}

static void on_poweroff(lv_event_t *e)
{
    (void)e;
    request("system.poweroff", "Powering off...");
}

static lv_obj_t *menu_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, bool accent)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, NULL);

    lv_obj_set_height(b, MENU_BTN_H);
    /* Touch only, like System's dialog: out of the keyboard's group, so the
     * menu coming and going never moves the focus of the app under it. */
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_group_remove_obj(b);
    if (!accent) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
    }
    return b;
}

void shell_power_menu_open(void)
{
    lv_obj_t *box;
    lv_obj_t *body;

    if (pm.scrim) {
        return;
    }
    pm.busy = false;
    pm.note_text[0] = '\0';
    pm.opens++;

    pm.scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pm.scrim);
    pos_style_add(pm.scrim, POS_STYLE_SCREEN, 0);
    lv_obj_set_style_bg_opa(pm.scrim, MENU_SCRIM_OPA, 0);
    lv_obj_set_size(pm.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(pm.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pm.scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(pm.scrim, on_scrim, LV_EVENT_CLICKED, NULL);

    box = pocketui_card(pm.scrim);
    pos_style_add(box, POS_STYLE_SCREEN, 0);
    pos_style_add(box, POS_STYLE_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_width(box, LV_PCT(84));
    lv_obj_set_style_max_width(box, MENU_MAX_W, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_center(box);

    pocketui_label(box, "Power", POS_STYLE_TITLE);
    body = pocketui_label(box, "Holding the power key for 5 seconds powers off.", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 20, 0);

    pm.buttons = lv_obj_create(box);
    lv_obj_remove_style_all(pm.buttons);
    lv_obj_set_size(pm.buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pm.buttons, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pm.buttons, MENU_GAP, 0);
    lv_obj_clear_flag(pm.buttons, LV_OBJ_FLAG_SCROLLABLE);
    /* Cancel first and accented: the safe choice (DS §17.5). */
    menu_button(pm.buttons, "Cancel", on_cancel, true);
    menu_button(pm.buttons, "Restart", on_restart, false);
    menu_button(pm.buttons, "Power off", on_poweroff, false);

    pm.note = pocketui_label(box, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(pm.note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(pm.note, LV_PCT(100));
    lv_obj_set_style_pad_top(pm.note, 12, 0);
    lv_obj_add_flag(pm.note, LV_OBJ_FLAG_HIDDEN);

    LOG_INFO("power menu: open");
}

void shell_power_menu_close(const char *why)
{
    if (!pm.scrim) {
        return;
    }
    /* Async: Cancel is a click on one of its own buttons. */
    lv_obj_delete_async(pm.scrim);
    pm.scrim = NULL;
    pm.buttons = NULL;
    pm.note = NULL;
    pm.busy = false;
    pm.note_text[0] = '\0';
    LOG_INFO("power menu: closed (%s)", why ? why : "close");
}

bool shell_power_menu_visible(void)
{
    return pm.scrim != NULL;
}

const char *shell_power_menu_note(void)
{
    return pm.note_text;
}

unsigned shell_power_menu_open_count(void)
{
    return pm.opens;
}
