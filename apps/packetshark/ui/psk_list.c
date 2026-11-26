/*
 * PacketShark's PACKETS tab (milestone M5; mockup 02, DS §59.3-§59.6, the
 * deviations in §59.11): the filter bar, the column heads, the list and its
 * rail, and the line under it.
 *
 * The list is a window, not a scroll (DS §59.4): a fixed pool of
 * PSK_LIST_ROWS rows of 56 px, filled from the rows the helper's last window
 * brought (at most PPC_WINDOW_MAX, DESIGN_DECISIONS D1). A drag moves the
 * position a row per 56 px; the rail beside it jumps to any place in the
 * view and carries a mark where the view has findings (M7). Where the list
 * is and which window to ask for next is psk_app.c's; this file only lays
 * out and paints.
 *
 * In the wide shape (M9, mockup 08) a row is one line of 40 px: time, word,
 * addresses, length and the sentence side by side, the gap left out.
 *
 * The rows are drawn in the caption role's Small face at every text size,
 * with its tracking removed (DS §59.3, the rule of §54.1): 8 px a character,
 * 63 in a portrait row. The controls above and the line below follow the
 * text size.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAIL_W 20
#define RAIL_SNAP 8
#define BAR_W 4
#define LINE1_Y 8
#define LINE2_Y 30
#define DRAG_SLOP 10

/* ---- small parts ---------------------------------------------------------------------- */

static lv_style_t *fixed_face(void)
{
    return pos_style_fixed_size(POS_STYLE_CAPTION);
}

/* The style a tone stands for, NULL for none. */
static lv_style_t *tone_style(int role)
{
    if (role >= 0) {
        return pos_style((enum pos_style_role)role);
    }
    if (role <= -10) {
        return pos_style_identity((unsigned)(-role - 10));
    }
    return NULL;
}

/* The roles that bring a font with their colour (the others are colour only). */
static bool brings_font(int role)
{
    return role == POS_STYLE_TEXT_PRIMARY || role == POS_STYLE_TEXT_SECONDARY || role == POS_STYLE_TEXT_MUTED;
}

void psk_tone(lv_obj_t *lb, int role, bool fixed)
{
    intptr_t had = lb ? (intptr_t)lv_obj_get_user_data(lb) : 0;
    intptr_t want = (intptr_t)role + 100;
    lv_style_t *old;
    lv_style_t *add = tone_style(role);

    if (!lb || had == want) {
        return;
    }
    old = had ? tone_style((int)(had - 100)) : NULL;
    lv_obj_set_user_data(lb, (void *)want);
    if (old) {
        lv_obj_remove_style(lb, old, 0);
    }
    if (add) {
        lv_obj_add_style(lb, add, 0);
    }
    if (fixed && brings_font(role)) {
        /* A role that brings its own font: the fixed face goes back on top
         * (rift_rxlog_view.c's rule). */
        lv_obj_remove_style(lb, fixed_face(), 0);
        lv_obj_add_style(lb, fixed_face(), 0);
        lv_obj_set_style_text_letter_space(lb, 0, 0);
    }
}

void psk_style_on(lv_obj_t *obj, enum pos_style_role role, bool on, bool *state)
{
    if (!obj || *state == on) {
        return;
    }
    *state = on;
    if (on) {
        pos_style_add(obj, role, 0);
    } else {
        lv_obj_remove_style(obj, pos_style(role), 0);
    }
}

/* The identity hue of a class as a tone (psk_tone's -10 - hue), or none
 * for OTHER: the caption's own secondary text. */
static int class_tone(unsigned cls)
{
    int hue = psk_class_hue(cls);

    return hue < 0 ? -1 : -10 - hue;
}

static void bar_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    int cls = (int)(intptr_t)lv_obj_get_user_data(obj) - 1;
    lv_draw_rect_dsc_t dsc;
    lv_area_t a;
    int hue = cls >= 0 && cls < PPC_CLASS_COUNT ? psk_class_hue((unsigned)cls) : -1;

    if (!layer || cls < -1) {
        return;
    }
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = hue < 0 ? pos_theme_color(POS_COLOR_TEXT_SECONDARY) : pos_identity_hue((unsigned)hue);
    lv_draw_rect(layer, &dsc, &a);
}

lv_obj_t *psk_class_bar(lv_obj_t *parent)
{
    lv_obj_t *b = psk_box(parent);

    lv_obj_set_width(b, BAR_W);
    lv_obj_set_user_data(b, (void *)(intptr_t)0);
    lv_obj_add_event_cb(b, bar_draw, LV_EVENT_DRAW_MAIN, NULL);
    pos_theme_watch(b);
    return b;
}

