/*
 * TRAFFIC. See rift_scope_view.h; what a path can be said to be is
 * rift_scope.h's, and this file only draws it and passes presses on.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_scope_view.h"

#include "pos_styles.h"
#include "rift_emoji.h"
#include "rift_emoji_style.h"
#include "rift_mapview.h"
#include "rift_route_view.h"
#include "rift_scope.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Packets in the list at a time; NEWER and OLDER page through the ring. */
#define ROWS 6
/* The panel beside the map in landscape, MAP's own panel's width; under it
 * in portrait, this share of MAP's height, whatever its words come to. */
#define PANEL_W_WIDE 400
#define PANEL_H_PCT 46
#define LINE_W 4
#define HOP_RING_PX 25
/* A projected point further out than this is not handed to the rasteriser. */
#define FAR_PX 30000

struct rift_scope_view {
    struct rift_app *app;
    lv_obj_t *parent; /* MAP's root */
    int wide;
    int on;

    lv_obj_t *panel;
    lv_obj_t *body; /* scrolled; the actions are under it */
    lv_obj_t *status;
    lv_obj_t *route_note;
    lv_obj_t *path_title;
    lv_obj_t *path_words;
    lv_obj_t *signal;
    lv_obj_t *node_title;
    lv_obj_t *node_words;
    lv_obj_t *node_detail;
    lv_obj_t *list_title;
    lv_obj_t *row[ROWS];
    lv_obj_t *row_text[ROWS];
    uint32_t row_uid[ROWS];
    lv_obj_t *list_note;
    lv_obj_t *detail;
    lv_obj_t *legend;
    lv_obj_t *bar_list;
    lv_obj_t *live;
    lv_obj_t *newer;
    lv_obj_t *older;
    lv_obj_t *bar_detail;
    lv_obj_t *show_path;
    lv_obj_t *to_list;
    lv_obj_t *to_route;
    lv_obj_t *button; /* MAP's TRAFFIC */
    int button_painted;

    /* The packet picked, copied: its detail and path stay put while the
     * ring moves on. 0 uid: none (LIVE). */
    struct rift_rx_entry picked;
    int have_picked;
    int detail_open;
    /* The list's place: the entry at the top of page 0 (0: the newest, so
     * the list follows the traffic), and the page under it. */
    uint32_t anchor;
    int page;
    int anchor_lost;
    int live_painted;

    int segments;
    int hops_drawn;
};

static void show(lv_obj_t *o, int on)
{
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *wrapping(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

/* Words that may carry a node's name, emoji folded as RIFT draws them. */
static void set_folded(lv_obj_t *label, const char *text)
{
    char folded[2400];

    rift_emoji_fold(text, folded, sizeof(folded));
    rift_label_set(label, folded);
}

static lv_obj_t *bar(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);

    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, RIFT_TOUCH_H);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

int rift_scope_view_on(const struct rift_app *app)
{
    const struct rift_scope_view *v = rift_map_view_scope(app);

    return v && v->on && app->section == RIFT_SEC_MAP;
}

/* The packet whose path the map draws: the one picked, or the live one. */
static const struct rift_rx_entry *drawn_packet(const struct rift_scope_view *v)
{
    if (v->have_picked) {
        return &v->picked;
    }
    return rift_scope_live(&v->app->model, &v->app->rxlog, rift_mono_ms());
}

static void go_live(struct rift_scope_view *v)
{
    v->have_picked = 0;
    v->detail_open = 0;
    v->anchor = 0;
    v->page = 0;
    v->anchor_lost = 0;
}

/* Hold the list where it is: from now on arrivals do not move it. */
static void hold(struct rift_scope_view *v)
{
    if (!v->anchor) {
        const struct rift_rx_entry *top = rift_rxlog_at(&v->app->rxlog, 0);

        v->anchor = top ? top->uid : 0;
    }
}

/* ---- presses ------------------------------------------------------------------ */

void rift_scope_view_set(struct rift_app *app, int on)
{
    struct rift_scope_view *v = rift_map_view_scope(app);

    if (!v || v->on == !!on) {
        return;
    }
    v->on = !!on;
    go_live(v);
    if (v->panel) {
        lv_obj_scroll_to_y(v->body, 0, LV_ANIM_OFF);
    }
    app->refresh_pending = 1;
    rift_app_refresh(app);
}

static void on_row(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    const struct rift_rx_entry *p = NULL;
    int i;

    for (i = 0; i < ROWS; i++) {
        if (v->row[i] == target) {
            p = rift_rxlog_find(&v->app->rxlog, v->row_uid[i]);
        }
    }
    if (!p) {
        return;
    }
    hold(v);
    v->picked = *p;
    v->have_picked = 1;
    v->detail_open = 1;
    lv_obj_scroll_to_y(v->body, 0, LV_ANIM_OFF);
    rift_app_refresh(v->app);
}

static void on_live(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);

    go_live(v);
    rift_app_refresh(v->app);
}

