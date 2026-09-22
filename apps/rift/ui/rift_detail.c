/*
 * The selected node, in full. See rift_detail.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_detail.h"

#include "pos_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The hop ladder is built when the path changes and updated never: a row
 * per hop, and a path with more hops than this is drawn as far as it goes
 * with the rest named in the count beside it. MeshCore's own path cannot
 * exceed 63 hops (Packet::pathHashCount is six bits). */
#define LADDER_MAX 16
#define LADDER_ROW_H 40
#define KV_COUNT 4

struct kv {
    lv_obj_t *key;
    lv_obj_t *value;
    lv_obj_t *unit;
};

struct ladder_row {
    lv_obj_t *row;
    lv_obj_t *index;
    lv_obj_t *glyph;
    lv_obj_t *label;
    lv_obj_t *note;
};

struct rift_detail {
    struct rift_app *app;
    int compact;
    lv_obj_t *root;
    lv_obj_t *empty;
    lv_obj_t *body;

    lv_obj_t *title;
    lv_obj_t *title_tag;

    lv_obj_t *state_glyph;
    lv_obj_t *state_word;
    lv_obj_t *state_note;
    struct kv kv[KV_COUNT];
    lv_obj_t *signal_note;

    lv_obj_t *ident_name;
    lv_obj_t *ident_role;
    lv_obj_t *ident_key;
    lv_obj_t *ident_advert;
    lv_obj_t *ident_obs;

    lv_obj_t *path_caption;
    lv_obj_t *chain;
    lv_obj_t *ladder;
    struct ladder_row rung[LADDER_MAX];
    int rung_count;
    lv_obj_t *ladder_note;
    char path_shape[RIFT_PATH_HEX_MAX + 16];

    lv_obj_t *history_panel;
    lv_obj_t *history_row[RIFT_PATH_HISTORY];

    lv_obj_t *actions;
    lv_obj_t *act_message;
    lv_obj_t *act_reset;
    lv_obj_t *act_forget;
    /* What became of the last change asked for on this node. */
    lv_obj_t *op_line;
    int op_warn;

    /* The confirmation FORGET asks for before anything is sent (DS §17.5):
     * shown in place of the action bar, for one node, and dropped the moment
     * the selection moves to another. */
    lv_obj_t *confirm;
    lv_obj_t *confirm_title;
    int confirming;
    char confirm_key[RIFT_KEY_HEX];
};

/* ---- small builders ----------------------------------------------------- */

static lv_obj_t *column(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, gap, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *row_of(lv_obj_t *parent, int32_t height, int32_t gap)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
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

static void build_kv(struct rift_detail *d, lv_obj_t *parent, int i, const char *key)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_t *line;

    lv_obj_remove_style_all(cell);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cell, 2, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    d->kv[i].key = lv_label_create(cell);
    lv_obj_remove_style_all(d->kv[i].key);
    pos_style_add(d->kv[i].key, POS_STYLE_CAPTION, 0);
    lv_label_set_text(d->kv[i].key, key);
    line = row_of(cell, LV_SIZE_CONTENT, 4);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    d->kv[i].value = lv_label_create(line);
    lv_obj_remove_style_all(d->kv[i].value);
    pos_style_add(d->kv[i].value, POS_STYLE_VALUE, 0);
    lv_label_set_text(d->kv[i].value, RIFT_UNKNOWN);
    d->kv[i].unit = lv_label_create(line);
    lv_obj_remove_style_all(d->kv[i].unit);
    pos_style_add(d->kv[i].unit, POS_STYLE_CAPTION, 0);
    lv_label_set_text(d->kv[i].unit, "");
}

/* ---- the panels --------------------------------------------------------- */

