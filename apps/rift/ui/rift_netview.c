/*
 * NET. See rift_netview.h, and rift_net.h for what a ring is.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_netview.h"

#include "pos_styles.h"
#include "rift_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Handoff §7: 68 px a ring in portrait, node pills 26 px. A pill's hit area
 * is grown to a 36 px row's, the size RIFT-DEV-1 lets select and nothing
 * more. */
#define RING_H 68
/* A ring with nobody on it keeps its place on the scale at a header row's
 * height rather than 68 px of nothing. */
#define RING_H_EMPTY RIFT_HEADER_ROW_H
#define PILL_H 26
#define PILL_HIT ((RIFT_ROW_H - PILL_H) / 2)
#define RING_LABEL_W 92
/* Landscape: room kept clear at the right of a ring's heading. The columns
 * meet edge to edge, so a heading that filled its column ran straight into
 * the next one's ("1 DIRECT 2", unit B at Large) without overlapping it. */
#define RING_GUTTER_WIDE 12
#define RAIL_W 2
/* Landscape: a column per ring, side by side under the path panel, sharing
 * the width equally (about 108 px each for all eleven). A pill's name is cut
 * to fit one. */
#define PILL_TEXT_WIDE 10
#define PILL_TEXT 20
#define PILLS_PER_RING (RIFT_NET_RING_SHOWN + 1) /* and one "+n" */

struct pill {
    lv_obj_t *btn;
    lv_obj_t *label;
    char key[RIFT_KEY_HEX]; /* "" for self and for "+n" */
    int look;               /* what it was last drawn as; -1 not yet */
};

struct ring_view {
    lv_obj_t *box;
    lv_obj_t *head;
    lv_obj_t *word;
    lv_obj_t *count;
    lv_obj_t *rail;
    lv_obj_t *pills;
    struct pill pill[PILLS_PER_RING];
    int built;
    int rail_tone; /* -1 not yet */
    int empty;     /* drawn as an empty ring; -1 not yet */
};

struct rift_net_view {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *path_name;
    lv_obj_t *path_state;
    lv_obj_t *path_chain;
    lv_obj_t *path_bar;
    lv_obj_t *path_message;
    lv_obj_t *path_detail;
    lv_obj_t *list_btn;
    lv_obj_t *legend;
    lv_obj_t *rings;
    lv_obj_t *note;
    struct ring_view ring[RIFT_NET_RINGS];
    int wide;
};

enum look {
    LOOK_PLAIN = 0,
    LOOK_ON_PATH,
    LOOK_SELECTED,
    LOOK_SELF,
};

static struct rift_net_view *view_of(const struct rift_app *app)
{
    return app ? app->net : NULL;
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *b = lv_obj_create(parent);

    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, flow);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
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

/* ---- pills -------------------------------------------------------------- */

static void on_pill(lv_event_t *e)
{
    struct rift_net_view *v = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    int r;
    int i;

    /* A pill selects its node and does nothing else, like a NODES row. */
    for (r = 0; r < RIFT_NET_RINGS; r++) {
        for (i = 0; i < v->ring[r].built; i++) {
            if (v->ring[r].pill[i].btn == btn && v->ring[r].pill[i].key[0]) {
                rift_app_select(v->app, v->ring[r].pill[i].key);
                return;
            }
        }
    }
}

