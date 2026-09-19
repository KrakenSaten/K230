/*
 * ACTIVITY. See rift_activity.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_activity.h"

#include "pos_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough of each to be information, few enough that the panels fit the
 * portrait body without the page becoming a log. */
#define HEARD_ROWS 6
#define FEED_ROWS 8

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
    lv_obj_t *svc_radio;
    lv_obj_t *svc_lease;
    lv_obj_t *svc_tx;
    lv_obj_t *svc_nodes;
    lv_obj_t *svc_build;
    lv_obj_t *svc_fault;

    lv_obj_t *id_glyph;
    lv_obj_t *id_name;
    lv_obj_t *id_hash;
    lv_obj_t *id_key;

    lv_obj_t *heard_note;
    struct heard_row heard[HEARD_ROWS];
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
    v->svc_radio = pocketui_kv_row(panel, "Radio", RIFT_UNKNOWN);
    v->svc_lease = pocketui_kv_row(panel, "Lease", RIFT_UNKNOWN);
    v->svc_tx = pocketui_kv_row(panel, "Transmit", RIFT_UNKNOWN);
    v->svc_nodes = pocketui_kv_row(panel, "Nodes held", RIFT_UNKNOWN);
    v->svc_build = pocketui_kv_row(panel, "Service", RIFT_UNKNOWN);
    v->svc_fault = wrapping(panel, POS_STYLE_STATUS_WARN_TEXT);
}

static void build_identity(struct rift_activity_view *v, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "THIS DEVICE");
    lv_obj_t *line = dense(panel, 10);

    v->id_glyph = rift_glyph_create(line);
    rift_glyph_set(v->id_glyph, RIFT_GLYPH_SELF);
    v->id_name = rift_cell(line, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->id_name, 1);
    v->id_hash = rift_cell(line, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_RIGHT);
    v->id_key = pocketui_kv_row(panel, "Key", RIFT_UNKNOWN);
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
    int i;

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
    lv_obj_set_scroll_dir(v->root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->root, LV_SCROLLBAR_MODE_AUTO);

    v->split = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->split);
    lv_obj_set_width(v->split, LV_PCT(100));
    lv_obj_set_height(v->split, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->split, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->split, 20, 0);
    lv_obj_set_style_pad_column(v->split, 20, 0);
    lv_obj_remove_flag(v->split, LV_OBJ_FLAG_SCROLLABLE);

    v->col[0] = column(v->split);
    v->col[1] = column(v->split);
    build_service(v, v->col[0]);
    build_identity(v, v->col[0]);
    build_heard(v, v->col[1]);
    build_feed(v, v->col[1]);
    return v->root;
}

void rift_activity_destroy(struct rift_app *app)
{
    if (!app || !app->activity) {
        return;
    }
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
    enum pos_style_role role = role_for(m->state);
    char text[RIFT_TEXT_MAX];

    if (role != v->state_role) {
        lv_obj_remove_style(v->svc_state, pos_style(v->state_role), 0);
        pos_style_add(v->svc_state, role, 0);
        v->state_role = role;
    }
    upper(rift_svc_state_word(m->state), text, sizeof(text));
    lv_label_set_text(v->svc_state, text);
    lv_label_set_text(v->svc_reason, m->reason[0] ? m->reason
                                                  : "meshcored has not said why yet.");
    if (!m->have_status) {
        lv_label_set_text(v->svc_radio, RIFT_UNKNOWN);
        lv_label_set_text(v->svc_lease, RIFT_UNKNOWN);
        lv_label_set_text(v->svc_tx, RIFT_UNKNOWN);
    } else {
        /* radiod's own state word is absent until radiod has said, and
         * absent is not "off" (docs/api/mesh.md). Three short answers
         * rather than two long ones: a value row clips, and half a sentence
         * about the radio is worse than none. */
        lv_label_set_text_fmt(v->svc_radio, "%s" RIFT_SEP "%s",
                              m->radio_connected ? "connected" : "no radiod",
                              m->have_radio_state ? m->radio_state : RIFT_UNKNOWN);
        lv_label_set_text(v->svc_lease, m->radio_lease_held ? "held" : "not held");
        lv_label_set_text(v->svc_tx, m->radio_online ? "ready" : "not ready");
    }
    if (m->have_nodes_reported) {
        lv_label_set_text_fmt(v->svc_nodes, "%d", m->nodes_reported);
    } else {
        lv_label_set_text(v->svc_nodes, RIFT_UNKNOWN);
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

static void refresh_identity(struct rift_activity_view *v)
{
    const struct rift_model *m = &v->app->model;
    char text[RIFT_KEY_SHORT_MAX];

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
    int i;

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
        lv_label_set_text_fmt(v->feed_note, "%u frame%s since RIFT opened.", m->activity_total,
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
    refresh_identity(v);
    refresh_heard(v, now);
    refresh_feed(v, now);
}
