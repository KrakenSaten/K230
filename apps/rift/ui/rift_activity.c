/*
 * ACTIVITY. See rift_activity.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_activity.h"

#include "pos_styles.h"
#include "rift_device.h"
#include "rift_graph.h"
#include "rift_manage.h"
#include "rift_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough of each to be information, few enough that the panels fit the
 * portrait body without the page becoming a log. */
#define HEARD_ROWS 6
#define FEED_ROWS 8
/* The window the mesh's recent traffic is counted over. */
#define TRAFFIC_WINDOW_MS (5 * 60 * 1000LL)
/* The DM sound's ON/OFF, as Settings draws a switch: a 56 px action beside
 * its title, primary while on. */
#define SOUND_TOGGLE_W 112

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
    lv_obj_t *svc_nodes;
    lv_obj_t *svc_build;
    lv_obj_t *svc_fault;

    lv_obj_t *svc_traffic;

    lv_obj_t *id_glyph;
    lv_obj_t *id_name;
    lv_obj_t *id_hash;
    lv_obj_t *id_key;
    lv_obj_t *advert_near;
    lv_obj_t *advert_mesh;
    lv_obj_t *advert_line;
    int advert_warn;

    lv_obj_t *sound_toggle;
    lv_obj_t *sound_label;
    lv_obj_t *sound_note;
    int sound_drawn; /* the state the toggle was last drawn in; -1 not yet */

    lv_obj_t *heard_note;
    struct heard_row heard[HEARD_ROWS];
    /* The last twenty minutes, a bar each (rift_graph.h), with its caption
     * and legend over it, at the head of the feed. */
    lv_obj_t *graph_caption;
    lv_obj_t *graph;
    lv_obj_t *feed_note;
    struct feed_row feed[FEED_ROWS];

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
    v->svc_nodes = pocketui_kv_row(panel, "Nodes held", RIFT_UNKNOWN);
    v->svc_build = pocketui_kv_row(panel, "Service", RIFT_UNKNOWN);
    /* The service's own counts of what it put on the air and what it heard,
     * in its own words: a transmit that went out is not one that failed. A
     * caption line rather than a key/value row: a row's value is cut with an
     * ellipsis at 60 % of the width, and a count cut short is a wrong count. */
    v->svc_traffic = wrapping(panel, POS_STYLE_CAPTION);
    v->svc_fault = wrapping(panel, POS_STYLE_STATUS_WARN_TEXT);
}

/* An advert, on a reader's press, and only then: the one handler for both
 * buttons, and the only caller of the advert in this app. Zero-hop is heard
 * in direct range and repeated by nobody; the mesh one is flooded. */
static void on_advert(lv_event_t *e)
{
    struct rift_activity_view *v = lv_event_get_user_data(e);
    int zero_hop = lv_event_get_target_obj(e) == v->advert_near;

    rift_ipc_send_advert(&v->app->ipc, zero_hop);
    rift_app_refresh(v->app);
}

static void build_identity(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "THIS DEVICE");
    lv_obj_t *line = dense(panel, 10);
    lv_obj_t *bar;

    v->id_glyph = rift_glyph_create(line);
    rift_glyph_set(v->id_glyph, RIFT_GLYPH_SELF);
    v->id_name = rift_cell(line, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->id_name, 1);
    v->id_hash = rift_cell(line, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_RIGHT);
    v->id_key = pocketui_kv_row(panel, "Key", RIFT_UNKNOWN);

    /* Telling the mesh this node is here. Nothing in RIFT does it on its
     * own - there is no periodic advert, by decision (docs/services/
     * MESHCORED.md) - so a peer that has lost this node's key, or one that
     * has never heard it, waits for a reader to press one of these. */
    bar = dense(panel, 12);
    lv_obj_set_height(bar, RIFT_TOUCH_H);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    v->advert_near = rift_action(bar, "ADVERT NEAR", 0, 0, on_advert, v);
    v->advert_mesh = rift_action(bar, "ADVERT MESH", 0, 0, on_advert, v);
    v->advert_line = wrapping(panel, POS_STYLE_CAPTION);
    /* The name and the path hash size: this node's own, so in its panel. */
    rift_device_build(v->app, panel);
}

/* The DM sound's setting: a press turns it over, and that is all it does.
 * It sends nothing and plays nothing. */
static void on_sound_toggle(lv_event_t *e)
{
    struct rift_activity_view *v = lv_event_get_user_data(e);

    rift_app_set_dm_sound(v->app, !v->app->prefs.dm_sound);
}