static void build_pill(struct rift_net_view *v, struct ring_view *rv)
{
    struct pill *p = &rv->pill[rv->built++];

    p->btn = lv_button_create(rv->pills);
    lv_obj_remove_style_all(p->btn);
    pos_style_add(p->btn, POS_STYLE_BUTTON_SECONDARY, 0);
    pos_style_add(p->btn, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_height(p->btn, PILL_H);
    lv_obj_set_width(p->btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(p->btn, 8, 0);
    lv_obj_set_style_radius(p->btn, PILL_H / 2, 0);
    lv_obj_set_ext_click_area(p->btn, PILL_HIT);
    lv_obj_remove_flag(p->btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(p->btn, on_pill, LV_EVENT_CLICKED, v);
    p->label = lv_label_create(p->btn);
    lv_obj_remove_style_all(p->label);
    pos_style_add(p->label, POS_STYLE_CAPTION, 0);
    lv_label_set_long_mode(p->label, LV_LABEL_LONG_CLIP);
    lv_obj_center(p->label);
    p->look = -1;
}

/* Selected: filled in the accent, the words on it in text_on_accent. On the
 * selected route: the focus outline (handoff §7: on-path pills keep the
 * surface fill and take the accent border). This device: the outline too,
 * the self marker's ring. The words always say who; the look only agrees. */
static void set_look(struct pill *p, int look)
{
    if (look == p->look) {
        return;
    }
    lv_obj_remove_style(p->btn, pos_style(POS_STYLE_CHIP_ACTIVE), 0);
    lv_obj_remove_style(p->btn, pos_style(POS_STYLE_SELECTED), 0);
    lv_obj_remove_style(p->label, pos_style(POS_STYLE_CHIP_ACTIVE), 0);
    lv_obj_set_style_bg_opa(p->btn, LV_OPA_COVER, 0);
    switch (look) {
    case LOOK_SELECTED:
        pos_style_add(p->btn, POS_STYLE_CHIP_ACTIVE, 0);
        pos_style_add(p->label, POS_STYLE_CHIP_ACTIVE, 0);
        break;
    case LOOK_ON_PATH:
    case LOOK_SELF:
        pos_style_add(p->btn, POS_STYLE_SELECTED, 0);
        break;
    default:
        break;
    }
    if (look == LOOK_SELF) {
        lv_obj_remove_flag(p->btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(p->btn, LV_OBJ_FLAG_CLICKABLE);
    }
    p->look = look;
}

static void fill_pill(struct pill *p, const char *text, const char *key, int look, int wide)
{
    /* A pill is a name at a glance, not the whole of it: cut on a character
     * boundary with an ellipsis - about ten characters in a landscape column,
     * eighteen in portrait. The PATH panel and NODES carry the rest. */
    char shortened[PILL_TEXT];

    rift_utf8_ellipsis(shortened, wide ? PILL_TEXT_WIDE : sizeof(shortened), text ? text : "");
    rift_label_set(p->label, shortened);
    snprintf(p->key, sizeof(p->key), "%s", key ? key : "");
    set_look(p, look);
    lv_obj_remove_flag(p->btn, LV_OBJ_FLAG_HIDDEN);
}

/* ---- building --------------------------------------------------------------- */

static void on_message(lv_event_t *e)
{
    struct rift_net_view *v = lv_event_get_user_data(e);
    const struct rift_node *n = rift_app_selected(v->app);

    /* Opens COMMS on this peer, as NODES' MESSAGE does. It sends nothing. */
    if (n) {
        rift_app_open_conversation(v->app, n->key);
    }
}

static void on_detail(lv_event_t *e)
{
    struct rift_net_view *v = lv_event_get_user_data(e);

    if (rift_app_selected(v->app)) {
        rift_app_show_section(v->app, RIFT_SEC_NODES);
        rift_app_open_detail(v->app, 1);
    }
}

/* NET lives under NODES: LIST goes back to the node list it came from. */
static void on_list(lv_event_t *e)
{
    struct rift_net_view *v = lv_event_get_user_data(e);

    rift_app_show_section(v->app, RIFT_SEC_NODES);
}

static void build_path(struct rift_net_view *v)
{
    lv_obj_t *panel = rift_panel(v->root, "PATH");

    v->path_name = wrapping(panel, POS_STYLE_ROW_TITLE);
    v->path_state = wrapping(panel, POS_STYLE_CAPTION);
    v->path_chain = wrapping(panel, POS_STYLE_CAPTION);
    v->path_bar = box(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(v->path_bar, 12, 0);
    lv_obj_set_style_pad_top(v->path_bar, 8, 0);
    v->path_message = rift_action(v->path_bar, "MESSAGE", 0, 1, on_message, v);
    v->path_detail = rift_action(v->path_bar, "DETAIL \xE2\x80\xBA", 0, 1, on_detail, v);
}

/* A ring's word, handoff §7 - 0 SELF, 1 DIRECT, 2 .. 9+, ? NO PATH - in the
 * longest form that fits its head: the number always survives. */
static void ring_word_fit(lv_obj_t *word, int r)
{
    char full[24];
    char brief[16];
    char number[8];
    const char *cand[3];
    int n = 0;

    if (r == RIFT_NET_RING_SELF || r == RIFT_NET_RING_DIRECT) {
        snprintf(full, sizeof(full), "%d %s", r, rift_net_ring_word(r));
        snprintf(brief, sizeof(brief), "%d %s", r, r == RIFT_NET_RING_SELF ? "ME" : "DIR");
        snprintf(number, sizeof(number), "%d", r);
        cand[n++] = full;
        cand[n++] = brief;
        cand[n++] = number;
    } else if (r == RIFT_NET_RING_NO_PATH) {
        cand[n++] = "? NO PATH";
        cand[n++] = "? NONE";
        cand[n++] = "?";
    } else {
        cand[n++] = rift_net_ring_word(r);
    }
    rift_cell_set_text_first_fit(word, cand, n);
}

/* How many, and how many of them an advert placed, in the longest form that
 * fits: "5 · 5 ADV", then "5·5 ADV", then the count alone - which the PATH
 * panel and a node's detail still qualify. Empty for an empty ring. */
static void ring_count_fit(lv_obj_t *count, int total, int advert)
{
    char full[48];
    char tight[32];
    char bare[16];
    const char *cand[3];
    int n = 0;

    if (total == 0) {
        rift_label_set(count, "");
        return;
    }
    snprintf(bare, sizeof(bare), "%d", total);
    if (advert) {
        snprintf(full, sizeof(full), "%d" RIFT_SEP "%d ADV", total, advert);
        snprintf(tight, sizeof(tight), "%d\xC2\xB7%d ADV", total, advert);
        cand[n++] = full;
        cand[n++] = tight;
    }
    cand[n++] = bare;
    rift_cell_set_text_first_fit(count, cand, n);
}

static void build_ring(struct rift_net_view *v, int r)
{
    struct ring_view *rv = &v->ring[r];

    rv->box = box(v->rings, LV_FLEX_FLOW_ROW);
    /* The divider under each ring is the box's own bottom border, so a ring
     * that is hidden takes its rule with it. */
    pos_style_add(rv->box, POS_STYLE_DIVIDER, 0);
    lv_obj_set_style_min_height(rv->box, RING_H, 0);
    lv_obj_set_style_pad_column(rv->box, 10, 0);
    rv->head = box(rv->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(rv->head, RING_LABEL_W);
    lv_obj_set_style_pad_top(rv->head, 6, 0);
    /* The word and the count are the head's width, one line each, and are
     * fitted to it on every refresh (ring_head_fit): a landscape column is
     * about a ninth of the body, and at a larger text size (DS §46) "1 DIRECT"
     * or "5 · 5 ADV" sized to its own text ran into the next ring's column. */
    rv->word = lv_label_create(rv->head);
    lv_obj_remove_style_all(rv->word);
    pos_style_add(rv->word, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(rv->word, LV_PCT(100));
    lv_label_set_long_mode(rv->word, LV_LABEL_LONG_CLIP);
    lv_label_set_text(rv->word, "");
    rv->count = lv_label_create(rv->head);
    lv_obj_remove_style_all(rv->count);
    pos_style_add(rv->count, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(rv->count, LV_PCT(100));
    lv_label_set_long_mode(rv->count, LV_LABEL_LONG_CLIP);
    lv_label_set_text(rv->count, "");
    /* The rail of handoff §7: the accent through the rings the selected route
     * crosses, muted through NO PATH, nothing elsewhere. A ring's segment is
     * the ring's own minimum height: a percentage of a row sized by its
     * content is a height LVGL never settles (rift_comms.c, 2026-09-28). */
    rv->rail = rift_vrule(rv->box, RAIL_W);
    lv_obj_set_height(rv->rail, RING_H);
    rv->rail_tone = -1;
    rv->empty = -1;
    rv->pills = box(rv->box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_grow(rv->pills, 1);
    lv_obj_set_width(rv->pills, 1);
    lv_obj_set_style_pad_all(rv->pills, 6, 0);
    lv_obj_set_style_pad_row(rv->pills, 8, 0);
    lv_obj_set_style_pad_column(rv->pills, 8, 0);
}

lv_obj_t *rift_net_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_net_view *v = calloc(1, sizeof(*v));
    int r;

    if (!v) {
        return NULL;
    }
    app->net = v;
    v->app = app;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v->root, RIFT_PAD, 0);
    lv_obj_set_style_pad_top(v->root, RIFT_PAD, 0);
    lv_obj_set_style_pad_row(v->root, 12, 0);
    lv_obj_set_scroll_dir(v->root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->root, LV_SCROLLBAR_MODE_AUTO);

    /* The way back to the list first, beside the legend: NET is a view of
     * NODES, not a section of its own. */
    {
        lv_obj_t *top = box(v->root, LV_FLEX_FLOW_ROW);

        lv_obj_set_style_pad_column(top, 12, 0);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        v->list_btn = rift_action(top, "\xE2\x80\xB9 LIST", 0, 1, on_list, v);
        lv_obj_set_flex_grow(v->list_btn, 0);
        lv_obj_set_width(v->list_btn, 120);
        v->legend = wrapping(top, POS_STYLE_CAPTION);
        lv_obj_set_flex_grow(v->legend, 1);
        lv_obj_set_width(v->legend, 1);
    }
    build_path(v);
    /* Hop count, as handoff §7 has it: ring 1 is direct, each relay adds one,
     * so the PATH panel's "RING 8 ... THROUGH 7 RELAYS" agrees with it. */
    lv_label_set_text(v->legend, "RING = HOPS: 1 IS DIRECT, EACH RELAY ADDS ONE - ON ITS "
                                 "LEARNED ROUTE, OR ON ITS LAST ADVERT WHERE NO ROUTE IS "
                                 "LEARNED");
    v->note = wrapping(v->root, POS_STYLE_TEXT_MUTED);
    lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    v->rings = box(v->root, LV_FLEX_FLOW_COLUMN);
    for (r = 0; r < RIFT_NET_RINGS; r++) {
        build_ring(v, r);
    }
    return v->root;
}

void rift_net_view_destroy(struct rift_app *app)
{
    struct rift_net_view *v = view_of(app);

    if (!v) {
        return;
    }
    /* The objects go with the section container; the block is this app's. */
    free(v);
    app->net = NULL;
}

void rift_net_view_shape(struct rift_app *app)
{
    struct rift_net_view *v = view_of(app);
    int r;

    if (!v) {
        return;
    }
    v->wide = app->wide;
    /* Landscape: the rings are columns side by side under the path panel,
     * and scroll sideways when eleven do not fit; portrait stacks them. */
    lv_obj_set_flex_flow(v->rings, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    if (app->wide) {
        lv_obj_set_width(v->rings, LV_PCT(100));
        lv_obj_add_flag(v->rings, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(v->rings, LV_DIR_HOR);
    } else {
        lv_obj_remove_flag(v->rings, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to_x(v->rings, 0, LV_ANIM_OFF);
    }
    for (r = 0; r < RIFT_NET_RINGS; r++) {
        struct ring_view *rv = &v->ring[r];

        lv_obj_set_flex_flow(rv->box, app->wide ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        /* Landscape columns take equal shares of the width (grow from 1 px);
         * portrait rows the whole of it. The divider is a portrait row's. */
        lv_obj_set_flex_grow(rv->box, app->wide ? 1 : 0);
        lv_obj_set_width(rv->box, app->wide ? 1 : LV_PCT(100));
        lv_obj_set_style_border_width(rv->box, app->wide ? 0 : 1, 0);
        rv->empty = -1; /* heights are the shape's: set again on refresh */
        lv_obj_set_width(rv->head, app->wide ? LV_PCT(100) : RING_LABEL_W);
        lv_obj_set_style_pad_right(rv->head, app->wide ? RING_GUTTER_WIDE : 0, 0);
        lv_obj_set_flex_grow(rv->pills, app->wide ? 0 : 1);
        lv_obj_set_width(rv->pills, app->wide ? LV_PCT(100) : 1);
        lv_obj_set_flex_flow(rv->pills, app->wide ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_hor(rv->pills, app->wide ? 0 : 6, 0);
        /* The rail runs down a portrait ring; in a landscape column it is
         * laid across the top, under the ring's word. Both sizes are pixels
         * or a share of a fixed width, never of a content-sized parent. */
        if (app->wide) {
            lv_obj_set_size(rv->rail, LV_PCT(100), RAIL_W);
        } else {
            lv_obj_set_size(rv->rail, RAIL_W, RING_H);
        }
    }
}

/* ---- refresh -------------------------------------------------------------------- */

static int on_path(const struct rift_node *const *path, int n, const struct rift_node *node)
{
    int i;

    for (i = 0; i < n; i++) {
        if (path[i] == node) {
            return 1;
        }
    }
    return 0;
}

static void paint_path(struct rift_net_view *v, const struct rift_node *sel, int sel_ring,
                       enum rift_net_source src)
{
    struct rift_app *a = v->app;
    const struct rift_model *m = &a->model;
    char label[RIFT_LABEL_MAX];
    char state[RIFT_STATE_MAX];

    if (!sel) {
        rift_label_set(v->path_name, "No node selected");
        rift_label_set(v->path_state, "Choose a node on a ring, or in NODES, to write out "
                                      "how a message to it would go.");
        rift_label_set(v->path_chain, "");
        lv_obj_add_flag(v->path_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(v->path_bar, LV_OBJ_FLAG_HIDDEN);
    /* No MESSAGE for a node that takes none (rift_model.h): a repeater keeps
     * DETAIL, where it says why. */
    if (rift_node_can_message(sel)) {
        lv_obj_remove_flag(v->path_message, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->path_message, LV_OBJ_FLAG_HIDDEN);
    }
    rift_fmt_label(sel, label, sizeof(label));
    rift_label_set(v->path_name, label);
    if (src == RIFT_NET_SOURCE_ROUTE) {
        struct rift_path p;
        char chain[RIFT_CHAIN_MAX];
        const char *self = (m->have_identity && m->self_name[0]) ? m->self_name : "this device";

        rift_fmt_state(sel, state, sizeof(state));
        lv_label_set_text_fmt(v->path_state, "RING %s" RIFT_SEP "%s" RIFT_SEP "LEARNED ROUTE",
                              rift_net_ring_word(sel_ring), state);
        if (rift_path_parse(sel, &p) != 0) {
            memset(&p, 0, sizeof(p));
        }
        rift_path_chain(self, &p, label, rift_app_resolve, a, chain, sizeof(chain));
        rift_label_set(v->path_chain, chain);
    } else if (src == RIFT_NET_SOURCE_ADVERT) {
        /* A count and no chain: an advert's path is the relays it came
         * through to here, and the service keeps only how many. */
        lv_label_set_text_fmt(v->path_state,
                              "RING %s" RIFT_SEP "NO ROUTE LEARNED" RIFT_SEP
                              "LAST ADVERT CAME THROUGH %d RELAY%s",
                              rift_net_ring_word(sel_ring), sel->advert_hops,
                              sel->advert_hops == 1 ? "" : "S");
        rift_label_set(v->path_chain, "No chain to draw: a route is learned when a message to "
                                      "this node is answered.");
    } else {
        rift_label_set(v->path_state, "NO PATH" RIFT_SEP "NO ROUTE LEARNED, NO ADVERT HEARD "
                                      "THIS RUN");
        rift_label_set(v->path_chain, "");
    }
}

void rift_net_view_refresh(struct rift_app *app)
{
    struct rift_net_view *v = view_of(app);
    static struct rift_net net;
    const struct rift_node *path[RIFT_MAX_HOPS];
    const struct rift_node *sel;
    const struct rift_model *m;
    enum rift_net_source sel_src = RIFT_NET_SOURCE_NONE;
    int sel_ring = -1;
    int on = 0;
    int r;

    if (!v) {
        return;
    }
    m = &app->model;
    rift_net_build(m, rift_app_now(app), &net);
    sel = rift_app_selected(app);
    if (sel) {
        sel_ring = rift_net_ring_of(sel, &sel_src);
        on = rift_net_path_nodes(m, sel, path, RIFT_MAX_HOPS);
    }
    paint_path(v, sel, sel_ring, sel_src);

    if (net.nodes == 0) {
        rift_label_set(v->note, !m->snapshot_valid ? "Waiting for meshcored."
                                                   : "No node has adverted since this service "
                                                     "started, so there is nothing to place.");
        lv_obj_remove_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    } else if (m->stale) {
        rift_label_set(v->note, "Cached: meshcored is not answering.");
        lv_obj_remove_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    }

    for (r = 0; r < RIFT_NET_RINGS; r++) {
        struct ring_view *rv = &v->ring[r];
        const struct rift_net_ring *ring = &net.ring[r];
        int want = r == RIFT_NET_RING_SELF ? 1 : ring->shown + (ring->count > ring->shown);
        int tone;
        int i;

        while (rv->built < want && rv->built < PILLS_PER_RING) {
            build_pill(v, rv);
        }
        ring_word_fit(rv->word, r);
        if (r == RIFT_NET_RING_SELF) {
            fill_pill(&rv->pill[0], (m->have_identity && m->self_name[0]) ? m->self_name
                                                                           : "this device",
                      NULL, LOOK_SELF, v->wide);
            rift_label_set(rv->count, "");
        } else {

            for (i = 0; i < ring->shown; i++) {
                char label[RIFT_LABEL_MAX];
                const struct rift_node *n = ring->node[i];
                int look = n == sel ? LOOK_SELECTED
                                    : on_path(path, on, n) ? LOOK_ON_PATH : LOOK_PLAIN;

                rift_fmt_label(n, label, sizeof(label));
                fill_pill(&rv->pill[i], label, n->key, look, v->wide);
            }
            if (ring->count > ring->shown) {
                char more[16];

                snprintf(more, sizeof(more), "+%d", ring->count - ring->shown);
                fill_pill(&rv->pill[i], more, NULL, LOOK_PLAIN, v->wide);
                lv_obj_remove_flag(rv->pill[i].btn, LV_OBJ_FLAG_CLICKABLE);
                i++;
            }
            for (; i < rv->built; i++) {
                lv_obj_add_flag(rv->pill[i].btn, LV_OBJ_FLAG_HIDDEN);
                rv->pill[i].key[0] = '\0';
            }
            /* How many, and how many of them an advert placed: those have a
             * count and no chain. Nothing for an empty ring. */
            ring_count_fit(rv->count, ring->count, ring->advert);
            /* An empty count takes no line, so an empty ring is the header
             * row it is drawn at and its rail meets the next one. */
            if (ring->count) {
                lv_obj_remove_flag(rv->count, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(rv->count, LV_OBJ_FLAG_HIDDEN);
            }
        }
        /* The rail: accent from here out to the selected node's ring when it
         * has a route, muted through NO PATH, nothing otherwise. */
        if (r == RIFT_NET_RING_NO_PATH) {
            tone = RIFT_TONE_MUTED;
        } else if (sel_src == RIFT_NET_SOURCE_ROUTE && r <= sel_ring) {
            tone = RIFT_TONE_ACCENT;
        } else {
            tone = RIFT_TONE_NONE;
        }
        if (tone != rv->rail_tone) {
            rift_vrule_set(rv->rail, (enum rift_tone)tone);
            rv->rail_tone = tone;
        }
        /* An empty ring inside the deepest keeps its place on the scale at a
         * header row's height (portrait); a full one has the handoff's 68. */
        {
            int empty = r != RIFT_NET_RING_SELF && ring->count == 0;

            if (empty != rv->empty) {
                int32_t h = (empty && !v->wide) ? RING_H_EMPTY : RING_H;

                lv_obj_set_style_min_height(rv->box, v->wide ? 0 : h, 0);
                if (!v->wide) {
                    lv_obj_set_height(rv->rail, h);
                }
                rv->empty = empty;
            }
        }
        /* Rings with nothing on them beyond the deepest held stay out of the
         * way; the inner ones stay, so the scale reads from 0. */
        if (r != RIFT_NET_RING_SELF && r != RIFT_NET_RING_NO_PATH && ring->count == 0 &&
            r > net.deepest) {
            lv_obj_add_flag(rv->box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(rv->box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

int rift_net_view_pills(const struct rift_app *app, int ring)
{
    struct rift_net_view *v = view_of(app);

    return (v && ring >= 0 && ring < RIFT_NET_RINGS) ? v->ring[ring].built : 0;
}
