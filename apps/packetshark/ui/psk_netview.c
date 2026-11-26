/*
 * PacketShark's network overview (M12.1, docs/apps/PACKETSHARK.md §4c, §7):
 * a second face of LIVE, reached from its NETWORK line. What the capture
 * has learnt of the network itself - how many devices, DHCP servers and
 * address pairs; the findings worth a look, each a tap to its evidence; the
 * DHCP, ARP, DNS and TCP diagnosis, each a tap to its sheet; and the devices
 * that sent the most, each a tap to its packets.
 *
 * It is a page, not a sheet: the tabs, the capture's state and its HOLD and
 * STOP stay where they are (ADR-016 rule 13). Only existing roles; a label
 * is written only when its text changes; the lists are asked for once a
 * second while the page shows and never otherwise (psk_app.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"
#include "psk_net.h"

#include <stdio.h>
#include <string.h>

/* The diagnosis lines, in the order the page shows them. */
static const uint8_t nv_kind[PSK_NV_DIAGS] = { PSK_DIAG_DHCP, PSK_DIAG_ARP, PSK_DIAG_DNS, PSK_DIAG_TCP };
static const uint8_t nv_class[PSK_NV_DIAGS] = { PPC_CLASS_DHCP, PPC_CLASS_ARP, PPC_CLASS_DNS, PPC_CLASS_TCP };

static lv_obj_t *column(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = psk_box(parent);

    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

static lv_obj_t *wrapping(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, "", role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    return lb;
}

/* One line that shortens with dots: a dots-mode label with an automatic
 * height wraps, so its height is its role's line. */
static lv_obj_t *one_line(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *lb = pocketui_label(parent, "", role);

    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, LV_PCT(100));
    lv_obj_set_height(lb, pocketui_role_line_height(role));
    return lb;
}

/* A row to tap: a slab with its text in a column and a chevron. Returns the
 * column. */
static lv_obj_t *slab_row(lv_obj_t *parent, lv_obj_t **row_out, lv_event_cb_t cb, struct psk_app *a, int index)
{
    lv_obj_t *r = psk_box(parent);
    lv_obj_t *left;
    lv_obj_t *chev;

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
    lv_obj_set_user_data(r, (void *)(intptr_t)index);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, a);
    left = column(r, 2);
    lv_obj_set_width(left, 1);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
    chev = pocketui_label(r, LV_SYMBOL_RIGHT, POS_STYLE_SYMBOL);
    pos_style_add(chev, POS_STYLE_ACCENT_TEXT, 0);
    *row_out = r;
    return left;
}

/* ---- events ----------------------------------------------------------------------------- */

static int index_of(lv_event_t *e)
{
    return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
}

/* A finding's evidence: the event's own sheet, or the DNS or TCP diagnosis
 * the line was taken from. */
static void on_find(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int i = index_of(e);

    if (i < 0 || (unsigned)i >= a->nv_nfinds) {
        return;
    }
    if (a->nv_finds[i].kind == PSK_DIAG_FINDING) {
        a->ds_event = a->nv_finds[i].ev;
    }
    psk_dsheet_show(a, a->nv_finds[i].kind);
}

static void on_diag(lv_event_t *e)
{
    int i = index_of(e);

    if (i >= 0 && i < PSK_NV_DIAGS) {
        psk_dsheet_show(lv_event_get_user_data(e), nv_kind[i]);
    }
}

/* A device's way to its packets: the list filtered on its address. */
static void on_host(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);
    int i = index_of(e);
    char host[PPC_HOST_TEXT];

    if (i < 0 || (uint32_t)i >= a->model->net.nhosts || !psk_capture_present(a)) {
        return;
    }
    snprintf(host, sizeof(host), "%s", psk_net_host_filter(a->model, (unsigned)i));
    if (!host[0]) {
        return; /* no address known: nothing its packets could be listed by */
    }
    psk_set_view(a, PPC_CLASSES_ALL, host, 0, false);
    psk_select_tab(a, PSK_TAB_PACKETS);
}

static void on_close(lv_event_t *e)
{
    psk_net_show(lv_event_get_user_data(e), false);
}

/* ---- building --------------------------------------------------------------------------- */

