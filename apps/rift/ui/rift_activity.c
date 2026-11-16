/*
 * ACTIVITY. See rift_activity.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_activity.h"

#include "pos_styles.h"
#include "rift_graph.h"
#include "rift_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough of each to be information, few enough that the panels fit the
 * portrait body without the page becoming a log. */
#define HEARD_ROWS 6
#define FEED_ROWS 8
/* The window the mesh's recent traffic is counted over. */
#define TRAFFIC_WINDOW_MS (5 * 60 * 1000LL)
/* The figures over the graph, and how many share a row. */
enum { TILE_RATE = 0, TILE_SPAN, TILE_LAST, TILE_RSSI, TILE_NODES, TILE_SENT, TILE_COUNT };
#define TILES_PER_ROW_WIDE 6
#define TILES_PER_ROW 3
#define TILE_GAP 8
/* The types the legend names even when none was heard, so it is never
 * empty: the three a reader looks for first. */
#define LEGEND_ALWAYS(t) ((t) == RIFT_AIR_ADVERT || (t) == RIFT_AIR_GRP_TXT || (t) == RIFT_AIR_TXT_MSG)

struct stat_tile {
    lv_obj_t *box;
    lv_obj_t *value;
    lv_obj_t *label;
};

struct legend_chip {
    lv_obj_t *box;
    lv_obj_t *count;
    unsigned drawn; /* the count it says, + 1; 0: nothing said yet */
};

struct heard_row {
    lv_obj_t *row;
    lv_obj_t *glyph;
    lv_obj_t *name;
    lv_obj_t *state;
    lv_obj_t *rssi;
    lv_obj_t *age;
};

struct feed_row {
    lv_obj_t *row;
    lv_obj_t *swatch; /* the frame's type, in the graph's colour */
    lv_obj_t *age;
    lv_obj_t *kind;
    lv_obj_t *detail;
};

struct rift_activity_view {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *split;
    lv_obj_t *col[2];

    lv_obj_t *svc_state;
    lv_obj_t *svc_reason;
    lv_obj_t *svc_radio; /* radiod, its state, the lease, and transmit: one line */
    lv_obj_t *svc_self; /* this node's name and hash: status, not a setting */
    lv_obj_t *svc_nodes;
    lv_obj_t *svc_build;
    lv_obj_t *svc_fault;

    lv_obj_t *svc_traffic;

    lv_obj_t *heard_note;
    struct heard_row heard[HEARD_ROWS];
    /* MESH ACTIVITY (rift_graph.h): what the graph counts on one line, the
     * figures, the live graph itself, the mix of types under it and the
     * legend that names them - a tap on a name shows that type alone. */
    lv_obj_t *graph_caption;
    lv_obj_t *tiles;
    struct stat_tile tile[TILE_COUNT];
    lv_obj_t *graph;
    lv_obj_t *mix;
    lv_obj_t *legend;
    struct legend_chip chip[RIFT_AIR_TYPES];
    lv_obj_t *span; /* the action that turns the graph's span over */
    lv_obj_t *feed_panel;
    int feed_wide; /* which column the feed is in: -1 not placed yet */
    lv_obj_t *feed_note;
    struct feed_row feed[FEED_ROWS];
    lv_obj_t *rxlog; /* the way into RX LOG */

    enum pos_style_role state_role;
};

