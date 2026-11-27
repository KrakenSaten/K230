/*
 * PacketShark's LIVE tab (setup, live status, the findings, the error
 * panel), the tab row and the capture bar, and the two shapes they take
 * (docs/apps/PACKETSHARK.md; the design is DS §59, accepted, with its
 * deviations in §59.11). PACKETS is ui/psk_list.c and INSPECT
 * ui/psk_inspect.c.
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

lv_obj_t *psk_panel(lv_obj_t *parent, const char *title)
{
    return panel(parent, title, NULL);
}

void psk_titles_out(lv_obj_t *holder)
{
    let_titles_out(holder);
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

/* The findings' words in the tall shape: one line each, or, where one does
 * not fit its cell at the text size in force (NO DNS REPLY at Large), every
 * word wrapping and every cell one caption line taller, so the cells stay a
 * grid (§46.5: wrap or grow, never cut a word). The wide shape sizes each cell
 * by its word and keeps one line. */
static void fit_signals(struct psk_app *a)
{
    int32_t line = pocketui_role_line_height(POS_STYLE_CAPTION);
    int32_t tallest = line;
    bool want = false;
    unsigned i;

    if (!a->wide) {
        for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
            lv_obj_t *w = lv_obj_get_child(lv_obj_get_child(a->sig_cell[i], 0), 0);
            int32_t room = lv_obj_get_content_width(w);
            lv_point_t sz;

            if (room <= 0) {
                return; /* not laid out yet */
            }
            if (pocketui_text_width(w, psk_signal_word(i)) > room) {
                want = true;
            }
            /* The word's height wrapped in its cell's room. */
            lv_text_get_size(&sz, psk_signal_word(i), lv_obj_get_style_text_font(w, LV_PART_MAIN),
                             lv_obj_get_style_text_letter_space(w, LV_PART_MAIN),
                             lv_obj_get_style_text_line_space(w, LV_PART_MAIN), room, LV_TEXT_FLAG_NONE);
            tallest = sz.y > tallest ? sz.y : tallest;
        }
    }
    a->sig_words_wrap = want;
    /* Applied where it differs only: a text size change lays the labels out
     * again, and setting a mode or a height that is already there costs a
     * relayout for nothing. Every word as tall as the tallest wrapped one,
     * so the six cells are one height. */
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        lv_obj_t *w = lv_obj_get_child(lv_obj_get_child(a->sig_cell[i], 0), 0);
        lv_label_long_mode_t mode = want ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT;
        int32_t h = want ? tallest : line;

        if (lv_label_get_long_mode(w) != mode) {
            lv_label_set_long_mode(w, mode);
            lv_label_set_text(w, psk_signal_word(i)); /* dots mode may have cut it */
        }
        if (lv_obj_get_style_height(w, LV_PART_MAIN) != h) {
            lv_obj_set_height(w, h);
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

/* Shown: neither it nor anything holding it hidden (another tab's page). */
static bool shown_obj(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

/* The text size changed (the theme restyles the panel): the fits again at
 * the next poll, whether or not the capture has anything new to paint. */
static void on_fit_style(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    a->fit_due = true;
}

void psk_live_fit(struct psk_app *a)
{
    static const char *const session[4] = { FIT_COUNT, FIT_BYTES, FIT_COUNT, "100%" };
    static const char *const last[4] = { FIT_COUNT, FIT_BYTES, FIT_COUNT, "NO" };
    bool live = shown_obj(a->live);
    bool error = shown_obj(a->error);

    if (!live && !error) {
        /* Measured where it is seen. LIVE or ERROR is the view, but on a
         * tab not shown: whatever asked for this fit (a change the capture
         * will not repeat once it has ended) is kept due for when it is. */
        if (!lv_obj_has_flag(a->live, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(a->error, LV_OBJ_FLAG_HIDDEN)) {
            a->fit_due = true;
        }
        return;
    }
    /* A part shown for the first time has not been laid out yet, and one
     * whose shape, text size or tab has just changed has not been laid out
     * again: lay out now (the poll timer calls this, outside a layout pass),
     * so the fits measure what will be drawn. */
    if (a->fit_due || (live && lv_obj_get_content_width(a->proto_cell[0]) <= 0) ||
        (error && lv_obj_get_content_width(lv_obj_get_parent(a->last[0])) <= 0)) {
        lv_obj_update_layout(a->frame);
    }
    a->fit_due = false;
    if (live) {
        fit_cells(a->cell, session, &a->cells_two_by_two);
        fit_protocols(a);
        fit_signals(a);
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

/* The readout's way to the packets: the list opens at the selected second
 * (or follows the newest when none is selected). */
static void on_to_packets(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    psk_show_second(a, psk_graph_selected(a->graph));
}

/* A protocol's way to its packets: the list filtered on that class. */
static void on_proto(lv_event_t *e)
{
    psk_show_class(lv_event_get_user_data(e), (unsigned)index_of(e));
}

/* A finding's way to its packets (M7). */
static void on_signal(lv_event_t *e)
{
    psk_show_signal(lv_event_get_user_data(e), (unsigned)index_of(e));
}

/* The DNS or TCP diagnosis, in its sheet (M11.1); the network's line opens
 * the network overview (M12.1). */
static void on_diag(lv_event_t *e)
{
    unsigned k = (unsigned)index_of(e);

    if (k < PSK_DIAG_KINDS) {
        psk_dsheet_show(lv_event_get_user_data(e), k);
    } else {
        psk_net_show(lv_event_get_user_data(e), true);
    }
}

/* MARK (N1.0): this moment; and the marks there are, the newest first. */
static void on_mark(lv_event_t *e)
{
    psk_mark(lv_event_get_user_data(e));
}

static void on_marks(lv_event_t *e)
{
    psk_ev_go(lv_event_get_user_data(e), 0);
}

static void on_saves(lv_event_t *e)
{
    psk_ssheet_show(lv_event_get_user_data(e));
}

/* The app's own back slab (wide): answered by the poll, outside the event. */
static void on_back(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    a->back_pending = true;
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
    lv_obj_t *strip;
    int i;

    /* The top row: the tabs, and in the wide shape a back slab before them
     * and the state and the capture's buttons after (psk_shape). The slab
     * itself lies over the row, in its target (build_back_reach); the row
     * keeps its place. */
    a->top = row(a->frame, PSK_GAP);
    lv_obj_set_height(a->top, PSK_TAB_H);
    a->back_space = psk_box(a->top);
    lv_obj_set_size(a->back_space, PSK_BACK_W, PSK_TOP_H);
    lv_obj_add_flag(a->back_space, LV_OBJ_FLAG_HIDDEN);
    /* Full width, not a grow item: this LVGL takes a gap from the space for
     * every grow item, the only one in a row included. */
    strip = row(a->top, PSK_GAP);
    a->tabs = strip;
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

/* The back slab and its target (H6, the finger test on Unit A, 2026-11-26).
 * Wide, the slab sits against the display's top edge beside its rounded
 * corner, where a finger missed it at 72 x 56. It is drawn as before and
 * answers PSK_TOP_REACH around it (its extended click area): into the gap
 * before LIVE, toward the side and down over the page's top padding, where
 * no control is. LVGL finds a tapped object inside its parent's area only,
 * so the slab cannot be the 56 px row's: it lies in a holder of the frame,
 * as large as that target, laid over the pages and under the sheets, and a
 * spacer keeps its place in the row (build_tabs). The holder is neither
 * drawn nor tapped itself, so the pages and their panel titles, which reach
 * up into that padding, show as before (5920f5a4 took the padding from the
 * pages instead, and the page's edge cut the titles on Unit A). */
static void build_back_reach(struct psk_app *a)
{
    lv_obj_t *chev;

    a->back_reach = psk_box(a->frame);
    lv_obj_add_flag(a->back_reach, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(a->back_reach, PSK_BACK_W + 2 * PSK_TOP_REACH, PSK_TOP_H + PSK_TOP_REACH);
    lv_obj_add_flag(a->back_reach, LV_OBJ_FLAG_HIDDEN);
    a->back = psk_box(a->back_reach);
    lv_obj_set_pos(a->back, PSK_TOP_REACH, 0);
    lv_obj_set_size(a->back, PSK_BACK_W, PSK_TOP_H);
    pos_style_add(a->back, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(a->back, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(a->back, 0, 0);
    lv_obj_add_flag(a->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(a->back, on_back, LV_EVENT_CLICKED, a);
    chev = pocketui_label(a->back, LV_SYMBOL_LEFT, POS_STYLE_SYMBOL);
    pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(chev);
    lv_obj_add_flag(a->back, LV_OBJ_FLAG_HIDDEN);
}

/* Where the row's spacer is: the row's inset in from the frame's content
 * box, at its top (a floating child is placed from that same box), with
 * the holder PSK_TOP_REACH further out. */
void psk_back_reach_place(struct psk_app *a)
{
    lv_obj_set_pos(a->back_reach, a->top_inset_l - PSK_TOP_REACH, 0);
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

    a->status_row = r;
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
    lv_obj_t *saved;
    lv_obj_t *empty;
    int i;

    a->setup = column(p, PSK_PANEL_GAP);
    let_titles_out(a->setup);
    a->setup_l = column(a->setup, PSK_PANEL_GAP);
    a->setup_r = column(a->setup, PSK_PANEL_GAP);
    a->if_panel = panel(a->setup_l, "INTERFACE", NULL);
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

    opts = panel(a->setup_l, "CAPTURE OPTIONS", NULL);
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

    priv = panel(a->setup_r, "PRIVACY", NULL);
    wrapping(priv,
             "Packets stay in memory on this device and are discarded when you leave, unless you save "
             "them. Nothing is sent anywhere. HEADERS keeps addresses, ports and protocol fields; "
             "payload only for DNS, DHCP, ARP, ICMP and NTP.",
             POS_STYLE_TEXT_SECONDARY);

    /* The saved captures (M8, ADR-016 rule 10): how many, and the way to
     * their list. */
    saved = panel(a->setup_r, "SAVED CAPTURES", NULL);
    lv_obj_set_style_pad_row(saved, 0, 0);
    a->saves_row = row(saved, 8);
    lv_obj_set_height(a->saves_row, POCKETUI_ROW_H);
    pos_style_add(a->saves_row, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(a->saves_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a->saves_row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(a->saves_row, on_saves, LV_EVENT_CLICKED, a);
    grow(one_line(a->saves_row, "On this device", POS_STYLE_TEXT_SECONDARY));
    a->saves_val = one_line(a->saves_row, "NONE", POS_STYLE_VALUE);
    pos_style_add(pocketui_label(a->saves_row, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL), POS_STYLE_ACCENT_TEXT, 0);

    empty = panel(a->setup_r, "TRAFFIC", NULL);
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
            lv_obj_set_user_data(cell, (void *)(intptr_t)c);
            lv_obj_add_event_cb(cell, on_proto, LV_EVENT_CLICKED, a);
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

/* SIGNALS (M7, mockup 01): the findings of the capture, each the way to its
 * packets. Six cells of 68 px, three to a row (the mockup drew four in a
 * line; M7 has five, M11.1 six: DS §59.11), DNS's on the first row, TCP's
 * on the second. A count above zero is drawn in status_warn, the colour
 * kept for findings (DS §59.2). Under them the DNS and TCP diagnosis
 * (M11.1): a line each with its severity, the way to its sheet. */
static void build_signals(struct psk_app *a, lv_obj_t *parent)
{
    lv_obj_t *p = panel(parent, "SIGNALS \xc2\xb7 TAP TO LIST", NULL);
    lv_obj_t *grid = psk_box(p);
    unsigned i;

    a->signals = p;
    lv_obj_add_event_cb(p, on_fit_style, LV_EVENT_STYLE_CHANGED, a);
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_top(p, 22, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, PSK_GAP, 0);
    lv_obj_set_style_pad_row(grid, PSK_GAP, 0);
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        /* DNS FAIL, NO DNS REPLY, UNREACH; TCP RST, RETRANS, NO ANSWER.
         * The user data is the finding's bit. */
        static const uint8_t order[PPC_SIGNAL_COUNT] = { 2, 5, 3, 1, 0, 4 };
        unsigned bit = order[i];
        lv_obj_t *c = psk_box(grid);
        lv_obj_t *left;
        lv_obj_t *chev;

        pos_style_add(c, POS_STYLE_SLAB, 0);
        pos_style_add(c, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_radius(c, POCKETUI_RADIUS, 0);
        lv_obj_set_width(c, LV_PCT(32));
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(c, 68, 0);
        lv_obj_set_style_pad_hor(c, 12, 0);
        lv_obj_set_style_pad_ver(c, 8, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(c, (void *)(intptr_t)bit);
        lv_obj_add_event_cb(c, on_signal, LV_EVENT_CLICKED, a);
        left = column(c, 2);
        grow(left);
        lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
        {
            /* One line: a dots-mode label with an automatic height wraps. */
            lv_obj_t *w = one_line(left, psk_signal_word(bit), POS_STYLE_CAPTION);

            lv_obj_set_width(w, LV_PCT(100));
            lv_obj_set_height(w, pocketui_role_line_height(POS_STYLE_CAPTION));
        }
        a->sig_val[bit] = one_line(left, "0", POS_STYLE_VALUE);
        lv_obj_set_width(a->sig_val[bit], LV_PCT(100));
        chev = pocketui_label(c, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
        pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
        a->sig_cell[bit] = c;
    }
    {
        lv_obj_t *dg = psk_box(p);
        unsigned k;

        a->diag_box = dg;
        lv_obj_set_width(dg, LV_PCT(100));
        lv_obj_set_height(dg, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(dg, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(dg, PSK_GAP, 0);
        lv_obj_set_style_pad_row(dg, PSK_GAP, 0);
        for (k = 0; k < PSK_LIVE_DIAGS; k++) {
            unsigned cls = k == PSK_DIAG_DNS ? PPC_CLASS_DNS : PPC_CLASS_TCP;
            int hue = k < PSK_DIAG_KINDS ? psk_class_hue(cls) : -1;
            /* The network's line stays in SIGNALS in both shapes: wide, DNS
             * and TCP move under TRAFFIC and it keeps the right column,
             * which has the room. */
            lv_obj_t *r = psk_box(k < PSK_DIAG_KINDS ? dg : p);
            lv_obj_t *left;
            lv_obj_t *top;
            lv_obj_t *w;
            lv_obj_t *chev;

            pos_style_add(r, POS_STYLE_SLAB, 0);
            pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_radius(r, POCKETUI_RADIUS, 0);
            lv_obj_set_width(r, LV_PCT(100));
            lv_obj_set_height(r, LV_SIZE_CONTENT);
            lv_obj_set_style_min_height(r, PSK_ROW_H, 0);
            lv_obj_set_style_pad_hor(r, 12, 0);
            lv_obj_set_style_pad_ver(r, 6, 0);
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(r, 10, 0);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_user_data(r, (void *)(intptr_t)k);
            lv_obj_add_event_cb(r, on_diag, LV_EVENT_CLICKED, a);
            /* Two lines: the protocol in its hue and the severity's word,
             * then the summary, problems first. */
            left = column(r, 2);
            grow(left);
            lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
            top = row(left, 10);
            lv_obj_add_flag(top, LV_OBJ_FLAG_EVENT_BUBBLE);
            w = one_line(top, k < PSK_DIAG_KINDS ? psk_class_word(cls) : "NETWORK", POS_STYLE_CAPTION);
            if (hue >= 0) {
                lv_obj_add_style(w, pos_style_identity((unsigned)hue), 0);
            }
            a->diag_sev[k] = one_line(top, "OK", POS_STYLE_CAPTION);
            /* One line: a dots-mode label with an automatic height wraps. */
            a->diag_text[k] = one_line(left, "", POS_STYLE_CAPTION);
            lv_obj_set_width(a->diag_text[k], LV_PCT(100));
            lv_obj_set_height(a->diag_text[k], pocketui_role_line_height(POS_STYLE_CAPTION));
            chev = pocketui_label(r, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
            pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
            a->diag_row[k] = r;
        }
    }
}

static void build_live(struct psk_app *a, lv_obj_t *p)
{
    lv_obj_t *r;
    lv_obj_t *session;

    a->live = column(p, PSK_PANEL_GAP);
    let_titles_out(a->live);
    a->live_l = column(a->live, PSK_PANEL_GAP);
    a->live_r = column(a->live, PSK_PANEL_GAP);
    build_traffic(a, a->live_l);
    build_protocols(a, a->live_r);

    session = panel(a->live_r, "SESSION", NULL);
    lv_obj_set_style_pad_top(session, 18, 0);
    lv_obj_set_style_pad_bottom(session, 18, 0);
    r = row(session, 12);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    a->cell[0] = cell(r, "PACKETS");
    a->cell[1] = cell(r, "BYTES");
    a->cell[2] = cell(r, "DROPPED");
    a->cell[3] = cell(r, "BUFFER");
    build_signals(a, a->live_r);
    a->later = wrapping(a->live_r, "", POS_STYLE_TEXT_SECONDARY);
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

/* MARK (N1.0, DS §59.11): a strip of its own above the capture bar, so the
 * bar's buttons stay where the mockups have them and MARK is under the same
 * thumb. Beside it, once the capture has a mark, the way to the marks: a
 * slab with a chevron, as every way to another page is. */
static void build_mark(struct psk_app *a)
{
    lv_obj_t *chev;

    a->mark_strip = row(a->frame, PSK_GAP);
    lv_obj_set_height(a->mark_strip, PSK_MARK_H);
    lv_obj_set_style_margin_top(a->mark_strip, 12, 0);
    a->mark_btn = psk_button(a->mark_strip, "MARK THIS MOMENT", false, on_mark, a);
    lv_obj_set_height(a->mark_btn, PSK_MARK_H);
    a->marks_btn = psk_box(a->mark_strip);
    lv_obj_set_size(a->marks_btn, LV_SIZE_CONTENT, PSK_MARK_H);
    pos_style_add(a->marks_btn, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(a->marks_btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_pad_ver(a->marks_btn, 0, 0);
    lv_obj_set_style_pad_hor(a->marks_btn, 12, 0);
    lv_obj_set_flex_flow(a->marks_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(a->marks_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(a->marks_btn, 8, 0);
    lv_obj_add_flag(a->marks_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(a->marks_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(a->marks_btn, on_marks, LV_EVENT_CLICKED, a);
    a->marks_text = pocketui_label(a->marks_btn, "", POS_STYLE_BUTTON_LABEL);
    chev = pocketui_label(a->marks_btn, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
    pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_add_flag(a->marks_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(a->mark_strip, LV_OBJ_FLAG_HIDDEN);
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
    /* The pages share one row: one of them in the tall shape, the list and
     * the inspector side by side in the wide one (psk_shape). */
    content = psk_box(a->frame);
    a->content = content;
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    for (i = 0; i < PSK_TAB_COUNT; i++) {
        a->page[i] = page(content);
        psk_set_hidden(a->page[i], i != PSK_TAB_LIVE);
    }
    build_back_reach(a); /* after the pages, before the bar and the sheets */
    build_status(a, a->page[PSK_TAB_LIVE]);
    build_setup(a, a->page[PSK_TAB_LIVE]);
    build_live(a, a->page[PSK_TAB_LIVE]);
    build_error(a, a->page[PSK_TAB_LIVE]);
    psk_list_build(a, a->page[PSK_TAB_PACKETS]);
    psk_inspect_build(a, a->page[PSK_TAB_INSPECT]);
    psk_net_build(a, page(content));
    psk_evp_build(a, page(content));
    build_mark(a);
    build_bar(a);
    lv_obj_set_style_margin_top(a->bar, 12, 0);
}

/* ---- the shape (M9) -------------------------------------------------------------------- *
 *
 * Tall (DS §59.7): the shell's header above, the tabs, one page, the capture
 * bar at the foot. Wide (mockup 08): no shell header (app.h NONE_LANDSCAPE),
 * so the top row is the app's own - a back slab, LIVE and PACKETS, the state
 * chip and one line of the session, and the capture's buttons; under it LIVE
 * in two columns, or the list with the inspector beside it. The same objects
 * in both: they are moved, never built twice. */

static void two_columns(lv_obj_t *holder, lv_obj_t *left, lv_obj_t *right, bool wide, int32_t left_w)
{
    lv_obj_set_flex_flow(holder, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(holder, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(holder, PSK_PANEL_GAP, 0);
    if (wide) {
        lv_obj_set_flex_grow(left, 0);
        lv_obj_set_width(left, left_w);
        grow(right);
    } else {
        lv_obj_set_flex_grow(left, 0);
        lv_obj_set_flex_grow(right, 0);
        lv_obj_set_width(left, LV_PCT(100));
        lv_obj_set_width(right, LV_PCT(100));
    }
}

void psk_shape(struct psk_app *a)
{
    bool w = a->wide;
    int i;

    /* The top row. */
    lv_obj_set_style_pad_left(a->top, w ? a->top_inset_l : 0, 0);
    lv_obj_set_style_pad_right(a->top, w ? a->top_inset_r : 0, 0);
    psk_set_hidden(a->back_space, !w);
    psk_set_hidden(a->back_reach, !w); /* the slab and its target (H6) */
    psk_set_hidden(a->back, !w);
    lv_obj_set_ext_click_area(a->back, w ? PSK_TOP_REACH : 0);
    psk_back_reach_place(a);
    psk_set_hidden(a->tab[PSK_TAB_INSPECT], w);
    lv_obj_set_width(a->tabs, w ? 2 * 128 + PSK_GAP : LV_PCT(100));
    /* The state: beside the tabs on every tab (ADR-016 rule 13), or the
     * first line of LIVE under the shell's header, which states it there. */
    lv_obj_set_parent(a->status_row, w ? a->top : a->page[PSK_TAB_LIVE]);
    if (w) {
        lv_obj_move_to_index(a->status_row, 2);
        grow(a->status_row);
        lv_obj_set_style_margin_bottom(a->status_row, 0, 0);
        lv_obj_set_style_pad_left(a->status_row, 8, 0);
    } else {
        lv_obj_move_to_index(a->status_row, 0);
        lv_obj_set_flex_grow(a->status_row, 0);
        lv_obj_set_width(a->status_row, LV_PCT(100));
        lv_obj_set_style_margin_bottom(a->status_row, 4, 0);
        lv_obj_set_style_pad_left(a->status_row, 0, 0);
    }
    psk_set_hidden(a->elapsed, w);
    /* MARK (N1.0): a strip above the capture bar, or in the top row before
     * the capture's buttons, where it is a word and the marks a figure. */
    lv_obj_set_parent(a->mark_strip, w ? a->top : a->frame);
    lv_obj_move_to_index(a->mark_strip, -1);
    lv_obj_set_style_margin_top(a->mark_strip, w ? 0 : 12, 0);
    lv_obj_set_height(a->mark_strip, w ? PSK_TOP_H : PSK_MARK_H);
    lv_obj_set_width(a->mark_strip, w ? LV_SIZE_CONTENT : LV_PCT(100));
    lv_obj_set_height(a->mark_btn, w ? PSK_TOP_H : PSK_MARK_H);
    lv_obj_set_height(a->marks_btn, w ? PSK_TOP_H : PSK_MARK_H);
    if (w) {
        lv_obj_set_width(a->mark_btn, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(a->mark_btn, 88, 0);
        lv_obj_set_style_pad_hor(a->mark_btn, 14, 0);
        lv_obj_set_width(a->marks_btn, LV_SIZE_CONTENT);
        lv_obj_set_style_min_width(a->marks_btn, 64, 0);
    } else {
        lv_obj_set_style_min_width(a->mark_btn, 0, 0);
        lv_obj_remove_local_style_prop(a->mark_btn, LV_STYLE_PAD_LEFT, 0);
        lv_obj_remove_local_style_prop(a->mark_btn, LV_STYLE_PAD_RIGHT, 0);
        lv_obj_set_width(a->mark_btn, LV_PCT(100));
        lv_obj_set_style_min_width(a->marks_btn, 0, 0);
    }
    /* The capture bar: at the foot, or the top row's right end. */
    lv_obj_set_parent(a->bar, w ? a->top : a->frame);
    lv_obj_move_to_index(a->bar, -1);
    lv_obj_set_style_margin_top(a->bar, w ? 0 : 12, 0);
    lv_obj_set_height(a->bar, w ? PSK_TOP_H : PSK_BAR_H);
    lv_obj_set_width(a->bar, w ? LV_SIZE_CONTENT : LV_PCT(100));
    for (i = 0; i < 3; i++) {
        lv_obj_set_height(a->btn[i], w ? PSK_TOP_H : PSK_BAR_H);
        if (w) {
            lv_obj_set_flex_grow(a->btn[i], 0);
            lv_obj_set_width(a->btn[i], LV_SIZE_CONTENT);
            lv_obj_set_style_min_width(a->btn[i], 104, 0);
            lv_obj_set_style_pad_hor(a->btn[i], 14, 0);
        } else {
            lv_obj_set_style_min_width(a->btn[i], 0, 0);
            lv_obj_remove_local_style_prop(a->btn[i], LV_STYLE_PAD_LEFT, 0);
            lv_obj_remove_local_style_prop(a->btn[i], LV_STYLE_PAD_RIGHT, 0);
            grow(a->btn[i]);
        }
    }
    /* The pages: LIVE alone; the list with the inspector beside it. */
    lv_obj_set_style_pad_column(a->content, w ? 14 : 0, 0);
    if (w) {
        lv_obj_set_width(a->page[PSK_TAB_PACKETS], 1);
        lv_obj_set_flex_grow(a->page[PSK_TAB_PACKETS], 1);
        lv_obj_set_width(a->page[PSK_TAB_INSPECT], PSK_INSPECT_W);
        if (a->cur == PSK_TAB_INSPECT) {
            a->cur = PSK_TAB_PACKETS;
        }
    } else {
        lv_obj_set_flex_grow(a->page[PSK_TAB_PACKETS], 0);
        lv_obj_set_width(a->page[PSK_TAB_PACKETS], LV_PCT(100));
        lv_obj_set_width(a->page[PSK_TAB_INSPECT], LV_PCT(100));
    }
    two_columns(a->live, a->live_l, a->live_r, w, PSK_LIVE_LEFT_W);
    two_columns(a->setup, a->setup_l, a->setup_r, w, PSK_LIVE_LEFT_W);
    /* The findings: three to a row, or all six in the wider column; the
     * DNS and TCP diagnosis one above the other, or side by side. */
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        lv_obj_t *c = a->sig_cell[i];
        lv_obj_t *left = lv_obj_get_child(c, 0);

        /* Wide: each cell as wide as its word, spread over the line, so the
         * six fit one line at text size Small (larger sizes wrap to a second
         * line); tall: three to a row. A grow item would take an equal share
         * whatever its word. */
        if (w) {
            lv_obj_set_width(c, LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(left, 0);
            lv_obj_set_width(left, LV_SIZE_CONTENT);
            lv_obj_set_width(lv_obj_get_child(left, 0), LV_SIZE_CONTENT);
            lv_obj_set_width(lv_obj_get_child(left, 1), LV_SIZE_CONTENT);
        } else {
            lv_obj_set_width(c, LV_PCT(32));
            lv_obj_set_width(left, 1);
            lv_obj_set_flex_grow(left, 1);
            lv_obj_set_width(lv_obj_get_child(left, 0), LV_PCT(100));
            lv_obj_set_width(lv_obj_get_child(left, 1), LV_PCT(100));
        }
        /* The chevron has no room in the narrower cell. */
        psk_set_hidden(lv_obj_get_child(c, 1), w);
        lv_obj_set_style_min_height(c, w ? 56 : 68, 0);
        lv_obj_set_style_pad_ver(c, w ? 4 : 8, 0);
        lv_obj_set_style_pad_hor(c, w ? 8 : 12, 0);
    }
    lv_obj_set_flex_align(lv_obj_get_parent(a->sig_cell[0]), w ? LV_FLEX_ALIGN_SPACE_BETWEEN : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    /* The DNS and TCP diagnosis: under the findings, one above the other;
     * wide, under TRAFFIC side by side, where the left column has room. The
     * same objects, moved (M9). */
    if (lv_obj_get_parent(a->diag_box) != (w ? a->live_l : a->signals)) {
        lv_obj_set_parent(a->diag_box, w ? a->live_l : a->signals);
    }
    for (i = 0; i < PSK_LIVE_DIAGS; i++) {
        lv_obj_set_width(a->diag_row[i], w && i < PSK_DIAG_KINDS ? LV_PCT(49) : LV_PCT(100));
    }
    psk_net_shape(a);
    psk_evp_shape(a);
    /* Wide: LIVE's three panels and the top row fit the body's height. */
    lv_obj_set_style_pad_row(a->live_r, w ? 12 : PSK_PANEL_GAP, 0);
    lv_obj_set_style_pad_top(a->page[PSK_TAB_LIVE], w ? 8 : 14, 0);
    psk_list_shape(a);
    psk_paint_tabs(a);
    a->window_due = true;
    a->marks_due = true;
}

/* ---- painting ------------------------------------------------------------------------- */

void psk_paint_tabs(struct psk_app *a)
{
    int i;
    bool inspect = a->insp_seq != 0 && psk_capture_present(a); /* a packet has been opened */

    for (i = 0; i < PSK_TAB_COUNT; i++) {
        bool active = i == (int)a->cur;
        bool enabled = i != PSK_TAB_INSPECT || inspect;
        /* Wide: the inspector is beside the list, on PACKETS. */
        bool shown = a->wide ? (i == PSK_TAB_LIVE) == (a->cur == PSK_TAB_LIVE) : active;

        /* LIVE's other faces, the network overview and a mark's evidence,
         * in LIVE's place. */
        if (i == PSK_TAB_LIVE && (a->net_view || a->ev_view)) {
            shown = false;
        }

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
        psk_set_hidden(a->page[i], !shown);
    }
    psk_set_hidden(a->nv_page, !psk_net_shown(a));
    psk_evp_place(a);
}

void psk_mark_fit(struct psk_app *a)
{
    int32_t w;
    int32_t third;
    int32_t rest;

    if (a->wide || lv_obj_has_flag(a->mark_strip, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_has_flag(a->marks_btn, LV_OBJ_FLAG_HIDDEN)) {
        if (lv_obj_get_style_width(a->mark_btn, LV_PART_MAIN) != LV_PCT(100)) {
            lv_obj_set_width(a->mark_btn, LV_PCT(100));
        }
        return;
    }
    w = lv_obj_get_content_width(a->mark_strip);
    if (w <= 3 * PSK_GAP) {
        return; /* not laid out yet: at the next poll */
    }
    third = (w - 2 * PSK_GAP + 2) / 3;
    rest = w - PSK_GAP - third;
    if (lv_obj_get_style_width(a->mark_btn, LV_PART_MAIN) != rest) {
        lv_obj_set_width(a->mark_btn, rest);
    }
    if (lv_obj_get_style_width(a->marks_btn, LV_PART_MAIN) != third) {
        lv_obj_set_width(a->marks_btn, third);
    }
}

bool psk_mark_offered(const struct psk_app *a)
{
    enum ppc_state s = a->model->state;

    return psk_capture_present(a) && !(a->wide && (s == PPC_STATE_STOPPED || s == PPC_STATE_SAVING));
}

/* MARK and the way to the marks (N1.0): on LIVE while a capture is on the
 * screen, whichever face LIVE shows. A capture that is being written takes
 * no mark. Wide, MARK leaves the top row once the capture has stopped or is
 * being saved (psk_mark_offered); the count stays while there are marks. */
static void paint_mark(struct psk_app *a, enum psk_view v)
{
    const struct psk_model *m = a->model;
    bool live = v == PSK_VIEW_LIVE && a->cur == PSK_TAB_LIVE && psk_capture_present(a);
    uint32_t held = psk_ev_held(m);
    bool mark = live && psk_mark_offered(a);
    bool on = mark || (live && held > 0);
    char b[24];

    if (on == lv_obj_has_flag(a->mark_strip, LV_OBJ_FLAG_HIDDEN)) {
        psk_set_hidden(a->mark_strip, !on);
        a->top_moved = true;
    }
    if (on && mark == lv_obj_has_flag(a->mark_btn, LV_OBJ_FLAG_HIDDEN)) {
        psk_set_hidden(a->mark_btn, !mark);
        a->top_moved = true;
    }
    if (on) {
        psk_text(lv_obj_get_child(a->mark_btn, 0), a->mark_wait ? "MARKING" : a->wide ? "MARK" : "MARK THIS MOMENT");
        psk_button_look(a->mark_btn, false, !a->mark_wait && m->state != PPC_STATE_SAVING);
        if ((held == 0) != lv_obj_has_flag(a->marks_btn, LV_OBJ_FLAG_HIDDEN)) {
            psk_set_hidden(a->marks_btn, held == 0);
            a->top_moved = true;
        }
        if (a->wide) {
            snprintf(b, sizeof(b), "%u", (unsigned)held);
        } else {
            snprintf(b, sizeof(b), "%u MARK%s", (unsigned)held, held == 1 ? "" : "S");
        }
        psk_text(a->marks_text, b);
        psk_mark_fit(a);
    }
}

void psk_paint_ifaces(struct psk_app *a)
{
    int i;

    for (i = 0; i < PSK_IFACES; i++) {
        struct psk_ifrow *r = &a->ifrow[i];
        const struct psk_iface *f = &a->ifs[i];
        char sub[96];
        bool usable = i < a->nifs && psk_iface_usable(f);

        psk_set_hidden(r->row, i >= a->nifs);
        if (i >= a->nifs) {
            continue;
        }
        psk_text(r->name, f->name);
        if (f->synthetic) {
            snprintf(sub, sizeof(sub), "SIMULATED TRAFFIC · NO NETWORK");
            chip_set(r->chip, CHIP_ACTIVE, "READY");
        } else if (f->wifi && f->carrier) {
            /* Managed mode: this device's own traffic and the network's
             * broadcast and multicast, as on a switch port (§2). */
            snprintf(sub, sizeof(sub), "WI-FI%s%s%s%s", f->ssid[0] ? " · " : "", f->ssid,
                     f->ipv4[0] ? " · " : "", f->ipv4);
            chip_set(r->chip, CHIP_ACTIVE, "CONNECTED");
        } else if (f->wifi) {
            /* rtl8189fs reports operstate down, not dormant, while it is
             * not associated, so "down" cannot be told from "off" here. */
            snprintf(sub, sizeof(sub), "WI-FI · NOT CONNECTED");
            chip_set(r->chip, f->up ? CHIP_WARN : CHIP_NA, f->up ? "NO LINK" : "DOWN");
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
        /* Every row takes a tap; only a usable one becomes the choice. */
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    }
    psk_set_hidden(a->if_none, a->nifs > 0);
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
        enum psk_action act = i < n ? acts[i] : PSK_ACT_NONE;

        /* Other buttons are other widths: in the wide shape the state's
         * line beside them is fitted again (psk_live_paint). */
        a->top_moved |= act != a->act[i];
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
    } else {
        /* The first button is the tab's own: FILTER on LIVE and on a list
         * that follows, LIVE and the count of newer packets on a held list,
         * FILTER BY on INSPECT. The state's buttons follow it on every tab
         * (ADR-016 rule 13). At most one primary (DS §9): RESUME in HOLD. */
        bool saving = m->state == PPC_STATE_SAVING;
        bool stopped = m->state == PPC_STATE_STOPPED || saving;
        bool hold = m->state == PPC_STATE_HOLD;
        enum psk_action acts[3];
        const char *words[3];
        bool en[3] = { true, true, true };
        char live[32];
        int primary = hold ? 1 : -1;
        bool by = m->have_pkt && a->insp_field >= 0 && (uint32_t)a->insp_field < m->pkt.nfields &&
                  m->pkt.field[a->insp_field].filter[0];

        /* Wide: the inspector is beside the list, so a selected field
         * offers FILTER BY there too. */
        if (a->cur == PSK_TAB_INSPECT || (a->wide && a->cur == PSK_TAB_PACKETS && by)) {
            acts[0] = PSK_ACT_FILTER_BY;
            words[0] = "FILTER BY";
            en[0] = by;
            if (!hold && !stopped) {
                primary = 0;
            }
        } else if (a->cur == PSK_TAB_PACKETS && !stopped && !a->follow && a->sort != 2) {
            char n[24];
            uint32_t newer;
            uint32_t shown = 0;
            int i;

            for (i = 0; i < a->vis && i < PSK_LIST_ROWS; i++) {
                shown += a->lrow[i].seq != 0;
            }
            if (a->sort == 1) {
                newer = a->top_rank;
            } else {
                newer = m->c.match > a->top_rank + shown ? m->c.match - a->top_rank - shown : 0;
            }
            snprintf(live, sizeof(live), "LIVE %s %s", a->sort == 1 ? "\xe2\x86\x91" : "\xe2\x86\x93",
                     psk_fmt_count(newer, n, sizeof(n)));
            acts[0] = PSK_ACT_LIVE;
            words[0] = live;
            if (!hold) {
                primary = 0;
            }
        } else if (a->cur == PSK_TAB_LIVE && stopped) {
            /* SAVE is the step forward of a stopped capture (M8): primary
             * until it has been saved. */
            acts[0] = PSK_ACT_SAVE;
            words[0] = saving ? "SAVING" : m->save == PSK_SAVE_DONE ? "SAVED" : "SAVE";
            en[0] = !saving && m->save != PSK_SAVE_DONE;
        } else {
            acts[0] = PSK_ACT_FILTER;
            words[0] = "FILTER";
        }
        if (stopped) {
            acts[1] = PSK_ACT_NEW;
            words[1] = "NEW CAPTURE";
            acts[2] = PSK_ACT_DISCARD;
            words[2] = "DISCARD";
            /* The file is being written: nothing ends the helper from here. */
            en[1] = !saving;
            en[2] = !saving;
            primary = (acts[0] == PSK_ACT_FILTER_BY || acts[0] == PSK_ACT_SAVE) && en[0] ? 0 : -1;
        } else {
            acts[1] = hold ? PSK_ACT_RESUME : PSK_ACT_HOLD;
            words[1] = hold ? "RESUME" : "HOLD";
            acts[2] = PSK_ACT_STOP;
            words[2] = "STOP";
        }
        set_bar(a, 3, acts, words, en, primary);
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
        case PPC_STATE_SAVING:
            snprintf(h, sizeof(h), "SAVING · %s", iface);
            break;
        default:
            snprintf(h, sizeof(h), "CAPTURING · %s", iface);
            break;
        }
    }
    if (a->reading) {
        /* ADR-016 rule 20: beside the capture state, on every screen. */
        size_t at = strlen(h);

        snprintf(h + at, sizeof(h) - at, " · COMPANION");
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
            const char *eth = NULL;
            bool wifi = false;
            int i;

            for (i = 0; i < a->nifs; i++) {
                wifi |= a->ifs[i].wifi;
                if (!a->ifs[i].wifi && !eth) {
                    eth = a->ifs[i].name;
                }
            }
            if (eth && wifi) {
                snprintf(text, sizeof(text), "CONNECT %s OR JOIN WI-FI", eth);
            } else if (eth) {
                snprintf(text, sizeof(text), "CONNECT A CABLE TO %s", eth);
            } else {
                snprintf(text, sizeof(text), "JOIN A WI-FI NETWORK");
            }
            upper(text);
        } else {
            snprintf(text, sizeof(text), "NO INTERFACE TO CAPTURE ON");
        }
        psk_text(a->elapsed, "");
    } else {
        const char *sz = a->run_full ? "FULL" : "HEADERS"; /* what this capture was started with */

        switch (m->state) {
        case PPC_STATE_HOLD:
            chip_set(a->chip, CHIP_OFF, "HOLD");
            break;
        case PPC_STATE_STOPPED:
            chip_set(a->chip, CHIP_OFF, "STOPPED");
            break;
        case PPC_STATE_SAVING:
            chip_set(a->chip, CHIP_OFF, "SAVING");
            break;
        default:
            chip_set(a->chip, CHIP_ACTIVE, "CAPTURING");
            break;
        }
        if (a->wide) {
            /* One line of the session beside the chip (mockup 08): the
             * elapsed time and the counts the tall shape shows on LIVE.
             * Where the line has no room for all of it, the drops stay
             * when there are any and the packet count when there are none. */
            char n[32];
            char d[32];
            int32_t room = lv_obj_get_width(a->status_text);
            uint64_t drops = m->c.kdrop + m->c.ifdrop;

            bool ended = m->state == PPC_STATE_STOPPED || m->state == PPC_STATE_SAVING;
            char head[64];

            if (room > 0 && a->reading) {
                /* The companion's mark is written before the line (below,
                 * rule 20): the line is fitted to what is left beside it,
                 * so neither is cut. */
                room -= pocketui_text_width(a->status_text, "COMPANION \xc2\xb7 ");
                if (room < 1) {
                    room = 1;
                }
            }

            /* A stopped capture says why first, as the tall shape does. */
            if (ended) {
                snprintf(head, sizeof(head), "%s · %s · %s", iface, reason_word(m->reason), el);
            } else {
                snprintf(head, sizeof(head), "%s · %s", iface, el);
            }
            psk_fmt_count(m->c.seen, n, sizeof(n));
            psk_fmt_count(drops, d, sizeof(d));
            snprintf(text, sizeof(text), "%s · %s PKT · %s DROPPED", head, n, d);
            if (room > 0 && pocketui_text_width(a->status_text, text) > room) {
                if (drops) {
                    snprintf(text, sizeof(text), "%s · %s DROPPED", head, d);
                } else {
                    snprintf(text, sizeof(text), "%s · %s PKT", head, n);
                }
            }
            if (room > 0 && pocketui_text_width(a->status_text, text) > room) {
                snprintf(text, sizeof(text), "%s", head);
            }
            /* With MARK in the row (N1.0) there is less of it. A line cut by
             * dots loses its end, which is what matters most, so the line
             * is shortened from what matters least instead: a stopped
             * capture keeps where and why, then why alone; a running one
             * keeps how long it has run. */
            if (room > 0 && pocketui_text_width(a->status_text, text) > room) {
                if (ended) {
                    snprintf(text, sizeof(text), "%s \xc2\xb7 %s", iface, reason_word(m->reason));
                } else {
                    snprintf(text, sizeof(text), "%s", el);
                }
            }
            if (ended && room > 0 && pocketui_text_width(a->status_text, text) > room) {
                snprintf(text, sizeof(text), "%s", reason_word(m->reason));
            }
        } else if (m->state == PPC_STATE_STOPPED || m->state == PPC_STATE_SAVING) {
            snprintf(text, sizeof(text), "%s · %s", iface, reason_word(m->reason));
        } else if (m->speed) {
            snprintf(text, sizeof(text), "%s · %u MBIT/S · %s", iface, (unsigned)m->speed, sz);
        } else if (strncmp(m->iface, "wlan", 4) == 0) {
            /* A Wi-Fi interface reports no speed (sysfs answers EINVAL). */
            snprintf(text, sizeof(text), "%s · WI-FI · %s", iface, sz);
        } else {
            snprintf(text, sizeof(text), "%s · %s", iface, sz);
        }
        psk_text(a->elapsed, el);
    }
    if (a->wide && a->reading) {
        /* The wide shape has no shell header: its own top row says it
         * (rule 20), first, where a line too long for the row is not cut. */
        static const char mark[] = "COMPANION · ";
        size_t n = strlen(text);

        if (n + sizeof(mark) <= sizeof(text)) {
            memmove(text + sizeof(mark) - 1, text, n + 1);
            memcpy(text, mark, sizeof(mark) - 1);
        }
    }
    psk_text(a->status_text, text);
    {
        /* Wide: the line beside the chip is one line (a dots-mode label
         * with an automatic height wraps, DS §46.5). */
        int32_t want = a->wide ? pocketui_role_line_height(POS_STYLE_CAPTION) : LV_SIZE_CONTENT;

        if (lv_obj_get_style_height(a->status_text, LV_PART_MAIN) != want) {
            lv_obj_set_height(a->status_text, want);
        }
    }

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
    } else if (v == PSK_VIEW_LIVE && (m->state == PPC_STATE_SAVING || m->save == PSK_SAVE_RUNNING)) {
        char n[32];
        char t[32];

        snprintf(text, sizeof(text), "SAVING · %s OF %s PACKETS", psk_fmt_count(m->save_packets, n, sizeof(n)),
                 psk_fmt_count(m->save_total, t, sizeof(t)));
    } else if (v == PSK_VIEW_LIVE && m->save == PSK_SAVE_DONE) {
        char n[32];
        char t[32];

        /* The file's name as it is in the folder: lower case, as written. */
        snprintf(text, sizeof(text), "SAVED · %s PACKETS · %s · %s", psk_fmt_count(m->save_packets, n, sizeof(n)),
                 psk_fmt_bytes(m->save_bytes, t, sizeof(t)), m->save_name);
    } else if (v == PSK_VIEW_LIVE && m->save == PSK_SAVE_FAILED) {
        snprintf(text, sizeof(text), "! NOT SAVED · %s", psk_save_fail_text(m->save_fail));
        warn = true;
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
    unsigned i;

    psk_text(a->rate, psk_fmt_count(c->pps, b, sizeof(b)));
    psk_text(a->throughput, psk_fmt_bits(c->bps, b, sizeof(b)));
    psk_text(a->cell[0], psk_fmt_count(c->seen, b, sizeof(b)));
    psk_text(a->cell[1], psk_fmt_bytes(c->bytes, b, sizeof(b)));
    psk_text(a->cell[2], psk_fmt_count(c->kdrop + c->ifdrop, b, sizeof(b)));
    tone(a->cell[2], c->kdrop + c->ifdrop ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_STATUS_OK_TEXT);
    snprintf(b, sizeof(b), "%u%%", (unsigned)((c->fill_permille + 5u) / 10u));
    psk_text(a->cell[3], b);
    /* The findings (M7): a count above zero in status_warn (DS §59.2). */
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        psk_text(a->sig_val[i], psk_fmt_count(c->sig[i], b, sizeof(b)));
        tone(a->sig_val[i], c->sig[i] ? POS_STYLE_STATUS_WARN_TEXT : TONE_NONE);
    }
    /* The DNS and TCP diagnosis (M11.1): its line, and a word for how it
     * reads - OK in status_ok, NOTE plain, ! CHECK in status_warn. */
    for (i = 0; i < PSK_LIVE_DIAGS; i++) {
        char t[96];
        unsigned sev = psk_diag_summary(m, i < PSK_DIAG_KINDS ? i : (unsigned)PSK_DIAG_NET, t, sizeof(t));

        psk_text(a->diag_text[i], t);
        psk_text(a->diag_sev[i], sev == PSK_SEV_WARN ? "! CHECK" : sev == PSK_SEV_NOTE ? "NOTE" : "OK");
        tone(a->diag_sev[i], sev == PSK_SEV_WARN ? POS_STYLE_STATUS_WARN_TEXT
                             : sev == PSK_SEV_OK ? POS_STYLE_STATUS_OK_TEXT
                                                 : TONE_NONE);
    }
    psk_text(a->later, m->save == PSK_SAVE_DONE
                           ? "This capture is saved. Leaving PacketShark removes it from memory; the file stays."
                           : "Leaving PacketShark discards the capture unless it is saved first: STOP, then SAVE.");
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
    psk_text(a->last[3], m->save == PSK_SAVE_DONE ? "YES" : "NO");
    tone(a->last[3], m->save == PSK_SAVE_DONE ? POS_STYLE_STATUS_OK_TEXT : POS_STYLE_STATUS_WARN_TEXT);
}

void psk_live_paint(struct psk_app *a, unsigned changed)
{
    enum psk_view v = view_of(a);

    a->view = v;
    if ((a->net_view || a->ev_view) && (v != PSK_VIEW_LIVE || !psk_capture_present(a))) {
        /* The network overview and a mark's evidence are of a capture: with
         * none, LIVE is LIVE. */
        a->net_view = false;
        a->ev_view = false;
        psk_paint_tabs(a);
    }
    psk_set_hidden(a->setup, v != PSK_VIEW_SETUP);
    psk_set_hidden(a->live, v != PSK_VIEW_LIVE);
    psk_set_hidden(a->error, v != PSK_VIEW_ERROR);
    /* The buttons first: in the wide shape they share the top row with the
     * state's line, which is fitted to the room they leave. When MARK, the
     * marks' count or the capture's buttons changed, that room is measured
     * anew before the line is written (never inside a layout pass: this is
     * called from the poll and from taps). */
    paint_mark(a, v);
    paint_bar(a, v);
    if (a->top_moved && a->wide) {
        lv_obj_update_layout(a->top);
    }
    a->top_moved = false;
    paint_status(a, v);
    paint_hint(a, v);
    if (v == PSK_VIEW_LIVE) {
        paint_counters(a);
        paint_traffic(a);
    } else if (v == PSK_VIEW_ERROR) {
        paint_error(a);
    } else {
        char n[32];
        char b[64];

        psk_paint_ifaces(a);
        psk_paint_options(a);
        if (a->saves_count) {
            snprintf(b, sizeof(b), "%u · %s", (unsigned)a->saves_count, psk_fmt_bytes(a->saves_bytes, n, sizeof(n)));
        } else {
            snprintf(b, sizeof(b), "NONE");
        }
        psk_text(a->saves_val, b);
    }
    if (changed & (PSK_CHANGED_EXITED | PSK_CHANGED_STATE | PSK_CHANGED_HELLO)) {
        psk_paint_tabs(a);
    }
    if (a->cur == PSK_TAB_PACKETS) {
        psk_list_paint(a);
    }
    if (psk_inspect_visible(a)) {
        psk_inspect_paint(a);
    }
}
