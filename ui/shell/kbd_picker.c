/*
 * The long-press letter picker. See kbd_picker.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kbd_picker.h"

#include "pos_styles.h"

#include <string.h>

/* Each choice is a 64 px touch target (DS §9), four apart, inside a 4 px
 * frame; the row keeps this far from the screen's edges and the field. */
#define CHOICE_PX 64
#define CHOICE_GAP 4
#define FRAME_PAD 4
#define EDGE_PX 16
#define FIELD_GAP 8

static struct {
    lv_obj_t *scrim;  /* full screen on the top layer: a tap outside cancels */
    lv_obj_t *box;    /* the row of letters */
    lv_obj_t *target; /* the text area the letter is for */
    lv_obj_t *choice[KBD_PICKER_MAX];
    pos_key_t key[KBD_PICKER_MAX];
    unsigned n;
    unsigned selected;
} pk;

static void target_deleted(lv_event_t *e);

/* A code point as UTF-8, for a label. Two bytes cover everything a picker
 * offers (Latin-1), three are allowed for. */
static void utf8_of(pos_key_t cp, char out[5])
{
    memset(out, 0, 5);
    if (cp < 0x80u) {
        out[0] = (char)cp;
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
    }
}

/* The object the keyboard's keys reach right now, when it is a text area. */
static lv_obj_t *focused_field(void)
{
    lv_obj_t *f;

    if (pos_input_group_redirected() || pos_input_raw_focused()) {
        return NULL; /* an alert has the keys, or the Terminal wants them raw */
    }
    f = pos_input_focused();
    if (!f || !lv_obj_check_type(f, &lv_textarea_class) || lv_obj_has_state(f, LV_STATE_DISABLED)) {
        return NULL;
    }
    return f;
}

bool kbd_picker_can_open(void)
{
    return focused_field() != NULL;
}

bool kbd_picker_is_open(void)
{
    return pk.scrim != NULL;
}

