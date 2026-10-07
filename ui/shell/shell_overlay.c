/*
 * The developer debug overlay. See shell_overlay.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "shell_overlay.h"

#include "app.h"
#include "overlay_model.h"
#include "pocketlog/pocketlog.h"
#include "pocketui.h"
#include "settings.h"
#include "shell_ipc.h"

#include <errno.h>
#include <string.h>

/* Above the foot of the content area: clear of the rounded corners, which
 * are 30 px squares at the edges - the line is centred and never reaches
 * them - and off the very edge, where the bezel's curve starts. */
#define OVERLAY_FOOT_GAP 6

static struct {
    lv_obj_t *content;
    lv_obj_t *box;
    lv_obj_t *label;
    lv_timer_t *timer;
    bool enabled;
    struct overlay_model model;
    unsigned refreshes;
} ov;

void shell_overlay_place(void)
{
    lv_area_t a;
    int32_t max_w;

    if (!ov.box || !ov.content) {
        return;
    }
    lv_obj_get_coords(ov.content, &a);
    max_w = lv_area_get_width(&a) - 2 * 40;
    lv_obj_set_style_max_width(ov.label, max_w > 100 ? max_w : 100, 0);
    lv_obj_update_layout(ov.box);
    lv_obj_set_pos(ov.box, a.x1 + (lv_area_get_width(&a) - lv_obj_get_width(ov.box)) / 2,
                   a.y2 + 1 - OVERLAY_FOOT_GAP - lv_obj_get_height(ov.box));
}

static void refresh(lv_timer_t *t)
{
    char err[96];
    cJSON *status;
    cJSON *radio = NULL;

    (void)t;
    err[0] = '\0';
    status = shell_ipc_call_timeout("sysd", "system.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    /* radiod is only asked while the status bar's own poll says it answers:
     * a radio that is not there costs nothing here. */
    if (pocketos_shell_radio_state()) {
        radio = shell_ipc_call_timeout("radiod", "radio.stats", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    }
    overlay_model_update(&ov.model, status, radio, lv_tick_get());
    cJSON_Delete(status);
    cJSON_Delete(radio);
    ov.refreshes++;
    if (ov.label) {
        lv_label_set_text(ov.label, ov.model.text);
        shell_overlay_place();
        lv_obj_move_foreground(ov.box);
    }
}

static void show(void)
{
    if (ov.box) {
        return;
    }
    overlay_model_init(&ov.model);
    ov.box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov.box);
    pos_style_add(ov.box, POS_STYLE_SLAB, 0);
    lv_obj_set_style_bg_opa(ov.box, LV_OPA_80, 0);
    lv_obj_set_size(ov.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(ov.box, 10, 0);
    lv_obj_set_style_pad_ver(ov.box, 4, 0);
    /* It takes no touch: a finger on it reaches whatever is under it. */
    lv_obj_remove_flag(ov.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov.box, LV_OBJ_FLAG_SCROLLABLE);
    ov.label = lv_label_create(ov.box);
    pos_style_add(ov.label, POS_STYLE_CAPTION, 0);
    pos_style_add(ov.label, POS_STYLE_TEXT_PRIMARY, 0);
    lv_obj_add_style(ov.label, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    lv_obj_set_style_text_align(ov.label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ov.label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ov.label, LV_SIZE_CONTENT);
    lv_label_set_text(ov.label, ov.model.text);
    shell_overlay_place();
    ov.timer = lv_timer_create(refresh, OVERLAY_REFRESH_MS, NULL);
    /* The first figures now rather than a dash for two seconds. */
    refresh(NULL);
}

static void hide(void)
{
    if (ov.timer) {
        lv_timer_delete(ov.timer);
        ov.timer = NULL;
    }
    if (ov.box) {
        lv_obj_delete(ov.box);
        ov.box = NULL;
        ov.label = NULL;
    }
}

void shell_overlay_init(lv_obj_t *content)
{
    const char *stored = settings_get(OVERLAY_SETTING, NULL);

    ov.content = content;
    ov.enabled = false;
    if (stored && strcmp(stored, "1") == 0) {
        ov.enabled = true;
    } else if (stored && strcmp(stored, "0") != 0) {
        LOG_WARN("overlay: stored %s=%s is not 0 or 1, off", OVERLAY_SETTING, stored);
    }
    if (ov.enabled) {
        show();
        LOG_INFO("overlay: on (stored)");
    }
}

bool shell_overlay_enabled(void)
{
    return ov.enabled;
}

int shell_overlay_set_enabled(bool on)
{
    if (on == ov.enabled) {
        return 0;
    }
    if (settings_set(OVERLAY_SETTING, on ? "1" : "0") < 0) {
        LOG_WARN("overlay: %s not persisted to %s: %s", on ? "on" : "off", settings_path(), strerror(errno));
        return -1;
    }
    ov.enabled = on;
    if (on) {
        show();
    } else {
        hide();
    }
    LOG_INFO("overlay: %s", on ? "on" : "off");
    return 0;
}

const char *shell_overlay_text(void)
{
    return ov.box ? ov.model.text : "";
}

unsigned shell_overlay_refreshes(void)
{
    return ov.refreshes;
}

bool shell_overlay_alive(void)
{
    return ov.box != NULL || ov.timer != NULL;
}

void shell_overlay_shutdown(void)
{
    hide();
}
