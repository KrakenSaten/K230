/*
 * NODES' find bar. See rift_find.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_find.h"

#include "app.h"
#include "pocketui.h"
#include "pos_input.h"
#include "pos_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The field's height in landscape: a line of type and 4 px around it, as the
 * landscape composer's (rift_app.c). Portrait keeps the field's own 64. */
#define FIELD_H_WIDE 32
#define ZERO_W 156
#define CLEAR_W 112
#define NET_W 96

struct rift_find {
    lv_obj_t *row;
    lv_obj_t *field;
    lv_obj_t *zero;
    lv_obj_t *zero_label;
    lv_obj_t *clear;
    lv_obj_t *net;
    int zero_drawn;  /* the toggle's look as last drawn; -1 not yet */
    int clear_shown; /* -1 not yet */
};

static struct rift_find *find_of(const struct rift_app *app)
{
    return app ? app->find : NULL;
}

/* Typing narrows the list as it goes. The refresh is asked for, not done
 * here: this runs inside the text area's own event, and the list measures
 * names against settled widths (rift_app.h, refresh_pending). */
static void on_changed(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(lv_event_get_target_obj(e));
    char kept[4 * RIFT_QUERY_MAX];
    size_t i;
    size_t o = 0;

    /* A tab or an Esc the field took in before any callback saw it is not
     * part of a name anybody could be looking for (lvgl-layout gotcha 2). */
    for (i = 0; text && text[i] && o + 1 < sizeof(kept); i++) {
        unsigned char c = (unsigned char)text[i];

        if (c >= 0x20 && c != 0x7F) {
            kept[o++] = (char)c;
        }
    }
    kept[o] = '\0';
    /* The field counts characters and the query holds bytes: cut on a
     * character boundary, never through one. */
    rift_utf8_copy(a->node_query, sizeof(a->node_query), kept);
    a->refresh_pending = 1;
}

static void on_clicked(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    /* Portrait only: no physical keyboard, so the Doors touch keyboard comes
     * up on focus (handoff §10). Landscape has the keyboard base. And only
     * for a finger: Enter on a keypad reaches the field as READY and then as
     * a CLICKED, and the sheet READY put away must not come straight back. */
    if (a->wide || lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, a);
    }
}

/* Enter, or the keyboard's Done: the search is what it is, and the arrows go
 * back to the list. Asked for from the timer - a focus change made inside the
 * event LVGL is dispatching does not stick. */
static void on_ready(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    a->focus_list_pending = 1;
    a->refresh_pending = 1;
}

/* Esc clears what was typed; on an empty field it hands the list its focus
 * back. The text area has already taken the Esc in as a character by the time
 * this runs, so "empty" means nothing anybody could have meant. */
static void on_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_find *f = find_of(a);
    int typed;

    if (lv_event_get_key(e) != LV_KEY_ESC || !f) {
        return;
    }
    /* Read before the field is emptied: emptying it raises VALUE_CHANGED,
     * which clears the query too. */
    typed = a->node_query[0] != '\0';
    lv_textarea_set_text(f->field, "");
    if (!typed) {
        a->focus_list_pending = 1;
    }
    a->node_query[0] = '\0';
    a->refresh_pending = 1;
}

static void on_clear(lv_event_t *e)
{
    rift_find_set_query(lv_event_get_user_data(e), "");
}

/* NET is a view of the same nodes, under NODES: the hop rings
 * (ui/rift_netview.c). LIST there comes back. */
static void on_net(lv_event_t *e)
{
    rift_app_show_section(lv_event_get_user_data(e), RIFT_SEC_NET);
}

static void on_zero(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    rift_find_set_zero_hop(a, !a->node_zero_hop);
}