static void on_newer(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);

    if (v->page > 0) {
        v->page--;
    }
    rift_app_refresh(v->app);
}

static void on_older(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);

    hold(v);
    v->page++; /* held to the last page by the paint */
    rift_app_refresh(v->app);
}

static void on_to_list(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);

    v->detail_open = 0;
    rift_app_refresh(v->app);
}

/* The picked packet's located hops in view, at the closest zoom that holds
 * them. A reader's press: the only thing in TRAFFIC that moves the map. */
static void on_show_path(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);
    const struct rift_map_view *g = rift_map_view_geometry(v->app);
    int path[RIFT_RXLOG_PATH_MAX];
    int n;

    v->detail_open = 0;
    n = rift_scope_path(&v->app->model, &v->picked, path);
    if (v->have_picked && g && n > 0) {
        struct rift_map_view fitted;

        if (rift_map_fit_nodes(&fitted, &v->app->model, path, n, g->w, g->h) > 0) {
            rift_map_view_place(v->app, &fitted);
        }
    }
    rift_app_refresh(v->app);
}

static void on_node_detail(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);

    /* RIFT's own node detail, under NODES. TRAFFIC stays on for the return. */
    if (rift_app_selected(v->app)) {
        rift_app_show_section(v->app, RIFT_SEC_NODES);
        rift_app_open_detail(v->app, 1);
    }
}

static void on_to_route(lv_event_t *e)
{
    struct rift_scope_view *v = lv_event_get_user_data(e);

    rift_scope_view_set(v->app, 0);
}

int rift_scope_view_back(struct rift_app *app)
{
    struct rift_scope_view *v = rift_map_view_scope(app);

    if (!rift_scope_view_on(app)) {
        return 0;
    }
    if (v->detail_open) {
        v->detail_open = 0;
        rift_app_refresh(app);
    } else {
        rift_scope_view_set(app, 0);
    }
    return 1;
}

int rift_scope_view_key(struct rift_app *app, uint32_t key)
{
    return key == LV_KEY_ESC && rift_scope_view_back(app);
}

/* ---- building ------------------------------------------------------------------- */

