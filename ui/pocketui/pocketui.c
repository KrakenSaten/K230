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
    ic = lv_label_create(tile);
    lv_label_set_text(ic, icon);
    pos_style_add(ic, POS_STYLE_SYMBOL_LARGE, 0);
    pos_style_add(ic, POS_STYLE_ACCENT_TEXT, 0);
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