static void build_link_panel(struct rift_detail *d, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "LINK STATE");
    lv_obj_t *line = row_of(panel, LV_SIZE_CONTENT, 10);
    lv_obj_t *kvs;
    int i;
    static const char *const keys[KV_COUNT] = { "HEARD", "LAST HOP", "SNR", "END-TO-END" };

    d->state_glyph = rift_glyph_create(line);
    d->state_word = lv_label_create(line);
    lv_obj_remove_style_all(d->state_word);
    pos_style_add(d->state_word, POS_STYLE_VALUE, 0);
    lv_label_set_text(d->state_word, "");
    d->state_note = lv_label_create(line);
    lv_obj_remove_style_all(d->state_note);
    pos_style_add(d->state_note, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(d->state_note, 1);
    lv_obj_set_style_text_align(d->state_note, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(d->state_note, "");

    kvs = row_of(panel, LV_SIZE_CONTENT, 12);
    lv_obj_set_flex_align(kvs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (i = 0; i < KV_COUNT; i++) {
        build_kv(d, kvs, i, keys[i]);
    }
    d->signal_note = wrapping(panel, POS_STYLE_CAPTION);
}

static void build_identity_panel(struct rift_detail *d, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "IDENTITY");

    d->ident_name = pocketui_kv_row(panel, "Name", RIFT_UNKNOWN);
    d->ident_role = pocketui_kv_row(panel, "Role", RIFT_UNKNOWN);
    d->ident_key = pocketui_kv_row(panel, "Key", RIFT_UNKNOWN);
    d->ident_advert = pocketui_kv_row(panel, "Advert", RIFT_UNKNOWN);
    /* This app's own count, and labelled as such: it is how many mesh.node
     * events have named this node since RIFT opened, not how many adverts
     * the service has seen. */
    d->ident_obs = pocketui_kv_row(panel, "Events since open", RIFT_UNKNOWN);
}

static void build_path_panel(struct rift_detail *d, lv_obj_t *parent)
{
    lv_obj_t *panel = rift_panel(parent, "PATH");

    d->path_caption = lv_label_create(panel);
    lv_obj_remove_style_all(d->path_caption);
    pos_style_add(d->path_caption, POS_STYLE_CAPTION, 0);
    lv_label_set_text(d->path_caption, "");
    d->chain = wrapping(panel, POS_STYLE_TEXT_SECONDARY);
    d->ladder = column(panel, 0);
    d->ladder_note = wrapping(panel, POS_STYLE_CAPTION);
}

static void build_history_panel(struct rift_detail *d, lv_obj_t *parent)
{
    int i;

    d->history_panel = rift_panel(parent, "PATH CHANGES SEEN BY RIFT");
    for (i = 0; i < RIFT_PATH_HISTORY; i++) {
        d->history_row[i] = lv_label_create(d->history_panel);
        lv_obj_remove_style_all(d->history_row[i]);
        pos_style_add(d->history_row[i], POS_STYLE_CAPTION, 0);
        lv_obj_set_width(d->history_row[i], LV_PCT(100));
        lv_obj_set_height(d->history_row[i], RIFT_ROW_H);
        lv_label_set_long_mode(d->history_row[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(d->history_row[i], "");
    }
}

static void on_back(lv_event_t *e)
{
    struct rift_detail *d = lv_event_get_user_data(e);

    rift_app_open_detail(d->app, 0);
}

static void on_message(lv_event_t *e)
{
    struct rift_detail *d = lv_event_get_user_data(e);
    const struct rift_node *n = rift_app_selected(d->app);

    /* This screen always shows the selected node, so that is the peer to
     * write to. It opens the conversation and nothing else. */
    if (n) {
        rift_app_open_conversation(d->app, n->key);
    }
}

/* Forget the route to this node: nothing is transmitted, and the next
 * message to it floods and learns a new one. Not destructive - the mesh
 * gives a route back with the next reply - so it is one press. */
static void on_reset(lv_event_t *e)
{
    struct rift_detail *d = lv_event_get_user_data(e);
    const struct rift_node *n = rift_app_selected(d->app);
    char label[RIFT_LABEL_MAX];

    if (!n) {
        return;
    }
    rift_fmt_label(n, label, sizeof(label));
    rift_ipc_reset_path(&d->app->ipc, n->key, label);
    rift_app_refresh(d->app);
}

/* FORGET only asks. Nothing is sent until the confirmation is pressed. */
static void on_forget(lv_event_t *e)
{
    struct rift_detail *d = lv_event_get_user_data(e);
    const struct rift_node *n = rift_app_selected(d->app);

    if (!n) {
        return;
    }
    d->confirming = 1;
    snprintf(d->confirm_key, sizeof(d->confirm_key), "%s", n->key);
    rift_app_refresh(d->app);
}

static void on_forget_cancel(lv_event_t *e)
{
    struct rift_detail *d = lv_event_get_user_data(e);

    d->confirming = 0;
    rift_app_refresh(d->app);
}

/* The one place a node is forgotten from: the confirmation, for the node it
 * was asked about - never the first press, and never a node the selection
 * has since moved to. */
static void on_forget_confirm(lv_event_t *e)
{
    struct rift_detail *d = lv_event_get_user_data(e);
    const struct rift_node *n = rift_app_selected(d->app);
    char label[RIFT_LABEL_MAX];

    if (n && d->confirming && strcmp(n->key, d->confirm_key) == 0) {
        rift_fmt_label(n, label, sizeof(label));
        rift_ipc_forget_node(&d->app->ipc, n->key, label);
    }
    d->confirming = 0;
    rift_app_refresh(d->app);
}

/* The actions come first, under the title, where a reader reaches them
 * without scrolling: at the foot of four panels they were below the fold in
 * the landscape pane and on the portrait screen alike. */
static void build_actions(struct rift_detail *d, lv_obj_t *parent)
{
    lv_obj_t *buttons;
    lv_obj_t *body;

    d->actions = row_of(parent, RIFT_TOUCH_H, 12);
    if (!d->compact) {
        rift_action(d->actions, "\xE2\x80\xB9 NODES", 0, 1, on_back, d);
    }
    d->act_message = rift_action(d->actions, "MESSAGE", 1, 1, on_message, d);
    d->act_reset = rift_action(d->actions, "RE-ROUTE", 0, 1, on_reset, d);
    d->act_forget = rift_action(d->actions, "FORGET", 0, 1, on_forget, d);

    /* DS §17.5: a panel with a title, the consequence in words, and exactly
     * two buttons, Cancel first. Forgetting a node cannot be undone from
     * here - it comes back only when it adverts - so the accent is on
     * Cancel, the power-off precedent. */
    d->confirm = rift_panel(parent, NULL);
    d->confirm_title = lv_label_create(d->confirm);
    lv_obj_remove_style_all(d->confirm_title);
    pos_style_add(d->confirm_title, POS_STYLE_TITLE, 0);
    lv_obj_set_width(d->confirm_title, LV_PCT(100));
    lv_label_set_long_mode(d->confirm_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(d->confirm_title, "");
    body = wrapping(d->confirm, POS_STYLE_TEXT_SECONDARY);
    lv_obj_set_style_pad_top(body, 12, 0);
    lv_obj_set_style_pad_bottom(body, 20, 0);
    lv_label_set_text(body, "The radio service drops this node, its route and its last "
                            "advert. It comes back when it next adverts; until then no "
                            "message can be sent to it.");
    buttons = row_of(d->confirm, RIFT_TOUCH_H, 8);
    rift_action(buttons, "CANCEL", 1, 1, on_forget_cancel, d);
    rift_action(buttons, "FORGET", 0, 1, on_forget_confirm, d);
    lv_obj_add_flag(d->confirm, LV_OBJ_FLAG_HIDDEN);

    d->op_line = wrapping(parent, POS_STYLE_CAPTION);
    lv_obj_add_flag(d->op_line, LV_OBJ_FLAG_HIDDEN);
}

struct rift_detail *rift_detail_create(struct rift_app *app, lv_obj_t *parent, int compact)
{
    struct rift_detail *d = calloc(1, sizeof(*d));
    lv_obj_t *head;

    if (!d) {
        return NULL;
    }
    d->app = app;
    d->compact = compact ? 1 : 0;

    d->root = lv_obj_create(parent);
    lv_obj_remove_style_all(d->root);
    lv_obj_set_size(d->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(d->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(d->root, compact ? RIFT_PANE_PAD : RIFT_PAD, 0);
    lv_obj_set_style_pad_row(d->root, 16, 0);
    lv_obj_set_scroll_dir(d->root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(d->root, LV_SCROLLBAR_MODE_AUTO);

    d->empty = wrapping(d->root, POS_STYLE_TEXT_MUTED);
    lv_label_set_text(d->empty, "Select a node to see its link and its path.");

    d->body = column(d->root, 16);
    head = row_of(d->body, LV_SIZE_CONTENT, 10);
    d->title = lv_label_create(head);
    lv_obj_remove_style_all(d->title);
    pos_style_add(d->title, POS_STYLE_TITLE, 0);
    lv_label_set_long_mode(d->title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(d->title, "");
    d->title_tag = lv_label_create(head);
    lv_obj_remove_style_all(d->title_tag);
    pos_style_add(d->title_tag, POS_STYLE_CAPTION, 0);
    lv_obj_set_flex_grow(d->title_tag, 1);
    lv_label_set_text(d->title_tag, "");

    /* What a reader acts on first, then what they came to read: the link,
     * the route and how it changed. The identity panel - the name and role
     * are already in the title - goes last. */
    build_actions(d, d->body);
    build_link_panel(d, d->body);
    build_path_panel(d, d->body);
    build_history_panel(d, d->body);
    build_identity_panel(d, d->body);
    return d;
}

void rift_detail_cancel_confirm(struct rift_detail *d)
{
    /* Equivalent to Cancel (DS §17.5: any other way out is Cancel). The
     * next refresh puts the action bar back. */
    if (d) {
        d->confirming = 0;
    }
}

void rift_detail_destroy(struct rift_detail *d)
{
    /* The objects are children of the parent the caller gave and go with
     * it; only this block is ours. */
    free(d);
}

/* ---- refresh ------------------------------------------------------------ */

static void set_text(lv_obj_t *label, const char *text)
{
    if (label) {
        lv_label_set_text(label, text ? text : "");
    }
}

static void show(lv_obj_t *obj, int visible)
{
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_ladder(struct rift_detail *d, const struct rift_path *p, const char *self_label,
                         const char *target_label)
{
    int total = p->known ? p->hops + 2 : 2;
    int i;

    lv_obj_clean(d->ladder);
    memset(d->rung, 0, sizeof(d->rung));
    d->rung_count = 0;
    if (total > LADDER_MAX) {
        total = LADDER_MAX;
    }
    for (i = 0; i < total; i++) {
        struct rift_ladder_row info;
        struct ladder_row *r = &d->rung[d->rung_count];
        int index = i;

        /* The last rung drawn is always the target, so a path too long for
         * the ladder still ends where it ends. */
        if (p->known && i == total - 1) {
            index = p->hops + 1;
        }
        if (rift_path_ladder_row(p, index, self_label, target_label, rift_app_resolve, d->app,
                                 &info) != 0) {
            continue;
        }
        r->row = row_of(d->ladder, LADDER_ROW_H, 8);
        r->index = rift_cell(r->row, POS_STYLE_CAPTION, 24, LV_TEXT_ALIGN_RIGHT);
        lv_label_set_text_fmt(r->index, "%d", info.index);
        r->glyph = rift_glyph_create(r->row);
        switch (info.kind) {
        case RIFT_CELL_SELF:
            rift_glyph_set(r->glyph, RIFT_GLYPH_SELF);
            break;
        case RIFT_CELL_UNKNOWN:
            rift_glyph_set(r->glyph, RIFT_GLYPH_UNKNOWN);
            break;
        case RIFT_CELL_TARGET:
            rift_glyph_set(r->glyph, p->known && p->direct ? RIFT_GLYPH_DIRECT
                                                           : RIFT_GLYPH_RELAYED);
            break;
        default:
            rift_glyph_set(r->glyph, RIFT_GLYPH_RELAYED);
            break;
        }
        r->label = rift_cell(r->row, POS_STYLE_ROW_TITLE, 150, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(r->label, info.label);
        r->note = rift_cell(r->row, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(r->note, 1);
        lv_label_set_text(r->note, info.note);
        d->rung_count++;
    }
}

static void refresh_path(struct rift_detail *d, const struct rift_node *n, const char *label)
{
    const struct rift_model *m = &d->app->model;
    struct rift_path p;
    char shape[sizeof(d->path_shape)];
    char chain[RIFT_CHAIN_MAX];
    const char *self_label = (m->have_identity && m->self_name[0]) ? m->self_name : "this device";

    if (rift_path_parse(n, &p) != 0) {
        memset(&p, 0, sizeof(p));
        p.known = 0;
    }
    snprintf(shape, sizeof(shape), "%d/%d/%s", p.known, p.hops, n->path_hex);
    if (strcmp(shape, d->path_shape) != 0) {
        snprintf(d->path_shape, sizeof(d->path_shape), "%s", shape);
        build_ladder(d, &p, self_label, label);
    }
    lv_label_set_text_fmt(d->path_caption, "%s" RIFT_ARROW "%s", self_label, label);
    rift_path_chain(self_label, &p, label, rift_app_resolve, d->app, chain, sizeof(chain));
    set_text(d->chain, chain);
    if (!p.known) {
        set_text(d->ladder_note, "No route back has been observed. The hop count is unknown, "
                                 "not zero.");
    } else if (p.unknown > 0) {
        lv_label_set_text_fmt(d->ladder_note,
                              "%d of %d hops are not named in the path this node advertised.",
                              p.unknown, p.hops);
    } else if (p.hops + 2 > LADDER_MAX) {
        lv_label_set_text_fmt(d->ladder_note, "%d hops; the first %d and the target are shown.",
                              p.hops, LADDER_MAX - 2);
    } else {
        set_text(d->ladder_note, "");
    }
    show(d->ladder_note, strlen(lv_label_get_text(d->ladder_note)) > 0);
}

static void refresh_history(struct rift_detail *d, const struct rift_node *n, int64_t now_ms)
{
    int shown = 0;
    int i;

    for (i = 0; i < RIFT_PATH_HISTORY; i++) {
        const struct rift_path_obs *o = &n->hist[i];
        char age[RIFT_AGE_MAX];

        if (i >= n->hist_count) {
            show(d->history_row[i], 0);
            continue;
        }
        rift_fmt_age(now_ms - o->mono_ms, o->have_mono, age, sizeof(age));
        if (!o->path_known) {
            lv_label_set_text_fmt(d->history_row[i], "%s" RIFT_SEP "no path%s", age,
                                  i == 0 ? RIFT_SEP "current" : "");
        } else {
            lv_label_set_text_fmt(d->history_row[i], "%s" RIFT_SEP "%d hop%s%s", age, o->hops,
                                  o->hops == 1 ? "" : "s", i == 0 ? RIFT_SEP "current" : "");
        }
        show(d->history_row[i], 1);
        shown++;
    }
    /* One observation is not a history: it is the only thing anybody has
     * told us, and a panel headed "changes" holding one row would read as
     * a change that did not happen. */
    show(d->history_panel, shown > 1);
}

/* Which actions can be pressed, whether the confirmation is up, and what
 * became of the last change asked for on this node. */
static void refresh_actions(struct rift_detail *d, const struct rift_node *n)
{
    const struct rift_model *m = &d->app->model;
    const struct rift_action_state *op = &m->node_op;
    /* The model's word for it, as the composer uses (rift_thread_refusal):
     * a request made while the socket is between connections is refused by
     * the client with a reason, where the reader pressed. */
    int answering = !m->stale && m->state != RIFT_SVC_ABSENT;
    int busy = rift_model_action_busy(m, RIFT_ACTION_FORGET);
    char label[RIFT_LABEL_MAX];
    char text[RIFT_ACTION_TEXT_MAX];
    int warn;

    /* A confirmation belongs to the node it was asked about. */
    if (d->confirming && strcmp(d->confirm_key, n->key) != 0) {
        d->confirming = 0;
    }
    show(d->actions, !d->confirming);
    show(d->confirm, d->confirming);
    if (d->confirming) {
        rift_fmt_label(n, label, sizeof(label));
        lv_label_set_text_fmt(d->confirm_title, "Forget %s?", label);
    }
    /* A route can be forgotten only when there is one; either change needs a
     * service to ask, and one change at a time. MESSAGE is always there: it
     * only opens a conversation. */
    rift_action_set_enabled(d->act_reset, 0, answering && !busy && n->path_known);
    rift_action_set_enabled(d->act_forget, 0, answering && !busy);

    if (op->kind == RIFT_ACTION_NONE || strcmp(op->key, n->key) != 0) {
        show(d->op_line, 0);
        return;
    }
    rift_fmt_action(op, rift_app_now(d->app), text, sizeof(text));
    set_text(d->op_line, text);
    show(d->op_line, text[0] != '\0');
    /* Colour agrees with the words, never carries them (DS §2). */
    warn = op->failed;
    if (warn != d->op_warn) {
        if (warn) {
            pos_style_add(d->op_line, POS_STYLE_STATUS_WARN_TEXT, 0);
        } else {
            lv_obj_remove_style(d->op_line, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        }
        d->op_warn = warn;
    }
}

void rift_detail_refresh(struct rift_detail *d, const struct rift_node *n)
{
    int64_t now = rift_app_now(d ? d->app : NULL);
    char label[RIFT_LABEL_MAX];
    char text[RIFT_STATE_MAX];
    char value[RIFT_SIGNAL_MAX];
    char rssi[RIFT_SIGNAL_MAX];
    char unit[8];
    const char *word;
    int direct;

    if (!d) {
        return;
    }
    show(d->empty, n == NULL);
    show(d->body, n != NULL);
    if (!n) {
        d->confirming = 0;
        return;
    }
    refresh_actions(d, n);
    rift_fmt_label(n, label, sizeof(label));
    set_text(d->title, label);
    word = rift_type_tag(n->type, n->have_type);
    if (word) {
        lv_label_set_text_fmt(d->title_tag, "%s" RIFT_SEP "%s", word, n->hash);
    } else {
        lv_label_set_text_fmt(d->title_tag, "%s", n->hash);
    }

    rift_glyph_set(d->state_glyph, rift_app_glyph(n, now));
    rift_fmt_state(n, text, sizeof(text));
    set_text(d->state_word, text);
    direct = rift_link_of(n) == RIFT_LINK_DIRECT;
    if (rift_node_is_stale(n, now)) {
        set_text(d->state_note, "NOT HEARD > 12 H");
    } else if (!n->have_heard) {
        set_text(d->state_note, "NEVER HEARD");
    } else {
        set_text(d->state_note, "");
    }

    rift_fmt_age_split(now - n->heard_mono_ms, n->have_heard, value, sizeof(value), unit,
                       sizeof(unit));
    set_text(d->kv[0].value, value);
    set_text(d->kv[0].unit, unit);
    rift_fmt_rssi(n->rssi_dbm, n->have_rssi, rssi, sizeof(rssi));
    set_text(d->kv[1].value, rssi);
    set_text(d->kv[1].unit, n->have_rssi ? "dBm" : "");
    rift_fmt_snr(n->snr_db, n->have_snr, value, sizeof(value));
    set_text(d->kv[2].value, value);
    set_text(d->kv[2].unit, n->have_snr ? "dB" : "");
    /* End to end over relays is not a measurement nobody has taken yet; it
     * is one that cannot exist. U+2014, not "?" (handoff §6). Heard direct,
     * the last hop is the node, so the same value is its own. */
    set_text(d->kv[3].value, direct ? rssi : RIFT_EMDASH);
    set_text(d->kv[3].unit, direct && n->have_rssi ? "dBm" : "");
    /* One line each: the same two facts the three-line versions said. */
    if (direct) {
        set_text(d->signal_note, "Heard with no relay between: the signal is its own.");
    } else {
        /* mesh.* reports the signal of the frame that was heard and does
         * not name which hop transmitted it, so this screen does not name
         * one either. */
        set_text(d->signal_note, "Signal of the last hop heard, not of the node.");
    }

    set_text(d->ident_name, n->have_name && n->name[0] ? n->name : RIFT_UNKNOWN);
    word = rift_type_word(n->type, n->have_type);
    set_text(d->ident_role, word ? word : RIFT_UNKNOWN);
    rift_fmt_key_short(n->key, text, sizeof(text));
    set_text(d->ident_key, text);
    if (n->have_advert) {
        /* Their clock, not ours, and said so: this board has no clock that
         * survives a power cut and the number is the sender's stamp. */
        lv_label_set_text_fmt(d->ident_advert, "%lld (their clock)",
                              (long long)n->advert_timestamp);
    } else {
        set_text(d->ident_advert, RIFT_UNKNOWN);
    }
    lv_label_set_text_fmt(d->ident_obs, "%u", n->observations);

    refresh_path(d, n, label);
    refresh_history(d, n, now);
}
