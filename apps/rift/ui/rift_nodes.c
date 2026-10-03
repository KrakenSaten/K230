/*
 * NODES. See rift_nodes.h.
 *
 * The list is virtual. RIFT holds as many nodes as meshcored does (256), and
 * a row is a dozen LVGL objects, so a row per node would be three thousand
 * objects rebuilt every time the mesh re-ordered them - which is every
 * advert. Instead the list is a spacer as tall as all the rows would be, and
 * a small pool of rows is placed over whatever part of it is on screen:
 * scrolling rebinds rows from the pool, and a re-ordering only moves them.
 * What it costs is set by the height of the pane, not by the size of the
 * mesh (rift_nodes_rows_built).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_nodes.h"

#include "pos_styles.h"
#include "rift_detail.h"
#include "rift_find.h"
#include "rift_node_row.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rows in the pool: enough to cover the tallest pane with a margin on each
 * side, and never more, however many nodes there are. */
#define POOL_MAX 48
/* How far past each edge of the pane rows are bound, so a short scroll
 * shows rows that are already filled in. */
#define BIND_MARGIN (2 * RIFT_ROW_H)

enum item_kind {
    ITEM_GROUP = 0,
    ITEM_NODE,
};

/* One line of the list as it would be laid out if every row existed. */
struct item {
    enum item_kind kind;
    int ref;   /* the group (0..2), or the node's place in the order */
    int32_t y;
    int32_t h;
};

struct rift_nodes {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *pane_list;
    lv_obj_t *head;
    lv_obj_t *head_cell[HEAD_CELLS];
    lv_obj_t *list;
    lv_obj_t *spacer;
    lv_obj_t *group[3];
    lv_obj_t *note;
    lv_obj_t *pane_ctx;
    struct rift_detail *detail_pane; /* landscape right pane */
    struct rift_detail *detail_full; /* portrait pushed screen */
    lv_obj_t *detail_full_root;

    struct rift_node_row row[POOL_MAX];
    int row_count; /* built */

    /* The list as it stands: the order of the last refresh, so a key press
     * can step through it and a scroll can bind rows from it, and the lines
     * it lays out into. */
    char order_key[RIFT_MAX_NODES][RIFT_KEY_HEX];
    char order_heard[RIFT_MAX_NODES]; /* heard at all, for the groups */
    int order_count;
    struct item item[RIFT_MAX_NODES + 3];
    int item_count;
    int32_t total_h;
    int sel_item;
    /* The selected row's height as last measured: a row, or in portrait a
     * row with its expansion under it. */
    int32_t sel_h;
    char shape_sel[RIFT_KEY_HEX];
    int shape_wide;

    /* The selection moved: bring its row into view on this refresh. A list
     * of thirty nodes is taller than a landscape body, and arrows that moved
     * the selection off the bottom of the pane were moving it out of sight. */
    int reveal;
    int binding; /* inside bind_window: a scroll it causes is its own */
};

/* ---- building ----------------------------------------------------------- */

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

    v->head = rift_node_row_line(v->pane_list, rift_header_row_h());
    spacer(v->head, RIFT_IDENT_W); /* under the rows' identity mark */
    spacer(v->head, COL_GLYPH);
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->head_cell[i], 1);
    lv_label_set_text(v->head_cell[i], "NODE");
    i++;
    v->head_cell[i] = rift_cell(v->head, POS_STYLE_CAPTION, rift_node_strip_width(a), LV_TEXT_ALIGN_LEFT);
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
    i++;
    /* The rows widen the same columns by the same words (rift_node_row.c). */
    rift_node_cols_widen(v->head_cell[2], v->head_cell[3], v->head_cell[4], v->head_cell[5]);
    /* Under nothing but the age it follows: the pulse is that age, bucketed. */
    v->head_cell[i] = spacer(v->head, COL_PULSE);
    lv_obj_set_style_margin_left(v->head_cell[i], PULSE_PULL, 0);
    rift_rule(v->pane_list);
}

/* ---- the virtual list ------------------------------------------------------ */

