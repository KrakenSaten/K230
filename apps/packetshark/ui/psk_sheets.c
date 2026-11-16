/*
 * PacketShark's confirmations (UI_UX.md §4.3, §5.5; DS §17.5): one sheet
 * over the app, a sentence that says what happens, the safe choice first
 * and accented, the other beside it. M3 has only these; the filter, sort,
 * save and files sheets are later milestones.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include <stdio.h>

static void on_safe(lv_event_t *e)
{
    psk_sheet_answer(lv_event_get_user_data(e), false);
}

static void on_go(lv_event_t *e)
{
    psk_sheet_answer(lv_event_get_user_data(e), true);
}

void psk_sheet_build(struct psk_app *a)
{
    lv_obj_t *card;
    lv_obj_t *buttons;

    /* Over everything in the frame, outside its flex column. */
    a->sheet = psk_box(a->frame);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_CLICKABLE); /* takes taps meant for what is under it */
    pos_style_add(a->sheet, POS_STYLE_SCREEN, 0);
    lv_obj_set_size(a->sheet, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(a->sheet, PSK_TAB_H + 14, 0);

    card = pocketui_card(a->sheet);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    a->sheet_title = pocketui_label(card, "", POS_STYLE_TITLE);
    lv_label_set_long_mode(a->sheet_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->sheet_title, LV_PCT(100));
    a->sheet_text = pocketui_label(card, "", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->sheet_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->sheet_text, LV_PCT(100));
    lv_obj_set_style_pad_bottom(a->sheet_text, 8, 0);

    buttons = psk_box(card);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, POCKETUI_TOUCH_MIN);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, PSK_GAP, 0);
    a->sheet_safe = psk_button(buttons, "", true, on_safe, a);
    a->sheet_go = psk_button(buttons, "", false, on_go, a);
    lv_obj_set_flex_grow(a->sheet_safe, 1);
    lv_obj_set_flex_grow(a->sheet_go, 1);
    lv_obj_set_height(a->sheet_safe, POCKETUI_TOUCH_MIN);
    lv_obj_set_height(a->sheet_go, POCKETUI_TOUCH_MIN);
    lv_obj_add_flag(a->sheet, LV_OBJ_FLAG_HIDDEN);
    a->sheet_kind = PSK_SHEET_NONE;
}

void psk_sheet_show(struct psk_app *a, enum psk_sheet kind)
{
    char n[32];
    char text[256];
    const char *title = "";
    const char *safe = "STAY";
    const char *go = "";
    uint64_t packets = a->model->c.seen;

    psk_fmt_count(packets, n, sizeof(n));
    switch (kind) {
    case PSK_SHEET_FULL:
        title = "Keep full packets?";
        snprintf(text, sizeof(text),
                 "Full packets include whatever passes in clear text, such as passwords and message "
                 "contents. This applies to the next capture only.");
        safe = "HEADERS";
        go = "FULL";
        break;
    case PSK_SHEET_NOLIMIT:
        title = "Capture without a time limit?";
        snprintf(text, sizeof(text),
                 "The capture runs until you stop it or leave PacketShark, and the screen stays on "
                 "meanwhile. This applies to the next capture only.");
        safe = "CANCEL";
        go = "NO LIMIT";
        break;
    case PSK_SHEET_DISCARD:
    case PSK_SHEET_NEW:
        title = "Discard this capture?";
        snprintf(text, sizeof(text), "Its %s packets are removed from memory. Nothing has been saved.", n);
        go = kind == PSK_SHEET_NEW ? "NEW CAPTURE" : "DISCARD";
        break;
    case PSK_SHEET_LEAVE:
        title = "Leave PacketShark?";
        snprintf(text, sizeof(text), "The capture stops and its %s packets are discarded.", n);
        go = "LEAVE";
        break;
    case PSK_SHEET_NONE:
    default:
        psk_sheet_hide(a);
        return;
    }
    a->sheet_kind = kind;
    psk_text(a->sheet_title, title);
    psk_text(a->sheet_text, text);
    psk_text(lv_obj_get_child(a->sheet_safe, 0), safe);
    psk_text(lv_obj_get_child(a->sheet_go, 0), go);
    psk_set_hidden(a->sheet, false);
    lv_obj_move_foreground(a->sheet);
}

void psk_sheet_hide(struct psk_app *a)
{
    a->sheet_kind = PSK_SHEET_NONE;
    psk_set_hidden(a->sheet, true);
}