void psk_class_bar_set(lv_obj_t *bar, int cls)
{
    intptr_t want = (intptr_t)cls + 1;

    if (bar && (intptr_t)lv_obj_get_user_data(bar) != want) {
        lv_obj_set_user_data(bar, (void *)want);
        lv_obj_invalidate(bar);
    }
}

/* A cell in the fixed Small face, in role's colour (POS_STYLE_CAPTION: the
 * caption's own secondary text). */
static lv_obj_t *cell_label(lv_obj_t *parent, enum pos_style_role role, lv_text_align_t align)
{
    lv_obj_t *lb = pocketui_label(parent, "", POS_STYLE_CAPTION);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lb, align, 0);
    lv_obj_add_flag(lb, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(lb, fixed_face(), 0);
    lv_obj_set_style_text_letter_space(lb, 0, 0);
    if (role != POS_STYLE_CAPTION) {
        psk_tone(lb, (int)role, true);
    }
    return lb;
}

static lv_obj_t *flex_row(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

/* ---- events --------------------------------------------------------------------------- */

static void on_filter(lv_event_t *e)
{
    psk_fsheet_show(lv_event_get_user_data(e), false);
}

static void on_sort(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    psk_set_sort(a, (a->sort + 1u) % 3u);
}

static void on_follow(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    psk_set_follow(a, !a->follow);
}

static int32_t pointer_y(void)
{
    lv_indev_t *in = lv_indev_active();
    lv_point_t pt = { 0, 0 };

    if (in) {
        lv_indev_get_point(in, &pt);
    }
    return pt.y;
}

/* A drag on the list moves it a row per PSK_ROW_H; a tap opens the row. */
static void on_list(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    int32_t y = pointer_y();

    if (code == LV_EVENT_PRESSED) {
        a->dragging = true;
        a->dragged = false;
        a->drag_y0 = y;
        a->drag_rank0 = a->top_rank;
    } else if (code == LV_EVENT_PRESSING && a->dragging) {
        int32_t dy = y - a->drag_y0;
        int64_t rank;

        if (!a->dragged && (dy > DRAG_SLOP || dy < -DRAG_SLOP)) {
            a->dragged = true;
        }
        if (a->dragged) {
            /* Finger down: earlier rows come into view. */
            rank = (int64_t)a->drag_rank0 - dy / a->row_h;
            psk_list_scroll_to(a, rank < 0 ? 0 : (uint32_t)rank, false);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (a->dragging && a->dragged) {
            psk_list_scroll_to(a, a->top_rank, true);
        }
        a->dragging = false;
    } else if (code == LV_EVENT_CLICKED && !a->dragged) {
        lv_obj_t *t = lv_event_get_target_obj(e);
        int i;

        while (t && t != a->pk_list && lv_obj_get_parent(t) != a->pk_list) {
            t = lv_obj_get_parent(t);
        }
        for (i = 0; i < PSK_LIST_ROWS; i++) {
            if (a->lrow[i].row == t && a->lrow[i].seq) {
                psk_open_packet(a, a->lrow[i].seq, 0);
                return;
            }
        }
    }
}

/* The rail: a tap or a drag puts that place of the view at the top - its
 * top end the oldest packets, its foot the newest. */
static void on_rail(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    const struct psk_model *m = a->model;
    uint32_t match = m->rows_match;
    uint32_t last = match > (uint32_t)a->vis ? match - (uint32_t)a->vis : 0;
    lv_area_t r;
    int32_t h;
    int32_t y;

    if (match == 0 || (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)) {
        return;
    }
    /* RAIL_SNAP px at each end reach the end itself. */
    lv_obj_get_coords(a->pk_rail, &r);
    h = lv_area_get_height(&r) - 1 - 2 * RAIL_SNAP;
    y = pointer_y() - r.y1 - RAIL_SNAP;
    if (y < 0) {
        y = 0;
    }
    if (y > h) {
        y = h;
    }
    psk_list_scroll_to(a, h > 0 ? (uint32_t)((int64_t)y * last / h) : 0, code == LV_EVENT_RELEASED);
}

/* ---- the rail's drawing --------------------------------------------------------------- */

struct rail_paint {
    uint32_t top;
    uint32_t shown;
    uint32_t match;
    uint32_t buckets; /* the view in this many parts; 0: no marks */
    uint64_t marks;   /* bit i: a finding in part i */
};

static void rail_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    const struct rail_paint *rp = lv_obj_get_user_data(obj);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t dsc;
    lv_area_t c;
    lv_area_t a;
    int32_t h;

    if (!rp || !layer) {
        return;
    }
    lv_obj_get_coords(obj, &c);
    h = lv_area_get_height(&c);
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    /* The track: 4 px, in the raised surface (mockup 02). */
    a.x1 = c.x1 + 12;
    a.x2 = a.x1 + 3;
    a.y1 = c.y1;
    a.y2 = c.y2;
    dsc.radius = 0;
    dsc.bg_color = pos_theme_color(POS_COLOR_SURFACE_RAISED);
    lv_draw_rect(layer, &dsc, &a);
    /* The finding marks (mockup 02): 6 x 2 px in status_warn at the track's
     * left, one for each part of the view that holds a finding (M7). */
    if (rp->buckets > 0 && rp->marks) {
        uint32_t i;

        dsc.bg_color = pos_theme_color(POS_COLOR_STATUS_WARN);
        for (i = 0; i < rp->buckets && i < 64u; i++) {
            if (rp->marks & (1ull << i)) {
                a.x1 = c.x1 + 3;
                a.x2 = a.x1 + 5;
                a.y1 = c.y1 + (int32_t)(((int64_t)h * (2 * i + 1)) / (2 * rp->buckets)) - 1;
                a.y2 = a.y1 + 1;
                lv_draw_rect(layer, &dsc, &a);
            }
        }
        dsc.bg_color = pos_theme_color(POS_COLOR_SURFACE_RAISED);
    }
    if (rp->match == 0 || rp->shown == 0) {
        return;
    }
    /* The thumb: where the rows shown are, at least 36 px. */
    {
        int32_t th = (int32_t)((int64_t)h * rp->shown / rp->match);
        int32_t ty;

        if (th < 36) {
            th = 36;
        }
        if (th > h) {
            th = h;
        }
        ty = (int32_t)((int64_t)(h - th) * rp->top / (rp->match > rp->shown ? rp->match - rp->shown : 1));
        if (rp->match <= rp->shown) {
            ty = 0;
        }
        if (ty > h - th) {
            ty = h - th;
        }
        a.x1 = c.x1 + 10;
        a.x2 = a.x1 + 7;
        a.y1 = c.y1 + ty;
        a.y2 = a.y1 + th - 1;
        dsc.radius = 3;
        dsc.bg_color = pos_theme_color(POS_COLOR_TEXT_SECONDARY);
        lv_draw_rect(layer, &dsc, &a);
    }
}

static void rail_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);

    free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