static void unbind(struct rift_node_row *r)
{
    r->key[0] = '\0';
    r->item = -1;
    rift_node_row_drop_expansion(r);
    lv_obj_add_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
}

/* Lay the lines out as if every row existed: the group labels, the rows, and
 * the selected row at the height it was last measured. */
static void lay_out(struct rift_nodes *v, int fresh)
{
    const char *sel = v->app->have_selected ? v->app->selected : NULL;
    int32_t y = 0;
    int stale_shown = 0;
    int unheard_shown = 0;
    int i;

    v->item_count = 0;
    v->sel_item = -1;
    for (i = 0; i < v->order_count; i++) {
        int heard = v->order_heard[i];
        int group = -1;
        struct item *it;

        /* The three groups of rift_model_order, each headed where it
         * starts: heard within 12 h, heard longer ago, never heard. A row
         * starts at most one of them. */
        if (i == 0 && fresh > 0) {
            group = 0;
        } else if (!stale_shown && i >= fresh && heard) {
            group = 1;
            stale_shown = 1;
        } else if (!unheard_shown && !heard) {
            group = 2;
            unheard_shown = 1;
        }
        if (group >= 0) {
            it = &v->item[v->item_count++];
            it->kind = ITEM_GROUP;
            it->ref = group;
            it->y = y;
            it->h = rift_group_h();
            y += it->h;
        }
        it = &v->item[v->item_count++];
        it->kind = ITEM_NODE;
        it->ref = i;
        it->y = y;
        it->h = RIFT_ROW_H;
        if (sel && strcmp(sel, v->order_key[i]) == 0) {
            v->sel_item = v->item_count - 1;
            it->h = (v->sel_h > 0 ? v->sel_h : RIFT_ROW_H + 2 * SELECTED_INSET_V) +
                    2 * SELECTED_AIR;
        }
        y += it->h;
    }
    v->total_h = y;
    /* The spacer is what gives the list its height to scroll: one pixel at
     * the bottom of where every row would be. */
    lv_obj_set_pos(v->spacer, 0, v->total_h > 0 ? v->total_h - 1 : 0);
}

static void place(struct rift_nodes *v, struct rift_node_row *r)
{
    const struct item *it = &v->item[r->item];
    int32_t y = it->y + (r->item == v->sel_item ? SELECTED_AIR : 0);

    if (lv_obj_get_y(r->slot) != y || lv_obj_get_x(r->slot) != 0) {
        lv_obj_set_pos(r->slot, 0, y);
    }
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
}

/* Bind the pool to the part of the list that is on screen, plus a margin,
 * plus the selected row wherever it is: it carries the expansion, and its
 * height is measured. Rows already showing a node that is still wanted keep
 * it; the rest are rebound. every says whether rows that kept their node are
 * refreshed too (a repaint) or only the newly bound ones (a scroll). */
