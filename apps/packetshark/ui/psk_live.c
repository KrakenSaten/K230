/*
 * PacketShark's LIVE tab (setup, live status, the error panel), the tab row,
 * the capture bar and the PACKETS and INSPECT pages, M3 scope
 * (docs/apps/PACKETSHARK.md; the design is DS §59, accepted, with its
 * deviations in §59.11).
 *
 * Only existing roles: panels, chips, key/value rows, buttons, captions and
 * values (tests/style_lint.sh). A label is written only when its text
 * changes, so a poll that brings nothing new costs nothing on the panel.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"
#include "psk_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small helpers -------------------------------------------------------------------- */

void psk_set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj && hidden != lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void psk_text(lv_obj_t *lb, const char *text)
{
    if (lb && strcmp(lv_label_get_text(lb), text) != 0) {
        lv_label_set_text(lb, text);
    }
}

lv_obj_t *psk_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *column(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *row(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

static lv_obj_t *one_line(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    return lb;
}

static lv_obj_t *wrapping(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

static lv_obj_t *grow(lv_obj_t *obj)
{
    lv_obj_set_width(obj, 1);
    lv_obj_set_flex_grow(obj, 1);
    return obj;
}

/* A colour-only role over a label's own role; only a change is applied. The
 * role in force is kept in the user data, offset by one so 0 is "none". */
static void tone(lv_obj_t *lb, int role)
{
    static const enum pos_style_role tones[] = { POS_STYLE_STATUS_OK_TEXT, POS_STYLE_STATUS_WARN_TEXT,
                                                 POS_STYLE_STATUS_ERROR_TEXT, POS_STYLE_ACCENT_TEXT };
    intptr_t want = (intptr_t)role + 1;
    unsigned i;

    if (!lb || (intptr_t)lv_obj_get_user_data(lb) == want) {
        return;
    }
    lv_obj_set_user_data(lb, (void *)want);
    for (i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
        lv_obj_remove_style(lb, pos_style(tones[i]), 0);
    }
    if (role >= 0) {
        pos_style_add(lb, (enum pos_style_role)role, 0);
    }
    lv_obj_invalidate(lb);
}

#define TONE_NONE (-1)

/* A chip (DS §9): 36 px, a word, in one of the chip looks. */
enum chip_look { CHIP_ACTIVE, CHIP_OFF, CHIP_WARN, CHIP_ERROR, CHIP_NA };

static lv_obj_t *chip(lv_obj_t *parent)
{
    lv_obj_t *c = pocketui_label(parent, "", POS_STYLE_CHIP);

    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    return c;
}

static void chip_set(lv_obj_t *c, enum chip_look look, const char *word)
{
    intptr_t want = (intptr_t)look + 1;

    psk_text(c, word);
    if ((intptr_t)lv_obj_get_user_data(c) == want) {
        return;
    }
    lv_obj_set_user_data(c, (void *)want);
    lv_obj_remove_style(c, pos_style(POS_STYLE_CHIP_ACTIVE), 0);
    lv_obj_remove_style(c, pos_style(POS_STYLE_CHIP_OFF), 0);
    lv_obj_remove_style(c, pos_style(POS_STYLE_CHIP_NA), 0);
    lv_obj_remove_style(c, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    lv_obj_remove_style(c, pos_style(POS_STYLE_STATUS_ERROR_TEXT), 0);
    switch (look) {
    case CHIP_ACTIVE:
        pos_style_add(c, POS_STYLE_CHIP_ACTIVE, 0);
        break;
    case CHIP_WARN:
        pos_style_add(c, POS_STYLE_CHIP_OFF, 0);
        pos_style_add(c, POS_STYLE_STATUS_WARN_TEXT, 0);
        break;
    case CHIP_ERROR:
        pos_style_add(c, POS_STYLE_CHIP_OFF, 0);
        pos_style_add(c, POS_STYLE_STATUS_ERROR_TEXT, 0);
        break;
    case CHIP_NA:
        pos_style_add(c, POS_STYLE_CHIP_NA, 0);
        break;
    case CHIP_OFF:
    default:
        pos_style_add(c, POS_STYLE_CHIP_OFF, 0);
        break;
    }
    lv_obj_invalidate(c);
}

lv_obj_t *psk_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = pocketui_button(parent, text, cb, user);

    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_label_set_long_mode(lv_obj_get_child(b, 0), LV_LABEL_LONG_CLIP);
    lv_obj_set_user_data(b, (void *)(intptr_t)(primary ? 2 : 1));
    if (!primary) {
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    }
    return b;
}

/* Primary, secondary or disabled (DS §9: at most one primary on a screen,
 * the action that moves the session forward). The look in force is kept in
 * the button's user data: 1 secondary, 2 primary, 3 disabled. */
void psk_button_look(lv_obj_t *b, bool primary, bool enabled)
{
    intptr_t want = !enabled ? 3 : primary ? 2 : 1;

    if ((intptr_t)lv_obj_get_user_data(b) == want) {
        return;
    }
    lv_obj_set_user_data(b, (void *)want);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
    if (want == 2) {
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY, 0);
        pos_style_add(b, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
    } else if (want == 1) {
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    } else {
        pos_style_add(b, POS_STYLE_BUTTON_DISABLED, 0);
    }
    if (enabled) {
        lv_obj_clear_state(b, LV_STATE_DISABLED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_state(b, LV_STATE_DISABLED);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_invalidate(b);
}

/* A panel with its title (UI_UX §5: a caption at the top of a hairline panel). */
#define PANEL_PAD_TOP 16
#define TITLE_PAD 6
#define TITLE_INSET 16 /* from the panel's left edge to the title's own box */

/* How far a panel's title reaches above it, at the current text size. In
 * this LVGL, OVERFLOW_VISIBLE lets children draw only as far as the
 * object's extended draw size (lv_refr.c and lv_obj_pos.c at 59dc7e4), so
 * the panel and whatever holds it report this reach. */
static void title_reach(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, pocketui_role_line_height(POS_STYLE_CAPTION) / 2 + 2);
}

static void let_titles_out(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(obj, title_reach, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(obj);
}

/* The title on the panel's top line: positions are from the content box, so
 * the panel's padding (which callers may change) is backed out each time. */
static void place_title(lv_obj_t *p)
{
    lv_obj_t *t = lv_obj_get_child(p, 0);

    if (t) {
        lv_obj_set_pos(t, TITLE_INSET - lv_obj_get_style_pad_left(p, LV_PART_MAIN),
                       -lv_obj_get_style_pad_top(p, LV_PART_MAIN) - lv_obj_get_style_border_width(p, LV_PART_MAIN));
    }
}

static void on_panel_style(lv_event_t *e)
{
    place_title(lv_event_get_current_target_obj(e));
}

/* A panel with its title set into the top hairline (UI_UX §5, the mockups):
 * a caption on the screen's own fill, so the line stops either side of it,
 * centred on the line at every text size (moved up by half its own height).
 * The title floats: it takes no room in the panel. */
static lv_obj_t *panel(lv_obj_t *parent, const char *title, lv_obj_t **title_out)
{
    lv_obj_t *p = pocketui_card(parent);
    lv_obj_t *t;

    lv_obj_set_width(p, LV_PCT(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(p, 8, 0);
    lv_obj_set_style_pad_top(p, PANEL_PAD_TOP, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    /* The title reaches above the panel: neither the panel nor whatever
     * holds it may clip it. */
    let_titles_out(p);
    let_titles_out(parent);
    t = pocketui_label(p, title, POS_STYLE_SCREEN);
    pos_style_add(t, POS_STYLE_CAPTION, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(t, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_pad_hor(t, TITLE_PAD, 0);
    lv_obj_set_style_translate_y(t, LV_PCT(-50), 0);
    place_title(p);
    lv_obj_add_event_cb(p, on_panel_style, LV_EVENT_STYLE_CHANGED, NULL);
    if (title_out) {
        *title_out = t;
    }
    return p;
}

/* A figure under its caption, one of a row of equal cells. */
static lv_obj_t *cell(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *c = column(parent, 2);
    lv_obj_t *v;

    grow(c);
    one_line(c, caption, POS_STYLE_CAPTION);
    v = one_line(c, "0", POS_STYLE_VALUE);
    lv_obj_set_width(v, LV_PCT(100));
    return v;
}

/* ---- room for the figures (DS §46.5) -------------------------------------------------- */

/* A figure is never broken across lines: a label in the dots mode with an
 * automatic height wraps instead of shortening (DS §46.5), and "492 000"
 * read as "492" over "000". The widest each figure is expected to reach
 * decides the shape, so it does not change in the middle of a capture: four
 * SESSION figures in a line and each class's rate beside its word where
 * they fit (Small, mockup 01), two by two and the rate on a line of its own
 * under its word where they do not (Medium and Large). A figure that grows
 * past its sample (a capture of ten million) gets the same room once seen. */
#define FIT_COUNT "8 888 888"
#define FIT_BYTES "88.8 GB"
#define FIT_RATE "88 888/s"
#define FIT_SHARE "100.0%"

static bool fits(lv_obj_t *lb, const char *sample, int32_t room)
{
    return pocketui_text_width(lb, sample) <= room && pocketui_text_width(lb, lv_label_get_text(lb)) <= room;
}

/* Four cells of figures (value labels from cell()), in a line or two by two. */
static void fit_cells(lv_obj_t *const v[4], const char *const sample[4], bool *two)
{
    lv_obj_t *r = lv_obj_get_parent(lv_obj_get_parent(v[0]));
    int32_t gap = lv_obj_get_style_pad_column(r, LV_PART_MAIN);
    int32_t width = lv_obj_get_content_width(r);
    int32_t room = (width - 3 * gap) / 4;
    bool want = false;
    int i;

    if (room <= 0) {
        return; /* not laid out yet */
    }
    for (i = 0; i < 4; i++) {
        lv_obj_t *caption = lv_obj_get_child(lv_obj_get_parent(v[i]), 0);

        if (!fits(v[i], sample[i], room) || pocketui_text_width(caption, lv_label_get_text(caption)) > room) {
            want = true;
        }
    }
    if (want == *two) {
        return;
    }
    *two = want;
    lv_obj_set_flex_flow(r, want ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_row(r, want ? gap : 0, 0);
    for (i = 0; i < 4; i++) {
        lv_obj_t *c = lv_obj_get_parent(v[i]);

        if (want) {
            lv_obj_set_flex_grow(c, 0);
            lv_obj_set_width(c, (width - gap) / 2);
        } else {
            grow(c);
        }
    }
}

/* Each class's rate beside its word, or on a line of its own under it. All
 * eight cells take the same shape, so the panel stays a grid. */
static void fit_protocols(struct psk_app *a)
{
    lv_obj_t *top = lv_obj_get_parent(a->proto_word[0]);
    int32_t gap = lv_obj_get_style_pad_column(top, LV_PART_MAIN);
    int32_t room = lv_obj_get_content_width(a->proto_cell[0]) - lv_obj_get_width(a->proto_swatch[0]) - 3 * gap;
    int32_t word = 0;
    bool want = false;
    unsigned c;

    if (lv_obj_get_content_width(a->proto_cell[0]) <= 0) {
        return; /* not laid out yet */
    }
    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        int32_t w = pocketui_text_width(a->proto_word[c], psk_class_word(c));
        int32_t s = pocketui_text_width(a->proto_share[c], lv_label_get_text(a->proto_share[c]));
        int32_t widest = pocketui_text_width(a->proto_share[c], FIT_SHARE);

        word = w > word ? w : word;
        if (room - w - (s > widest ? s : widest) <
            pocketui_text_width(a->proto_rate[c], lv_label_get_text(a->proto_rate[c]))) {
            want = true;
        }
    }
    if (room - word - pocketui_text_width(a->proto_share[0], FIT_SHARE) < pocketui_text_width(a->proto_rate[0], FIT_RATE)) {
        want = true;
    }
    if (want == a->proto_rate_below) {
        return;
    }
    a->proto_rate_below = want;
    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        lv_obj_t *rate = a->proto_rate[c];

        if (want) {
            /* Under the word: the swatch's width and the gap in from the left. */
            lv_obj_set_parent(rate, a->proto_cell[c]);
            lv_obj_move_to_index(rate, 1);
            lv_obj_set_flex_grow(rate, 0);
            lv_obj_set_width(rate, LV_PCT(100));
            lv_obj_set_style_pad_left(rate, lv_obj_get_width(a->proto_swatch[c]) + gap, 0);
            grow(a->proto_word[c]); /* keeps the share at the right */
        } else {
            lv_obj_set_flex_grow(a->proto_word[c], 0);
            lv_obj_set_width(a->proto_word[c], LV_SIZE_CONTENT);
            lv_obj_set_parent(rate, lv_obj_get_parent(a->proto_share[c]));
            lv_obj_move_to_index(rate, 2);
            lv_obj_set_style_pad_left(rate, 0, 0);
            grow(rate);
        }
    }
}

void psk_live_fit(struct psk_app *a)
{
    static const char *const session[4] = { FIT_COUNT, FIT_BYTES, FIT_COUNT, "100%" };
    static const char *const last[4] = { FIT_COUNT, FIT_BYTES, FIT_COUNT, "NO" };
    bool live = !lv_obj_has_flag(a->live, LV_OBJ_FLAG_HIDDEN);
    bool error = !lv_obj_has_flag(a->error, LV_OBJ_FLAG_HIDDEN);

    if (!live && !error) {
        return;
    }
    /* A part shown for the first time has not been laid out yet: lay out
     * now (the poll timer calls this, outside a layout pass), so its first
     * frame already has its shape. */
    if ((live && lv_obj_get_content_width(a->proto_cell[0]) <= 0) ||
        (error && lv_obj_get_content_width(lv_obj_get_parent(a->last[0])) <= 0)) {
        lv_obj_update_layout(a->frame);
    }
    if (live) {
        fit_cells(a->cell, session, &a->cells_two_by_two);
        fit_protocols(a);
    }
    if (error) {
        fit_cells(a->last, last, &a->last_two_by_two);
    }
}

/* ---- events --------------------------------------------------------------------------- */

static int index_of(lv_event_t *e)
{
    return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
}

static void on_tab(lv_event_t *e)
{
    psk_select_tab(lv_event_get_user_data(e), (enum psk_tab)index_of(e));
}

static void on_iface(lv_event_t *e)
{
    psk_select_iface(lv_event_get_user_data(e), index_of(e));
}

static void on_option(lv_event_t *e)
{
    psk_option(lv_event_get_user_data(e), index_of(e));
}

static void on_graph(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t pt;

    if (!in) {
        return;
    }
    lv_indev_get_point(in, &pt);
    psk_graph_tap(a->graph, pt.x);
    psk_live_paint(a, PSK_CHANGED_BIN);
}

/* The readout's and a protocol's way to the packets: the list is M5, so in
 * this milestone they open PACKETS as it is. */
static void on_to_packets(lv_event_t *e)
{
    psk_select_tab(lv_event_get_user_data(e), PSK_TAB_PACKETS);
}

static void on_bar(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    lv_obj_t *t = lv_event_get_current_target(e);
    int i;

    for (i = 0; i < 3; i++) {
        if (a->btn[i] == t) {
            psk_do(a, a->act[i]);
            return;
        }
    }
}

/* ---- building ------------------------------------------------------------------------- */

static const char *const tab_word[PSK_TAB_COUNT] = { "LIVE", "PACKETS", "INSPECT" };

static void build_tabs(struct psk_app *a)
{
    lv_obj_t *strip = row(a->frame, PSK_GAP);
    int i;

    lv_obj_set_height(strip, PSK_TAB_H);
    for (i = 0; i < PSK_TAB_COUNT; i++) {
        lv_obj_t *t = psk_box(strip);

        grow(t);
        lv_obj_set_height(t, PSK_TAB_H);
        pos_style_add(t, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(t, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_pad_bottom(t, 8, 0);
        lv_obj_set_style_pad_row(t, 6, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(t, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, on_tab, LV_EVENT_CLICKED, a);
        a->tab[i] = t;
        a->tab_label[i] = pocketui_label(t, tab_word[i], POS_STYLE_BUTTON_LABEL);
        a->tab_rule[i] = psk_box(t);
        /* The 2 px underline in the accent: a fill role, not a colour. */
        pos_style_add(a->tab_rule[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_style_radius(a->tab_rule[i], 0, 0);
        lv_obj_set_size(a->tab_rule[i], 46, 2);
        lv_obj_set_style_opa(a->tab_rule[i], LV_OPA_TRANSP, 0);
    }
}

static lv_obj_t *page(lv_obj_t *parent)
{
    lv_obj_t *p = psk_box(parent);

    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, PSK_PANEL_GAP, 0);
    lv_obj_set_style_pad_top(p, 14, 0);
    lv_obj_set_style_pad_bottom(p, PSK_PANEL_GAP, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    /* Clickable with nothing to click, so a drag that starts on text
     * scrolls the page (the Zabbix finding on unit A). */
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return p;
}

static void build_status(struct psk_app *a, lv_obj_t *p)
{
    lv_obj_t *r = row(p, 12);

    lv_obj_set_style_min_height(r, 36, 0);
    /* 26 px to the first panel (the mockups): the page's 22 and these 4. */
    lv_obj_set_style_margin_bottom(r, 4, 0);
    a->chip = chip(r);
    a->status_text = one_line(r, "", POS_STYLE_CAPTION);
    grow(a->status_text);
    a->elapsed = one_line(r, "", POS_STYLE_VALUE);
    a->banner = psk_box(p);
    pos_style_add(a->banner, POS_STYLE_SLAB, 0);
    lv_obj_set_width(a->banner, LV_PCT(100));
    lv_obj_set_height(a->banner, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(a->banner, 14, 0);
    a->banner_label = wrapping(a->banner, "", POS_STYLE_CAPTION);
    lv_obj_add_flag(a->banner, LV_OBJ_FLAG_HIDDEN);
}

static const char *const opt_key[4] = { "Stored bytes", "Stop after", "Capture filter", "Promiscuous mode" };

static void build_setup(struct psk_app *a, lv_obj_t *p)
{
    lv_obj_t *opts;
    lv_obj_t *priv;
    lv_obj_t *empty;
    int i;

    a->setup = column(p, PSK_PANEL_GAP);
    a->if_panel = panel(a->setup, "INTERFACE", NULL);
    for (i = 0; i < PSK_IFACES; i++) {
        struct psk_ifrow *r = &a->ifrow[i];
        lv_obj_t *left;

        r->row = row(a->if_panel, 12);
        lv_obj_set_style_min_height(r->row, 72, 0);
        lv_obj_set_style_pad_hor(r->row, 12, 0);
        lv_obj_set_style_radius(r->row, POCKETUI_RADIUS, 0);
        pos_style_add(r->row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(r->row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(r->row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r->row, on_iface, LV_EVENT_CLICKED, a);
        left = column(r->row, 2);
        grow(left);
        r->name = one_line(left, "", POS_STYLE_ROW_TITLE);
        r->sub = one_line(left, "", POS_STYLE_CAPTION);
        lv_obj_set_width(r->sub, LV_PCT(100));
        r->chip = chip(r->row);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
    a->if_none = wrapping(a->if_panel, "No network interface is reported by the system service.",
                          POS_STYLE_TEXT_SECONDARY);

    opts = panel(a->setup, "CAPTURE OPTIONS", NULL);
    lv_obj_set_style_pad_row(opts, 0, 0);
    for (i = 0; i < 4; i++) {
        lv_obj_t *r = row(opts, 8);
        lv_obj_t *k;
        lv_obj_t *chev;

        lv_obj_set_height(r, POCKETUI_ROW_H);
        if (i < 3) {
            pos_style_add(r, POS_STYLE_DIVIDER, 0);
        }
        pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, on_option, LV_EVENT_CLICKED, a);
        k = one_line(r, opt_key[i], POS_STYLE_TEXT_SECONDARY);
        grow(k);
        a->opt_val[i] = one_line(r, "", POS_STYLE_VALUE);
        chev = pocketui_label(r, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
        pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
        a->opt_row[i] = r;
    }
    /* Promiscuous mode is not offered in this version: its restoration has
     * not been verified on the hardware (docs/apps/PACKETSHARK.md). */
    lv_obj_clear_flag(a->opt_row[3], LV_OBJ_FLAG_CLICKABLE);
    psk_set_hidden(lv_obj_get_child(a->opt_row[3], 2), true);

    priv = panel(a->setup, "PRIVACY", NULL);
    wrapping(priv,
             "Packets stay in memory on this device and are discarded when you leave. "
             "Nothing is written to storage and nothing is sent anywhere. HEADERS keeps addresses, "
             "ports and protocol fields; payload only for DNS, DHCP, ARP, ICMP and NTP.",
             POS_STYLE_TEXT_SECONDARY);

    empty = panel(a->setup, "TRAFFIC", NULL);
    lv_obj_set_style_min_height(empty, 120, 0);
    lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    one_line(empty, "NO CAPTURE YET", POS_STYLE_CAPTION);
}

struct proto_paint {
    lv_obj_t *swatch[PPC_CLASS_COUNT];
    lv_obj_t *bar[PPC_CLASS_COUNT];
    uint16_t share[PPC_CLASS_COUNT]; /* permille */
};

static lv_color_t class_color(unsigned cls)
{
    int hue = psk_class_hue(cls);

    return hue < 0 ? pos_theme_color(POS_COLOR_TEXT_SECONDARY) : pos_identity_hue((unsigned)hue);
}

static void paint_rect(lv_layer_t *layer, const lv_area_t *a, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;

    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 0;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.bg_color = color;
    lv_draw_rect(layer, &dsc, a);
}

static void protos_draw(lv_event_t *e)
{
    const struct proto_paint *pp = lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    unsigned c;

    if (!pp || !layer) {
        return;
    }
    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        lv_area_t sw;
        lv_area_t bar;
        lv_area_t fill;
        int32_t w;

        if (lv_obj_has_flag(pp->swatch[c], LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_obj_get_coords(pp->swatch[c], &sw);
        paint_rect(layer, &sw, class_color(c));
        lv_obj_get_coords(pp->bar[c], &bar);
        paint_rect(layer, &bar, pos_theme_color(POS_COLOR_SURFACE_RAISED));
        w = (int32_t)((int64_t)lv_area_get_width(&bar) * pp->share[c] / 1000);
        if (w < 1 && pp->share[c] > 0) {
            w = 1; /* present, however small */
        }
        if (w > 0) {
            fill = bar;
            fill.x2 = fill.x1 + w - 1;
            paint_rect(layer, &fill, class_color(c));
        }
    }
}

static void protos_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);

    free(lv_obj_get_user_data(obj));
    lv_obj_set_user_data(obj, NULL);
}

#define SCALE_W 70 /* graph and scale: 488 px, the content width of a portrait panel */

static void build_traffic(struct psk_app *a, lv_obj_t *parent)
{
    static const char *const scale[] = { "10", "100", "1k", "10k" };
    static const uint64_t decade[] = { 10, 100, 1000, 10000 };
    lv_obj_t *t = panel(parent, "TRAFFIC · LAST 60 S", NULL);
    int32_t lh = pocketui_role_line_height(POS_STYLE_CAPTION);
    lv_obj_t *head;
    lv_obj_t *left;
    lv_obj_t *right;
    lv_obj_t *band;
    lv_obj_t *axis;
    lv_obj_t *rule;
    lv_obj_t *rd;
    lv_obj_t *lb;
    int32_t y;
    int k;

    a->traffic = t;
    lv_obj_set_style_pad_row(t, 0, 0);
    lv_obj_set_style_pad_bottom(t, 14, 0);
    /* The rate and the throughput: 76 px at Small, so the graph starts where
     * the mockup has it; taller at a larger text size. */
    head = row(t, 12);
    lv_obj_set_style_min_height(head, 76, 0);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    left = row(head, 10);
    lv_obj_set_width(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    a->rate = one_line(left, "0", POS_STYLE_HERO_48);
    lb = one_line(left, "PKT/S", POS_STYLE_CAPTION);
    lv_obj_set_style_pad_bottom(lb, 10, 0);
    right = column(head, 2);
    lv_obj_set_width(right, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(right, 6, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    a->throughput = one_line(right, "0 BIT/S", POS_STYLE_VALUE);
    one_line(right, "THROUGHPUT", POS_STYLE_CAPTION);

    /* The graph and its scale: fixed size at every text size (DS §59.3). */
    /* The scale stays beside the graph at every width (landscape too). */
    band = psk_box(t);
    lv_obj_set_size(band, PSK_GRAPH_W + SCALE_W, PSK_GRAPH_H);
    a->graph = psk_graph_create(band);
    lv_obj_add_flag(a->graph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a->graph, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(a->graph, on_graph, LV_EVENT_CLICKED, a);
    for (k = 0; k < 4; k++) {
        lb = pocketui_label(band, scale[k], POS_STYLE_CAPTION);
        lv_obj_add_style(lb, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
        y = PSK_GRAPH_BAND - psk_graph_height_of(decade[k]) - lh / 2;
        /* 10k sits 4 px below the band's top: its label stays inside. */
        lv_obj_align(lb, LV_ALIGN_TOP_RIGHT, 0, y < 0 ? 0 : y);
    }
    axis = psk_box(t);
    lv_obj_set_size(axis, PSK_GRAPH_W, lh);
    lv_obj_set_style_margin_top(axis, 7, 0);
    lb = pocketui_label(axis, "-60 S", POS_STYLE_CAPTION);
    lv_obj_add_style(lb, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 0, 0);
    lb = pocketui_label(axis, "NOW", POS_STYLE_CAPTION);
    lv_obj_add_style(lb, pos_style_fixed_size(POS_STYLE_CAPTION), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_RIGHT, 0, 0);
    rule = psk_box(t);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_margin_top(rule, 2, 0);
    pos_style_add(rule, POS_STYLE_DIVIDER, 0);

    /* The readout: the selected second (or the newest), and the way to its
     * packets. The row is 24 px; its click area reaches 20 px past it. */
    rd = row(t, 8);
    lv_obj_set_style_min_height(rd, 24, 0);
    lv_obj_set_style_margin_top(rd, 5, 0);
    lv_obj_set_flex_align(rd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(rd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rd, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_ext_click_area(rd, 20);
    lv_obj_add_event_cb(rd, on_to_packets, LV_EVENT_CLICKED, a);
    a->readout = one_line(rd, "", POS_STYLE_CAPTION);
    grow(a->readout);
    lb = pocketui_label(rd, "\xe2\x80\xba PACKETS", POS_STYLE_CAPTION);
    pos_style_add(lb, POS_STYLE_ACCENT_TEXT, 0);
}

static void build_protocols(struct psk_app *a, lv_obj_t *parent)
{
    lv_obj_t *p = panel(parent, "PROTOCOLS · SESSION", NULL);
    struct proto_paint *pp = calloc(1, sizeof(*pp));
    unsigned r;

    a->protos = p;
    /* Cells 236 x 52 at x 40 and 292 (mockup 01): 19 px each side. */
    lv_obj_set_style_pad_hor(p, 19, 0);
    lv_obj_set_style_pad_top(p, 17, 0);
    lv_obj_set_style_pad_bottom(p, 13, 0);
    lv_obj_set_style_pad_row(p, 0, 0);
    for (r = 0; r < PPC_CLASS_COUNT / 2; r++) {
        lv_obj_t *line = row(p, 16);
        unsigned k;

        lv_obj_set_style_min_height(line, 52, 0);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        for (k = 0; k < 2; k++) {
            unsigned c = r * 2u + k;
            int hue = psk_class_hue(c);
            lv_obj_t *cell = column(line, 0);
            lv_obj_t *top;

            grow(cell);
            lv_obj_set_style_min_height(cell, 52, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_add_event_cb(cell, on_to_packets, LV_EVENT_CLICKED, a);
            a->proto_cell[c] = cell;
            top = row(cell, 8);
            lv_obj_set_style_min_height(top, 30, 0);
            a->proto_swatch[c] = psk_box(top);
            lv_obj_set_size(a->proto_swatch[c], 10, 10);
            a->proto_word[c] = one_line(top, psk_class_word(c), POS_STYLE_CAPTION);
            if (hue >= 0) {
                lv_obj_add_style(a->proto_word[c], pos_style_identity((unsigned)hue), 0);
            }
            a->proto_rate[c] = one_line(top, "0/s", POS_STYLE_CAPTION);
            grow(a->proto_rate[c]);
            a->proto_share[c] = one_line(top, "0.0%", POS_STYLE_VALUE);
            a->proto_bar[c] = psk_box(cell);
            lv_obj_set_size(a->proto_bar[c], LV_PCT(100), 3);
            lv_obj_set_style_margin_top(a->proto_bar[c], 6, 0);
            if (pp) {
                pp->swatch[c] = a->proto_swatch[c];
                pp->bar[c] = a->proto_bar[c];
            }
        }
    }
    /* Swatches and share bars in their class colours: drawn by one object
     * over the panel (DS §59.2 places), last child so it paints on top, and
     * watched for theme and display-mode changes. */
    a->proto_paint = psk_box(p);
    lv_obj_add_flag(a->proto_paint, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(a->proto_paint, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(a->proto_paint, LV_PCT(100), LV_PCT(100));
    lv_obj_set_user_data(a->proto_paint, pp);
    lv_obj_add_event_cb(a->proto_paint, protos_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(a->proto_paint, protos_delete, LV_EVENT_DELETE, NULL);
    pos_theme_watch(a->proto_paint);
}

static void build_live(struct psk_app *a, lv_obj_t *p)
{
    lv_obj_t *r;
    lv_obj_t *session;

    a->live = column(p, PSK_PANEL_GAP);
    build_traffic(a, a->live);
    build_protocols(a, a->live);

    session = panel(a->live, "SESSION", NULL);
    lv_obj_set_style_pad_top(session, 18, 0);
    lv_obj_set_style_pad_bottom(session, 18, 0);
    r = row(session, 12);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    a->cell[0] = cell(r, "PACKETS");
    a->cell[1] = cell(r, "BYTES");
    a->cell[2] = cell(r, "DROPPED");
    a->cell[3] = cell(r, "BUFFER");
    /* The SIGNALS panel of the mockups is M7 (DS §59.11): not built yet. */
    a->later = wrapping(a->live,
                        "The packet list, the inspector, signals and SAVE are not in this version yet. "
                        "Leaving PacketShark discards the capture.",
                        POS_STYLE_TEXT_SECONDARY);
}

static void build_error(struct psk_app *a, lv_obj_t *p)
{
    lv_obj_t *e;
    lv_obj_t *t;
    lv_obj_t *last;
    lv_obj_t *r;

    a->error = column(p, PSK_PANEL_GAP);
    e = panel(a->error, "CAPTURE HELPER · STOPPED", &t);
    /* The error state's border (DS §2: status_error appears only here). */
    pos_style_add(e, POS_STYLE_FIELD_ERROR, 0);
    pos_style_add(t, POS_STYLE_STATUS_ERROR_TEXT, 0);
    wrapping(e, "Capture ended unexpectedly", POS_STYLE_TITLE);
    wrapping(e,
             "The capture helper stopped while it was running. The packets it held in memory went "
             "with it. Nothing was written to storage.",
             POS_STYLE_TEXT_SECONDARY);
    a->err_reason = pocketui_kv_row(e, "Reason", "");
    a->err_iface = pocketui_kv_row(e, "Interface", "RELEASED");
    a->err_report = pocketui_kv_row(e, "Report", "LOGGED");
    tone(a->err_reason, POS_STYLE_STATUS_ERROR_TEXT);
    tone(a->err_iface, POS_STYLE_STATUS_OK_TEXT);

    last = panel(a->error, "LAST SESSION", NULL);
    r = row(last, 12);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    a->last[0] = cell(r, "PACKETS");
    a->last[1] = cell(r, "BYTES");
    a->last[2] = cell(r, "DROPPED");
    a->last[3] = cell(r, "SAVED");
    psk_text(a->last[3], "NO");
    tone(a->last[3], POS_STYLE_STATUS_WARN_TEXT);
}

static void build_packets(struct psk_app *a)
{
    lv_obj_t *p = panel(a->page[PSK_TAB_PACKETS], "PACKET LIST", NULL);

    lv_obj_set_style_min_height(p, 160, 0);
    a->pk_title = wrapping(p, "NO PACKETS YET", POS_STYLE_ROW_TITLE);
    a->pk_text = wrapping(p, "", POS_STYLE_TEXT_SECONDARY);
    p = panel(a->page[PSK_TAB_INSPECT], "PACKET INSPECTOR", NULL);
    wrapping(p, "NO PACKET OPEN", POS_STYLE_ROW_TITLE);
}

static void build_bar(struct psk_app *a)
{
    int i;

    a->bar = row(a->frame, PSK_GAP);
    lv_obj_set_height(a->bar, PSK_BAR_H);
    for (i = 0; i < 3; i++) {
        a->btn[i] = psk_button(a->bar, "", false, on_bar, a);
        grow(a->btn[i]);
        lv_obj_set_height(a->btn[i], PSK_BAR_H);
    }
}

void psk_live_build(struct psk_app *a)
{
    lv_obj_t *content;
    int i;

    build_tabs(a);
    content = psk_box(a->frame);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    for (i = 0; i < PSK_TAB_COUNT; i++) {
        a->page[i] = page(content);
        psk_set_hidden(a->page[i], i != PSK_TAB_LIVE);
    }
    build_status(a, a->page[PSK_TAB_LIVE]);
    build_setup(a, a->page[PSK_TAB_LIVE]);
    build_live(a, a->page[PSK_TAB_LIVE]);
    build_error(a, a->page[PSK_TAB_LIVE]);
    build_packets(a);
    build_bar(a);
    lv_obj_set_style_margin_top(a->bar, 12, 0);
}

/* ---- painting ------------------------------------------------------------------------- */

void psk_paint_tabs(struct psk_app *a)
{
    int i;
    bool inspect = false; /* nothing can be opened before the list exists (M5, M6) */

    for (i = 0; i < PSK_TAB_COUNT; i++) {
        bool active = i == (int)a->cur;
        bool enabled = i != PSK_TAB_INSPECT || inspect;

        tone(a->tab_label[i], active ? POS_STYLE_ACCENT_TEXT : TONE_NONE);
        lv_obj_set_style_opa(a->tab_rule[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        /* A disabled tab is dimmed, not recoloured: the label keeps its role
         * (a muted role would also change its font). */
        if (enabled) {
            lv_obj_add_flag(a->tab[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(a->tab_label[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_clear_flag(a->tab[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(a->tab_label[i], LV_OPA_40, 0);
        }
        psk_set_hidden(a->page[i], !active);
    }
}

void psk_paint_ifaces(struct psk_app *a)
{
    int i;

    for (i = 0; i < PSK_IFACES; i++) {
        struct psk_ifrow *r = &a->ifrow[i];
        const struct psk_iface *f = &a->ifs[i];
        char sub[96];
        bool usable = i < a->nifs && !f->wifi && f->carrier;

        psk_set_hidden(r->row, i >= a->nifs);
        if (i >= a->nifs) {
            continue;
        }
        psk_text(r->name, f->name);
        if (f->synthetic) {
            snprintf(sub, sizeof(sub), "SIMULATED TRAFFIC · NO NETWORK");
            chip_set(r->chip, CHIP_ACTIVE, "READY");
        } else if (f->wifi) {
            snprintf(sub, sizeof(sub), "WI-FI · NOT AVAILABLE IN THIS VERSION");
            chip_set(r->chip, CHIP_NA, f->up ? "UP" : "DOWN");
        } else if (f->carrier) {
            snprintf(sub, sizeof(sub), "ETHERNET%s%s", f->ipv4[0] ? " · " : "", f->ipv4);
            chip_set(r->chip, CHIP_ACTIVE, "LINK UP");
        } else {
            snprintf(sub, sizeof(sub), "ETHERNET · %s", f->up ? "UP, NO CARRIER" : "DOWN");
            chip_set(r->chip, CHIP_WARN, "NO LINK");
        }
        psk_text(r->sub, sub);
        if (usable && i == a->sel) {
            pos_style_add(r->row, POS_STYLE_SELECTED, 0);
        } else {
            lv_obj_remove_style(r->row, pos_style(POS_STYLE_SELECTED), 0);
        }
        if (f->wifi) {
            lv_obj_clear_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(r->name, LV_OPA_60, 0);
        } else {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    psk_set_hidden(a->if_none, a->nifs > 0);
}

const char *psk_cfilter_label(enum ppc_cfilter f)
{
    static const char *const words[PPC_CFILTER_COUNT] = { "EVERYTHING", "IP AND ARP", "DNS AND DHCP",
                                                          "NOT MY SSH" };

    return (unsigned)f < PPC_CFILTER_COUNT ? words[f] : "EVERYTHING";
}

void psk_paint_options(struct psk_app *a)
{
    char lim[24];

    psk_text(a->opt_val[0], a->keep == PPC_KEEP_FULL ? "FULL" : "HEADERS");
    tone(a->opt_val[0], a->keep == PPC_KEEP_FULL ? POS_STYLE_STATUS_WARN_TEXT : TONE_NONE);
    if (psk_limit_min[a->limit_idx]) {
        snprintf(lim, sizeof(lim), "%u MIN", (unsigned)psk_limit_min[a->limit_idx]);
    } else {
        snprintf(lim, sizeof(lim), "NO LIMIT");
    }
    psk_text(a->opt_val[1], lim);
    psk_text(a->opt_val[2], psk_cfilter_label(a->cfilter));
    psk_text(a->opt_val[3], "OFF");
}

static enum psk_view view_of(const struct psk_app *a)
{
    if (a->failed_view) {
        return PSK_VIEW_ERROR;
    }
    if (a->discarding) {
        return PSK_VIEW_SETUP;
    }
    if (a->model->running && a->model->had_capture && a->model->state != PPC_STATE_IDLE) {
        return PSK_VIEW_LIVE;
    }
    return PSK_VIEW_SETUP;
}

static const char *reason_word(enum ppc_reason r)
{
    switch (r) {
    case PPC_REASON_USER:
        return "BY YOU";
    case PPC_REASON_LIMIT:
        return "TIME LIMIT";
    case PPC_REASON_LINK:
        return "LINK LOST";
    case PPC_REASON_END:
        return "REPLAY ENDED";
    case PPC_REASON_ERROR:
        return "SOURCE FAILED";
    default:
        return "";
    }
}

static void upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

static void set_bar(struct psk_app *a, int n, const enum psk_action *acts, const char *const *words,
                    const bool *enabled, int primary)
{
    int i;

    for (i = 0; i < 3; i++) {
        psk_set_hidden(a->btn[i], i >= n);
        if (i >= n) {
            a->act[i] = PSK_ACT_NONE;
            continue;
        }
        a->act[i] = acts[i];
        psk_text(lv_obj_get_child(a->btn[i], 0), words[i]);
        psk_button_look(a->btn[i], i == primary, enabled[i]);
    }
}

static void paint_bar(struct psk_app *a, enum psk_view v)
{
    const struct psk_model *m = a->model;

    if (v == PSK_VIEW_ERROR) {
        static const enum psk_action acts[] = { PSK_ACT_NEW };
        static const char *const words[] = { "NEW CAPTURE" };
        static const bool en[] = { true };

        set_bar(a, 1, acts, words, en, 0);
    } else if (v == PSK_VIEW_SETUP) {
        static const enum psk_action acts[] = { PSK_ACT_START };
        const char *words[] = { a->starting ? "STARTING..." : "START CAPTURE" };
        bool en[] = { !a->starting && a->sel >= 0 };

        set_bar(a, 1, acts, words, en, 0);
    } else if (m->state == PPC_STATE_STOPPED) {
        static const enum psk_action acts[] = { PSK_ACT_SAVE, PSK_ACT_NEW, PSK_ACT_DISCARD };
        static const char *const words[] = { "SAVE", "NEW CAPTURE", "DISCARD" };
        static const bool en[] = { false, true, true };

        /* SAVE is the step forward but not in this version: disabled, and
         * no other button takes the primary look in its place. */
        set_bar(a, 3, acts, words, en, -1);
    } else {
        static const enum psk_action acts_c[] = { PSK_ACT_FILTER, PSK_ACT_HOLD, PSK_ACT_STOP };
        static const enum psk_action acts_h[] = { PSK_ACT_FILTER, PSK_ACT_RESUME, PSK_ACT_STOP };
        static const char *const words_c[] = { "FILTER", "HOLD", "STOP" };
        static const char *const words_h[] = { "FILTER", "RESUME", "STOP" };
        static const bool en[] = { false, true, true };
        bool hold = m->state == PPC_STATE_HOLD;

        set_bar(a, 3, hold ? acts_h : acts_c, hold ? words_h : words_c, en, hold ? 1 : -1);
    }
}

static void paint_hint(struct psk_app *a, enum psk_view v)
{
    char h[48];
    char iface[PPC_IFACE_MAX];

    snprintf(iface, sizeof(iface), "%s", a->model->iface);
    upper(iface);
    if (v == PSK_VIEW_ERROR) {
        snprintf(h, sizeof(h), "CAPTURE ENDED");
    } else if (v == PSK_VIEW_SETUP) {
        snprintf(h, sizeof(h), "%s", a->simulated ? "SIMULATED" : "NOT CAPTURING");
    } else {
        switch (a->model->state) {
        case PPC_STATE_HOLD:
            snprintf(h, sizeof(h), "HOLD · %s", iface);
            break;
        case PPC_STATE_STOPPED:
            snprintf(h, sizeof(h), "STOPPED · %s", iface);
            break;
        default:
            snprintf(h, sizeof(h), "CAPTURING · %s", iface);
            break;
        }
    }
    if (strcmp(h, a->hint) != 0) {
        snprintf(a->hint, sizeof(a->hint), "%s", h);
        pocketos_shell_set_status_hint(a->hint);
    }
}

static void paint_status(struct psk_app *a, enum psk_view v)
{
    const struct psk_model *m = a->model;
    char text[160];
    char iface[PPC_IFACE_MAX];
    char el[24];
    bool warn = false;

    snprintf(iface, sizeof(iface), "%s", m->iface);
    upper(iface);
    psk_fmt_elapsed(m->c.elapsed_ms, el, sizeof(el));
    if (v == PSK_VIEW_ERROR) {
        chip_set(a->chip, CHIP_ERROR, "STOPPED");
        snprintf(text, sizeof(text), "%s", iface);
        psk_text(a->elapsed, el);
    } else if (v == PSK_VIEW_SETUP) {
        bool any = a->sel >= 0;

        chip_set(a->chip, any ? CHIP_OFF : CHIP_WARN, any ? "READY" : "NO LINK");
        if (any) {
            snprintf(text, sizeof(text), "NOTHING IS CAPTURED UNTIL YOU START");
        } else if (a->nifs > 0) {
            snprintf(text, sizeof(text), "CONNECT A CABLE TO %s", a->ifs[0].wifi ? "ETHERNET" : a->ifs[0].name);
            upper(text);
        } else {
            snprintf(text, sizeof(text), "NO INTERFACE TO CAPTURE ON");
        }
        psk_text(a->elapsed, "");
    } else {
        const char *sz = a->keep == PPC_KEEP_FULL ? "FULL" : "HEADERS";

        switch (m->state) {
        case PPC_STATE_HOLD:
            chip_set(a->chip, CHIP_OFF, "HOLD");
            break;
        case PPC_STATE_STOPPED:
            chip_set(a->chip, CHIP_OFF, "STOPPED");
            break;
        default:
            chip_set(a->chip, CHIP_ACTIVE, "CAPTURING");
            break;
        }
        if (m->state == PPC_STATE_STOPPED) {
            snprintf(text, sizeof(text), "%s · %s", iface, reason_word(m->reason));
        } else if (m->speed) {
            snprintf(text, sizeof(text), "%s · %u MBIT/S · %s", iface, (unsigned)m->speed, sz);
        } else {
            snprintf(text, sizeof(text), "%s · %s", iface, sz);
        }
        psk_text(a->elapsed, el);
    }
    psk_text(a->status_text, text);

    /* The banner: one sentence, what most needs saying. */
    text[0] = '\0';
    if (v == PSK_VIEW_SETUP && a->notice[0]) {
        snprintf(text, sizeof(text), "! %s", a->notice);
        warn = true;
    } else if (v == PSK_VIEW_LIVE && m->c.kdrop + m->c.ifdrop > 0) {
        char n[32];

        snprintf(text, sizeof(text), "! %s PACKETS DROPPED BY THE KERNEL · CAPTURE INCOMPLETE",
                 psk_fmt_count(m->c.kdrop + m->c.ifdrop, n, sizeof(n)));
        warn = true;
    } else if (v == PSK_VIEW_LIVE && m->shed) {
        snprintf(text, sizeof(text), "! TOO FAST TO STORE · COUNTING ONLY");
        warn = true;
    } else if (v == PSK_VIEW_LIVE && m->state == PPC_STATE_HOLD) {
        char n[32];

        snprintf(text, sizeof(text), "HOLD: %s PACKETS PASSED AND WERE COUNTED, NOT KEPT",
                 psk_fmt_count(m->c.held, n, sizeof(n)));
    }
    psk_text(a->banner_label, text);
    tone(a->banner_label, warn ? POS_STYLE_STATUS_WARN_TEXT : TONE_NONE);
    psk_set_hidden(a->banner, text[0] == '\0');
}

static void paint_counters(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    const struct ppc_counters *c = &m->c;
    char b[48];

    psk_text(a->rate, psk_fmt_count(c->pps, b, sizeof(b)));
    psk_text(a->throughput, psk_fmt_bits(c->bps, b, sizeof(b)));
    psk_text(a->cell[0], psk_fmt_count(c->seen, b, sizeof(b)));
    psk_text(a->cell[1], psk_fmt_bytes(c->bytes, b, sizeof(b)));
    psk_text(a->cell[2], psk_fmt_count(c->kdrop + c->ifdrop, b, sizeof(b)));
    tone(a->cell[2], c->kdrop + c->ifdrop ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_STATUS_OK_TEXT);
    snprintf(b, sizeof(b), "%u%%", (unsigned)((c->fill_permille + 5u) / 10u));
    psk_text(a->cell[3], b);
}

/* The graph, the readout and the protocol distribution (M4). */
static void paint_traffic(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    struct psk_graph_window w;
    struct proto_paint *pp = lv_obj_get_user_data(a->proto_paint);
    char b[64];
    unsigned c;

    psk_model_graph(m, &w);
    psk_graph_set(a->graph, &w);
    psk_text(a->readout, psk_model_readout(m, psk_graph_selected(a->graph), b, sizeof(b)));
    for (c = 0; c < PPC_CLASS_COUNT; c++) {
        uint32_t share = psk_model_class_share_permille(m, c);
        char n[24];

        snprintf(b, sizeof(b), "%s/s", psk_fmt_count(psk_model_class_rate(m, c), n, sizeof(n)));
        psk_text(a->proto_rate[c], b);
        snprintf(b, sizeof(b), "%u.%u%%", (unsigned)(share / 10u), (unsigned)(share % 10u));
        psk_text(a->proto_share[c], b);
        if (pp && pp->share[c] != share) {
            pp->share[c] = (uint16_t)share;
            lv_obj_invalidate(a->proto_paint);
        }
    }
}

static void paint_error(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    char b[48];

    psk_text(a->err_reason, psk_exit_text(m->exit, m->exit_value, b, sizeof(b)));
    psk_text(a->last[0], psk_fmt_count(m->c.seen, b, sizeof(b)));
    psk_text(a->last[1], psk_fmt_bytes(m->c.bytes, b, sizeof(b)));
    psk_text(a->last[2], psk_fmt_count(m->c.kdrop + m->c.ifdrop, b, sizeof(b)));
}

static void paint_packets(struct psk_app *a, enum psk_view v)
{
    const struct psk_model *m = a->model;
    char n[32];
    char t[96];

    if (v == PSK_VIEW_LIVE && m->c.last_seq >= m->c.first_seq && m->c.last_seq > 0) {
        snprintf(t, sizeof(t), "%s PACKETS IN MEMORY",
                 psk_fmt_count((uint64_t)m->c.last_seq - m->c.first_seq + 1u, n, sizeof(n)));
        psk_text(a->pk_title, t);
        psk_text(a->pk_text, "The packet list is not in this version of PacketShark yet. "
                             "The packets stay in the capture helper's memory until you leave.");
    } else {
        psk_text(a->pk_title, "NO PACKETS YET");
        psk_text(a->pk_text, "Start a capture on LIVE.");
    }
}

void psk_live_paint(struct psk_app *a, unsigned changed)
{
    enum psk_view v = view_of(a);

    (void)changed;
    a->view = v;
    psk_set_hidden(a->setup, v != PSK_VIEW_SETUP);
    psk_set_hidden(a->live, v != PSK_VIEW_LIVE);
    psk_set_hidden(a->error, v != PSK_VIEW_ERROR);
    paint_status(a, v);
    paint_bar(a, v);
    paint_hint(a, v);
    if (v == PSK_VIEW_LIVE) {
        paint_counters(a);
        paint_traffic(a);
    } else if (v == PSK_VIEW_ERROR) {
        paint_error(a);
    } else {
        psk_paint_ifaces(a);
        psk_paint_options(a);
    }
    paint_packets(a, v);
}
