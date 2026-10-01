/*
 * PocketFleet local widgets. See fleet_widgets.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_widgets.h"

#include <stddef.h>
#include <stdint.h>

#define CAPTION_INSET 16   /* DS §2: 16 px from the panel's left edge */
#define CAPTION_PAD 6
#define CAPTION_RISE FLEET_CAPTION_RISE
#define SEGMENT_H 56
#define SEGMENT_GAP 4
#define PAIRED_H 56

/* OVERFLOW_VISIBLE only lets children draw into the panel's extended draw
 * area, so the panel has to ask for one before the caption can sit on the
 * border rather than under it. */
static void panel_ext_draw(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, CAPTION_RISE + 4);
}

static void panel_caption(lv_obj_t *panel, const char *title)
{
    lv_obj_t *cap;
    int32_t pad_top;
    int32_t pad_left;

    if (!title) {
        return;
    }
    cap = lv_label_create(panel);
    lv_label_set_text(cap, title);
    /* The screen role paints the caption's own background so it breaks the
     * border rule it sits on; the caption role gives it mono 14 tracking. */
    pos_style_add(cap, POS_STYLE_SCREEN, 0);
    pos_style_add(cap, POS_STYLE_CAPTION, 0);
    lv_obj_set_style_pad_hor(cap, CAPTION_PAD, 0);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_IGNORE_LAYOUT);
    /* The caption sits on the border, which is outside the content box the
     * alignment is measured from, so back out the panel's own padding. */
    pad_top = lv_obj_get_style_pad_top(panel, LV_PART_MAIN);
    pad_left = lv_obj_get_style_pad_left(panel, LV_PART_MAIN);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, CAPTION_INSET - pad_left,
                 -pad_top - CAPTION_RISE);
    /* Straddling the border means drawing outside the panel. */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(panel, panel_ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(panel);
}

lv_obj_t *fleet_panel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *panel = pocketui_card(parent);

    lv_obj_set_style_pad_row(panel, 12, 0);
    panel_caption(panel, title);
    return panel;
}

lv_obj_t *fleet_list_panel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *panel = pocketui_card(parent);

    lv_obj_set_style_pad_ver(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    panel_caption(panel, title);
    return panel;
}

static lv_obj_t *button_with_role(lv_obj_t *parent, const char *text, enum pos_style_role role,
                                  int height, lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *label = lv_label_create(btn);

    lv_obj_remove_style_all(btn);
    pos_style_add(btn, role, 0);
    pos_style_add(btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, height);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, text);
    pos_style_add(label, POS_STYLE_BUTTON_LABEL, 0);
    lv_obj_center(label);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    return btn;
}

lv_obj_t *fleet_button_secondary(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                                 void *user)
{
    return button_with_role(parent, text, POS_STYLE_BUTTON_SECONDARY, 64, cb, user);
}

lv_obj_t *fleet_button_paired(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                              void *user)
{
    lv_obj_t *btn = button_with_role(parent, text, POS_STYLE_BUTTON_SECONDARY, PAIRED_H, cb,
                                     user);

    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_flex_grow(btn, 1);
    return btn;
}

void fleet_button_set_enabled(lv_obj_t *button, int enabled)
{
    if (!button) {
        return;
    }
    if (enabled) {
        lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
        pos_style_add(button, POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_style(button, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        pos_style_add(button, POS_STYLE_BUTTON_DISABLED, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_CLICKABLE);
    }
    /* The label carries only the button font, so it inherits the colour the
     * state above just set. */
    lv_obj_invalidate(button);
}

lv_obj_t *fleet_segments(lv_obj_t *parent, const char *const *labels, int count)
{
    lv_obj_t *bar = lv_obj_create(parent);
    int i;

    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), SEGMENT_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, SEGMENT_GAP, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < count; i++) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_t *label = lv_label_create(seg);

        lv_obj_remove_style_all(seg);
        pos_style_add(seg, POS_STYLE_SLAB, 0);
        /* The caption role sits on the segment, not the label, so the label
         * inherits the colour a selected segment sets over it. */
        pos_style_add(seg, POS_STYLE_CAPTION, 0);
        lv_obj_set_flex_grow(seg, 1);
        /* Local height beats the 64 px the button role would impose when the
         * segment is selected. */
        lv_obj_set_height(seg, SEGMENT_H);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(label, labels[i]);
        lv_obj_center(label);
    }
    return bar;
}

void fleet_segments_fit(lv_obj_t *bar, int32_t bar_w)
{
    uint32_t count = bar ? lv_obj_get_child_count(bar) : 0;
    int32_t widest = 0;
    int32_t seg_w;
    bool one_row;
    uint32_t i;

    if (!count || bar_w <= 0) {
        return;
    }
    /* Measured in the type it draws in: Fleet's, held at Small. */
    pos_style_hold_small(bar, NULL);
    for (i = 0; i < count; i++) {
        lv_obj_t *seg = lv_obj_get_child(bar, (int32_t)i);
        lv_obj_t *label = lv_obj_get_child(seg, 0);

        /* The caption role is on the segment; the label inherits it. */
        widest = LV_MAX(widest, pocketui_text_width(label, lv_label_get_text(label)));
    }
    seg_w = (bar_w - (int32_t)(count - 1) * SEGMENT_GAP) / (int32_t)count;
    one_row = widest <= seg_w + 2;
    lv_obj_set_flex_flow(bar, one_row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(bar, SEGMENT_GAP, 0);
    for (i = 0; i < count; i++) {
        /* Two to a row: each at least just under half, so the third wraps,
         * and the two then share the row's width. */
        lv_obj_set_style_min_width(lv_obj_get_child(bar, (int32_t)i), one_row ? 0 : LV_PCT(45), 0);
    }
    lv_obj_set_height(bar, one_row ? SEGMENT_H : (int32_t)((count + 1) / 2) * (SEGMENT_H + SEGMENT_GAP) - SEGMENT_GAP);
}

void fleet_segments_select(lv_obj_t *bar, int index)
{
    uint32_t count;
    uint32_t i;

    if (!bar) {
        return;
    }
    count = lv_obj_get_child_count(bar);
    for (i = 0; i < count; i++) {
        lv_obj_t *seg = lv_obj_get_child(bar, (int32_t)i);

        lv_obj_remove_style(seg, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        if ((int)i == index) {
            pos_style_add(seg, POS_STYLE_BUTTON_PRIMARY, 0);
            lv_obj_set_height(seg, SEGMENT_H);
        }
    }
}

lv_obj_t *fleet_row(lv_obj_t *parent, int height, int divider)
{
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    if (divider) {
        pos_style_add(row, POS_STYLE_DIVIDER, 0);
    }
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t *fleet_hbox(lv_obj_t *parent, int height, int gap)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), height);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, gap, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}