static lv_obj_t *dense(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, RIFT_ROW_H);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t *wrapping(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_remove_style_all(label);
    pos_style_add(label, role, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 20, 0);
    /* Room for the first panel's caption, which rises above the panel into
     * whatever is over it: a column clips its children to its own box, so
     * the room has to be the column's (rift_panel). Taken from the padding
     * above, so the panels sit where they did. */
    lv_obj_set_style_pad_top(c, rift_caption_overhang(), 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void build_service(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "RADIO SERVICE");

    /* The state word carries the state; the colour only agrees with it
     * (DS §2). The four roles it swaps between are colour-only apart from
     * the body font they all share, so swapping one for another changes the
     * colour and nothing else. */
    v->svc_state = lv_label_create(panel);
    lv_obj_remove_style_all(v->svc_state);
    pos_style_add(v->svc_state, POS_STYLE_TEXT_MUTED, 0);
    lv_label_set_text(v->svc_state, "");
    v->state_role = POS_STYLE_TEXT_MUTED;
    v->svc_reason = wrapping(panel, POS_STYLE_TEXT_SECONDARY);
    v->svc_radio = wrapping(panel, POS_STYLE_CAPTION);
    /* Who this node is on the air, read only: renaming and adverts are
     * SYSTEM's (DEVICE). */
    v->svc_self = pocketui_kv_row(panel, "This node", RIFT_UNKNOWN);
    v->svc_nodes = pocketui_kv_row(panel, "Nodes held", RIFT_UNKNOWN);
    v->svc_build = pocketui_kv_row(panel, "Service", RIFT_UNKNOWN);
    /* The service's own counts of what it put on the air and what it heard,
     * in its own words: a transmit that went out is not one that failed. A
     * caption line rather than a key/value row: a row's value is cut with an
     * ellipsis at 60 % of the width, and a count cut short is a wrong count. */
    v->svc_traffic = wrapping(panel, POS_STYLE_CAPTION);
    v->svc_fault = wrapping(panel, POS_STYLE_STATUS_WARN_TEXT);
}

static void build_heard(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "RECENTLY HEARD");
    int i;

    for (i = 0; i < HEARD_ROWS; i++) {
        struct heard_row *r = &v->heard[i];

        r->row = dense(panel, 8);
        r->glyph = rift_glyph_create(r->row);
        r->name = rift_cell(r->row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(r->name, 1);
        r->state = rift_cell(r->row, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_RIGHT);
        r->rssi = rift_cell(r->row, POS_STYLE_CAPTION, 52, LV_TEXT_ALIGN_RIGHT);
        r->age = rift_cell(r->row, POS_STYLE_CAPTION, 40, LV_TEXT_ALIGN_RIGHT);
    }
    v->heard_note = wrapping(panel, POS_STYLE_TEXT_MUTED);
}

static void on_rxlog(lv_event_t *e)
{
    rift_app_show_section(lv_event_get_user_data(e), RIFT_SEC_RXLOG);
}

/* ---- MESH ACTIVITY ---------------------------------------------------------- */

static void paint_span(struct rift_activity_view *v)
{
    int five = rift_traffic_graph_span(v->graph) == RIFT_GRAPH_SPAN_5MIN;

    /* The action says what a press gives, as every action here does. */
    lv_label_set_text(lv_obj_get_child(v->span, 0), five ? "SHOW 20 MIN" : "SHOW 5 MIN");
    lv_label_set_text(v->tile[TILE_SPAN].label, five ? "5 MIN" : "20 MIN");
}

/* The graph's span, turned over by a tap on the graph or on its action. It
 * changes what is drawn and nothing else. */
static void on_span(lv_event_t *e)
{
    struct rift_activity_view *v = lv_event_get_user_data(e);

    rift_traffic_graph_set_span(v->graph, rift_traffic_graph_span(v->graph) == RIFT_GRAPH_SPAN_5MIN
                                              ? RIFT_GRAPH_SPAN_20MIN : RIFT_GRAPH_SPAN_5MIN);
    paint_span(v);
    rift_app_refresh(v->app);
}

static void paint_isolated(struct rift_activity_view *v)
{
    int only = rift_traffic_graph_isolated(v->graph);
    int c;

    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        lv_obj_remove_style(v->chip[c].box, pos_style(POS_STYLE_SELECTED), 0);
        if (c == only) {
            pos_style_add(v->chip[c].box, POS_STYLE_SELECTED, 0);
        }
    }
}

/* A name in the legend, tapped: that type alone at full strength in the
 * graph, and the same name tapped again for all of them. */
static void on_chip(lv_event_t *e)
{
    struct rift_activity_view *v = lv_event_get_user_data(e);
    int type = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));

    rift_traffic_graph_isolate(v->graph, rift_traffic_graph_isolated(v->graph) == type ? -1 : type);
    paint_isolated(v);
    rift_app_refresh(v->app);
}