static void build(struct rift_scope_view *v)
{
    int i;

    v->panel = rift_panel(v->parent, "TRAFFIC");
    v->body = lv_obj_create(v->panel);
    lv_obj_remove_style_all(v->body);
    lv_obj_set_width(v->body, LV_PCT(100));
    lv_obj_set_flex_grow(v->body, 1);
    lv_obj_set_flex_flow(v->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->body, 8, 0);
    lv_obj_set_scroll_dir(v->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->body, LV_SCROLLBAR_MODE_AUTO);
    v->status = wrapping(v->body, POS_STYLE_CAPTION);
    v->route_note = wrapping(v->body, POS_STYLE_CAPTION);
    pos_style_add(v->route_note, POS_STYLE_ACCENT_TEXT, 0);
    v->path_title = wrapping(v->body, POS_STYLE_ROW_TITLE);
    v->path_words = wrapping(v->body, POS_STYLE_CAPTION);
    v->signal = wrapping(v->body, POS_STYLE_CAPTION);
    v->detail = wrapping(v->body, POS_STYLE_TEXT_PRIMARY);
    rift_emoji_style_add(v->detail, POS_STYLE_TEXT_PRIMARY);
    v->list_title = wrapping(v->body, POS_STYLE_ROW_TITLE);
    for (i = 0; i < ROWS; i++) {
        lv_obj_t *b = lv_button_create(v->body);

        lv_obj_remove_style_all(b);
        pos_style_add(b, POS_STYLE_BUTTON_SECONDARY, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        pos_style_add(b, POS_STYLE_SELECTED, LV_STATE_CHECKED); /* the picked packet */
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(b, RIFT_TOUCH_H, 0);
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_set_style_pad_ver(b, 8, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, v);
        v->row_text[i] = wrapping(b, POS_STYLE_CAPTION);
        pos_style_add(v->row_text[i], POS_STYLE_TEXT_PRIMARY, 0);
        rift_emoji_style_add(v->row_text[i], POS_STYLE_CAPTION);
        v->row[i] = b;
    }
    v->list_note = wrapping(v->body, POS_STYLE_CAPTION);
    v->node_title = wrapping(v->body, POS_STYLE_ROW_TITLE);
    rift_emoji_style_add(v->node_title, POS_STYLE_ROW_TITLE);
    v->node_words = wrapping(v->body, POS_STYLE_CAPTION);
    v->node_detail = rift_action(v->body, "NODE DETAIL \xE2\x80\xBA", 0, 1, on_node_detail, v);
    lv_obj_set_flex_grow(v->node_detail, 0);
    lv_obj_set_width(v->node_detail, LV_PCT(100));
    v->legend = wrapping(v->body, POS_STYLE_TEXT_MUTED);
    lv_label_set_text(v->legend,
                      "Observed at this receiver from received packets. Hops are inferred "
                      "from node prefixes in the node list: unknown and colliding prefixes "
                      "stay unresolved, and a line joins only adjacent hops that each "
                      "resolved to one node with a location. A direct packet's path is the "
                      "route still ahead of it and a TRACE carries signal samples, so "
                      "neither is drawn. RSSI and SNR are this device's reception of the "
                      "packet, not of each link. This is not a verified route and is never "
                      "used to send: routes are chosen in COMMS with SET ROUTE. The packets "
                      "are this RIFT session's receive log, the newest 1000: kept while RIFT "
                      "runs in the background, gone with CLOSE RIFT or a restart of the shell, "
                      "a rotation included.");

    v->to_route = rift_action(v->panel, "BACK TO ROUTE", 1, 1, on_to_route, v);
    lv_obj_set_flex_grow(v->to_route, 0);
    lv_obj_set_width(v->to_route, LV_PCT(100));
    v->bar_list = bar(v->panel);
    v->live = rift_action(v->bar_list, "LIVE", 0, 1, on_live, v);
    v->newer = rift_action(v->bar_list, "NEWER", 0, 1, on_newer, v);
    v->older = rift_action(v->bar_list, "OLDER", 0, 1, on_older, v);
    v->bar_detail = bar(v->panel);
    v->to_list = rift_action(v->bar_detail, "\xE2\x80\xB9 LIST", 0, 1, on_to_list, v);
    v->show_path = rift_action(v->bar_detail, "SHOW PATH", 1, 1, on_show_path, v);
    lv_obj_add_flag(v->panel, LV_OBJ_FLAG_HIDDEN);
}

static void apply_shape(struct rift_scope_view *v)
{
    if (!v->panel) {
        return;
    }
    /* A fixed size, never its content's: what arrives in the panel scrolls
     * in its body and cannot move or shrink the map (rift_route_view.c). */
    if (v->wide) {
        lv_obj_set_size(v->panel, PANEL_W_WIDE, LV_PCT(100));
    } else {
        lv_obj_set_size(v->panel, LV_PCT(100), LV_PCT(PANEL_H_PCT));
    }
}

static void on_button(lv_event_t *e)
{
    struct rift_app *app = lv_event_get_user_data(e);

    /* A view of what was heard: reads the session's ring and nodes, asks
     * the service nothing, and keeps any route draft as it is. */
    rift_scope_view_set(app, !rift_scope_view_on(app));
}

lv_obj_t *rift_scope_view_button(struct rift_app *app, lv_obj_t *bar)
{
    lv_obj_t *b = rift_action(bar, "TRAFFIC", 0, 1, on_button, app);

    lv_obj_set_flex_grow(b, 0);
    return b;
}

struct rift_scope_view *rift_scope_view_create(struct rift_app *app, lv_obj_t *parent,
                                               lv_obj_t *button)
{
    struct rift_scope_view *v = calloc(1, sizeof(*v));

    if (!v) {
        return NULL;
    }
    v->app = app;
    v->parent = parent;
    v->button = button;
    v->button_painted = 0; /* rift_action made it secondary */
    v->live_painted = -1;
    /* The panel is built the first time TRAFFIC is turned on: the plain map
     * costs nothing more for it than its button. */
    return v;
}

void rift_scope_view_destroy(struct rift_scope_view *v)
{
    free(v); /* the objects are MAP's, deleted with it */
}

void rift_scope_view_shape(struct rift_scope_view *v, int wide)
{
    if (v) {
        v->wide = wide;
        apply_shape(v);
    }
}

/* ---- painting ------------------------------------------------------------------- */

/* The leading active state: primary while it is what the panel shows. */
static void paint_primary(lv_obj_t *b, int on, int *painted)
{
    if (!b || on == *painted) {
        return; /* a refresh every second restyles nothing nobody touched */
    }
    *painted = on;
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED), LV_STATE_PRESSED);
    lv_obj_remove_style(b, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
    lv_obj_remove_style(b, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
    pos_style_add(b, on ? POS_STYLE_BUTTON_PRIMARY : POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(b, on ? POS_STYLE_BUTTON_PRIMARY_PRESSED : POS_STYLE_SLAB_PRESSED,
                  LV_STATE_PRESSED);
}

static void paint_status(struct rift_scope_view *v)
{
    const struct rift_rxlog *log = &v->app->rxlog;
    struct rift_scope_stats st;
    char text[320];
    const char *link;

    rift_scope_stats(&v->app->model, log, rift_mono_ms(), &st);
    if (log->supported == 0) {
        link = "THIS MESHCORED KEEPS NO RECEIVE LOG";
    } else if (!rift_ipc_connected(&v->app->ipc) || log->lost) {
        link = "SERVICE LOST" RIFT_SEP "HELD PACKETS ONLY";
    } else {
        link = "LISTENING";
    }
    snprintf(text, sizeof(text),
             "%s" RIFT_SEP "%u RX" RIFT_SEP "%u REPEATS" RIFT_SEP "%u IN 1 MIN" RIFT_SEP
             "RING %d/%d",
             link, st.frames, st.duplicates, st.recent, log->count, RIFT_RXLOG_MAX);
    rift_label_set(v->status, text);
}

static void paint_path(struct rift_scope_view *v)
{
    const struct rift_model *m = &v->app->model;
    const struct rift_rx_entry *p = drawn_packet(v);
    int path[RIFT_RXLOG_PATH_MAX];
    struct rift_scope_hops h;
    char rssi[48];
    char snr[48];
    char text[200];
    int n = rift_scope_path(m, p, path);

    rift_label_set(v->path_title, v->have_picked ? "OBSERVED PATH" RIFT_SEP "PICKED PACKET"
                                                 : "OBSERVED PATH" RIFT_SEP "LIVE");
    if (!p) {
        rift_label_set(v->path_words, rift_map_located(m)
                                          ? "No flood path heard in the last 8 s."
                                          : "No node has a location yet: paths are counted, "
                                            "not drawn.");
        show(v->signal, 0);
        return;
    }
    if (n == 0) {
        rift_label_set(v->path_words,
                       (p->flags & RIFT_RXF_PATH_SNR)
                           ? "A TRACE: its path bytes are signal samples, not nodes."
                           : (p->route == RIFT_RXRT_DIRECT ||
                              p->route == RIFT_RXRT_TRANSPORT_DIRECT)
                                 ? "A direct packet: its path is the route still ahead "
                                   "of it, not one it travelled."
                                 : "No travelled path was reported for this packet.");
    } else {
        size_t len;

        /* What could not be placed is named whenever there is any of it;
         * a zero is left out to keep the line short. */
        rift_scope_hops_of(m, path, n, &h);
        len = (size_t)snprintf(text, sizeof(text), "%d HOPS" RIFT_SEP "%d RESOLVED", h.hops,
                               h.resolved);
        if (h.unknown && len < sizeof(text)) {
            len += (size_t)snprintf(text + len, sizeof(text) - len, RIFT_SEP "%d UNKNOWN",
                                    h.unknown);
        }
        if (h.ambiguous && len < sizeof(text)) {
            len += (size_t)snprintf(text + len, sizeof(text) - len, RIFT_SEP "%d AMBIGUOUS",
                                    h.ambiguous);
        }
        if (h.unlocated && len < sizeof(text)) {
            len += (size_t)snprintf(text + len, sizeof(text) - len, RIFT_SEP "%d NO LOCATION",
                                    h.unlocated);
        }
        if (len < sizeof(text)) {
            snprintf(text + len, sizeof(text) - len, RIFT_SEP "%d %s DRAWN", h.segments,
                     h.segments == 1 ? "LINE" : "LINES");
        }
        rift_label_set(v->path_words, text);
    }
    rift_rxlog_fmt_rssi(p, rssi, sizeof(rssi));
    rift_rxlog_fmt_snr(p, snr, sizeof(snr));
    snprintf(text, sizeof(text), "%s %s" RIFT_SEP "%s" RIFT_SEP "%s AT THIS DEVICE",
             rift_rxlog_type_word(p), v->have_picked ? "PICKED" : "HEARD", rssi, snr);
    rift_label_set(v->signal, text);
    show(v->signal, 1);
}

static void paint_list(struct rift_scope_view *v)
{
    const struct rift_rxlog *log = &v->app->rxlog;
    int offset = 0;
    int count;
    int start;
    int i;
    char text[RIFT_RXLOG_LINE_MAX + 64];

    if (v->anchor) {
        offset = rift_rxlog_position(log, RIFT_RXF_ALL, v->anchor);
        if (offset < 0) {
            /* A thousand newer packets pushed the held place out of the
             * ring: back to the newest, and the list says why. */
            v->anchor = 0;
            v->page = 0;
            v->anchor_lost = 1;
            offset = 0;
        }
    }
    count = log->count - offset;
    if (v->page * ROWS >= count) {
        v->page = count > 0 ? (count - 1) / ROWS : 0;
    }
    start = v->page * ROWS;
    snprintf(text, sizeof(text), "PACKETS %d-%d/%d%s", count ? start + 1 : 0,
             start + ROWS < count ? start + ROWS : count, count,
             v->anchor ? RIFT_SEP "HELD" : "");
    rift_label_set(v->list_title, text);
    for (i = 0; i < ROWS; i++) {
        const struct rift_rx_entry *p = start + i < count ? rift_rxlog_at(log, offset + start + i)
                                                          : NULL;
        char time[48];
        char state[48];
        char rssi[48];
        char snr[48];
        char hops[64] = "PATH --";

        show(v->row[i], p != NULL);
        v->row_uid[i] = p ? p->uid : 0;
        if (!p) {
            continue;
        }
        rift_rxlog_fmt_time(p, time, sizeof(time));
        if (p->kind == RIFT_RX_MARK) {
            snprintf(text, sizeof(text), "%s  %s", time, p->text);
        } else {
            rift_rxlog_state_word(p, state, sizeof(state));
            rift_rxlog_fmt_rssi(p, rssi, sizeof(rssi));
            rift_rxlog_fmt_snr(p, snr, sizeof(snr));
            if (p->flags & RIFT_RXF_PARSED) {
                snprintf(hops, sizeof(hops), "%s %u",
                         (p->flags & RIFT_RXF_PATH_SNR) ? "TRACE SAMPLES"
                         : (p->route == RIFT_RXRT_DIRECT || p->route == RIFT_RXRT_TRANSPORT_DIRECT)
                             ? "AHEAD"
                             : "HOPS",
                         p->path_hops);
            }
            snprintf(text, sizeof(text), "%s  %s  %s  %uB\n%s  %s  %s", time,
                     rift_rxlog_type_word(p), state, p->bytes, rssi, snr, hops);
        }
        if (v->have_picked && p->uid == v->picked.uid) {
            lv_obj_add_state(v->row[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(v->row[i], LV_STATE_CHECKED);
        }
        set_folded(v->row_text[i], text);
    }
    if (log->supported == 0) {
        rift_label_set(v->list_note, "TRAFFIC needs a meshcored that reports receptions.");
    } else if (count == 0) {
        rift_label_set(v->list_note, "Nothing received yet in this RIFT session.");
    } else if (v->anchor_lost) {
        rift_label_set(v->list_note, "The held place left the ring; showing the newest.");
    } else {
        rift_label_set(v->list_note, "");
    }
    show(v->list_note, lv_label_get_text(v->list_note)[0] != '\0');
    rift_action_set_enabled(v->newer, 0, v->page > 0);
    rift_action_set_enabled(v->older, 0, start + ROWS < count);
}

static void paint_node(struct rift_scope_view *v)
{
    const struct rift_node *n = rift_app_selected(v->app);
    char label[RIFT_LABEL_MAX];
    char text[160];

    show(v->node_title, n != NULL);
    show(v->node_words, n != NULL);
    /* NODES is another section, and leaving MAP is the route builder's
     * CANCEL: while a draft is kept, the detail is not offered from here. */
    show(v->node_detail, n != NULL && !v->app->model.route.draft.active);
    if (!n) {
        return;
    }
    rift_fmt_label(n, label, sizeof(label));
    snprintf(text, sizeof(text), "NODE %s", label);
    set_folded(v->node_title, text);
    snprintf(text, sizeof(text),
             "IN %u HELD OBSERVED PATHS, BY A UNIQUE PREFIX" RIFT_SEP "%s",
             rift_scope_node_packets(&v->app->model, &v->app->rxlog,
                                     (int)(n - v->app->model.nodes)),
             n->have_location ? "LOCATION AS ITS ADVERTS SAY" : "NO LOCATION");
    rift_label_set(v->node_words, text);
}

unsigned rift_scope_view_refresh(struct rift_scope_view *v)
{
    const struct rift_rx_entry *p;
    char text[1600];
    int keep_route;
    lv_obj_t *route_panel;
    unsigned stamp;

    if (!v) {
        return 0;
    }
    paint_primary(v->button, rift_scope_view_on(v->app), &v->button_painted);
    if (!rift_scope_view_on(v->app)) {
        if (v->panel) {
            show(v->panel, 0);
        }
        return 0;
    }
    if (!v->panel) {
        build(v);
        apply_shape(v);
    }
    show(v->panel, 1);
    /* A route draft kept under TRAFFIC: its panel is not shown either. */
    route_panel = rift_route_view_panel(rift_map_view_route(v->app));
    if (route_panel) {
        show(route_panel, 0);
    }
    keep_route = v->app->model.route.draft.active;
    show(v->route_note, keep_route);
    show(v->to_route, keep_route);
    if (keep_route) {
        rift_label_set(v->route_note, "ROUTE DRAFT KEPT, NOT SHOWN: the observed path below "
                                      "is not the route being built.");
    }
    paint_status(v);
    paint_path(v);
    show(v->detail, v->detail_open);
    show(v->list_title, !v->detail_open);
    show(v->bar_list, !v->detail_open);
    show(v->bar_detail, v->detail_open);
    if (v->detail_open) {
        int i;

        for (i = 0; i < ROWS; i++) {
            show(v->row[i], 0);
        }
        show(v->list_note, 0);
        rift_rxlog_fmt_detail(&v->picked, text, sizeof(text));
        set_folded(v->detail, text);
        rift_action_set_enabled(v->show_path, 1,
                                rift_scope_path(&v->app->model, &v->picked,
                                                (int[RIFT_RXLOG_PATH_MAX]){ 0 }) > 0);
    } else {
        paint_list(v);
    }
    paint_node(v);
    paint_primary(v->live, !v->have_picked && !v->anchor, &v->live_painted);
    p = drawn_packet(v);
    stamp = (p ? p->uid : 0) ^ ((unsigned)v->have_picked << 31) ^
            ((unsigned)v->app->model.seq << 20);
    return stamp ? stamp : 1; /* 0 is the stamp of TRAFFIC off */
}

/* ---- drawing -------------------------------------------------------------------- */

static void line(lv_layer_t *layer, double x1, double y1, double x2, double y2, lv_color_t c)
{
    lv_draw_line_dsc_t d;

    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = LINE_W;
    d.opa = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = (lv_value_precise_t)x1;
    d.p1.y = (lv_value_precise_t)y1;
    d.p2.x = (lv_value_precise_t)x2;
    d.p2.y = (lv_value_precise_t)y2;
    lv_draw_line(layer, &d);
}

static void ring(lv_layer_t *layer, int32_t cx, int32_t cy, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_area_t a = { cx - HOP_RING_PX / 2, cy - HOP_RING_PX / 2, cx + HOP_RING_PX / 2,
                    cy + HOP_RING_PX / 2 };

    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_TRANSP;
    d.border_width = 3;
    d.border_color = c;
    d.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &d, &a);
}

void rift_scope_view_draw(struct rift_scope_view *v, lv_layer_t *layer, const lv_area_t *ar,
                          const struct rift_map_view *view)
{
    const struct rift_model *m;
    const struct rift_rx_entry *p;
    lv_color_t ink;
    int path[RIFT_RXLOG_PATH_MAX];
    int n;
    int i;

    if (!v) {
        return;
    }
    v->segments = 0;
    v->hops_drawn = 0;
    if (!rift_scope_view_on(v->app)) {
        return;
    }
    m = &v->app->model;
    p = drawn_packet(v);
    n = rift_scope_path(m, p, path);
    ink = pos_theme_color(POS_COLOR_RADIO_RX);
    /* Adjacent hops, each resolved to one located node: never across an
     * unknown, ambiguous or unlocated hop, and never to this device or to
     * the sender, whose places the path does not give. */
    for (i = 1; i < n; i++) {
        const struct rift_node *a;
        const struct rift_node *b;
        double x1;
        double y1;
        double x2;
        double y2;

        if (path[i - 1] < 0 || path[i] < 0) {
            continue;
        }
        a = &m->nodes[path[i - 1]];
        b = &m->nodes[path[i]];
        if (!a->have_location || !b->have_location) {
            continue;
        }
        rift_map_project(view, a->lat, a->lon, &x1, &y1);
        rift_map_project(view, b->lat, b->lon, &x2, &y2);
        if (fabs(x1) > FAR_PX || fabs(y1) > FAR_PX || fabs(x2) > FAR_PX || fabs(y2) > FAR_PX) {
            continue;
        }
        line(layer, ar->x1 + x1, ar->y1 + y1, ar->x1 + x2, ar->y1 + y2, ink);
        v->segments++;
    }
    for (i = 0; i < n; i++) {
        const struct rift_node *a;
        double x;
        double y;

        if (path[i] < 0 || !m->nodes[path[i]].have_location) {
            continue;
        }
        a = &m->nodes[path[i]];
        rift_map_project(view, a->lat, a->lon, &x, &y);
        if (x < -HOP_RING_PX || y < -HOP_RING_PX || x > view->w + HOP_RING_PX ||
            y > view->h + HOP_RING_PX) {
            continue;
        }
        ring(layer, ar->x1 + (int32_t)x, ar->y1 + (int32_t)y, ink);
        v->hops_drawn++;
    }
}

/* ---- for the tests -------------------------------------------------------------- */

lv_obj_t *rift_scope_view_panel(const struct rift_scope_view *v)
{
    return v ? v->panel : NULL;
}

lv_obj_t *rift_scope_view_row(const struct rift_scope_view *v, int i)
{
    return v && i >= 0 && i < ROWS ? v->row[i] : NULL;
}

const struct rift_rx_entry *rift_scope_view_picked(const struct rift_scope_view *v)
{
    return v && v->have_picked ? &v->picked : NULL;
}

int rift_scope_view_detail_open(const struct rift_scope_view *v)
{
    return v ? v->detail_open : 0;
}

int rift_scope_view_drawn_segments(const struct rift_scope_view *v)
{
    return v ? v->segments : 0;
}

int rift_scope_view_drawn_hops(const struct rift_scope_view *v)
{
    return v ? v->hops_drawn : 0;
}