static void build_sound(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "NOTIFY");
    lv_obj_t *row = dense(panel, 12);
    lv_obj_t *title;

    lv_obj_set_height(row, RIFT_TOUCH_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    title = rift_cell(row, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, "Sound for a new DM");
    v->sound_toggle = rift_action(row, "ON", 1, 1, on_sound_toggle, v);
    lv_obj_set_flex_grow(v->sound_toggle, 0);
    lv_obj_set_width(v->sound_toggle, SOUND_TOGGLE_W);
    v->sound_label = lv_obj_get_child(v->sound_toggle, 0);
    v->sound_note = wrapping(panel, POS_STYLE_CAPTION);
    v->sound_drawn = -1;
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

static void build_feed(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "MESH ACTIVITY");
    lv_obj_t *legend;
    int i;
    int c;

    /* The graph's caption and legend on one 24 px line: what the bars
     * count on the left, and the three classes named on the right, each
     * word after a swatch in its colour. The words carry the classes; the
     * swatches only agree with them (handoff §5). */
    legend = dense(panel, 6);
    lv_obj_set_height(legend, rift_group_h());
    v->graph_caption = rift_cell(legend, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->graph_caption, 1);
    lv_obj_set_width(v->graph_caption, 1);
    for (c = 0; c < RIFT_TRAFFIC_CLASSES; c++) {
        lv_obj_t *word;

        rift_traffic_swatch_create(legend, (enum rift_traffic_class)c);
        word = rift_cell(legend, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(word, rift_traffic_class_word((enum rift_traffic_class)c));
        lv_obj_set_style_margin_right(word, c + 1 < RIFT_TRAFFIC_CLASSES ? 6 : 0, 0);
    }
    v->graph = rift_traffic_graph_create(panel);
    lv_obj_set_style_margin_bottom(v->graph, 6, 0);

    for (i = 0; i < FEED_ROWS; i++) {
        struct feed_row *r = &v->feed[i];

        r->row = dense(panel, 8);
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
    /* The radio service alone on the left; this device - with the ADVERT
     * buttons - at the head of the right, where landscape shows it without
     * a scroll. Stacked in portrait, the order is the same as ever: service,
     * device, heard, feed. */
    build_service(v, v->col[0]);
    build_sound(v, v->col[0]);
    /* The channels this node holds, joined and left here (ui/rift_manage.c);
     * built before THIS DEVICE, which adds its controls to the same block. */
    rift_manage_build_channels(app, v->col[0]);
    build_identity(v, v->col[1]);
    build_heard(v, v->col[1]);
    build_feed(v, v->col[1]);
    return v->root;
}

void rift_activity_destroy(struct rift_app *app)
{
    if (!app || !app->activity) {
        return;
    }
    rift_manage_destroy(app);
    rift_device_destroy(app);
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
    /* Landscape shows the same four panels side by side rather than
     * stacked. Nothing exists here that portrait cannot show (handoff §1). */
    lv_obj_set_flex_flow(v->split, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    for (i = 0; i < 2; i++) {
        lv_obj_set_flex_grow(v->col[i], app->wide ? 1 : 0);
        lv_obj_set_width(v->col[i], app->wide ? LV_SIZE_CONTENT : LV_PCT(100));
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
    lv_label_set_text(v->svc_reason, m->reason[0] ? m->reason
                                                  : "meshcored has not said why yet.");
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

/* The ADVERT buttons: pressable only when the service is answering, its
 * radio can send, and no advert is already on its way. The line under them
 * says what became of the last one, or - before any - what the two do. */
static void refresh_advert(struct rift_activity_view *v)
{
    const struct rift_model *m = &v->app->model;
    /* The model's word for it, as the composer's (rift_thread_refusal): the
     * service said its radio can send. A press while the socket is between
     * connections is refused by the client, with a reason, here. */
    int ready = !m->stale && m->state != RIFT_SVC_ABSENT && m->have_status &&
                m->radio_online && !rift_model_action_busy(m, RIFT_ACTION_ADVERT_MESH);
    char text[RIFT_ACTION_TEXT_MAX];
    int warn = 0;

    rift_action_set_enabled(v->advert_near, 0, ready);
    rift_action_set_enabled(v->advert_mesh, 0, ready);
    rift_fmt_action(&m->advert, rift_app_now(v->app), text, sizeof(text));
    if (text[0]) {
        warn = m->advert.failed;
        lv_label_set_text(v->advert_line, text);
    } else if (!ready && (m->stale || m->state == RIFT_SVC_ABSENT)) {
        lv_label_set_text(v->advert_line, "meshcored is not answering.");
    } else if (!ready && !m->advert.active) {
        lv_label_set_text(v->advert_line, "The radio is not ready to send.");
    } else {
        lv_label_set_text(v->advert_line, "NEAR: heard in direct range, repeated by nobody. "
                                          "MESH: flooded through every repeater.");
    }
    if (warn != v->advert_warn) {
        if (warn) {
            pos_style_add(v->advert_line, POS_STYLE_STATUS_WARN_TEXT, 0);
        } else {
            lv_obj_remove_style(v->advert_line, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        }
        v->advert_warn = warn;
    }
}

/* The DM sound: the switch, and one line saying what it will actually do on
 * this device - which, with no platform sound to ask for, is nothing, and a
 * switch that says ON must not leave that unsaid. */
static void refresh_sound(struct rift_activity_view *v)
{
    const struct rift_app *a = v->app;
    int on = a->prefs.dm_sound ? 1 : 0;
    const char *what;

    if (on != v->sound_drawn) {
        lv_label_set_text(v->sound_label, on ? "ON" : "OFF");
        lv_obj_remove_style(v->sound_toggle, pos_style(POS_STYLE_BUTTON_PRIMARY), 0);
        lv_obj_remove_style(v->sound_toggle, pos_style(POS_STYLE_BUTTON_PRIMARY_PRESSED),
                            LV_STATE_PRESSED);
        lv_obj_remove_style(v->sound_toggle, pos_style(POS_STYLE_BUTTON_SECONDARY), 0);
        lv_obj_remove_style(v->sound_toggle, pos_style(POS_STYLE_SLAB_PRESSED), LV_STATE_PRESSED);
        if (on) {
            pos_style_add(v->sound_toggle, POS_STYLE_BUTTON_PRIMARY, 0);
            pos_style_add(v->sound_toggle, POS_STYLE_BUTTON_PRIMARY_PRESSED, LV_STATE_PRESSED);
        } else {
            pos_style_add(v->sound_toggle, POS_STYLE_BUTTON_SECONDARY, 0);
            pos_style_add(v->sound_toggle, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        }
        v->sound_drawn = on;
    }
    if (!on) {
        what = "Off: a new direct message is shown, not heard.";
    } else if (!rift_sound_available()) {
        what = rift_sound_why();
    } else if (!rift_app_can_sound(a)) {
        what = "Doors is muted: nothing is heard until the volume is up.";
    } else {
        what = "One short sound for a new direct message, at most one every 10 s. "
               "Not for channels, history or your own.";
    }
    if (!a->prefs_saved) {
        lv_label_set_text_fmt(v->sound_note, "%s Not saved: this lasts until RIFT closes.",
                              what);
    } else {
        lv_label_set_text(v->sound_note, what);
    }
}

static void refresh_identity(struct rift_activity_view *v)
{
    const struct rift_model *m = &v->app->model;
    char text[RIFT_KEY_SHORT_MAX];

    refresh_advert(v);
    if (!m->have_identity) {
        lv_label_set_text(v->id_name, RIFT_UNKNOWN);
        lv_label_set_text(v->id_hash, "");
        lv_label_set_text(v->id_key, RIFT_UNKNOWN);
        return;
    }
    lv_label_set_text(v->id_name, m->self_name[0] ? m->self_name : RIFT_UNKNOWN);
    lv_label_set_text(v->id_hash, m->self_hash);
    rift_fmt_key_short(m->self_key, text, sizeof(text));
    lv_label_set_text(v->id_key, text);
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

static void refresh_feed(struct rift_activity_view *v, int64_t now)
{
    const struct rift_model *m = &v->app->model;
    struct rift_traffic_bins bins;
    int i;

    /* The last twenty minutes as heard: read as of now, so a quiet minute
     * since the last frame is a quiet bar and not the last busy one held. */
    rift_traffic_read(&m->traffic, now, &bins);
    rift_traffic_graph_set(v->graph, &bins);
    if (!bins.started) {
        lv_label_set_text(v->graph_caption, "HEARD ON AIR" RIFT_SEP "20 MIN" RIFT_SEP "NOTHING YET");
    } else {
        lv_label_set_text_fmt(v->graph_caption, "HEARD ON AIR" RIFT_SEP "20 MIN" RIFT_SEP
                                                "PEAK %u/MIN", rift_traffic_peak(&bins));
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
        rift_fmt_age(now - act->mono_ms, act->have_mono, age, sizeof(age));
        lv_label_set_text(r->age, age);
        lv_label_set_text(r->kind, act->kind == RIFT_ACT_RX ? "RX" : "TX");
        if (act->have_bytes) {
            snprintf(bytes, sizeof(bytes), "%d B", act->bytes);
        } else {
            snprintf(bytes, sizeof(bytes), "%s B", RIFT_UNKNOWN);
        }
        if (act->kind == RIFT_ACT_RX) {
            rift_fmt_rssi(act->rssi_dbm, act->have_rssi, rssi, sizeof(rssi));
            rift_fmt_snr(act->snr_db, act->have_snr, snr, sizeof(snr));
            lv_label_set_text_fmt(r->detail, "%s" RIFT_SEP "%s" RIFT_SEP "%s" RIFT_SEP "%s",
                                  act->word, bytes, rssi, snr);
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
    refresh_sound(v);
    refresh_identity(v);
    rift_manage_refresh(app);
    rift_device_refresh(app);
    refresh_heard(v, now);
    refresh_feed(v, now);
}

lv_obj_t *rift_activity_graph(const struct rift_app *app)
{
    return (app && app->activity) ? app->activity->graph : NULL;
}
