/*
 * PocketUI implementation. See pocketui.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketui.h"

static struct pocketui_tokens tokens;
static lv_style_t style_card;
static lv_style_t style_tile;
static lv_style_t style_tile_pressed;
static lv_style_t style_button;

const struct pocketui_tokens *pocketui_tokens(void)
{
    return &tokens;
}

void pocketui_init(void)
{
    tokens.bg = lv_color_hex(0x0b0e12);
    tokens.surface = lv_color_hex(0x161b22);
    tokens.surface_hi = lv_color_hex(0x1f2630);
    tokens.text = lv_color_hex(0xe6edf3);
    tokens.text_dim = lv_color_hex(0x8b949e);
    tokens.accent = lv_color_hex(0x2fd6c8);
    tokens.accent_2 = lv_color_hex(0x8a7cff);
    tokens.ok = lv_color_hex(0x3fb950);
    tokens.warn = lv_color_hex(0xd29922);
    tokens.error = lv_color_hex(0xf85149);

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, tokens.surface);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_radius(&style_card, POCKETUI_RADIUS);
    lv_style_set_pad_all(&style_card, POCKETUI_PAD);
    lv_style_set_text_color(&style_card, tokens.text);

    lv_style_init(&style_tile);
    lv_style_set_bg_color(&style_tile, tokens.surface);
    lv_style_set_bg_opa(&style_tile, LV_OPA_COVER);
    lv_style_set_border_width(&style_tile, 0);
    lv_style_set_radius(&style_tile, POCKETUI_RADIUS);
    lv_style_set_pad_all(&style_tile, 12);
    lv_style_set_text_color(&style_tile, tokens.text);

    lv_style_init(&style_tile_pressed);
    lv_style_set_bg_color(&style_tile_pressed, tokens.surface_hi);
    lv_style_set_outline_width(&style_tile_pressed, 2);
    lv_style_set_outline_color(&style_tile_pressed, tokens.accent);

    lv_style_init(&style_button);
    lv_style_set_bg_color(&style_button, tokens.accent);
    lv_style_set_bg_opa(&style_button, LV_OPA_COVER);
    lv_style_set_text_color(&style_button, tokens.bg);
    lv_style_set_radius(&style_button, POCKETUI_RADIUS);
    lv_style_set_pad_ver(&style_button, 18);
    lv_style_set_shadow_width(&style_button, 0);
}

void pocketui_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, tokens.bg, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, tokens.text, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *pocketui_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *pocketui_tile(lv_obj_t *parent, const char *icon, const char *label,
                        lv_event_cb_t on_click, void *user_data)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_t *ic;
    lv_obj_t *lb;

    lv_obj_add_style(tile, &style_tile, 0);
    lv_obj_add_style(tile, &style_tile_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(tile, LV_PCT(100), 150);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) {
        lv_obj_add_event_cb(tile, on_click, LV_EVENT_CLICKED, user_data);
    }
    ic = lv_label_create(tile);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ic, tokens.accent, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
    lb = lv_label_create(tile);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_20, 0);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return tile;
}

lv_obj_t *pocketui_kv_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_t *k;
    lv_obj_t *v;

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, tokens.text_dim, 0);
    v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, LV_PCT(60), 0);
    return v;
}

lv_obj_t *pocketui_button(lv_obj_t *parent, const char *text, lv_event_cb_t on_click,
                          void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb = lv_label_create(btn);

    lv_obj_add_style(btn, &style_button, 0);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_20, 0);
    lv_obj_center(lb);
    if (on_click) {
        lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}
