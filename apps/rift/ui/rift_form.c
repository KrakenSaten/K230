/*
 * The parts of SYSTEM's management panels. See rift_form.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_form.h"

#include "app.h"
#include "pocketui.h"
#include "pos_input.h"
#include "pos_styles.h"

lv_obj_t *rift_form_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, RIFT_FORM_GAP, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

lv_obj_t *rift_form_column(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

lv_obj_t *rift_form_text(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

void rift_form_show(lv_obj_t *obj, int on)
{
    if (!obj) {
        return;
    }
    if (on) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void rift_form_chosen(lv_obj_t *button, int on)
{
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(button, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    if (on) {
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(button, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(button, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
}

void rift_form_typed(lv_obj_t *field, char *out, size_t out_len)
{
    const char *t = field ? lv_textarea_get_text(field) : "";
    size_t o = 0;
    size_t i;

    if (!out || out_len == 0) {
        return;
    }
    for (i = 0; t && t[i] && o + 1 < out_len; i++) {
        unsigned char c = (unsigned char)t[i];

        if (c >= 0x20 && c != 0x7F) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Portrait has no keyboard base: the Doors touch keyboard comes up for a
 * finger on a field (handoff §10), and only for a finger - keypad Enter
 * reaches a field as READY and then CLICKED. */
static void on_field_clicked(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (a->wide || lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, a);
    }
}

static void on_field_ready(lv_event_t *e)
{
    (void)e;
    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
}

lv_obj_t *rift_form_field(struct rift_app *app, lv_obj_t *parent, const char *placeholder,
                          uint32_t max_chars)
{
    lv_obj_t *f = pocketui_text_field(parent, placeholder, true);

    if (f) {
        lv_textarea_set_max_length(f, max_chars);
        lv_obj_add_event_cb(f, on_field_clicked, LV_EVENT_CLICKED, app);
        lv_obj_add_event_cb(f, on_field_ready, LV_EVENT_READY, app);
        /* Out of the focus group until its form opens (rift_form_field_live). */
        rift_form_field_live(f, 0);
    }
    return f;
}

void rift_form_field_live(lv_obj_t *field, int live)
{
    int in_group;

    if (!field) {
        return;
    }
    in_group = lv_obj_get_group(field) != NULL;
    if (live && !in_group) {
        pos_input_add_obj(field);
    } else if (!live && in_group) {
        lv_group_remove_obj(field);
    }
}

int rift_form_service_ready(const struct rift_app *app)
{
    const struct rift_model *m = &app->model;

    return !m->stale && m->state != RIFT_SVC_ABSENT && m->have_identity;
}