void kbd_picker_close(void)
{
    if (!pk.scrim) {
        return;
    }
    if (pk.target) {
        lv_obj_remove_event_cb_with_user_data(pk.target, target_deleted, NULL);
    }
    /* Async: a tap on a letter closes the picker from inside that letter's
     * own event, and an object must not be deleted under its own event.
     * Hidden now, so it is neither drawn nor tapped in the meantime. */
    lv_obj_add_flag(pk.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(pk.scrim);
    memset(&pk, 0, sizeof(pk));
}

static void target_deleted(lv_event_t *e)
{
    (void)e;
    pk.target = NULL; /* already going; nothing to unhook */
    kbd_picker_close();
}

static void show_selection(void)
{
    unsigned i;

    for (i = 0; i < pk.n; i++) {
        lv_obj_t *b = pk.choice[i];

        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
        pos_style_add(b, i == pk.selected ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY, 0);
    }
}

/* Type the choice into the field it was opened for, then close. Nothing is
 * typed if that field no longer has the keys. */
static void choose(unsigned i)
{
    pos_key_t key = i < pk.n ? pk.key[i] : 0;
    bool ours = pk.target && focused_field() == pk.target;

    kbd_picker_close();
    if (key && ours) {
        pos_input_push_key(key);
    }
}

static void on_choice(lv_event_t *e)
{
    choose((unsigned)(uintptr_t)lv_event_get_user_data(e));
}

static void on_scrim(lv_event_t *e)
{
    (void)e;
    kbd_picker_close();
}

/* Beside the field: above it when there is room, below it otherwise, and
 * never past an edge of the screen. */
static void place(void)
{
    int32_t sw = lv_display_get_horizontal_resolution(NULL);
    int32_t sh = lv_display_get_vertical_resolution(NULL);
    int32_t w;
    int32_t h;
    int32_t x;
    int32_t y;
    lv_area_t a;

    lv_obj_update_layout(pk.box);
    w = lv_obj_get_width(pk.box);
    h = lv_obj_get_height(pk.box);
    lv_obj_get_coords(pk.target, &a);

    x = a.x1;
    if (x + w > sw - EDGE_PX) {
        x = sw - EDGE_PX - w;
    }
    if (x < EDGE_PX) {
        x = EDGE_PX;
    }
    y = a.y1 - FIELD_GAP - h;
    if (y < EDGE_PX) {
        y = a.y2 + FIELD_GAP;
    }
    if (y + h > sh - EDGE_PX) {
        y = sh - EDGE_PX - h;
    }
    lv_obj_set_pos(pk.box, x, y);
}

bool kbd_picker_open(const pos_key_t *choices, unsigned n)
{
    lv_obj_t *field = focused_field();
    unsigned i;

    if (!field || !choices || n == 0 || n > KBD_PICKER_MAX) {
        return false;
    }
    kbd_picker_close();
    pk.target = field;
    pk.n = n;
    pk.selected = 0;
    lv_obj_add_event_cb(field, target_deleted, LV_EVENT_DELETE, NULL);

    pk.scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pk.scrim);
    lv_obj_set_size(pk.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(pk.scrim, LV_OBJ_FLAG_CLICKABLE);
    /* None of the picker is click-focusable: a tap on it must leave the
     * field's focus, and LVGL's focus bookkeeping, exactly as they were. */
    lv_obj_clear_flag(pk.scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(pk.scrim, on_scrim, LV_EVENT_CLICKED, NULL);

    pk.box = lv_obj_create(pk.scrim);
    lv_obj_remove_style_all(pk.box);
    pos_style_add(pk.box, POS_STYLE_SCREEN, 0);
    pos_style_add(pk.box, POS_STYLE_PANEL, 0);
    /* The panel's outline on the screen's background: PANEL leaves the
     * background transparent, and added last it would let the text beside
     * the field show between the letters. */
    lv_obj_set_style_bg_opa(pk.box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(pk.box, FRAME_PAD, 0);
    lv_obj_set_style_pad_column(pk.box, CHOICE_GAP, 0);
    lv_obj_set_size(pk.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pk.box, LV_FLEX_FLOW_ROW);
    /* A tap between two letters is not a tap outside. */
    lv_obj_add_flag(pk.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pk.box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);

    for (i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(pk.box);
        lv_obj_t *lb = lv_label_create(b);
        char text[5];

        lv_obj_remove_style_all(b);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_size(b, CHOICE_PX, CHOICE_PX);
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
        utf8_of(choices[i], text);
        lv_label_set_text(lb, text);
        pos_style_add(lb, POS_STYLE_BUTTON_LABEL, 0);
        lv_obj_center(lb);
        lv_obj_add_event_cb(b, on_choice, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        pk.choice[i] = b;
        pk.key[i] = choices[i];
    }
    show_selection();
    place();
    return true;
}

bool kbd_picker_key(pos_key_t key)
{
    if (!pk.scrim) {
        return false;
    }
    switch (key) {
    case LV_KEY_LEFT:
        if (pk.selected > 0) {
            pk.selected--;
            show_selection();
        }
        return true;
    case LV_KEY_RIGHT:
        if (pk.selected + 1 < pk.n) {
            pk.selected++;
            show_selection();
        }
        return true;
    case LV_KEY_ENTER:
        choose(pk.selected);
        return true;
    case LV_KEY_ESC:
        kbd_picker_close();
        return true;
    default:
        kbd_picker_close();
        return false;
    }
}

void kbd_picker_check(void)
{
    if (pk.scrim && focused_field() != pk.target) {
        kbd_picker_close();
    }
}

unsigned kbd_picker_selected(void)
{
    return pk.selected;
}

lv_obj_t *kbd_picker_choice(unsigned i)
{
    return pk.scrim && i < pk.n ? pk.choice[i] : NULL;
}
