/*
 * PacketShark's marks and their evidence (N1.0, docs/apps/PACKETSHARK.md
 * §4d, §7): a face of LIVE, as the network overview is. A mark is a moment
 * the person pointed at. This page says what the capture saw of DHCP, ARP
 * and DNS in the minute before it and the minute after: how much of each
 * window was watched, what was counted, the observations themselves - each
 * a tap to its packet - and what one interface cannot tell.
 *
 * It is a page, not a sheet: the tabs, the capture's state, MARK and the
 * capture's own buttons stay where they are (ADR-016 rule 13). Only
 * existing roles; a label is written only when its text changes; the
 * evidence is asked for while the page shows and never otherwise
 * (psk_app.c). Every word on it comes from psk_ev.c, where it is tested.
 *
 * The page is some 350 objects, and a layout pass walks every object of the
 * active screen, hidden or not: LIVE and PACKETS force one at each poll that
 * changed something. So the page waits on a screen of its own, which is
 * never loaded, and is on the active screen only while it shows
 * (psk_evp_place). A capture that is never marked pays nothing for it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"
#include "psk_ev.h"

#include <stdio.h>
#include <string.h>

#define DOT " \xc2\xb7 "
#define CLS_W 52  /* a protocol's name in the counts */
#define FIG_W 84  /* a count */

static lv_obj_t *column(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *line(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

static lv_obj_t *wrapping(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, "", role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

/* A label that takes what its row has left, and wraps there. */
static lv_obj_t *rest(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, "", role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, 1);
    lv_obj_set_flex_grow(lb, 1);
    return lb;
}

/* A figure in a column of its own, at the column's right edge. */
static lv_obj_t *figure(lv_obj_t *parent, const char *text, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, text, role);

    lv_obj_set_width(lb, FIG_W);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_RIGHT, 0);
    return lb;
}

/* ---- events ----------------------------------------------------------------------------- */

static int index_of(lv_event_t *e)
{
    return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
}

/* An observation's packet, in the inspector. A row whose packet has left
 * the buffer takes no tap (psk_evp_paint), and has nothing to open here. */
static void on_row(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int i = index_of(e);

    if (i >= 0 && (unsigned)i < PSK_EV_ROWS && a->ev_row_seq[i]) {
        psk_ev_packet(a, a->ev_row_seq[i]);
    }
}

static void on_list(lv_event_t *e)
{
    psk_ev_packets(lv_event_get_user_data(e));
}

static void on_close(lv_event_t *e)
{
    psk_evp_show(lv_event_get_user_data(e), false);
}

/* The mark before or after the one shown. */
static void on_step(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    const struct ppc_evt_ev_head *h = &a->model->ev.head;
    int step = index_of(e);

    if (!a->model->have_ev || !a->ev_fresh) {
        return;
    }
    if (step < 0 && h->id > h->first) {
        psk_ev_go(a, h->id - 1u);
    } else if (step > 0 && h->id < h->last) {
        psk_ev_go(a, h->id + 1u);
    }
}

/* ---- building --------------------------------------------------------------------------- */

/* An arrow to the mark before or after: a slab as the app's own back slab
 * is, with the chevron in the accent. */
static lv_obj_t *arrow(lv_obj_t *parent, const char *symbol, struct psk_app *a, int step)
{
    lv_obj_t *b = psk_box(parent);
    lv_obj_t *chev;

    lv_obj_set_size(b, POCKETUI_TOUCH_MIN, PSK_TAB_H);
    pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(b, (void *)(intptr_t)step);
    lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, a);
    chev = pocketui_label(b, symbol, POS_STYLE_SYMBOL);
    pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
    lv_obj_center(chev);
    return b;
}

/* One line of the counts: the protocol on its first line, the message, and
 * how many before the mark and after it. */
