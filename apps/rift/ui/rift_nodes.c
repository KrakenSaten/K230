/*
 * NODES. See rift_nodes.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_nodes.h"

#include "pos_styles.h"
#include "rift_detail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Column widths. Every column but the name is fixed, so a row cannot reflow
 * as its values change width, and the header and the rows are built from the
 * same list: a role tag of its own would be one item wider than the header
 * and would put every column after it out by the tag's width, so the name
 * and its tag share one box that is the column.
 *
 * The widths are what the widest value in each column measures in Mono 14
 * (RSSI is "−103", HEARD is "HEARD" in the header) with room to spare
 * in Outdoor. */
#define COL_GAP 8
#define COL_GLYPH RIFT_GLYPH_BOX
#define COL_HOPS 40
#define COL_RSSI 56
#define COL_SNR 44
#define COL_HEARD 48
#define SELECTED_INSET 12

struct node_row {
    lv_obj_t *slot;
    lv_obj_t *line;
    lv_obj_t *glyph;
    lv_obj_t *namebox;
    lv_obj_t *name;
    lv_obj_t *tag;
    lv_obj_t *strip;
    lv_obj_t *hops;
    lv_obj_t *rssi;
    lv_obj_t *snr;
    lv_obj_t *heard;
    /* The expansion, portrait and selected only. */
    lv_obj_t *expand;
    lv_obj_t *exp_glyph;
    lv_obj_t *exp_state;
    lv_obj_t *exp_chain;
    lv_obj_t *exp_signal;
    char key[RIFT_KEY_HEX];
    struct rift_nodes *owner;
};

struct rift_nodes {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *pane_list;
    lv_obj_t *head;
    lv_obj_t *head_cell[6];
    lv_obj_t *list;
    lv_obj_t *note;
    lv_obj_t *pane_ctx;
    struct rift_detail *detail_pane; /* landscape right pane */
    struct rift_detail *detail_full; /* portrait pushed screen */
    lv_obj_t *detail_full_root;

    struct node_row row[RIFT_MAX_NODES];
    int row_count;

    /* What the built list was chosen from. */
    char shape_key[RIFT_MAX_NODES][RIFT_KEY_HEX];
    int shape_count;
    int shape_fresh;
    char shape_sel[RIFT_KEY_HEX];
    int shape_wide;
    int shape_valid;

    /* The order of the last refresh, so a key press can step through it. */
    char order_key[RIFT_MAX_NODES][RIFT_KEY_HEX];
    int order_count;

    /* The selection moved: bring its row into view on this refresh. A list
     * of thirty nodes is taller than a landscape body, and arrows that moved
     * the selection off the bottom of the pane were moving it out of sight. */
    int reveal;
};

static int32_t strip_width(const struct rift_app *a)
{
    return a->wide ? RIFT_STRIP_W_WIDE : RIFT_STRIP_W;
}

/* ---- building ----------------------------------------------------------- */

static lv_obj_t *dense_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, height);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, COL_GAP, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    /* A layout box takes no taps. The row's own line asks for them back,
     * and it is the only thing in the list that is clickable: a nested box
     * that kept the default would swallow the tap meant for the row under
     * it (RIFT-DEV-1 wants the whole row width as the hit area). */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* A column that is only there so the header lines up with the rows. */
static lv_obj_t *spacer(lv_obj_t *parent, int32_t width)
{
    lv_obj_t *s = lv_obj_create(parent);

    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, width, 1);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    return s;
}

static void build_head(struct rift_nodes *v)
{
    struct rift_app *a = v->app;
    int i = 0;

    v->head = dense_row(v->pane_list, RIFT_HEADER_ROW_H);
    spacer(v->head, COL_GLYPH);
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->head_cell[i], 1);
    lv_label_set_text(v->head_cell[i], "NODE");
    i++;
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, strip_width(a), LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(v->head_cell[i], "PATH");
    i++;
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, COL_HOPS, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(v->head_cell[i], "HOPS");
    i++;
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, COL_RSSI, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(v->head_cell[i], "RSSI");
    i++;
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, COL_SNR, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(v->head_cell[i], "SNR");
    i++;
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(v->head_cell[i], "HEARD");
    rift_rule(v->pane_list);
}

