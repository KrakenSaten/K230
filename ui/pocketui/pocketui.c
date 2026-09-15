/*
 * PocketUI widgets on shared role styles. See pocketui.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketui.h"

void pocketui_init(void)
{
    pos_styles_init();
}

void pocketui_style_screen(lv_obj_t *screen)
{
    pos_style_add(screen, POS_STYLE_SCREEN, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *pocketui_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_remove_style_all(card);
    pos_style_add(card, POS_STYLE_PANEL, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *pocketui_tile(lv_obj_t *parent, const char *icon, const char *label,
                        lv_event_cb_t on_click, void *user_data)
{
    return pocketui_tile_mask(parent, NULL, icon, label, on_click, user_data);
}

lv_obj_t *pocketui_tile_mask(lv_obj_t *parent, const lv_image_dsc_t *mask, const char *icon,
                             const char *label, lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_t *ic;
    lv_obj_t *lb;

    lv_obj_remove_style_all(tile);
    pos_style_add(tile, POS_STYLE_SLAB, 0);
    pos_style_add(tile, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_size(tile, LV_PCT(100), POCKETUI_TILE_H);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) {
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, user_data);
    }
    /* Either way the icon is the tile's first child, top-left in the 12 px
     * inset, and not clickable, so a tap on it is a tap on the tile. */
    if (mask) {
        ic = lv_image_create(tile);
        lv_obj_remove_style_all(ic);
        pos_style_add(ic, POS_STYLE_APP_ICON, 0);
        lv_image_set_src(ic, mask);
    } else {
        ic = lv_label_create(tile);
        lv_label_set_text(ic, icon);
        pos_style_add(ic, POS_STYLE_SYMBOL_LARGE, 0);
        pos_style_add(ic, POS_STYLE_ACCENT_TEXT, 0);
    }
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
    lb = lv_label_create(tile);
    lv_label_set_text(lb, label);
    pos_style_add(lb, POS_STYLE_ROW_TITLE, 0);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return tile;
}

lv_obj_t *pocketui_kv_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *k;
    lv_obj_t *v;

    lv_obj_remove_style_all(row);
    pos_style_add(row, POS_STYLE_DIVIDER, 0);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, POCKETUI_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    k = lv_label_create(row);
    lv_label_set_text(k, key);
    pos_style_add(k, POS_STYLE_TEXT_SECONDARY, 0);
    v = lv_label_create(row);
    lv_label_set_text(v, value);
    pos_style_add(v, POS_STYLE_VALUE, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, LV_PCT(60), 0);
    return v;
}

lv_obj_t *pocketui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click,
                          void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY, 0);
    pos_style_add(btn, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lb, text);
    pos_style_add(lb, POS_STYLE_BUTTON_LABEL, 0);
    lv_obj_center(lb);
    if (on_click) {
        lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

lv_obj_t *pocketui_label(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = lv_label_create(parent);

    lv_label_set_text(lb, text ? text : "");
    pos_style_add(lb, role, 0);
    return lb;
}

/* ---- Text field (DS §17.1) --------------------------------------------- */

#define POCKETUI_CARET_BLINK_MS 500 /* DS §17.1: 500 ms on, 500 ms off */
#define POCKETUI_FIELD_MIN_LINES 3

static bool reduced_motion;

void pocketui_set_reduced_motion(bool on)
{
    reduced_motion = on;
}

/* A tap focuses the field through the one focus model of DS §17.2, rather
 * than leaving LVGL's click-focus to run beside the group. */
static void field_clicked_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (!lv_obj_has_state(ta, LV_STATE_DISABLED)) {
        pos_input_focus(ta);
    }
}

lv_obj_t *pocketui_text_field(lv_obj_t *parent, const char *placeholder, bool single_line)
{
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_t *ta;

    lv_obj_remove_style_all(wrap);
    lv_obj_set_width(wrap, LV_PCT(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wrap, 8, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    ta = lv_textarea_create(wrap);
    lv_obj_remove_style_all(ta);
    pos_style_add(ta, POS_STYLE_FIELD, 0);
    pos_style_add(ta, POS_STYLE_FIELD_FOCUSED, LV_STATE_FOCUSED);
    pos_style_add(ta, POS_STYLE_FIELD_DISABLED, LV_STATE_DISABLED);
    pos_style_add(ta, POS_STYLE_FIELD_PLACEHOLDER, LV_PART_TEXTAREA_PLACEHOLDER);
    pos_style_add(ta, POS_STYLE_FIELD_CURSOR, LV_PART_CURSOR);
    lv_obj_set_width(ta, LV_PCT(100));

    lv_textarea_set_one_line(ta, single_line);
    if (single_line) {
        lv_obj_set_height(ta, POCKETUI_ROW_H);
    } else {
        /* Three body lines is the floor of DS §17.1. The field takes the
         * height its parent gives it and scrolls once the text passes that;
         * body line-height is 1.5 (DS §3). */
        const lv_font_t *f = lv_obj_get_style_text_font(ta, LV_PART_MAIN);
        int32_t line = f ? lv_font_get_line_height(f) : 16;

        lv_obj_set_height(ta, LV_PCT(100));
        lv_obj_set_style_min_height(ta, POCKETUI_FIELD_MIN_LINES * line * 3 / 2, 0);
    }
    if (placeholder) {
        lv_textarea_set_placeholder_text(ta, placeholder);
    }

    /* Blinking caret, or a solid one under reduced motion (DS §12, §17.1). */
    lv_obj_set_style_anim_duration(ta, reduced_motion ? 0 : POCKETUI_CARET_BLINK_MS,
                                   LV_PART_CURSOR);

    lv_obj_add_event_cb(ta, field_clicked_cb, LV_EVENT_CLICKED, NULL);
    /* Deletion needs no handler: LVGL removes an object from its group in
     * the destructor, which clears the focus with it. */
    pos_input_add_obj(ta);
    return ta;
}

/* The caption lives in the wrapper, after the field. It is created the first
 * time an error is shown and hidden rather than deleted afterwards, so a
 * field toggling between valid and invalid does not churn objects. */
static lv_obj_t *field_error_label(lv_obj_t *ta, bool create)
{
    lv_obj_t *wrap = lv_obj_get_parent(ta);
    lv_obj_t *lb;

    if (!wrap) {
        return NULL;
    }
    if (lv_obj_get_child_count(wrap) > 1) {
        return lv_obj_get_child(wrap, 1);
    }
    if (!create) {
        return NULL;
    }
    lb = pocketui_label(wrap, "", POS_STYLE_STATUS_ERROR_TEXT);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

void pocketui_text_field_set_error(lv_obj_t *field, const char *message)
{
    lv_obj_t *lb;

    if (!field) {
        return;
    }
    if (message && message[0]) {
        pos_style_add(field, POS_STYLE_FIELD_ERROR, 0);
        lb = field_error_label(field, true);
        if (lb) {
            lv_label_set_text(lb, message);
            lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_obj_remove_style(field, pos_style(POS_STYLE_FIELD_ERROR), 0);
    lb = field_error_label(field, false);
    if (lb) {
        lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
    }
}

void pocketui_text_field_set_enabled(lv_obj_t *field, bool enabled)
{
    if (!field) {
        return;
    }
    if (enabled) {
        lv_obj_clear_state(field, LV_STATE_DISABLED);
        lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
        pos_input_add_obj(field);
        return;
    }
    /* Leaving the group is what gives up the focus: LVGL has no unfocused
     * state, so a disabled field is removed rather than merely defocused. */
    lv_group_remove_obj(field);
    lv_obj_add_state(field, LV_STATE_DISABLED);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_CLICKABLE);
}