static void build_tile(struct stat_tile *t, lv_obj_t *parent, const char *label)
{
    t->box = lv_obj_create(parent);
    lv_obj_remove_style_all(t->box);
    pos_style_add(t->box, POS_STYLE_SLAB, 0);
    lv_obj_set_height(t->box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(t->box, 10, 0);
    lv_obj_set_style_pad_ver(t->box, 6, 0);
    lv_obj_set_style_pad_row(t->box, 0, 0);
    lv_obj_remove_flag(t->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t->box, LV_OBJ_FLAG_CLICKABLE);
    t->value = lv_label_create(t->box);
    lv_obj_remove_style_all(t->value);
    pos_style_add(t->value, POS_STYLE_TITLE, 0);
    lv_label_set_text(t->value, RIFT_UNKNOWN);
    t->label = lv_label_create(t->box);
    lv_obj_remove_style_all(t->label);
    pos_style_add(t->label, POS_STYLE_CAPTION, 0);
    lv_label_set_text(t->label, label);
}

static void build_graph(struct rift_activity_view *v, lv_obj_t *parent)
{
    static const char *const tile_label[TILE_COUNT] = { "1 MIN", "20 MIN", "HEARD",
                                                        "RSSI", "NODES", "SENT" };
    lv_obj_t *panel = rift_panel(parent, "MESH ACTIVITY");
    lv_obj_t *actions;
    int i;

    /* What the graph counts, alone on its line so the value at its end is
     * never cut (DS §46): it gives up the span first, then the words. */
    v->graph_caption = rift_cell(panel, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(v->graph_caption, LV_PCT(100));

    v->tiles = lv_obj_create(panel);
    lv_obj_remove_style_all(v->tiles);
    lv_obj_set_width(v->tiles, LV_PCT(100));
    lv_obj_set_height(v->tiles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->tiles, LV_FLEX_FLOW_ROW_WRAP);
    /* What a row has over its tiles' shares goes between them. */
    lv_obj_set_flex_align(v->tiles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(v->tiles, TILE_GAP, 0);
    lv_obj_set_style_pad_row(v->tiles, TILE_GAP, 0);
    lv_obj_remove_flag(v->tiles, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < TILE_COUNT; i++) {
        build_tile(&v->tile[i], v->tiles, tile_label[i]);
    }

    v->graph = rift_traffic_graph_create(panel);
    rift_traffic_graph_feed(v->graph, &v->app->model.air);
    lv_obj_add_event_cb(v->graph, on_span, LV_EVENT_CLICKED, v);
    v->mix = rift_traffic_mix_create(panel);

    /* The legend: every type that was heard in the span, named in words
     * with its count; the swatch only agrees with the word (handoff §5). */
    v->legend = lv_obj_create(panel);
    lv_obj_remove_style_all(v->legend);
    lv_obj_set_width(v->legend, LV_PCT(100));
    lv_obj_set_height(v->legend, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(v->legend, 4, 0);
    /* Room for the outline of the name being shown alone, which is drawn
     * outside its box and would be cut at the legend's own edge. */
    lv_obj_set_style_pad_all(v->legend, 4, 0);
    lv_obj_remove_flag(v->legend, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < RIFT_AIR_TYPES; i++) {
        struct legend_chip *c = &v->chip[i];
        lv_obj_t *word;

        c->box = dense(v->legend, 6);
        lv_obj_set_width(c->box, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(c->box, 8, 0);
        pos_style_add(c->box, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_add_flag(c->box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(c->box, (void *)(intptr_t)i);
        lv_obj_add_event_cb(c->box, on_chip, LV_EVENT_CLICKED, v);
        rift_traffic_swatch_create(c->box, i);
        word = rift_cell(c->box, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(word, rift_air_type_word((enum rift_air_type)i));
        c->count = rift_cell(c->box, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
        pos_style_add(c->count, POS_STYLE_TEXT_PRIMARY, 0);
        pos_style_add(c->count, POS_STYLE_CAPTION, 0);
    }

    /* The graph's span, and the packet inspector behind all of this
     * (ui/rift_rxlog_view.h): every reception, repeats included, which this
     * summary is not. R opens it from the keys. */
    actions = dense(panel, 12);
    lv_obj_set_height(actions, RIFT_TOUCH_H);
    v->span = rift_action(actions, "SHOW 5 MIN", 0, 1, on_span, v);
    v->rxlog = rift_action(actions, "RX LOG", 0, 1, on_rxlog, v->app);
    paint_span(v);
}

static void build_feed(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "LIVE FEED");
    int i;

    v->feed_panel = panel;
    v->feed_wide = -1;
    for (i = 0; i < FEED_ROWS; i++) {
        struct feed_row *r = &v->feed[i];

        r->row = dense(panel, 8);
        r->swatch = rift_traffic_swatch_create(r->row, RIFT_AIR_UNKNOWN);
        r->age = rift_cell(r->row, POS_STYLE_CAPTION, 40, LV_TEXT_ALIGN_LEFT);
        r->kind = rift_cell(r->row, POS_STYLE_CAPTION, 32, LV_TEXT_ALIGN_LEFT);
        r->detail = rift_cell(r->row, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(r->detail, 1);
    }
    v->feed_note = wrapping(panel, POS_STYLE_TEXT_MUTED);
}

lv_obj_t *rift_activity_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_activity_view *v = calloc(1, sizeof(*v));

    if (!v) {
        return NULL;
    }
    app->activity = v;
    v->app = app;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v->root, RIFT_PAD, 0);
    lv_obj_set_style_pad_top(v->root, RIFT_PAD - rift_caption_overhang(), 0);
    lv_obj_set_scroll_dir(v->root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->root, LV_SCROLLBAR_MODE_AUTO);

    v->split = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->split);
    lv_obj_set_width(v->split, LV_PCT(100));
    lv_obj_set_height(v->split, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->split, LV_FLEX_FLOW_COLUMN);
    /* 20 between the stacked columns, of which the second column's own
     * caption room (above) is part. */
    lv_obj_set_style_pad_row(v->split, 20 - rift_caption_overhang(), 0);
    lv_obj_set_style_pad_column(v->split, 20, 0);
    lv_obj_remove_flag(v->split, LV_OBJ_FLAG_SCROLLABLE);

    v->col[0] = column(v->split);
    v->col[1] = column(v->split);
    /* The live graph leads, with the radio service and the repeaters this
     * node hears directly under it; the feed and what was heard are beside
     * them in landscape. In portrait the feed moves up under the graph it
     * is the detail of (rift_activity_shape). Everything that changes a
     * setting is on SYSTEM (ui/rift_system.c). */
    build_graph(v, v->col[0]);
    build_service(v, v->col[0]);
    rift_scan_build(app, v->col[0]);
    build_feed(v, v->col[1]);
    build_heard(v, v->col[1]);
    return v->root;
}

void rift_activity_destroy(struct rift_app *app)
{
    if (!app || !app->activity) {
        return;
    }
    rift_scan_destroy(app);
    free(app->activity);
    app->activity = NULL;
}

void rift_activity_shape(struct rift_app *app)
{
    struct rift_activity_view *v = app ? app->activity : NULL;
    int i;

    if (!v) {
        return;
    }
    /* Landscape shows the same panels side by side rather than stacked,
     * the graph's column the wider. Nothing exists here that portrait
     * cannot show (handoff §1). */
    lv_obj_set_flex_flow(v->split, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    for (i = 0; i < 2; i++) {
        lv_obj_set_flex_grow(v->col[i], app->wide ? (i == 0 ? 3 : 2) : 0);
        lv_obj_set_width(v->col[i], app->wide ? LV_SIZE_CONTENT : LV_PCT(100));
    }
    if (v->feed_wide != (app->wide ? 1 : 0)) {
        /* The feed beside the graph in landscape, directly under it in
         * portrait: only when the shape turned, not on every layout. */
        v->feed_wide = app->wide ? 1 : 0;
        lv_obj_set_parent(v->feed_panel, v->col[app->wide ? 1 : 0]);
        lv_obj_move_to_index(v->feed_panel, app->wide ? 0 : 1);
    }
    for (i = 0; i < TILE_COUNT; i++) {
        int per_row = app->wide ? TILES_PER_ROW_WIDE : TILES_PER_ROW;

        /* A share of the row, less room for the gaps between them. */
        lv_obj_set_width(v->tile[i].box, LV_PCT(per_row == TILES_PER_ROW ? 31 : 15));
    }
}

/* ---- refresh ------------------------------------------------------------ */

static enum pos_style_role role_for(enum rift_svc_state s)
{
    switch (s) {
    case RIFT_SVC_ONLINE:
        return POS_STYLE_STATUS_OK_TEXT;
    case RIFT_SVC_DEGRADED:
    case RIFT_SVC_WAITING_RADIOD:
    case RIFT_SVC_WAITING_LEASE:
    case RIFT_SVC_CONFIGURING:
    case RIFT_SVC_STARTING:
        return POS_STYLE_STATUS_WARN_TEXT;
    case RIFT_SVC_ERROR:
    case RIFT_SVC_ABSENT:
        return POS_STYLE_STATUS_ERROR_TEXT;
    case RIFT_SVC_UNKNOWN:
    default:
        return POS_STYLE_TEXT_MUTED;
    }
}

static void upper(const char *src, char *out, size_t out_len)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < out_len; i++) {
        out[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 'a' + 'A') : src[i];
    }
    out[i] = '\0';
}

static void refresh_service(struct rift_activity_view *v)
{
    const struct rift_model *m = &v->app->model;
    /* Switched off by the owner is a choice, not a warning. */
    enum pos_style_role role = rift_model_radio_off(m) && m->state == RIFT_SVC_DEGRADED
                                   ? POS_STYLE_TEXT_MUTED : role_for(m->state);
    char text[RIFT_TEXT_MAX];

    if (role != v->state_role) {
        lv_obj_remove_style(v->svc_state, pos_style(v->state_role), 0);
        pos_style_add(v->svc_state, role, 0);
        v->state_role = role;
    }
    upper(rift_model_state_label(m), text, sizeof(text));
    lv_label_set_text(v->svc_state, text);
    if (m->state == RIFT_SVC_ABSENT) {
        /* A fresh card runs no meshcored at all: the way to one is Controls'
         * LoRa radio tile, which sets the unit up (sysd's radio_setup). */
        lv_label_set_text_fmt(v->svc_reason, "%s. To set up the LoRa radio for RIFT, tap LoRa radio in Controls.",
                              m->reason[0] ? m->reason : "meshcored is not answering");
    } else {
        lv_label_set_text(v->svc_reason, m->reason[0] ? m->reason
                                                      : "meshcored has not said why yet.");
    }
    if (!m->have_status) {
        lv_label_set_text(v->svc_radio, "RADIOD " RIFT_UNKNOWN RIFT_SEP "LEASE " RIFT_UNKNOWN
                                        RIFT_SEP "TRANSMIT " RIFT_UNKNOWN);
    } else {
        /* radiod's own state word is absent until radiod has said, and
         * absent is not "off" (docs/api/mesh.md). Three short answers on
         * one caption line - they were three 64 px rows, which in landscape
         * pushed everything under this panel below the fold - and a line
         * that wraps rather than clips, because half a sentence about the
         * radio is worse than none. */
        lv_label_set_text_fmt(v->svc_radio,
                              "RADIOD %s" RIFT_SEP "%s" RIFT_SEP "LEASE %s" RIFT_SEP "TRANSMIT %s",
                              m->radio_connected ? "CONNECTED" : "NOT CONNECTED",
                              m->have_radio_state ? m->radio_state : RIFT_UNKNOWN,
                              m->radio_lease_held ? "HELD" : "NOT HELD",
                              m->radio_online ? "READY" : "NOT READY");
    }
    if (m->have_identity && m->self_name[0]) {
        lv_label_set_text_fmt(v->svc_self, "%s" RIFT_SEP "%s", m->self_name, m->self_hash);
    } else {
        lv_label_set_text(v->svc_self, RIFT_UNKNOWN);
    }
    if (m->have_nodes_reported) {
        lv_label_set_text_fmt(v->svc_nodes, "%d", m->nodes_reported);
    } else {
        lv_label_set_text(v->svc_nodes, RIFT_UNKNOWN);
    }
    if (m->have_traffic) {
        /* "RX 2627 · TX 6 OK", and the other transmit outcomes only when
         * there were any: a value row clips, and "0 failed" twice over is
         * width that says nothing. */
        char tail[48] = "";
        size_t at = 0;

        if (m->tx_failed) {
            at += (size_t)snprintf(tail + at, sizeof(tail) - at, RIFT_SEP "%u FAILED",
                                   m->tx_failed);
        }
        if (m->tx_unknown && at < sizeof(tail)) {
            at += (size_t)snprintf(tail + at, sizeof(tail) - at, RIFT_SEP "%u UNKNOWN",
                                   m->tx_unknown);
        }
        if (m->tx_rx_resume_failed && at < sizeof(tail)) {
            snprintf(tail + at, sizeof(tail) - at, RIFT_SEP "%u NO RX AFTER",
                     m->tx_rx_resume_failed);
        }
        lv_label_set_text_fmt(v->svc_traffic, "TRAFFIC" RIFT_SEP "RX %u" RIFT_SEP "TX %u OK%s",
                              m->rx_events, m->tx_ok, tail);
        lv_obj_remove_flag(v->svc_traffic, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Not reported is not zero: no line at all rather than a row of 0s. */
        lv_obj_add_flag(v->svc_traffic, LV_OBJ_FLAG_HIDDEN);
    }
    if (m->have_info) {
        lv_label_set_text_fmt(v->svc_build, "%s %s" RIFT_SEP "%s", m->protocol, m->version,
                              m->build);
    } else {
        lv_label_set_text(v->svc_build, RIFT_UNKNOWN);
    }
    if (m->have_state_fault) {
        /* "This node forgot what it knew" is a different answer from "this
         * node has never heard anyone", and the API keeps them apart so a
         * client can too. */
        lv_label_set_text_fmt(v->svc_fault, "Stored node table was not read: %s",
                              m->state_fault);
        lv_obj_remove_flag(v->svc_fault, LV_OBJ_FLAG_HIDDEN);
    } else if (rift_model_unretained_recent(m) > 0) {
        /* The table is full, and it matters beyond the list: a node the
         * service could not keep is one no message can be sent to. It found
         * its way onto unit A's bench as a direct message that could not be
         * sent (docs/hardware/RIFT_CHANNELS_GATE.md). Counted since the last
         * node this app saw forgotten, so it goes away once room is made. */
        unsigned turned_away = rift_model_unretained_recent(m);

        lv_label_set_text_fmt(v->svc_fault,
                              "The node table is full: %u advert%s could not be kept, and "
                              "nothing can be sent to those nodes. Forget a node in NODES "
                              "to make room.",
                              turned_away, turned_away == 1 ? "" : "s");
        lv_obj_remove_flag(v->svc_fault, LV_OBJ_FLAG_HIDDEN);
    } else if (m->events_malformed > 0) {
        lv_label_set_text_fmt(v->svc_fault, "%u message%s from meshcored could not be read and "
                                            "%s ignored.",
                              m->events_malformed, m->events_malformed == 1 ? "" : "s",
                              m->events_malformed == 1 ? "was" : "were");
        lv_obj_remove_flag(v->svc_fault, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->svc_fault, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_heard(struct rift_activity_view *v, int64_t now)
{
    struct rift_app *a = v->app;
    const struct rift_node *order[RIFT_MAX_NODES];
    int count = rift_model_order(&a->model, now, order, RIFT_MAX_NODES);
    int shown = 0;
    int i;

    for (i = 0; i < HEARD_ROWS; i++) {
        struct heard_row *r = &v->heard[i];
        char text[RIFT_STATE_MAX];

        if (i >= count || !order[i]->have_heard) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        rift_glyph_set(r->glyph, rift_app_glyph(order[i], now));
        /* Everything with a width of its own first, then the row laid out,
         * so the name is fitted to the room that is actually left. */
        rift_fmt_state(order[i], text, sizeof(text));
        lv_label_set_text(r->state, text);
        rift_fmt_rssi(order[i]->rssi_dbm, order[i]->have_rssi, text, sizeof(text));
        lv_label_set_text(r->rssi, text);
        rift_fmt_age(now - order[i]->heard_mono_ms, 1, text, sizeof(text));
        lv_label_set_text(r->age, text);
        lv_obj_update_layout(r->row);
        rift_fmt_label(order[i], text, sizeof(text));
        rift_cell_set_text_fit(r->name, text);
        shown++;
    }
    if (shown == 0) {
        lv_label_set_text(v->heard_note, a->model.snapshot_valid
                                             ? "Nothing has been heard yet."
                                             : "Waiting for meshcored.");
        lv_obj_remove_flag(v->heard_note, LV_OBJ_FLAG_HIDDEN);
    } else if (count > shown) {
        lv_label_set_text_fmt(v->heard_note, "%d more in NODES.", count - shown);
        lv_obj_remove_flag(v->heard_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->heard_note, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The figures over the graph: each a count or a measurement the service
 * reported, and a dash where there is none - never a zero for "not known". */
static void refresh_tiles(struct rift_activity_view *v, int64_t now,
                          const struct rift_air_view *view)
{
    const struct rift_model *m = &v->app->model;
    char text[RIFT_AGE_MAX + 8];
    double dbm = 0;

    snprintf(text, sizeof(text), "%u", rift_air_heard_within(&m->air, now, RIFT_TRAFFIC_MINUTE_MS));
    rift_label_set(v->tile[TILE_RATE].value, text);
    snprintf(text, sizeof(text), "%u", (unsigned)view->total);
    rift_label_set(v->tile[TILE_SPAN].value, text);
    /* The newest frame heard, not sent. */
    rift_fmt_age(now - m->air.last_rx_ms, m->air.have_last_rx, text, sizeof(text));
    rift_label_set(v->tile[TILE_LAST].value, text);
    if (rift_air_mean_rssi(&m->air, now, TRAFFIC_WINDOW_MS, &dbm) > 0) {
        snprintf(text, sizeof(text), "%d", (int)(dbm < 0 ? dbm - 0.5 : dbm + 0.5));
    } else {
        snprintf(text, sizeof(text), "%s", RIFT_UNKNOWN);
    }
    rift_label_set(v->tile[TILE_RSSI].value, text);
    snprintf(text, sizeof(text), "%d", rift_model_fresh_count(m, now));
    rift_label_set(v->tile[TILE_NODES].value, m->snapshot_valid ? text : RIFT_UNKNOWN);
    snprintf(text, sizeof(text), "%u", m->tx_ok);
    rift_label_set(v->tile[TILE_SENT].value, m->have_traffic ? text : RIFT_UNKNOWN);
}

static void refresh_legend(struct rift_activity_view *v, const struct rift_air_view *view)
{
    int only = rift_traffic_graph_isolated(v->graph);
    int c;

    rift_traffic_mix_set(v->mix, view->type_total);
    for (c = 0; c < RIFT_AIR_TYPES; c++) {
        struct legend_chip *chip = &v->chip[c];
        unsigned n = (unsigned)view->type_total[c];

        /* A type not heard in the span is not named, apart from the three
         * the legend always carries - and the one being shown alone, which
         * must stay to be tapped again. */
        if (n == 0 && !LEGEND_ALWAYS(c) && c != only) {
            lv_obj_add_flag(chip->box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(chip->box, LV_OBJ_FLAG_HIDDEN);
        if (chip->drawn != n + 1) {
            lv_label_set_text_fmt(chip->count, "%u", n);
            chip->drawn = n + 1;
        }
    }
}

static void refresh_feed(struct rift_activity_view *v, int64_t now)
{
    const struct rift_model *m = &v->app->model;
    struct rift_traffic_bins bins;
    struct rift_air_view view;
    int five = rift_traffic_graph_span(v->graph) == RIFT_GRAPH_SPAN_5MIN;
    int i;

    /* The minute bins as heard, read as of now: the caption's PEAK. The
     * graph reads its own finer record on its own clock (rift_graph.c). */
    rift_traffic_read(&m->traffic, now, &bins);
    rift_traffic_graph_set(v->graph, &bins);
    rift_traffic_graph_set_listening(v->graph, m->have_status && m->radio_online && !m->stale);
    rift_air_read(&m->air, now, rift_traffic_graph_span(v->graph), &view);
    refresh_tiles(v, now, &view);
    refresh_legend(v, &view);
    /* The caption gives up the span first - the graph's own axis says it -
     * then what the bars count, and keeps the value, which nothing else on
     * the panel says (DS §46). */
    {
        char value[32];
        char full[80];
        char counted[64];
        const char *candidate[3];

        if (!bins.started) {
            snprintf(value, sizeof(value), "NOTHING YET");
        } else {
            unsigned peak = 0;

            /* The busiest minute of the span shown. */
            for (i = five ? RIFT_TRAFFIC_MINUTES - 5 : 0; i < RIFT_TRAFFIC_MINUTES; i++) {
                unsigned n = bins.count[i][0] + bins.count[i][1] + bins.count[i][2];

                peak = n > peak ? n : peak;
            }
            snprintf(value, sizeof(value), "PEAK %u/MIN", peak);
        }
        snprintf(full, sizeof(full), "HEARD ON AIR" RIFT_SEP "%s MIN" RIFT_SEP "%s", five ? "5" : "20",
                 value);
        snprintf(counted, sizeof(counted), "HEARD ON AIR" RIFT_SEP "%s", value);
        candidate[0] = full;
        candidate[1] = counted;
        candidate[2] = value;
        rift_cell_set_text_first_fit(v->graph_caption, candidate, 3);
    }

    for (i = 0; i < FEED_ROWS; i++) {
        const struct rift_activity *act = rift_model_activity_at(m, i);
        struct feed_row *r = &v->feed[i];
        char age[RIFT_AGE_MAX];
        char rssi[RIFT_SIGNAL_MAX];
        char snr[RIFT_SIGNAL_MAX];
        char bytes[16];

        if (!act) {
            lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        rift_traffic_swatch_set(r->swatch, act->kind == RIFT_ACT_RX ? (int)rift_air_type_of(act->word)
                                                                   : RIFT_SWATCH_TX);
        rift_fmt_age(now - act->mono_ms, act->have_mono, age, sizeof(age));
        rift_label_set(r->age, age);
        rift_label_set(r->kind, act->kind == RIFT_ACT_RX ? "RX" : "TX");
        if (act->have_bytes) {
            snprintf(bytes, sizeof(bytes), "%d B", act->bytes);
        } else {
            snprintf(bytes, sizeof(bytes), "%s B", RIFT_UNKNOWN);
        }
        if (act->kind == RIFT_ACT_RX) {
            char full[96];
            char no_snr[80];
            char least[56];
            const char *candidate[3];

            /* A row that is too narrow for all of it - portrait, or a larger
             * text size (seen on unit B at Large, 2026-11-16) - gives up the
             * SNR, then the RSSI, rather than showing half a figure. */
            rift_fmt_rssi(act->rssi_dbm, act->have_rssi, rssi, sizeof(rssi));
            rift_fmt_snr(act->snr_db, act->have_snr, snr, sizeof(snr));
            snprintf(full, sizeof(full), "%s" RIFT_SEP "%s" RIFT_SEP "%s" RIFT_SEP "%s", act->word,
                     bytes, rssi, snr);
            snprintf(no_snr, sizeof(no_snr), "%s" RIFT_SEP "%s" RIFT_SEP "%s", act->word, bytes, rssi);
            snprintf(least, sizeof(least), "%s" RIFT_SEP "%s", act->word, bytes);
            candidate[0] = full;
            candidate[1] = no_snr;
            candidate[2] = least;
            lv_obj_update_layout(r->row);
            rift_cell_set_text_first_fit(r->detail, candidate, 3);
        } else {
            /* The result word, never the kind: three of the five results a
             * tx activity can carry are not "it went out". */
            lv_label_set_text_fmt(r->detail, "%s" RIFT_SEP "%s", act->word, bytes);
        }
    }
    if (m->activity_total == 0) {
        lv_label_set_text(v->feed_note, "No frame has been seen since RIFT opened.");
        lv_obj_remove_flag(v->feed_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        int rx = 0;
        int tx = 0;
        int more = 0;

        /* How busy the channel is: frames the service reported in the last
         * five minutes, from the feed RIFT holds. A count of frames, not a
         * measure of the link; "48+" when the feed is full and all of it is
         * that recent, because then there may have been more. */
        rift_model_recent_frames(m, now, TRAFFIC_WINDOW_MS, &rx, &tx, &more);
        lv_label_set_text_fmt(v->feed_note,
                              "LAST 5 MIN" RIFT_SEP "RX %d%s" RIFT_SEP "TX %d%s" RIFT_SEP
                              "%u frame%s since RIFT opened.",
                              rx, more ? "+" : "", tx, more ? "+" : "", m->activity_total,
                              m->activity_total == 1 ? "" : "s");
        lv_obj_remove_flag(v->feed_note, LV_OBJ_FLAG_HIDDEN);
    }
}

void rift_activity_refresh(struct rift_app *app)
{
    struct rift_activity_view *v = app ? app->activity : NULL;
    int64_t now;

    if (!v) {
        return;
    }
    now = rift_app_now(app);
    refresh_service(v);
    rift_scan_refresh(app);
    refresh_heard(v, now);
    refresh_feed(v, now);
}

lv_obj_t *rift_activity_graph(const struct rift_app *app)
{
    return (app && app->activity) ? app->activity->graph : NULL;
}

lv_obj_t *rift_activity_rxlog_button(const struct rift_app *app)
{
    return (app && app->activity) ? app->activity->rxlog : NULL;
}