static void bind_window(struct rift_nodes *v, int every)
{
    struct rift_app *a = v->app;
    int64_t now = rift_app_now(a);
    int32_t top = lv_obj_get_scroll_y(v->list) - BIND_MARGIN;
    int32_t bottom = lv_obj_get_scroll_y(v->list) + lv_obj_get_height(v->list) + BIND_MARGIN;
    char want[RIFT_MAX_NODES + 3];
    int fresh_rows = 0;
    int i;
    int j;

    v->binding = 1;
    memset(want, 0, sizeof(want));
    for (i = 0; i < v->item_count; i++) {
        const struct item *it = &v->item[i];

        if (it->kind == ITEM_NODE &&
            ((it->y + it->h > top && it->y < bottom) || i == v->sel_item)) {
            want[i] = 1;
        }
    }
    /* Keep what is still wanted, where it is; free the rest. */
    for (j = 0; j < v->row_count; j++) {
        struct rift_node_row *r = &v->row[j];
        int kept = -1;

        if (!r->key[0]) {
            continue;
        }
        for (i = 0; i < v->item_count; i++) {
            if (want[i] == 1 && strcmp(v->order_key[v->item[i].ref], r->key) == 0) {
                kept = i;
                break;
            }
        }
        if (kept < 0) {
            unbind(r);
            continue;
        }
        want[kept] = 2; /* taken */
        r->item = kept;
    }
    /* Bind what is wanted and not yet showing. */
    for (i = 0; i < v->item_count; i++) {
        struct rift_node_row *r = NULL;

        if (want[i] != 1) {
            continue;
        }
        for (j = 0; j < v->row_count; j++) {
            if (!v->row[j].key[0]) {
                r = &v->row[j];
                break;
            }
        }
        if (!r && v->row_count < POOL_MAX) {
            r = &v->row[v->row_count++];
            rift_node_row_build(r, a, v->list);
        }
        if (!r) {
            break; /* the pool is spent; the rest is off screen */
        }
        snprintf(r->key, sizeof(r->key), "%s", v->order_key[v->item[i].ref]);
        r->item = i;
        want[i] = 3; /* newly bound */
    }
    /* Placed and styled first, then one layout if anything new needs it,
     * then filled in: a name is fitted to the width its row has, and a row
     * just taken from the pool has not been given one yet. */
    for (j = 0; j < v->row_count; j++) {
        struct rift_node_row *r = &v->row[j];

        if (!r->key[0]) {
            continue;
        }
        if (!rift_model_find(&a->model, r->key)) {
            /* Gone from the cache since the order was taken. */
            unbind(r);
            continue;
        }
        rift_node_row_select(r, r->item == v->sel_item);
        place(v, r);
        if (want[r->item] == 3 && lv_obj_get_width(r->namebox) <= 1) {
            fresh_rows = 1;
        }
    }
    if (fresh_rows) {
        lv_obj_update_layout(v->list);
    }
    for (j = 0; j < v->row_count; j++) {
        struct rift_node_row *r = &v->row[j];
        const struct rift_node *n = r->key[0] ? rift_model_find(&a->model, r->key) : NULL;

        if (n && (every || want[r->item] == 3)) {
            rift_node_row_update(r, n, now);
        }
    }
    /* The three group labels are always there to be placed, and there are
     * only three: no pool needed. */
    for (i = 0; i < 3; i++) {
        lv_obj_add_flag(v->group[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (i = 0; i < v->item_count; i++) {
        if (v->item[i].kind == ITEM_GROUP) {
            lv_obj_set_pos(v->group[v->item[i].ref], 0, v->item[i].y);
            lv_obj_remove_flag(v->group[v->item[i].ref], LV_OBJ_FLAG_HIDDEN);
        }
    }
    v->binding = 0;
}

static void on_list_scroll(lv_event_t *e)
{
    struct rift_nodes *v = lv_event_get_user_data(e);

    if (v->binding) {
        return;
    }
    /* Rows for what just came into view, filled in now so a scroll never
     * shows an empty one; the next repaint settles anything fitted to a
     * width a layout had not yet finished giving. */
    bind_window(v, 0);
    v->app->refresh_pending = 1;
}

/* A list that got shorter under a reader who had scrolled down it: the
 * spacer moved up, and LVGL leaves the offset where it was, looking at
 * nothing. Bring it back to the last screenful there is. */
static void clamp_scroll(struct rift_nodes *v)
{
    int32_t most = v->total_h - lv_obj_get_height(v->list);

    if (most < 0) {
        most = 0;
    }
    if (lv_obj_get_scroll_y(v->list) > most) {
        lv_obj_scroll_to_y(v->list, most, LV_ANIM_OFF);
    }
}

/* Bring the selected row, expansion and all, into view. */
static void reveal_selected(struct rift_nodes *v)
{
    const struct item *it;
    int32_t y = lv_obj_get_scroll_y(v->list);
    int32_t h = lv_obj_get_height(v->list);

    if (v->sel_item < 0) {
        return;
    }
    it = &v->item[v->sel_item];
    if (it->y < y) {
        lv_obj_scroll_to_y(v->list, it->y, LV_ANIM_OFF);
    } else if (it->y + it->h > y + h) {
        lv_obj_scroll_to_y(v->list, it->y + it->h - h, LV_ANIM_OFF);
    }
}

/* ---- layout ------------------------------------------------------------- */

void rift_nodes_shape(struct rift_app *app)
{
    struct rift_nodes *v = app ? app->nodes : NULL;
    int i;

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
    rift_find_shape(app);
    if (v->head_cell[1]) {
        lv_obj_set_width(v->head_cell[1], rift_node_strip_width(app));
    }
    lv_obj_add_flag(v->head_cell[4], LV_OBJ_FLAG_HIDDEN);
    if (app->wide) {
        lv_obj_remove_flag(v->head_cell[4], LV_OBJ_FLAG_HIDDEN);
    }
    /* A new shape is a new set of widths and, in portrait, an expansion:
     * every row is rebound from nothing and the selected one re-measured. */
    for (i = 0; i < v->row_count; i++) {
        unbind(&v->row[i]);
    }
    v->sel_h = 0;
    v->shape_wide = app->wide;
    v->reveal = 1;
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
    int i;

    if (!v) {
        return NULL;
    }
    app->nodes = v;
    v->app = app;
    v->sel_item = -1;

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
    /* Finding a node first, over the list it narrows (ui/rift_find.c). */
    rift_find_create(app, v->pane_list);
    build_head(v);

    /* The list lays nothing out itself: every child is placed by
     * bind_window, over a spacer that is as tall as all the rows. */
    v->list = lv_obj_create(v->pane_list);
    lv_obj_remove_style_all(v->list);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(v->list, on_list_scroll, LV_EVENT_SCROLL, v);
    v->spacer = spacer(v->list, 1);
    for (i = 0; i < 3; i++) {
        v->group[i] = rift_group_label(v->list, "");
        lv_obj_add_flag(v->group[i], LV_OBJ_FLAG_HIDDEN);
    }

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
    /* The list goes with the section container, and a scroll event on the
     * way down would reach this block after it is freed. */
    if (v->list) {
        lv_obj_remove_event_cb_with_user_data(v->list, on_list_scroll, v);
    }
    rift_detail_destroy(v->detail_pane);
    rift_detail_destroy(v->detail_full);
    /* Every object is a child of the section container and is deleted with
     * it by the shell; the private blocks are this app's to release. */
    free(app->find);
    app->find = NULL;
    free(v);
    app->nodes = NULL;
}

int rift_nodes_rows_built(const struct rift_app *app)
{
    return (app && app->nodes) ? app->nodes->row_count : 0;
}

/* The list's footer: shown only for what the rows cannot say themselves -
 * that there are none, that they are cached, that a node was just forgotten,
 * or that the service's table had no room for some. */
static void paint_note(struct rift_nodes *v, int count, int held)
{
    const struct rift_model *m = &v->app->model;
    const struct rift_action_state *op = &m->node_op;
    int64_t now = rift_app_now(v->app);
    char text[RIFT_ACTION_TEXT_MAX];
    int show = 1;
    int searching = v->app->node_query[0] != '\0';
    int zero_hop = v->app->node_zero_hop;

    unsigned turned_away = rift_model_unretained_recent(m);
    int forgot = op->kind == RIFT_ACTION_FORGET && op->done;

    if ((searching || zero_hop) && count == 0 && held > 0) {
        /* Nothing answers: said, rather than an empty list that reads as an
         * empty mesh. What zero-hop is built on is said with it, because an
         * empty view there is a quiet neighbourhood, not a fault. */
        if (zero_hop && !searching) {
            lv_label_set_text(v->note, "No repeater heard zero-hop since the radio service "
                                       "started. One appears here when its advert reaches "
                                       "this device with no relay in between; nothing is "
                                       "sent to ask.");
        } else if (zero_hop) {
            lv_label_set_text_fmt(v->note, "No zero-hop repeater matches \"%s\".",
                                  v->app->node_query);
        } else {
            lv_label_set_text_fmt(v->note, "No node matches \"%s\" (name, or hash from 2 hex).",
                                  v->app->node_query);
        }
    } else if (zero_hop && count > 0) {
        lv_label_set_text_fmt(v->note,
                              "%d of %d" RIFT_SEP "repeaters whose last advert reached this "
                              "device with no relay in between",
                              count, held);
    } else if (searching && count > 0) {
        lv_label_set_text_fmt(v->note, "%d of %d match", count, held);
    } else if (count > 0 && m->stale) {
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
        /* MeshCore's contact table is fixed (1000) and keeps no more; a node it had
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
    int64_t now;
    int count;
    int held;
    int fresh;
    int stale_count = 0;
    int unheard_count = 0;
    int i;

    if (!v) {
        return;
    }
    now = rift_app_now(app);
    held = rift_model_order(&app->model, now, order, RIFT_MAX_NODES);
    /* The find bar narrows the order and changes nothing in it: the same
     * groups, the same order, fewer rows. */
    count = rift_node_filter(order, held, app->node_query, app->node_zero_hop);
    /* The order puts the fresh first, so they are the head of what is left. */
    for (fresh = 0; fresh < count && order[fresh]->have_heard &&
                    !rift_node_is_stale(order[fresh], now);
         fresh++) {
    }
    rift_find_refresh(app);
    sel = rift_app_selected(app);

    /* A detail with nothing to show is closed rather than left saying so: the
     * node was forgotten, or the service no longer holds it. */
    if (app->detail_open && !sel) {
        app->detail_open = 0;
    }
    if (strcmp(sel ? sel->key : "", v->shape_sel) != 0) {
        v->reveal = 1;
        v->sel_h = 0; /* a different row, measured afresh */
        snprintf(v->shape_sel, sizeof(v->shape_sel), "%s", sel ? sel->key : "");
    }
    v->order_count = count;
    for (i = 0; i < count; i++) {
        snprintf(v->order_key[i], sizeof(v->order_key[i]), "%s", order[i]->key);
        v->order_heard[i] = order[i]->have_heard ? 1 : 0;
        if (i >= fresh) {
            if (order[i]->have_heard) {
                stale_count++;
            } else {
                unheard_count++;
            }
        }
    }
    /* The group labels carry the counts (and in landscape the strip does). */
    {
        /* In the zero-hop view every group says so: the toggle's colour is
         * never the only thing that does. */
        const char *pre = app->node_zero_hop ? "ZERO-HOP RPT" RIFT_SEP : "";
        char text[64];

        snprintf(text, sizeof(text), "%sHEARD < 12 H" RIFT_SEP "%d", pre, fresh);
        lv_label_set_text(v->group[0], text);
        snprintf(text, sizeof(text), "%sNOT HEARD > 12 H" RIFT_SEP "%d", pre, stale_count);
        lv_label_set_text(v->group[1], text);
        snprintf(text, sizeof(text), "%sNEVER HEARD" RIFT_SEP "%d", pre, unheard_count);
        lv_label_set_text(v->group[2], text);
    }
    /* The footer first, only when it has something the list does not already
     * say: whether it is there decides how tall the list is. */
    paint_note(v, count, held);
    lay_out(v, fresh);
    /* One layout, so the list's height and every row's name box are settled
     * before anything is bound or fitted against them. */
    lv_obj_update_layout(v->pane_list);
    /* Its own scrolls, not a reader's: bound below, once. */
    v->binding = 1;
    clamp_scroll(v);
    if (v->reveal) {
        reveal_selected(v);
    }
    v->binding = 0;
    bind_window(v, 1);
    /* The selected row is the one whose height is its content's: measure it,
     * and if it is not what the lines were laid out for, lay them out again
     * so nothing under it is hidden or left a gap. */
    for (i = 0; i < v->row_count; i++) {
        struct rift_node_row *r = &v->row[i];

        if (r->key[0] && r->item == v->sel_item && v->sel_item >= 0) {
            int32_t h;

            lv_obj_update_layout(r->slot);
            h = lv_obj_get_height(r->slot);
            if (h > 0 && h != v->sel_h) {
                v->sel_h = h;
                lay_out(v, fresh);
                lv_obj_update_layout(v->list);
                v->binding = 1;
                clamp_scroll(v);
                if (v->reveal) {
                    reveal_selected(v);
                }
                v->binding = 0;
                bind_window(v, 0);
            }
            break;
        }
    }
    v->reveal = 0;

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
            /* Nothing for a node that takes no direct messages: the detail
             * beside the list already says so. */
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