void rift_find_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_find *f = calloc(1, sizeof(*f));

    if (!f) {
        return;
    }
    app->find = f;
    f->zero_drawn = -1;
    f->clear_shown = -1;

    f->row = lv_obj_create(parent);
    lv_obj_remove_style_all(f->row);
    lv_obj_set_width(f->row, LV_PCT(100));
    lv_obj_set_height(f->row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(f->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(f->row, 12, 0);
    lv_obj_set_style_pad_ver(f->row, 6, 0);
    lv_obj_remove_flag(f->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(f->row, LV_OBJ_FLAG_CLICKABLE);

    f->field = pocketui_text_field(f->row, "Find a node: name or hash", true);
    if (f->field) {
        lv_obj_set_flex_grow(lv_obj_get_parent(f->field), 1);
        lv_textarea_set_max_length(f->field, RIFT_QUERY_MAX - 1);
        lv_obj_add_event_cb(f->field, on_changed, LV_EVENT_VALUE_CHANGED, app);
        lv_obj_add_event_cb(f->field, on_clicked, LV_EVENT_CLICKED, app);
        lv_obj_add_event_cb(f->field, on_ready, LV_EVENT_READY, app);
        lv_obj_add_event_cb(f->field, on_key, LV_EVENT_KEY, app);
    }
    f->clear = rift_action(f->row, "CLEAR", 0, 1, on_clear, app);
    lv_obj_set_flex_grow(f->clear, 0);
    lv_obj_set_width(f->clear, CLEAR_W);
    lv_obj_add_flag(f->clear, LV_OBJ_FLAG_HIDDEN);
    f->zero = rift_action(f->row, "ZERO-HOP", 0, 1, on_zero, app);
    lv_obj_set_flex_grow(f->zero, 0);
    lv_obj_set_width(f->zero, ZERO_W);
    f->zero_label = lv_obj_get_child(f->zero, 0);
    f->net = rift_action(f->row, "NET", 0, 1, on_net, app);
    lv_obj_set_flex_grow(f->net, 0);
    lv_obj_set_width(f->net, NET_W);
}

void rift_find_shape(struct rift_app *app)
{
    struct rift_find *f = find_of(app);
    int32_t h;

    if (!f) {
        return;
    }
    h = app->wide ? RIFT_ROW_H : RIFT_TOUCH_H;
    lv_obj_set_style_pad_ver(f->row, app->wide ? 2 : 6, 0);
    lv_obj_set_height(f->zero, h);
    lv_obj_set_height(f->clear, h);
    lv_obj_set_height(f->net, h);
    if (f->field) {
        lv_obj_set_height(f->field, app->wide ? FIELD_H_WIDE : 64);
        lv_obj_set_style_pad_ver(f->field, app->wide ? 4 : 16, 0);
    }
}

/* The toggle in the look of what it is: primary while the view is on, as
 * SYSTEM's SOUND switches are. The colour is never the only thing that says
 * so: the list's group label reads ZERO-HOP REPEATERS while it is on. */
static void paint_zero(struct rift_find *f, int on)
{
    if (on == f->zero_drawn) {
        return;
    }
    lv_obj_remove_style(f->zero, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(f->zero, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(f->zero, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(f->zero, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (on) {
        pos_style_add(f->zero, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(f->zero, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(f->zero, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(f->zero, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    f->zero_drawn = on;
}

void rift_find_refresh(struct rift_app *app)
{
    struct rift_find *f = find_of(app);
    int clear;

    if (!f) {
        return;
    }
    paint_zero(f, app->node_zero_hop ? 1 : 0);
    /* CLEAR only while there is something to clear: a touch reader's way
     * back to the whole list without the keyboard. */
    clear = app->node_query[0] != '\0';
    if (clear != f->clear_shown) {
        if (clear) {
            lv_obj_remove_flag(f->clear, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(f->clear, LV_OBJ_FLAG_HIDDEN);
        }
        f->clear_shown = clear;
    }
}

lv_obj_t *rift_find_field(const struct rift_app *app)
{
    struct rift_find *f = find_of(app);

    return f ? f->field : NULL;
}

void rift_find_set_query(struct rift_app *app, const char *query)
{
    struct rift_find *f = find_of(app);
    char text[RIFT_QUERY_MAX];

    if (!app) {
        return;
    }
    rift_utf8_copy(text, sizeof(text), query ? query : "");
    /* From a copy, never from node_query itself: a field with a length limit
     * takes text a character at a time and raises VALUE_CHANGED for each,
     * and on_changed rewrites node_query from what the field holds so far -
     * reading from it here would stop after the first character. */
    if (f && f->field && strcmp(lv_textarea_get_text(f->field), text) != 0) {
        lv_textarea_set_text(f->field, text);
    }
    snprintf(app->node_query, sizeof(app->node_query), "%s", text);
    app->refresh_pending = 1;
}

void rift_find_set_zero_hop(struct rift_app *app, int on)
{
    if (!app) {
        return;
    }
    app->node_zero_hop = on ? 1 : 0;
    if (on) {
        /* A fresh list from the service, so the view is as of now rather
         * than as of the last periodic snapshot. A question to the service:
         * nothing is put on the air. */
        rift_ipc_request_nodes(&app->ipc);
    }
    app->refresh_pending = 1;
}