static void on_row(lv_event_t *e)
{
    const struct node_row *r = lv_event_get_user_data(e);

    rift_app_select(r->owner->app, r->key);
}

static void on_detail(lv_event_t *e)
{
    struct rift_nodes *v = lv_event_get_user_data(e);

    rift_app_open_detail(v->app, 1);
}

/* Write to this node. It opens COMMS on a conversation with the peer -
 * which may hold nothing yet, and is not given anything to make it look as
 * though it does. It sends nothing; it only points the composer. */
static void on_message(lv_event_t *e)
{
    const struct node_row *r = lv_event_get_user_data(e);

    rift_app_open_conversation(r->owner->app, r->key);
}

static void build_expansion(struct rift_nodes *v, struct node_row *r)
{
    lv_obj_t *line;
    lv_obj_t *bar;

    r->expand = lv_obj_create(r->slot);
    lv_obj_remove_style_all(r->expand);
    lv_obj_set_width(r->expand, LV_PCT(100));
    lv_obj_set_height(r->expand, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->expand, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r->expand, 8, 0);
    lv_obj_set_style_pad_top(r->expand, 8, 0);
    lv_obj_remove_flag(r->expand, LV_OBJ_FLAG_SCROLLABLE);

    line = dense_row(r->expand, RIFT_ROW_H);
    r->exp_glyph = rift_glyph_create(line);
    r->exp_state = rift_cell(line, POS_STYLE_VALUE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->exp_state, 1);

    r->exp_chain = lv_label_create(r->expand);
    lv_obj_remove_style_all(r->exp_chain);
    pos_style_add(r->exp_chain, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(r->exp_chain, LV_PCT(100));
    lv_label_set_long_mode(r->exp_chain, LV_LABEL_LONG_WRAP);
    lv_label_set_text(r->exp_chain, "");

    r->exp_signal = lv_label_create(r->expand);
    lv_obj_remove_style_all(r->exp_signal);
    pos_style_add(r->exp_signal, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(r->exp_signal, LV_PCT(100));
    lv_label_set_long_mode(r->exp_signal, LV_LABEL_LONG_WRAP);
    lv_label_set_text(r->exp_signal, "");

    /* Two actions. The design's third, PATH, was drawn in the disabled
     * treatment with nothing behind it; the path is on DETAIL, whole, and a
     * button that can never be pressed is width taken from the two that can. */
    bar = dense_row(r->expand, RIFT_TOUCH_H);
    lv_obj_set_style_pad_column(bar, 12, 0);
    rift_action(bar, "MESSAGE", 1, 1, on_message, r);
    rift_action(bar, "DETAIL \xE2\x80\xBA", 0, 1, on_detail, v);
}

static void build_row(struct rift_nodes *v, const struct rift_node *n, int selected)
{
    struct rift_app *a = v->app;
    struct node_row *r = &v->row[v->row_count];

    memset(r, 0, sizeof(*r));
    r->owner = v;
    snprintf(r->key, sizeof(r->key), "%s", n->key);

    r->slot = lv_obj_create(v->list);
    lv_obj_remove_style_all(r->slot);
    lv_obj_set_width(r->slot, LV_PCT(100));
    lv_obj_set_height(r->slot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_SCROLLABLE);
    if (selected) {
        pos_style_add(r->slot, POS_STYLE_SLAB, 0);
        pos_style_add(r->slot, POS_STYLE_SELECTED, 0);
        lv_obj_set_style_pad_all(r->slot, SELECTED_INSET, 0);
    }

    r->line = dense_row(r->slot, RIFT_ROW_H);
    /* The whole row is the hit area, and a tap on it selects and does
     * nothing else (RIFT-DEV-1). */
    lv_obj_add_flag(r->line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->line, on_row, LV_EVENT_CLICKED, r);

    r->glyph = rift_glyph_create(r->line);
    /* The name and its role tag are one column, so the columns after them
     * do not move by the width of a tag that some rows have and some do
     * not. The name takes what the tag leaves and ends in an ellipsis when
     * it does not fit: a remote name is chosen by whoever is on the air and
     * can be any length. */
    r->namebox = dense_row(r->line, RIFT_ROW_H);
    lv_obj_set_flex_grow(r->namebox, 1);
    lv_obj_set_width(r->namebox, 1);
    lv_obj_set_style_pad_column(r->namebox, 6, 0);
    r->name = rift_cell(r->namebox, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->name, 1);
    lv_obj_set_width(r->name, 1);
    r->tag = rift_cell(r->namebox, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    r->strip = rift_strip_create(r->line, strip_width(a));
    r->hops = rift_cell(r->line, POS_STYLE_CAPTION, COL_HOPS, LV_TEXT_ALIGN_RIGHT);
    r->rssi = rift_cell(r->line, POS_STYLE_CAPTION, COL_RSSI, LV_TEXT_ALIGN_RIGHT);
    r->snr = rift_cell(r->line, POS_STYLE_CAPTION, COL_SNR, LV_TEXT_ALIGN_RIGHT);
    r->heard = rift_cell(r->line, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    if (selected && !a->wide) {
        build_expansion(v, r);
    }
    v->row_count++;
}

static void rebuild(struct rift_nodes *v, const struct rift_node **order, int count, int fresh)
{
    struct rift_app *a = v->app;
    const struct rift_node *sel = rift_app_selected(a);
    int stale_shown = 0;
    int unheard_shown = 0;
    int stale_count = 0;
    int unheard_count = 0;
    int i;

    lv_obj_clean(v->list);
    v->row_count = 0;
    for (i = fresh; i < count; i++) {
        if (order[i]->have_heard) {
            stale_count++;
        } else {
            unheard_count++;
        }
    }
    for (i = 0; i < count && v->row_count < RIFT_MAX_NODES; i++) {
        const struct rift_node *n = order[i];

        if (i == 0 && fresh > 0) {
            char text[48];

            snprintf(text, sizeof(text), "HEARD < 12 H" RIFT_SEP "%d", fresh);
            rift_group_label(v->list, text);
        }
        if (!stale_shown && i >= fresh && n->have_heard) {
            char text[48];

            snprintf(text, sizeof(text), "NOT HEARD > 12 H" RIFT_SEP "%d", stale_count);
            rift_group_label(v->list, text);
            stale_shown = 1;
        }
        if (!unheard_shown && !n->have_heard) {
            char text[48];

            snprintf(text, sizeof(text), "NEVER HEARD" RIFT_SEP "%d", unheard_count);
            rift_group_label(v->list, text);
            unheard_shown = 1;
        }
        build_row(v, n, sel && strcmp(sel->key, n->key) == 0);
    }
}

/* ---- updating ----------------------------------------------------------- */

static void update_row(struct rift_nodes *v, struct node_row *r, const struct rift_node *n,
                       int64_t now)
{
    struct rift_app *a = v->app;
    struct rift_path p;
    char text[RIFT_CHAIN_MAX];
    char small[RIFT_SIGNAL_MAX];
    const char *tag;
    int stale = rift_node_is_stale(n, now);

    if (rift_path_parse(n, &p) != 0) {
        memset(&p, 0, sizeof(p));
    }
    rift_glyph_set(r->glyph, rift_app_glyph(n, now));
    /* The tag was set, and the pane laid out, before this loop began: the
     * name and the tag share one column, the tag is the one with a size of
     * its own, and a name fitted before its tag had taken its width would be
     * fitted to room it does not have. See rift_nodes_refresh. */
    (void)tag;
    rift_fmt_label(n, text, sizeof(text));
    rift_cell_set_text_fit(r->name, text);
    rift_strip_set_width(r->strip, strip_width(a));
    rift_strip_set(r->strip, &p, stale);
    rift_fmt_hops(n, small, sizeof(small));
    lv_label_set_text(r->hops, small);
    rift_fmt_rssi(n->rssi_dbm, n->have_rssi, small, sizeof(small));
    lv_label_set_text(r->rssi, small);
    rift_fmt_snr(n->snr_db, n->have_snr, small, sizeof(small));
    lv_label_set_text(r->snr, small);
    /* SNR is one of the extra columns landscape has room for (handoff §9);
     * portrait shows the same node with one column fewer, never with a
     * value squeezed into a place it does not fit. */
    if (a->wide) {
        lv_obj_remove_flag(r->snr, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(r->snr, LV_OBJ_FLAG_HIDDEN);
    }
    rift_fmt_age(now - n->heard_mono_ms, n->have_heard, small, sizeof(small));
    lv_label_set_text(r->heard, small);

    if (r->expand) {
        const struct rift_model *m = &a->model;
        const char *self =
            (m->have_identity && m->self_name[0]) ? m->self_name : "this device";
        char label[RIFT_LABEL_MAX];
        char rssi[RIFT_SIGNAL_MAX];
        char snr[RIFT_SIGNAL_MAX];
        char advert[RIFT_AGE_MAX];

        rift_glyph_set(r->exp_glyph, rift_app_glyph(n, now));
        rift_fmt_state(n, text, sizeof(text));
        lv_label_set_text(r->exp_state, text);
        rift_fmt_label(n, label, sizeof(label));
        rift_path_chain(self, &p, label, rift_app_resolve, a, text, sizeof(text));
        lv_label_set_text(r->exp_chain, text);
        rift_fmt_rssi(n->rssi_dbm, n->have_rssi, rssi, sizeof(rssi));
        rift_fmt_snr(n->snr_db, n->have_snr, snr, sizeof(snr));
        rift_fmt_age(now - n->heard_mono_ms, n->have_heard, advert, sizeof(advert));
        lv_label_set_text_fmt(r->exp_signal,
                              "LAST HOP %s" RIFT_SEP "SNR %s" RIFT_SEP "HEARD %s" RIFT_SEP
                              "%u OBS",
                              rssi, snr, advert, n->observations);
    }
}

/* ---- layout ------------------------------------------------------------- */

void rift_nodes_shape(struct rift_app *app)
{
    struct rift_nodes *v = app ? app->nodes : NULL;

    if (!v) {
        return;
    }
    lv_obj_set_flex_flow(v->root, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(v->pane_list, 1);
    if (app->wide) {
        lv_obj_set_height(v->pane_list, LV_PCT(100));
        lv_obj_set_width(v->pane_ctx, RIFT_CONTEXT_W);
        lv_obj_set_height(v->pane_ctx, LV_PCT(100));
        lv_obj_remove_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_width(v->pane_list, LV_PCT(100));
        lv_obj_add_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    }
    /* Portrait pushes a DETAIL screen; landscape never does, because the
     * pane beside the list is already showing it. A confirmation up in the
     * old shape does not follow the reader into the new one. */
    if (app->wide && app->detail_open) {
        app->detail_open = 0;
    }
    rift_nodes_cancel_confirm(app);
    if (v->head_cell[1]) {
        lv_obj_set_width(v->head_cell[1], strip_width(app));
    }
    lv_obj_add_flag(v->head_cell[4], LV_OBJ_FLAG_HIDDEN);
    if (app->wide) {
        lv_obj_remove_flag(v->head_cell[4], LV_OBJ_FLAG_HIDDEN);
    }
    v->shape_valid = 0; /* the rows are laid out from the shape too */
}

/* ---- the portrait DETAIL screen ----------------------------------------- */

static void ensure_detail_full(struct rift_nodes *v, int open)
{
    if (open && !v->detail_full) {
        v->detail_full_root = lv_obj_create(v->root);
        lv_obj_remove_style_all(v->detail_full_root);
        lv_obj_set_size(v->detail_full_root, LV_PCT(100), LV_PCT(100));
        lv_obj_remove_flag(v->detail_full_root, LV_OBJ_FLAG_SCROLLABLE);
        v->detail_full = rift_detail_create(v->app, v->detail_full_root, 0);
    }
    if (v->detail_full_root) {
        if (open) {
            lv_obj_remove_flag(v->detail_full_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(v->detail_full_root, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (v->pane_list) {
        if (open) {
            lv_obj_add_flag(v->pane_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(v->pane_list, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ---- the public entry points -------------------------------------------- */

lv_obj_t *rift_nodes_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_nodes *v = calloc(1, sizeof(*v));

    if (!v) {
        return NULL;
    }
    app->nodes = v;
    v->app = app;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);

    v->pane_list = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->pane_list);
    lv_obj_set_width(v->pane_list, LV_PCT(100));
    lv_obj_set_flex_grow(v->pane_list, 1);
    lv_obj_set_flex_flow(v->pane_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(v->pane_list, RIFT_PAD, 0);
    lv_obj_remove_flag(v->pane_list, LV_OBJ_FLAG_SCROLLABLE);
    build_head(v);

    v->list = lv_obj_create(v->pane_list);
    lv_obj_remove_style_all(v->list);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    lv_obj_set_flex_flow(v->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->list, LV_SCROLLBAR_MODE_AUTO);

    v->note = lv_label_create(v->pane_list);
    lv_obj_remove_style_all(v->note);
    pos_style_add(v->note, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(v->note, LV_PCT(100));
    lv_obj_set_style_pad_ver(v->note, 6, 0);
    lv_label_set_long_mode(v->note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v->note, "");
    lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);

    v->pane_ctx = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->pane_ctx);
    /* The context pane is a DS panel: a hairline in `line` is what divides
     * the two panes of the landscape split, and the panel role is where
     * that hairline is defined. Its own padding is cleared because the
     * detail inside brings the pane padding of handoff §4. */
    pos_style_add(v->pane_ctx, POS_STYLE_PANEL, 0);
    lv_obj_set_style_pad_all(v->pane_ctx, 0, 0);
    lv_obj_set_width(v->pane_ctx, RIFT_CONTEXT_W);
    lv_obj_set_height(v->pane_ctx, LV_PCT(100));
    lv_obj_remove_flag(v->pane_ctx, LV_OBJ_FLAG_SCROLLABLE);
    v->detail_pane = rift_detail_create(app, v->pane_ctx, 1);
    lv_obj_add_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    return v->root;
}

void rift_nodes_cancel_confirm(struct rift_app *app)
{
    struct rift_nodes *v = app ? app->nodes : NULL;

    if (v) {
        rift_detail_cancel_confirm(v->detail_pane);
        rift_detail_cancel_confirm(v->detail_full);
    }
}

void rift_nodes_destroy(struct rift_app *app)
{
    struct rift_nodes *v = app ? app->nodes : NULL;

    if (!v) {
        return;
    }
    rift_detail_destroy(v->detail_pane);
    rift_detail_destroy(v->detail_full);
    /* Every object is a child of the section container and is deleted with
     * it by the shell; the private blocks are this app's to release. */
    free(v);
    app->nodes = NULL;
}

/* The list's footer: shown only for what the rows cannot say themselves -
 * that there are none, that they are cached, that a node was just forgotten,
 * or that the service's table had no room for some. */
static void paint_note(struct rift_nodes *v, int count)
{
    const struct rift_model *m = &v->app->model;
    const struct rift_action_state *op = &m->node_op;
    int64_t now = rift_app_now(v->app);
    char text[RIFT_ACTION_TEXT_MAX];
    int show = 1;

    unsigned turned_away = rift_model_unretained_recent(m);
    int forgot = op->kind == RIFT_ACTION_FORGET && op->done;

    if (count > 0 && m->stale) {
        lv_label_set_text_fmt(v->note, "%d node%s, cached: meshcored is not answering.", count,
                              count == 1 ? "" : "s");
    } else if (op->kind == RIFT_ACTION_FORGET && (op->done || op->failed) && op->have_mono &&
               now - op->mono_ms < RIFT_ACTION_NOTE_MS) {
        /* A forgotten node has left the list, and with it the detail that
         * would have said so; the list says it instead, for a while - ahead
         * of saying the list is empty, when it was the last one. */
        rift_fmt_action(op, now, text, sizeof(text));
        lv_label_set_text(v->note, text);
    } else if (count == 0) {
        lv_label_set_text(v->note, !m->snapshot_valid ? "Waiting for meshcored."
                                   : forgot ? "The service holds no node."
                                            : "No node has adverted since this service "
                                              "started.");
    } else if (turned_away > 0) {
        /* MeshCore's contact table holds 32 and keeps no more; a node it had
         * no room for is one nobody can be asked about or written to, and a
         * quiet list would not say so (docs/api/mesh.md). Forgetting a node
         * on its DETAIL is what makes room. Counted from the last node this
         * app saw forgotten: the service's counter only grows, and once room
         * has been made the table is not full until adverts are turned away
         * again. */
        lv_label_set_text_fmt(v->note, "%u advert%s the service had no room to keep" RIFT_SEP
                                       "forget a node to make room",
                              turned_away, turned_away == 1 ? "" : "s");
    } else {
        show = 0;
    }
    if (show) {
        lv_obj_remove_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    }
}

void rift_nodes_refresh(struct rift_app *app)
{
    struct rift_nodes *v = app ? app->nodes : NULL;
    const struct rift_node *order[RIFT_MAX_NODES];
    const struct rift_node *sel;
    int32_t keep_y = -1;
    int64_t now;
    int count;
    int fresh;
    int changed;
    int i;

    if (!v) {
        return;
    }
    now = rift_app_now(app);
    count = rift_model_order(&app->model, now, order, RIFT_MAX_NODES);
    fresh = rift_model_fresh_count(&app->model, now);
    if (fresh > count) {
        fresh = count;
    }
    sel = rift_app_selected(app);

    /* A detail with nothing to show is closed rather than left saying so: the
     * node was forgotten, or the service no longer holds it. */
    if (app->detail_open && !sel) {
        app->detail_open = 0;
    }
    if (strcmp(sel ? sel->key : "", v->shape_sel) != 0) {
        v->reveal = 1;
    }
    changed = !v->shape_valid || count != v->shape_count || fresh != v->shape_fresh ||
              app->wide != v->shape_wide ||
              strcmp(sel ? sel->key : "", v->shape_sel) != 0;
    for (i = 0; !changed && i < count; i++) {
        if (strcmp(order[i]->key, v->shape_key[i]) != 0) {
            changed = 1;
        }
    }
    if (changed) {
        /* Rebuilt in place, and read from where the reader was. lv_obj_clean
         * scrolls the list back to its top, and a rebuild is what every new
         * node, every re-ordering by last heard and every selection is - so
         * on a live mesh a list somebody had scrolled down jumped back to
         * its first row every few seconds. */
        keep_y = lv_obj_get_scroll_y(v->list);
        rebuild(v, order, count, fresh);
        v->shape_valid = 1;
        v->shape_count = count;
        v->shape_fresh = fresh;
        v->shape_wide = app->wide;
        snprintf(v->shape_sel, sizeof(v->shape_sel), "%s", sel ? sel->key : "");
        for (i = 0; i < count; i++) {
            snprintf(v->shape_key[i], sizeof(v->shape_key[i]), "%s", order[i]->key);
        }
    }
    v->order_count = count;
    for (i = 0; i < count; i++) {
        snprintf(v->order_key[i], sizeof(v->order_key[i]), "%s", order[i]->key);
    }
    /* Everything that sizes a column first, then one layout, then the text
     * that has to be fitted into what is left.
     *
     * rift_cell_set_text_fit measures the width the cell actually has. The
     * name shares its column with the role tag, and the tag is content-sized,
     * so a name measured before its tag had taken its width is measured
     * against room that does not exist - and the name comes out unshortened
     * and is then clipped instead of ellipsised. Whether that happened used
     * to depend on how many refreshes had run since the row was built, which
     * is to say on timing: the same fixtures gave two different screens.
     * Sizing every tag, laying out once and only then fitting makes one
     * refresh enough. */
    for (i = 0; i < v->row_count && i < count; i++) {
        const char *tag = rift_type_tag(order[i]->type, order[i]->have_type);

        lv_label_set_text(v->row[i].tag, tag ? tag : "");
    }
    /* The footer, only when it has something the list does not already say.
     * The counts are in the group labels (and in landscape the strip), so
     * an ordinary list gives the footer's line back to its rows. */
    paint_note(v, count);
    lv_obj_update_layout(v->pane_list);
    for (i = 0; i < v->row_count && i < count; i++) {
        update_row(v, &v->row[i], order[i], now);
    }
    if (keep_y > 0) {
        /* Settled first: the rows' text is what decides how tall the list
         * is, and the scroll is bounded by that. */
        lv_obj_update_layout(v->pane_list);
        lv_obj_scroll_to_y(v->list, keep_y, LV_ANIM_OFF);
    }
    if (v->reveal) {
        v->reveal = 0;
        for (i = 0; i < v->row_count; i++) {
            if (sel && strcmp(v->row[i].key, sel->key) == 0) {
                /* The whole slot, expansion and action bar included, so a
                 * selection made at the bottom of the pane shows what it
                 * offers rather than hiding it under the edge. */
                lv_obj_scroll_to_view(v->row[i].slot, LV_ANIM_OFF);
                break;
            }
        }
    }

    ensure_detail_full(v, app->detail_open && !app->wide);
    if (app->wide) {
        rift_detail_refresh(v->detail_pane, sel);
    } else if (app->detail_open && v->detail_full) {
        rift_detail_refresh(v->detail_full, sel);
    }
}

int rift_nodes_key(struct rift_app *app, uint32_t key)
{
    struct rift_nodes *v = app ? app->nodes : NULL;
    int at = -1;
    int i;

    if (!v || v->order_count == 0) {
        return 0;
    }
    for (i = 0; i < v->order_count; i++) {
        if (app->have_selected && strcmp(v->order_key[i], app->selected) == 0) {
            at = i;
            break;
        }
    }
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_LEFT:
        at = at <= 0 ? 0 : at - 1;
        rift_app_select(app, v->order_key[at]);
        return 1;
    case LV_KEY_DOWN:
    case LV_KEY_RIGHT:
        at = (at < 0 || at + 1 >= v->order_count) ? (at < 0 ? 0 : at) : at + 1;
        rift_app_select(app, v->order_key[at]);
        return 1;
    case LV_KEY_ENTER:
        /* The node as the cache holds it now: a selection whose node was
         * forgotten opens nothing. */
        if (!rift_app_selected(app)) {
            return 0;
        }
        /* Portrait pushes the detail. Landscape already shows it beside the
         * list, so Enter does what the keyboard is there for: it opens the
         * conversation with the selected node. It sends nothing. */
        if (app->wide) {
            rift_app_open_conversation(app, app->selected);
        } else {
            rift_app_open_detail(app, 1);
        }
        return 1;
    case LV_KEY_ESC:
        if (app->detail_open) {
            rift_app_open_detail(app, 0);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}