static void count_row(struct psk_app *a, lv_obj_t *parent, unsigned i)
{
    lv_obj_t *r = line(parent, 10);

    a->ev_count[i] = r;
    a->ev_count_cls[i] = pocketui_label(r, "", POS_STYLE_CAPTION);
    lv_obj_set_width(a->ev_count_cls[i], CLS_W);
    a->ev_count_name[i] = rest(r, POS_STYLE_CAPTION);
    a->ev_count_b[i] = figure(r, "", POS_STYLE_VALUE);
    a->ev_count_a[i] = figure(r, "", POS_STYLE_VALUE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
}

/* One observation: a slab to tap, with its protocol in its hue and when it
 * was seen, what was seen, what that means for what could be seen here, and
 * what the buffer still holds of the packet. */
static void obs_row(struct psk_app *a, lv_obj_t *parent, unsigned i)
{
    lv_obj_t *r = psk_box(parent);
    lv_obj_t *left;
    lv_obj_t *top;

    pos_style_add(r, POS_STYLE_SLAB, 0);
    pos_style_add(r, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, POCKETUI_RADIUS, 0);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, PSK_ROW_H, 0);
    lv_obj_set_style_pad_hor(r, 12, 0);
    lv_obj_set_style_pad_ver(r, 8, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(r, (void *)(intptr_t)i);
    lv_obj_add_event_cb(r, on_row, LV_EVENT_CLICKED, a);
    left = column(r, 2);
    lv_obj_set_width(left, 1);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
    top = line(left, 10);
    lv_obj_add_flag(top, LV_OBJ_FLAG_EVENT_BUBBLE);
    a->ev_row_cls[i] = pocketui_label(top, "", POS_STYLE_CAPTION);
    a->ev_row_when[i] = rest(top, POS_STYLE_CAPTION);
    a->ev_row_what[i] = wrapping(left, POS_STYLE_VALUE);
    a->ev_row_sub[i] = wrapping(left, POS_STYLE_TEXT_SECONDARY);
    a->ev_row_held[i] = wrapping(left, POS_STYLE_CAPTION);
    a->ev_row_chev[i] = pocketui_label(r, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
    pos_style_add(a->ev_row_chev[i], POS_STYLE_ACCENT_TEXT, 0);
    a->ev_row[i] = r;
    a->ev_row_can[i] = true;
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
}

void psk_evp_build(struct psk_app *a, lv_obj_t *page)
{
    lv_obj_t *head;
    lv_obj_t *p;
    lv_obj_t *r;
    lv_obj_t *b;
    unsigned s;
    unsigned i;

    a->ev_page = page;
    /* The page's head: which mark this is, the marks either side of it, and
     * the way back to LIVE. */
    head = line(page, PSK_GAP);
    a->ev_title = pocketui_label(head, "Mark", POS_STYLE_TITLE);
    lv_label_set_long_mode(a->ev_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->ev_title, 1);
    lv_obj_set_flex_grow(a->ev_title, 1);
    a->ev_prev = arrow(head, LV_SYMBOL_LEFT, a, -1);
    a->ev_next = arrow(head, LV_SYMBOL_RIGHT, a, 1);
    b = psk_button(head, "BACK", false, on_close, a);
    lv_obj_set_height(b, PSK_TAB_H);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, 120, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    a->ev_close = b;
    a->ev_when = wrapping(page, POS_STYLE_CAPTION);
    a->ev_wait = wrapping(page, POS_STYLE_TEXT_SECONDARY);

    a->ev_cols = column(page, PSK_PANEL_GAP);
    psk_titles_out(a->ev_cols);
    a->ev_l = column(a->ev_cols, PSK_PANEL_GAP);
    a->ev_r = column(a->ev_cols, PSK_PANEL_GAP);

    /* The two windows: how long each is, what ended it or keeps it open,
     * and how much of it the capture saw. */
    p = psk_panel(a->ev_l, "WHAT WAS WATCHED");
    lv_obj_set_style_pad_top(p, 18, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    r = line(p, PSK_GAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (s = 0; s < 2; s++) {
        lv_obj_t *c = column(r, 2);

        lv_obj_set_width(c, LV_PCT(48));
        a->ev_win[s] = c;
        a->ev_win_head[s] = wrapping(c, POS_STYLE_VALUE);
        a->ev_win_why[s] = wrapping(c, POS_STYLE_CAPTION);
        a->ev_win_seen[s] = wrapping(c, POS_STYLE_CAPTION);
    }
    /* What the capture let in: every claim above and below is of that. */
    a->ev_filter = wrapping(p, POS_STYLE_CAPTION);
    lv_obj_set_style_pad_top(a->ev_filter, 6, 0);

    /* What was observed, counted: a table of three protocols. */
    p = psk_panel(a->ev_l, "OBSERVED" DOT "MESSAGES COUNTED");
    a->ev_counts_panel = p;
    lv_obj_set_style_pad_top(p, 18, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    a->ev_nothing = wrapping(p, POS_STYLE_TEXT_SECONDARY);
    a->ev_counts = column(p, 6);
    r = line(a->ev_counts, 10);
    {
        lv_obj_t *gap = psk_box(r);

        lv_obj_set_size(gap, 1, 1);
        lv_obj_set_flex_grow(gap, 1);
    }
    figure(r, "BEFORE", POS_STYLE_CAPTION);
    figure(r, "AFTER", POS_STYLE_CAPTION);
    for (i = 0; i < PSK_EV_COUNTS; i++) {
        count_row(a, a->ev_counts, i);
    }

    /* The observations: before the mark, the mark, after it. */
    p = psk_panel(a->ev_r, "OBSERVATIONS" DOT "TAP FOR THE PACKET");
    a->ev_obs_panel = p;
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_top(p, 22, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    for (s = 0; s < 2; s++) {
        if (s == 1) {
            /* The mark itself, where it stands among them. */
            a->ev_at = wrapping(p, POS_STYLE_CAPTION);
            pos_style_add(a->ev_at, POS_STYLE_ACCENT_TEXT, 0);
            lv_obj_set_style_text_align(a->ev_at, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_ver(a->ev_at, 6, 0);
        }
        a->ev_side_cap[s] = wrapping(p, POS_STYLE_CAPTION);
        for (i = 0; i < PPC_EV_KEEP; i++) {
            obs_row(a, p, s * PPC_EV_KEEP + i);
        }
    }
    b = psk_button(p, "DHCP, ARP AND DNS IN PACKETS", false, on_list, a);
    lv_obj_set_height(b, PSK_TAB_H);
    lv_obj_set_style_margin_top(b, 6, 0);
    a->ev_list = b;

    /* What this can and cannot tell: last on the tall page, under the
     * counts on the wide one (psk_evp_shape). */
    p = psk_panel(a->ev_r, "WHAT THIS CAN AND CANNOT TELL");
    a->ev_limits_panel = p;
    lv_obj_set_style_pad_top(p, 18, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    a->ev_limits = wrapping(p, POS_STYLE_TEXT_SECONDARY);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    /* Off the active screen until it shows. */
    a->ev_park = lv_obj_create(NULL);
    lv_obj_remove_style_all(a->ev_park);
    lv_obj_set_parent(page, a->ev_park);
}

void psk_evp_place(struct psk_app *a)
{
    bool shown = psk_evp_shown(a);
    lv_obj_t *home = shown ? a->content : a->ev_park;

    if (lv_obj_get_parent(a->ev_page) != home) {
        lv_obj_set_parent(a->ev_page, home);
    }
    psk_set_hidden(a->ev_page, !shown);
}

void psk_evp_destroy(struct psk_app *a)
{
    if (a->ev_park) {
        lv_obj_delete(a->ev_park);
        a->ev_park = NULL;
    }
}

/* Tall: one column. Wide: the windows, the counts and the limits beside the
 * observations. The same objects, moved (M9). */
void psk_evp_shape(struct psk_app *a)
{
    bool w = a->wide;
    lv_obj_t *home = w ? a->ev_l : a->ev_r;

    lv_obj_set_flex_flow(a->ev_cols, w ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->ev_cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(a->ev_cols, PSK_PANEL_GAP, 0);
    if (w) {
        lv_obj_set_flex_grow(a->ev_l, 0);
        lv_obj_set_width(a->ev_l, PSK_LIVE_LEFT_W);
        lv_obj_set_width(a->ev_r, 1);
        lv_obj_set_flex_grow(a->ev_r, 1);
    } else {
        lv_obj_set_flex_grow(a->ev_l, 0);
        lv_obj_set_flex_grow(a->ev_r, 0);
        lv_obj_set_width(a->ev_l, LV_PCT(100));
        lv_obj_set_width(a->ev_r, LV_PCT(100));
    }
    if (lv_obj_get_parent(a->ev_limits_panel) != home) {
        lv_obj_set_parent(a->ev_limits_panel, home);
    }
    lv_obj_set_style_pad_top(a->ev_page, w ? 8 - PSK_TOP_REACH : 14, 0); /* less the top row's strip */
}

/* ---- painting --------------------------------------------------------------------------- */

/* An arrow with somewhere to go, or dimmed where there is nowhere. */
static void arrow_look(lv_obj_t *b, bool can)
{
    if (can != lv_obj_has_flag(b, LV_OBJ_FLAG_CLICKABLE)) {
        if (can) {
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_style_opa(lv_obj_get_child(b, 0), can ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void paint_row(struct psk_app *a, unsigned k, const struct psk_ev_row *r)
{
    /* A packet the buffer no longer holds cannot be opened: its row says so
     * in words, loses its chevron and takes no tap. One whose bytes alone
     * have left still opens, with what is left of it. */
    bool can = r->seq != 0 && r->alive != PPC_EV_GONE;
    int hue = psk_class_hue(r->cls);
    char b[96];
    char n[24];

    psk_set_hidden(a->ev_row[k], false);
    a->ev_row_seq[k] = can ? r->seq : 0;
    psk_text(a->ev_row_cls[k], psk_class_word(r->cls));
    psk_tone(a->ev_row_cls[k], hue >= 0 ? -10 - hue : -1, false);
    if (r->seq) {
        snprintf(b, sizeof(b), "%s" DOT "PACKET %s", r->when, psk_fmt_count(r->seq, n, sizeof(n)));
    } else {
        snprintf(b, sizeof(b), "%s", r->when);
    }
    psk_text(a->ev_row_when[k], b);
    psk_text(a->ev_row_what[k], r->what);
    psk_text(a->ev_row_sub[k], r->sub);
    psk_set_hidden(a->ev_row_sub[k], r->sub[0] == '\0');
    if (r->held[0] && !can) {
        /* status_warn always with `!` and a word (DS §59.2). */
        snprintf(b, sizeof(b), "! %s", r->held);
        psk_text(a->ev_row_held[k], b);
    } else {
        psk_text(a->ev_row_held[k], r->held);
    }
    psk_set_hidden(a->ev_row_held[k], r->held[0] == '\0');
    psk_tone(a->ev_row_held[k], can ? -1 : (int)POS_STYLE_STATUS_WARN_TEXT, false);
    if (can != a->ev_row_can[k]) {
        a->ev_row_can[k] = can;
        if (can) {
            lv_obj_add_flag(a->ev_row[k], LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_clear_flag(a->ev_row[k], LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_style_opa(a->ev_row_chev[k], can ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void psk_evp_paint(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    const struct psk_evidence *ev = &m->ev;
    bool have = m->have_ev && a->ev_fresh;
    struct psk_ev_row rows[PSK_EV_ROWS];
    struct psk_ev_count counts[PSK_EV_COUNTS];
    unsigned used[2] = { 0, 0 };
    char b[2048];
    char c[64];
    char d[256];
    unsigned n;
    unsigned i;
    unsigned s;

    if (!psk_evp_shown(a)) {
        return;
    }
    psk_set_hidden(a->ev_cols, !have);
    psk_set_hidden(a->ev_wait, have);
    if (!have) {
        /* The mark is set and its evidence is on its way: nothing is said
         * of it until it is here. */
        psk_text(a->ev_title, "Mark");
        psk_set_hidden(a->ev_when, true);
        psk_text(a->ev_wait, "Reading what the capture has seen.");
        psk_set_hidden(a->ev_prev, true);
        psk_set_hidden(a->ev_next, true);
        return;
    }
    psk_text(a->ev_title, psk_ev_title(ev, b, sizeof(b)));
    psk_text(a->ev_when, psk_ev_when(ev, b, sizeof(b)));
    psk_set_hidden(a->ev_when, false);
    {
        bool many = ev->head.last > ev->head.first;

        psk_set_hidden(a->ev_prev, !many);
        psk_set_hidden(a->ev_next, !many);
        arrow_look(a->ev_prev, ev->head.id > ev->head.first);
        arrow_look(a->ev_next, ev->head.id < ev->head.last);
    }

    /* The windows. */
    for (s = 0; s < 2; s++) {
        unsigned sev = psk_ev_window(ev, s, b, 64, c, sizeof(c), d, sizeof(d));

        psk_text(a->ev_win_head[s], b);
        psk_text(a->ev_win_why[s], c);
        psk_set_hidden(a->ev_win_why[s], c[0] == '\0');
        if (sev == PSK_SEV_WARN) {
            snprintf(b, sizeof(b), "! %s", d);
            psk_text(a->ev_win_seen[s], b);
        } else {
            psk_text(a->ev_win_seen[s], d);
        }
        psk_set_hidden(a->ev_win_seen[s], d[0] == '\0');
        psk_tone(a->ev_win_seen[s], sev == PSK_SEV_WARN ? (int)POS_STYLE_STATUS_WARN_TEXT
                                    : sev == PSK_SEV_OK ? (int)POS_STYLE_STATUS_OK_TEXT : -1,
                 false);
    }
    psk_text(a->ev_filter, psk_ev_filter(ev, b, sizeof(b)));

    /* The counts, or the one sentence for windows that hold nothing. */
    psk_ev_nothing(ev, b, sizeof(b));
    psk_text(a->ev_nothing, b);
    psk_set_hidden(a->ev_nothing, b[0] == '\0');
    psk_set_hidden(a->ev_counts, b[0] != '\0');
    n = b[0] ? 0 : psk_ev_counts(ev, counts, PSK_EV_COUNTS);
    for (i = 0; i < PSK_EV_COUNTS; i++) {
        bool on = i < n;
        int hue;

        psk_set_hidden(a->ev_count[i], !on);
        if (!on) {
            continue;
        }
        hue = psk_class_hue(counts[i].cls);
        psk_text(a->ev_count_cls[i], counts[i].first ? psk_class_word(counts[i].cls) : "");
        psk_tone(a->ev_count_cls[i], hue >= 0 ? -10 - hue : -1, false);
        psk_text(a->ev_count_name[i], counts[i].name);
        psk_text(a->ev_count_b[i], counts[i].before);
        psk_text(a->ev_count_a[i], counts[i].after);
    }

    /* The observations: each side's rows, oldest first. */
    n = psk_ev_rows(ev, rows, PSK_EV_ROWS);
    for (i = 0; i < n; i++) {
        s = rows[i].side ? 1 : 0;
        if (used[s] < PPC_EV_KEEP) {
            paint_row(a, s * PPC_EV_KEEP + used[s]++, &rows[i]);
        }
    }
    for (s = 0; s < 2; s++) {
        for (i = used[s]; i < PPC_EV_KEEP; i++) {
            a->ev_row_seq[s * PPC_EV_KEEP + i] = 0;
            psk_set_hidden(a->ev_row[s * PPC_EV_KEEP + i], true);
        }
        psk_text(a->ev_side_cap[s], psk_ev_side(ev, s, b, sizeof(b)));
    }
    snprintf(b, sizeof(b), "THE MARK" DOT "%s", psk_fmt_elapsed(ev->head.at_ms, c, sizeof(c)));
    psk_text(a->ev_at, b);
    /* The list of every DHCP, ARP and DNS packet, where the evidence
     * begins: offered while there is anything to list. */
    psk_set_hidden(a->ev_list, n == 0);

    psk_text(a->ev_limits, psk_ev_limits(ev, b, sizeof(b)));
}

void psk_evp_show(struct psk_app *a, bool on)
{
    if (on && (!psk_capture_present(a) || a->model->nmarks == 0)) {
        on = false;
    }
    if (on == a->ev_view) {
        if (on && a->cur != PSK_TAB_LIVE) {
            psk_select_tab(a, PSK_TAB_LIVE);
        }
        return;
    }
    a->ev_view = on;
    if (on) {
        a->net_view = false; /* one face of LIVE at a time */
        a->cur = PSK_TAB_LIVE;
        a->ev_due = true;
        lv_obj_scroll_to_y(a->ev_page, 0, LV_ANIM_OFF);
    }
    psk_paint_tabs(a);
    psk_evp_paint(a);
    psk_live_paint(a, PSK_CHANGED_STATE);
}

bool psk_evp_shown(const struct psk_app *a)
{
    return a->ev_view && a->cur == PSK_TAB_LIVE;
}
