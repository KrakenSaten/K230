/*
 * PacketShark's LIVE tab (setup, live status, the error panel), the tab row,
 * the capture bar and the PACKETS and INSPECT pages, M3 scope
 * (docs/apps/PACKETSHARK.md; the design is UI_UX.md §5 and DS §59, proposed).
 *
 * Only existing roles: panels, chips, key/value rows, buttons, captions and
 * values (tests/style_lint.sh). A label is written only when its text
 * changes, so a poll that brings nothing new costs nothing on the panel.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include <stdio.h>
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
static lv_obj_t *panel(lv_obj_t *parent, const char *title, lv_obj_t **title_out)
{
    lv_obj_t *p = pocketui_card(parent);
    lv_obj_t *t;

    lv_obj_set_width(p, LV_PCT(100));
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(p, 8, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    t = one_line(p, title, POS_STYLE_CAPTION);
    lv_obj_set_width(t, LV_PCT(100));
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

static void build_live(struct psk_app *a, lv_obj_t *p)
{
    lv_obj_t *traffic;
    lv_obj_t *r;
    lv_obj_t *left;
    lv_obj_t *right;
    lv_obj_t *session;

    a->live = column(p, PSK_PANEL_GAP);
    traffic = panel(a->live, "TRAFFIC", NULL);
    r = row(traffic, 12);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    left = column(r, 0);
    lv_obj_set_width(left, LV_SIZE_CONTENT);
    a->rate = one_line(left, "0", POS_STYLE_HERO_48);
    one_line(left, "PKT/S", POS_STYLE_CAPTION);
    right = column(r, 2);
    lv_obj_set_width(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    a->throughput = one_line(right, "0 BIT/S", POS_STYLE_VALUE);
    one_line(right, "THROUGHPUT", POS_STYLE_CAPTION);
    a->waiting = one_line(traffic, "WAITING FOR PACKETS", POS_STYLE_CAPTION);

    session = panel(a->live, "SESSION", NULL);
    r = row(session, 12);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    a->cell[0] = cell(r, "PACKETS");
    a->cell[1] = cell(r, "BYTES");
    a->cell[2] = cell(r, "DROPPED");
    a->cell[3] = cell(r, "BUFFER");
    a->detail = wrapping(session, "", POS_STYLE_CAPTION);
    a->later = wrapping(a->live,
                        "The traffic graph, the packet list, the inspector and SAVE are not in this "
                        "version yet. Leaving PacketShark discards the capture.",
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
            snprintf(text, sizeof(text), "%s · %u MB/S · %s", iface, (unsigned)m->speed, sz);
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
    char s1[24];
    char s2[24];
    char s3[24];
    char line[128];

    psk_text(a->rate, psk_fmt_count(c->pps, b, sizeof(b)));
    psk_text(a->throughput, psk_fmt_bits(c->bps, b, sizeof(b)));
    psk_set_hidden(a->waiting, c->seen > 0 || m->state == PPC_STATE_STOPPED);
    psk_text(a->cell[0], psk_fmt_count(c->seen, b, sizeof(b)));
    psk_text(a->cell[1], psk_fmt_bytes(c->bytes, b, sizeof(b)));
    psk_text(a->cell[2], psk_fmt_count(c->kdrop + c->ifdrop, b, sizeof(b)));
    tone(a->cell[2], c->kdrop + c->ifdrop ? POS_STYLE_STATUS_WARN_TEXT : POS_STYLE_STATUS_OK_TEXT);
    snprintf(b, sizeof(b), "%u%%", (unsigned)((c->fill_permille + 5u) / 10u));
    psk_text(a->cell[3], b);
    snprintf(line, sizeof(line), "STORED %s · HELD %s · SHED %s", psk_fmt_count(c->stored, s1, sizeof(s1)),
             psk_fmt_count(c->held, s2, sizeof(s2)), psk_fmt_count(c->shed, s3, sizeof(s3)));
    psk_text(a->detail, line);
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
    } else if (v == PSK_VIEW_ERROR) {
        paint_error(a);
    } else {
        psk_paint_ifaces(a);
        psk_paint_options(a);
    }
    paint_packets(a, v);
}