/* ---- building ------------------------------------------------------------------------- */

static lv_obj_t *tool_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = psk_button(parent, text, false, cb, user);

    lv_obj_set_height(b, PSK_TAB_H);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, PSK_TAB_H, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    return b;
}

static void build_row(struct psk_app *a, struct psk_listrow *r)
{
    r->row = psk_box(a->pk_list);
    lv_obj_set_size(r->row, LV_PCT(100), PSK_ROW_H);
    lv_obj_set_style_radius(r->row, POCKETUI_RADIUS, 0);
    pos_style_add(r->row, POS_STYLE_DIVIDER, 0);
    pos_style_add(r->row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    /* PRESS_LOCK: a drag that leaves the row is still the row's press. */
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_PRESS_LOCK);
    /* No scroll chain: a drag on the list moves the list, never a
     * scrollable parent (the shell's body scrolls). */
    lv_obj_clear_flag(r->row, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(r->row, LV_SCROLLBAR_MODE_OFF);
    r->bar = psk_class_bar(r->row);
    lv_obj_set_height(r->bar, PSK_ROW_H - 13);
    lv_obj_set_pos(r->bar, 0, 6);
    lv_obj_add_flag(r->bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    r->time = cell_label(r->row, POS_STYLE_CAPTION, LV_TEXT_ALIGN_LEFT);
    r->dir = cell_label(r->row, POS_STYLE_CAPTION, LV_TEXT_ALIGN_LEFT);
    r->word = cell_label(r->row, POS_STYLE_CAPTION, LV_TEXT_ALIGN_LEFT);
    r->addr = cell_label(r->row, POS_STYLE_TEXT_PRIMARY, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(r->addr, LV_LABEL_LONG_DOT);
    r->len = cell_label(r->row, POS_STYLE_CAPTION, LV_TEXT_ALIGN_RIGHT);
    r->gap = cell_label(r->row, POS_STYLE_TEXT_MUTED, LV_TEXT_ALIGN_LEFT);
    r->info = cell_label(r->row, POS_STYLE_CAPTION, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(r->info, LV_LABEL_LONG_DOT);
    r->pin = cell_label(r->row, POS_STYLE_ACCENT_TEXT, LV_TEXT_ALIGN_RIGHT);
    psk_text(r->pin, "PIN");
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    r->seq = 0;
}

void psk_list_build(struct psk_app *a, lv_obj_t *page)
{
    lv_obj_t *sum;
    lv_obj_t *head;
    lv_obj_t *lb;
    struct rail_paint *rp;
    int i;

    /* The page holds the list in place: no scrolling of its own, and the
     * spacing of mockup 02 (filter bar at 164, rows from 256, the line under
     * them 10 px below the last). */
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(page, 0, 0);
    lv_obj_set_style_pad_top(page, 12, 0);
    lv_obj_set_style_pad_bottom(page, 0, 0);

    /* The filter bar (mockup 02: FILTER, the filter in words, TIME, FOLLOW). */
    a->pk_tools = flex_row(page, PSK_GAP);
    lv_obj_set_style_min_height(a->pk_tools, PSK_TAB_H, 0);
    lv_obj_set_style_margin_bottom(a->pk_tools, 10, 0);
    a->pk_filter = tool_button(a->pk_tools, "FILTER", on_filter, a);
    lv_obj_set_style_min_width(a->pk_filter, 118, 0);
    sum = psk_box(a->pk_tools);
    lv_obj_set_height(sum, LV_SIZE_CONTENT);
    lv_obj_set_width(sum, 1);
    lv_obj_set_flex_grow(sum, 1);
    lv_obj_set_flex_flow(sum, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(sum, 4, 0);
    lv_obj_set_style_pad_row(sum, 2, 0);
    a->pk_sum1 = pocketui_label(sum, "ALL PROTOCOLS", POS_STYLE_CAPTION);
    lv_label_set_long_mode(a->pk_sum1, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a->pk_sum1, LV_PCT(100));
    a->pk_sum2 = pocketui_label(sum, "", POS_STYLE_CAPTION);
    lv_label_set_long_mode(a->pk_sum2, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a->pk_sum2, LV_PCT(100));
    a->pk_sort = tool_button(a->pk_tools, "TIME \xe2\x86\x93", on_sort, a);
    lv_obj_set_style_min_width(a->pk_sort, 104, 0);
    a->pk_follow = tool_button(a->pk_tools, "FOLLOW", on_follow, a);
    lv_obj_set_style_min_width(a->pk_follow, 102, 0);

    /* The empty state: no capture holds packets. */
    a->pk_empty = pocketui_card(page);
    lv_obj_set_width(a->pk_empty, LV_PCT(100));
    lv_obj_set_height(a->pk_empty, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(a->pk_empty, 8, 0);
    lv_obj_clear_flag(a->pk_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_top(a->pk_empty, 2, 0);
    a->pk_title = pocketui_label(a->pk_empty, "NO PACKETS YET", POS_STYLE_ROW_TITLE);
    lv_label_set_long_mode(a->pk_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->pk_title, LV_PCT(100));
    a->pk_text = pocketui_label(a->pk_empty, "Start a capture on LIVE.", POS_STYLE_TEXT_SECONDARY);
    lv_label_set_long_mode(a->pk_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->pk_text, LV_PCT(100));

    /* The column heads, in the rows' face (mockup 02). */
    head = psk_box(page);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, 26);
    pos_style_add(head, POS_STYLE_DIVIDER, 0);
    a->pk_head = head;
    lb = cell_label(head, POS_STYLE_TEXT_MUTED, LV_TEXT_ALIGN_LEFT);
    psk_text(lb, "TIME / GAP");
    lb = cell_label(head, POS_STYLE_TEXT_MUTED, LV_TEXT_ALIGN_LEFT);
    psk_text(lb, "PROTO");
    lb = cell_label(head, POS_STYLE_TEXT_MUTED, LV_TEXT_ALIGN_LEFT);
    psk_text(lb, "SOURCE \xe2\x86\x92 DESTINATION");
    lb = cell_label(head, POS_STYLE_TEXT_MUTED, LV_TEXT_ALIGN_RIGHT);
    psk_text(lb, "LEN");
    lb = cell_label(head, POS_STYLE_TEXT_MUTED, LV_TEXT_ALIGN_LEFT);
    psk_text(lb, "INFO"); /* the wide shape's fifth column */
    lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);

    /* The list and its rail. */
    a->pk_body = psk_box(page);
    lv_obj_set_width(a->pk_body, LV_PCT(100));
    lv_obj_set_height(a->pk_body, 1);
    lv_obj_set_flex_grow(a->pk_body, 1);
    lv_obj_set_flex_flow(a->pk_body, LV_FLEX_FLOW_ROW);
    a->pk_list = psk_box(a->pk_body);
    lv_obj_set_height(a->pk_list, LV_PCT(100));
    lv_obj_set_width(a->pk_list, 1);
    lv_obj_set_flex_grow(a->pk_list, 1);
    lv_obj_set_flex_flow(a->pk_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(a->pk_list, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(a->pk_list, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(a->pk_list, on_list, LV_EVENT_ALL, a);
    for (i = 0; i < PSK_LIST_ROWS; i++) {
        build_row(a, &a->lrow[i]);
    }
    a->pk_none = pocketui_label(a->pk_list, "NO PACKET MATCHES THIS FILTER", POS_STYLE_CAPTION);
    lv_obj_set_style_pad_top(a->pk_none, 24, 0);
    lv_obj_add_flag(a->pk_none, LV_OBJ_FLAG_HIDDEN);
    a->pk_rail = psk_box(a->pk_body);
    lv_obj_set_size(a->pk_rail, RAIL_W, LV_PCT(100));
    lv_obj_add_flag(a->pk_rail, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(a->pk_rail, LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_ext_click_area(a->pk_rail, 12);
    rp = calloc(1, sizeof(*rp));
    lv_obj_set_user_data(a->pk_rail, rp);
    lv_obj_add_event_cb(a->pk_rail, rail_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(a->pk_rail, rail_delete, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(a->pk_rail, on_rail, LV_EVENT_ALL, a);
    pos_theme_watch(a->pk_rail);

    /* The line under the list. */
    a->pk_foot = flex_row(page, 12);
    lv_obj_set_style_margin_top(a->pk_foot, 10, 0);
    lv_obj_set_flex_align(a->pk_foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    a->pk_foot_l = pocketui_label(a->pk_foot, "", POS_STYLE_CAPTION);
    lv_label_set_long_mode(a->pk_foot_l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(a->pk_foot_l, 1);
    lv_obj_set_flex_grow(a->pk_foot_l, 1);
    a->pk_foot_r = pocketui_label(a->pk_foot, "", POS_STYLE_CAPTION);
    a->vis = 1;
}

/* ---- fitting -------------------------------------------------------------------------- */

/* A cell on one line of the fixed face: a label in the dots mode with an
 * automatic height wraps instead of shortening (DS §46.5), so its height
 * is held at one line. */
static void place(lv_obj_t *lb, int32_t x, int32_t y, int32_t w)
{
    const lv_font_t *f = lv_obj_get_style_text_font(lb, LV_PART_MAIN);

    lv_obj_set_pos(lb, x, y);
    lv_obj_set_width(lb, w > 0 ? w : 1);
    if (f) {
        lv_obj_set_height(lb, lv_font_get_line_height(f));
    }
}

/* The sentence's width in a row: what is left of the row, less the PIN mark
 * when the row carries one. */
static int32_t info_x(const struct psk_app *a)
{
    int32_t cw = a->char_w;

    /* Tall: under the word. Wide: after the length (time 12, direction 2,
     * word 7, addresses 30, length 6, a character between each). */
    return a->wide ? 14 + (13 + 2 + 7 + 31 + 7) * cw : 14 + 15 * cw;
}

static int32_t info_w(const struct psk_app *a, int32_t width, bool pinned)
{
    int32_t w = width - 8 - info_x(a) - (pinned ? 4 * a->char_w : 0);

    return w > 1 ? w : 1;
}

/* The wide shape's columns (mockup 08): one line. */
static void place_columns_wide(struct psk_app *a, int32_t width)
{
    int32_t cw = a->char_w;
    int32_t lh = lv_font_get_line_height(lv_obj_get_style_text_font(a->lrow[0].time, LV_PART_MAIN));
    int32_t y = (a->row_h - lh) / 2;
    int32_t x_time = 14;
    int32_t x_dir = x_time + 13 * cw;
    int32_t x_word = x_dir + 2 * cw;
    int32_t x_addr = x_word + 7 * cw;
    int32_t x_len = x_addr + 31 * cw;
    int32_t len_w = 6 * cw;
    int32_t pin_w = 3 * cw;
    lv_obj_t *h = a->pk_head;
    int i;

    for (i = 0; i < PSK_LIST_ROWS; i++) {
        struct psk_listrow *r = &a->lrow[i];

        place(r->time, x_time, y, 12 * cw);
        place(r->dir, x_dir, y, 2 * cw);
        place(r->word, x_word, y, 7 * cw);
        place(r->addr, x_addr, y, 30 * cw);
        place(r->len, x_len, y, len_w);
        place(r->info, info_x(a), y, info_w(a, width, !lv_obj_has_flag(r->pin, LV_OBJ_FLAG_HIDDEN)));
        place(r->pin, width - 8 - pin_w, y, pin_w);
    }
    place(lv_obj_get_child(h, 0), x_time, 2, 12 * cw);
    place(lv_obj_get_child(h, 1), x_word, 2, 6 * cw);
    place(lv_obj_get_child(h, 2), x_addr, 2, 30 * cw);
    place(lv_obj_get_child(h, 3), x_len, 2, len_w);
    place(lv_obj_get_child(h, 4), info_x(a), 2, info_w(a, width, false));
}

/* Columns in characters of the fixed face (mockup 02: time 12 at 14 px in,
 * the direction at character 13, the word at 15, the addresses at 22; the
 * length right-aligned 8 px from the edge). */
static void place_columns(struct psk_app *a, int32_t width)
{
    int32_t cw = a->char_w;
    int32_t x_time = 14;
    int32_t x_dir = x_time + 13 * cw;
    int32_t x_word = x_dir + 2 * cw;
    int32_t x_addr = x_word + 7 * cw;
    int32_t len_w = 6 * cw;
    int32_t x_len = width - 8 - len_w;
    int32_t pin_w = 3 * cw;
    int i;

    if (a->wide) {
        place_columns_wide(a, width);
        return;
    }
    for (i = 0; i < PSK_LIST_ROWS; i++) {
        struct psk_listrow *r = &a->lrow[i];

        place(r->time, x_time, LINE1_Y, 12 * cw);
        place(r->dir, x_dir, LINE1_Y, 2 * cw);
        place(r->word, x_word, LINE1_Y, 7 * cw);
        place(r->addr, x_addr, LINE1_Y, x_len - x_addr - cw);
        place(r->len, x_len, LINE1_Y, len_w);
        place(r->gap, x_time, LINE2_Y, 12 * cw);
        place(r->info, x_word, LINE2_Y, info_w(a, width, !lv_obj_has_flag(r->pin, LV_OBJ_FLAG_HIDDEN)));
        place(r->pin, width - 8 - pin_w, LINE2_Y, pin_w);
    }
    {
        lv_obj_t *h = a->pk_head;

        place(lv_obj_get_child(h, 0), x_time, 2, 12 * cw);
        place(lv_obj_get_child(h, 1), x_word, 2, 6 * cw);
        place(lv_obj_get_child(h, 2), x_addr, 2, x_len - x_addr);
        place(lv_obj_get_child(h, 3), x_len, 2, len_w);
    }
}

/* The filter in words beside the buttons where it fits (Small, mockup 02),
 * on a line of its own under them where it does not (Medium and Large):
 * decided from the widest expected words, so the shape does not change as
 * the count grows, the way LIVE's figures are fitted. */
#define FIT_SUM1 "ALL PROTOCOLS"
#define FIT_SUM2 "8 888 888 PACKETS"

static void fit_summary(struct psk_app *a)
{
    lv_obj_t *sum = lv_obj_get_parent(a->pk_sum1);
    int32_t gap = lv_obj_get_style_pad_column(a->pk_tools, LV_PART_MAIN);
    int32_t room = lv_obj_get_content_width(a->pk_tools) - lv_obj_get_width(a->pk_filter) -
                   lv_obj_get_width(a->pk_sort) - lv_obj_get_width(a->pk_follow) - 3 * gap -
                   lv_obj_get_style_pad_left(sum, LV_PART_MAIN);
    int32_t need = pocketui_text_width(a->pk_sum1, FIT_SUM1);
    bool below;

    if (pocketui_text_width(a->pk_sum2, FIT_SUM2) > need) {
        need = pocketui_text_width(a->pk_sum2, FIT_SUM2);
    }
    if (lv_obj_get_width(a->pk_filter) <= 0) {
        return; /* not laid out yet */
    }
    below = need > room;
    if (below == a->sum_below) {
        return;
    }
    a->sum_below = below;
    lv_obj_set_flex_flow(a->pk_tools, below ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_row(a->pk_tools, below ? 6 : 0, 0);
    if (below) {
        /* The buttons keep the row; the words go under them, full width. */
        lv_obj_move_to_index(sum, -1);
        lv_obj_set_flex_grow(sum, 0);
        lv_obj_set_width(sum, LV_PCT(100));
        lv_obj_set_style_pad_left(sum, 0, 0);
        lv_obj_set_flex_grow(a->pk_sort, 1);
        lv_obj_set_flex_align(a->pk_tools, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_move_to_index(sum, 1);
        lv_obj_set_width(sum, 1);
        lv_obj_set_flex_grow(sum, 1);
        lv_obj_set_style_pad_left(sum, 4, 0);
        lv_obj_set_flex_grow(a->pk_sort, 0);
    }
    lv_obj_update_layout(a->frame);
}

/* The rows for the shape in force: two lines of 56 px, or one of 40 with
 * the gap left out and the sentence as a fifth column (M9). The columns
 * are placed again by the next psk_list_fit(). */
void psk_list_shape(struct psk_app *a)
{
    int i;

    a->row_h = a->wide ? PSK_ROW_H_WIDE : PSK_ROW_H;
    for (i = 0; i < PSK_LIST_ROWS; i++) {
        struct psk_listrow *r = &a->lrow[i];

        lv_obj_set_height(r->row, a->row_h);
        lv_obj_set_height(r->bar, a->row_h - 13);
        psk_set_hidden(r->gap, a->wide);
    }
    psk_text(lv_obj_get_child(a->pk_head, 0), a->wide ? "TIME" : "TIME / GAP");
    psk_set_hidden(lv_obj_get_child(a->pk_head, 4), !a->wide);
    a->char_w = 0;
}

void psk_list_fit(struct psk_app *a)
{
    int32_t h;
    int32_t w;
    int32_t cw;
    int vis;

    if (lv_obj_has_flag(a->page[PSK_TAB_PACKETS], LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_get_content_height(a->pk_list) <= 0) {
        lv_obj_update_layout(a->frame);
    }
    fit_summary(a);
    h = lv_obj_get_content_height(a->pk_list);
    w = lv_obj_get_content_width(a->pk_list);
    cw = pocketui_text_width(a->lrow[0].time, "0");
    if (h <= 0 || w <= 0) {
        return;
    }
    vis = h / a->row_h;
    if (vis < 1) {
        vis = 1;
    }
    if (vis > PSK_LIST_ROWS) {
        vis = PSK_LIST_ROWS;
    }
    if (cw > 0 && (cw != a->char_w || w != lv_obj_get_width(a->lrow[0].row))) {
        a->char_w = cw;
        place_columns(a, w);
    }
    if (vis != a->vis) {
        a->vis = vis;
        a->window_due = true;
        psk_list_paint(a);
    }
}

/* ---- painting ------------------------------------------------------------------------- */

static void paint_row(struct psk_app *a, struct psk_listrow *r, const struct ppc_evt_row *row)
{
    char b[PPC_ADDR_TEXT * 2 + 8];
    char t[24];
    bool pinned = (row->flags & PPC_ROWF_PINNED) != 0;
    bool hidden_pin = lv_obj_has_flag(r->pin, LV_OBJ_FLAG_HIDDEN);

    r->seq = row->seq;
    psk_class_bar_set(r->bar, row->cls);
    psk_text(r->time, psk_fmt_rel(row->rel_us, t, sizeof(t)));
    psk_text(r->gap, psk_fmt_gap(row->gap_us, t, sizeof(t)));
    psk_text(r->dir, psk_dir_glyph(row->dir));
    psk_text(r->word, row->word);
    psk_tone(r->word, class_tone(row->cls), true);
    snprintf(b, sizeof(b), "%s \xe2\x86\x92 %s", row->src, row->dst);
    psk_text(r->addr, b);
    snprintf(t, sizeof(t), "%u", (unsigned)row->len);
    psk_text(r->len, t);
    if (row->signal) {
        char w[PPC_INFO_TEXT + 4];

        snprintf(w, sizeof(w), "! %s", row->info);
        psk_text(r->info, w);
        psk_tone(r->info, POS_STYLE_STATUS_WARN_TEXT, true);
    } else {
        psk_text(r->info, row->info);
        psk_tone(r->info, -1, true);
    }
    psk_set_hidden(r->pin, !pinned);
    if (hidden_pin == pinned && a->char_w > 0) {
        /* The sentence gives the PIN mark its room, or takes it back. */
        lv_obj_set_width(r->info, info_w(a, lv_obj_get_content_width(a->pk_list), pinned));
    }
    psk_style_on(r->row, POS_STYLE_SELECTED, row->seq == a->sel_seq, &r->selected);
    lv_obj_set_x(r->bar, r->selected ? 3 : 0);
    psk_set_hidden(r->row, false);
}

static const char *sort_word(unsigned sort)
{
    switch (sort) {
    case 1:
        return "TIME \xe2\x86\x91"; /* the newest first: time grows upwards */
    case 2:
        return "LEN \xe2\x86\x91";  /* the largest first */
    default:
        return "TIME \xe2\x86\x93"; /* the oldest first: time grows downwards */
    }
}

/* One line of the caption role at the text size in force (DS §46.5: a
 * dots-mode label with an automatic height wraps). */
static void caption_line(lv_obj_t *lb)
{
    int32_t h = pocketui_role_line_height(POS_STYLE_CAPTION);

    if (lv_obj_get_style_height(lb, LV_PART_MAIN) != h) {
        lv_obj_set_height(lb, h);
    }
}

static void paint_tools(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    char words[160];
    char n[32];
    char t[96];
    uint64_t held = m->c.last_seq >= m->c.first_seq && m->c.last_seq ? (uint64_t)m->c.last_seq - m->c.first_seq + 1u : 0;
    uint32_t match = m->rows_filter_id == a->view_id ? m->rows_match : m->c.match;
    bool filtered = a->classes != PPC_CLASSES_ALL || a->host[0] || a->port || a->pinned_only || a->sigmask || a->diagf;

    psk_text(a->pk_sum1,
             psk_filter_words(a->classes, a->host, a->port, a->pinned_only, a->sigmask, a->diagf, words, sizeof(words)));
    if (filtered) {
        char all[32];

        snprintf(t, sizeof(t), "%s OF %s PACKETS", psk_fmt_count(match, n, sizeof(n)), psk_fmt_count(held, all, sizeof(all)));
    } else {
        snprintf(t, sizeof(t), "%s PACKETS", psk_fmt_count(match, n, sizeof(n)));
    }
    psk_text(a->pk_sum2, t);
    caption_line(a->pk_sum1);
    caption_line(a->pk_sum2);
    psk_text(lv_obj_get_child(a->pk_sort, 0), sort_word(a->sort));
    /* FOLLOW on: its word in the accent, as the active tab's (an outline
     * would reach past the page's edge). */
    psk_tone(lv_obj_get_child(a->pk_follow, 0), a->follow ? POS_STYLE_ACCENT_TEXT : -1, false);
    /* FOLLOW keeps to the newest packets: the length order has no newest. */
    psk_button_look(a->pk_follow, false, a->sort != 2);
}

void psk_list_paint(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    bool present = psk_capture_present(a);
    bool rows_ok = present && m->rows_filter_id == a->view_id;
    uint32_t shown = 0;
    char n[32];
    char t[96];
    struct rail_paint *rp = lv_obj_get_user_data(a->pk_rail);
    int i;

    psk_set_hidden(a->pk_empty, present);
    psk_set_hidden(a->pk_tools, !present);
    psk_set_hidden(a->pk_head, !present);
    psk_set_hidden(a->pk_body, !present);
    psk_set_hidden(a->pk_foot, !present);
    if (!present) {
        psk_text(a->pk_title, "NO PACKETS YET");
        psk_text(a->pk_text, a->failed_view ? "The packets went with the capture helper. Nothing was written to storage."
                                            : "Start a capture on LIVE.");
        for (i = 0; i < PSK_LIST_ROWS; i++) {
            a->lrow[i].seq = 0;
        }
        return;
    }
    paint_tools(a);
    for (i = 0; i < PSK_LIST_ROWS; i++) {
        struct psk_listrow *r = &a->lrow[i];
        int64_t k = (int64_t)a->top_rank + i - (int64_t)m->rows_first_rank;

        if (i >= a->vis || !rows_ok || k < 0 || k >= (int64_t)m->row_count) {
            r->seq = 0;
            psk_set_hidden(r->row, true);
            continue;
        }
        paint_row(a, r, &m->rows[k]);
        shown++;
    }
    psk_set_hidden(a->pk_none, !(rows_ok && m->rows_match == 0));
    if (rp) {
        uint32_t match = rows_ok ? m->rows_match : 0;
        /* The marks of this view, and no other's; none where the view
         * itself shows only findings. */
        bool marked = rows_ok && !a->sigmask && m->marks_view == a->view_id && m->marks_buckets > 0;
        uint32_t buckets = marked ? m->marks_buckets : 0;
        uint64_t marks = marked ? m->marks_mask : 0;

        if (rp->top != a->top_rank || rp->shown != shown || rp->match != match || rp->buckets != buckets ||
            rp->marks != marks) {
            rp->top = a->top_rank;
            rp->shown = shown;
            rp->match = match;
            rp->buckets = buckets;
            rp->marks = marks;
            lv_obj_invalidate(a->pk_rail);
        }
    }
    /* The line under the list: where the view is held, what is in memory. */
    if (m->state == PPC_STATE_STOPPED) {
        snprintf(t, sizeof(t), "CAPTURE STOPPED");
    } else if (a->follow) {
        snprintf(t, sizeof(t), "FOLLOWING THE NEWEST");
    } else {
        uint32_t held_at = 0;

        for (i = a->vis - 1; i >= 0; i--) {
            if (a->lrow[i].seq) {
                held_at = a->lrow[i].seq;
                break;
            }
        }
        snprintf(t, sizeof(t), "VIEW HELD AT #%s", psk_fmt_count(held_at, n, sizeof(n)));
    }
    psk_text(a->pk_foot_l, t);
    caption_line(a->pk_foot_l);
    if (m->c.last_seq) {
        snprintf(t, sizeof(t), "BUFFER FROM #%s", psk_fmt_count(m->c.first_seq ? m->c.first_seq : 1, n, sizeof(n)));
    } else {
        t[0] = '\0';
    }
    psk_text(a->pk_foot_r, t);
}