void psk_net_build(struct psk_app *a, lv_obj_t *page)
{
    static const char *const cap[PSK_NV_CELLS] = { "DEVICES", "DHCP SERVERS", "ARP PAIRS", "TO CHECK" };
    lv_obj_t *head;
    lv_obj_t *p;
    lv_obj_t *grid;
    lv_obj_t *b;
    int i;

    a->nv_page = page;
    /* The page's head: where this is, and the way back to LIVE. */
    head = psk_box(page);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    {
        lv_obj_t *t = pocketui_label(head, "Network overview", POS_STYLE_TITLE);

        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, 1);
        lv_obj_set_flex_grow(t, 1);
    }
    b = psk_button(head, "BACK", false, on_close, a);
    lv_obj_set_height(b, PSK_TAB_H);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(b, 120, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    a->nv_close = b;
    a->nv_note = wrapping(page, POS_STYLE_TEXT_SECONDARY);

    a->nv_cols = column(page, PSK_PANEL_GAP);
    psk_titles_out(a->nv_cols);
    a->nv_l = column(a->nv_cols, PSK_PANEL_GAP);
    a->nv_r = column(a->nv_cols, PSK_PANEL_GAP);

    /* What was seen, in four figures and one line. */
    p = psk_panel(a->nv_l, "SEEN ON THIS INTERFACE");
    lv_obj_set_style_pad_top(p, 18, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    grid = psk_box(p);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, PSK_GAP, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    for (i = 0; i < PSK_NV_CELLS; i++) {
        lv_obj_t *c = column(grid, 2);
        lv_obj_t *k = pocketui_label(c, cap[i], POS_STYLE_CAPTION);

        lv_obj_set_width(c, LV_PCT(48));
        lv_label_set_long_mode(k, LV_LABEL_LONG_DOT);
        lv_obj_set_width(k, LV_PCT(100));
        lv_obj_set_height(k, pocketui_role_line_height(POS_STYLE_CAPTION));
        a->nv_cell[i] = one_line(c, POS_STYLE_VALUE);
        a->nv_cellbox[i] = c;
    }
    a->nv_mix = wrapping(p, POS_STYLE_CAPTION);

    /* The findings: the helper's DHCP and ARP events and what the DNS and
     * TCP diagnosis calls worth a look, warnings first. */
    p = psk_panel(a->nv_l, "FINDINGS \xc2\xb7 TAP FOR EVIDENCE");
    a->nv_finds_panel = p;
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_top(p, 22, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    a->nv_find_none = wrapping(p, POS_STYLE_CAPTION);
    for (i = 0; i < PSK_NV_FINDS; i++) {
        lv_obj_t *left = slab_row(p, &a->nv_find[i], on_find, a, i);

        a->nv_find_text[i] = wrapping(left, POS_STYLE_VALUE);
        a->nv_find_sub[i] = wrapping(left, POS_STYLE_CAPTION);
        a->nv_find_tone[i] = -1;
        lv_obj_add_flag(a->nv_find[i], LV_OBJ_FLAG_HIDDEN);
    }
    a->nv_find_more = wrapping(p, POS_STYLE_CAPTION);

    /* DHCP, ARP, DNS and TCP: a word for how each reads, and its summary. */
    p = psk_panel(a->nv_r, "DIAGNOSIS \xc2\xb7 TAP TO OPEN");
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_top(p, 22, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    for (i = 0; i < PSK_NV_DIAGS; i++) {
        lv_obj_t *left = slab_row(p, &a->nv_diag[i], on_diag, a, i);
        lv_obj_t *top = psk_box(left);
        int hue = psk_class_hue(nv_class[i]);
        lv_obj_t *w;

        lv_obj_set_width(top, LV_PCT(100));
        lv_obj_set_height(top, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(top, 10, 0);
        lv_obj_add_flag(top, LV_OBJ_FLAG_EVENT_BUBBLE);
        w = pocketui_label(top, psk_class_word(nv_class[i]), POS_STYLE_CAPTION);
        if (hue >= 0) {
            lv_obj_add_style(w, pos_style_identity((unsigned)hue), 0);
        }
        a->nv_diag_sev[i] = pocketui_label(top, "OK", POS_STYLE_CAPTION);
        a->nv_diag_text[i] = one_line(left, POS_STYLE_CAPTION);
    }

    /* The devices that sent the most, as far as this interface saw. */
    p = psk_panel(a->nv_r, "DEVICES \xc2\xb7 BY BYTES SENT");
    lv_obj_set_style_pad_hor(p, 14, 0);
    lv_obj_set_style_pad_top(p, 22, 0);
    lv_obj_set_style_pad_bottom(p, 14, 0);
    a->nv_host_none = wrapping(p, POS_STYLE_CAPTION);
    for (i = 0; i < PSK_NV_HOSTS; i++) {
        lv_obj_t *left = slab_row(p, &a->nv_host[i], on_host, a, i);

        a->nv_host_l1[i] = one_line(left, POS_STYLE_VALUE);
        a->nv_host_l2[i] = one_line(left, POS_STYLE_CAPTION);
        /* Its share of the bytes, as a line under it: a fill role in the
         * accent, as the tabs' underline is. */
        a->nv_host_bar[i] = psk_box(left);
        pos_style_add(a->nv_host_bar[i], POS_STYLE_BUTTON_PRIMARY, 0);
        lv_obj_set_style_radius(a->nv_host_bar[i], 0, 0);
        lv_obj_set_size(a->nv_host_bar[i], 2, 3);
        a->nv_host_share[i] = -1;
        lv_obj_add_flag(a->nv_host[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

/* Tall: one column. Wide: the figures and findings beside the diagnosis and
 * the devices. The same objects, moved (M9). */
void psk_net_shape(struct psk_app *a)
{
    bool w = a->wide;
    lv_obj_set_flex_flow(a->nv_cols, w ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->nv_cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(a->nv_cols, PSK_PANEL_GAP, 0);
    if (w) {
        lv_obj_set_flex_grow(a->nv_l, 0);
        lv_obj_set_width(a->nv_l, PSK_LIVE_LEFT_W);
        lv_obj_set_width(a->nv_r, 1);
        lv_obj_set_flex_grow(a->nv_r, 1);
    } else {
        lv_obj_set_flex_grow(a->nv_l, 0);
        lv_obj_set_flex_grow(a->nv_r, 0);
        lv_obj_set_width(a->nv_l, LV_PCT(100));
        lv_obj_set_width(a->nv_r, LV_PCT(100));
    }
    lv_obj_set_style_pad_top(a->nv_page, w ? 8 : 14, 0);
}

/* A one-line label is as high as its role's line at the text size in force:
 * set where it differs, so a change of text size is followed at the next
 * paint and a label is never cut by the height it was built with. */
static void line_h(lv_obj_t *lb, enum pos_style_role role)
{
    int32_t h = pocketui_role_line_height(role);

    if (lb && lv_obj_get_style_height(lb, LV_PART_MAIN) != h) {
        lv_obj_set_height(lb, h);
    }
}

static void fit_lines(struct psk_app *a)
{
    int i;

    a->nv_line_h = pocketui_role_line_height(POS_STYLE_VALUE);
    for (i = 0; i < PSK_NV_CELLS; i++) {
        line_h(lv_obj_get_child(a->nv_cellbox[i], 0), POS_STYLE_CAPTION);
        line_h(a->nv_cell[i], POS_STYLE_VALUE);
    }
    for (i = 0; i < PSK_NV_DIAGS; i++) {
        line_h(a->nv_diag_text[i], POS_STYLE_CAPTION);
    }
    for (i = 0; i < PSK_NV_HOSTS; i++) {
        line_h(a->nv_host_l1[i], POS_STYLE_VALUE);
        line_h(a->nv_host_l2[i], POS_STYLE_CAPTION);
    }
}

/* ---- painting --------------------------------------------------------------------------- */

static void sev_word(lv_obj_t *lb, unsigned sev)
{
    psk_text(lb, sev == PSK_SEV_WARN ? "! CHECK" : sev == PSK_SEV_NOTE ? "NOTE" : "OK");
    psk_tone(lb, sev == PSK_SEV_WARN ? (int)POS_STYLE_STATUS_WARN_TEXT : sev == PSK_SEV_OK ? (int)POS_STYLE_STATUS_OK_TEXT : -1,
             false);
}

void psk_net_paint(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    const struct ppc_net_totals *t = &m->nt;
    struct psk_find all[PPC_NET_WIRE_EVENTS + 2 * PSK_DIAG_LINES];
    char b[512];
    char l1[128];
    char l2[128];
    unsigned total;
    unsigned i;

    if (!psk_net_shown(a)) {
        return;
    }
    fit_lines(a);
    psk_text(a->nv_note, psk_net_confidence(m, PSK_DIAG_NET, b, sizeof(b)));
    psk_text(a->nv_cell[0], psk_fmt_count(t->hosts, b, sizeof(b)));
    psk_text(a->nv_cell[1], psk_fmt_count(t->dhcp_servers, b, sizeof(b)));
    psk_tone(a->nv_cell[1], t->dhcp_servers >= 2 ? (int)POS_STYLE_STATUS_WARN_TEXT : -1, false);
    psk_text(a->nv_cell[2], psk_fmt_count(t->arp_binds, b, sizeof(b)));
    psk_text(a->nv_cell[3], psk_fmt_count(t->events_warn, b, sizeof(b)));
    psk_tone(a->nv_cell[3], t->events_warn ? (int)POS_STYLE_STATUS_WARN_TEXT : (int)POS_STYLE_STATUS_OK_TEXT, false);
    psk_text(a->nv_mix, psk_net_mix(m, b, sizeof(b)));

    /* The findings: as many as the page has rows for, and how many more. */
    total = psk_findings(m, all, sizeof(all) / sizeof(all[0]));
    a->nv_nfinds = total < PSK_NV_FINDS ? total : PSK_NV_FINDS;
    for (i = 0; i < PSK_NV_FINDS; i++) {
        bool on = i < a->nv_nfinds;

        psk_set_hidden(a->nv_find[i], !on);
        if (!on) {
            continue;
        }
        a->nv_finds[i] = all[i];
        /* status_warn always with `!` and a word (DS §59.2). */
        snprintf(b, sizeof(b), "%s%s", all[i].sev == PSK_SEV_WARN ? "! " : "", all[i].text);
        psk_text(a->nv_find_text[i], b);
        psk_text(a->nv_find_sub[i], all[i].sub);
        if (a->nv_find_tone[i] != (int)all[i].sev) {
            a->nv_find_tone[i] = (int)all[i].sev;
            psk_tone(a->nv_find_text[i], all[i].sev == PSK_SEV_WARN ? (int)POS_STYLE_STATUS_WARN_TEXT : -1, false);
        }
    }
    psk_set_hidden(a->nv_find_none, total != 0);
    psk_text(a->nv_find_none, m->have_net_lists
                                  ? "Nothing to report. A finding listed here is worth a look, not proof of a fault."
                                  : "Reading what the capture has seen.");
    psk_set_hidden(a->nv_find_more, total <= PSK_NV_FINDS);
    if (total > PSK_NV_FINDS) {
        snprintf(b, sizeof(b), "AND %u MORE \xc2\xb7 THE DIAGNOSIS BELOW COUNTS THEM", total - PSK_NV_FINDS);
        psk_text(a->nv_find_more, b);
    }

    for (i = 0; i < PSK_NV_DIAGS; i++) {
        unsigned sev = psk_diag_summary(m, nv_kind[i], b, 96);

        psk_text(a->nv_diag_text[i], b);
        sev_word(a->nv_diag_sev[i], sev);
    }

    for (i = 0; i < PSK_NV_HOSTS; i++) {
        bool on = i < m->net.nhosts;
        int share;

        psk_set_hidden(a->nv_host[i], !on);
        if (!on) {
            continue;
        }
        {
            /* A device with an address lists the packets to and from it;
             * one without has no chevron and takes no tap. */
            bool can = psk_net_host_filter(m, i)[0] != '\0';
            lv_obj_t *chev = lv_obj_get_child(a->nv_host[i], 1);

            if (can != lv_obj_has_flag(a->nv_host[i], LV_OBJ_FLAG_CLICKABLE)) {
                if (can) {
                    lv_obj_add_flag(a->nv_host[i], LV_OBJ_FLAG_CLICKABLE);
                } else {
                    lv_obj_clear_flag(a->nv_host[i], LV_OBJ_FLAG_CLICKABLE);
                }
            }
            lv_obj_set_style_opa(chev, can ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        psk_net_host_lines(m, i, l1, sizeof(l1), l2, sizeof(l2));
        psk_text(a->nv_host_l1[i], l1);
        psk_text(a->nv_host_l2[i], l2);
        share = (int)((psk_net_host_share(m, i) + 5u) / 10u);
        if (share != a->nv_host_share[i]) {
            a->nv_host_share[i] = share;
            lv_obj_set_width(a->nv_host_bar[i], LV_PCT(share < 1 ? 1 : share > 100 ? 100 : share));
        }
    }
    psk_set_hidden(a->nv_host_none, m->net.nhosts != 0);
    psk_text(a->nv_host_none, m->have_net_lists ? "No device has been seen sending yet." : "Reading what the capture has seen.");
}

void psk_net_show(struct psk_app *a, bool on)
{
    if (on && !psk_capture_present(a)) {
        on = false;
    }
    if (on == a->net_view) {
        return;
    }
    a->net_view = on;
    a->net_due = on;
    if (on) {
        a->ev_view = false; /* one face of LIVE at a time */
        a->cur = PSK_TAB_LIVE;
        lv_obj_scroll_to_y(a->nv_page, 0, LV_ANIM_OFF);
    }
    psk_paint_tabs(a);
    psk_net_paint(a);
    psk_live_paint(a, PSK_CHANGED_STATE);
}

bool psk_net_shown(const struct psk_app *a)
{
    return a->net_view && a->cur == PSK_TAB_LIVE;
}
