/*
 * The route builder. See rift_route_view.h; the draft and its rules are
 * rift_route.h's, and this file only draws them and passes presses on.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_route_view.h"

#include "pos_styles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rows in the panel's list: the route (63 relays at most) or the repeaters a
 * reader may add. Built as they are first needed. */
#define ROWS_MAX 64
#define ROW_GAP 8
/* The panel beside the map in landscape: MAP's own info panel's width. Under
 * it in portrait: this share of MAP's height, the map the rest. */
#define PANEL_W_WIDE 400
#define PANEL_H_PCT 52

struct rift_route_view {
    struct rift_app *app;
    lv_obj_t *parent; /* MAP's root, where the panel is built when first needed */
    int wide;
    lv_obj_t *panel;
    lv_obj_t *body;  /* the panel's words and list, scrolled; the actions are under it */
    lv_obj_t *title;
    lv_obj_t *who;
    lv_obj_t *mode;
    lv_obj_t *chain;
    lv_obj_t *status;
    lv_obj_t *list;
    /* Each row: its number, the name (cut to the room with an ellipsis) and
     * a badge that is never cut - ON MAP, NO LOCATION. */
    lv_obj_t *row[ROWS_MAX];
    lv_obj_t *row_num[ROWS_MAX];
    lv_obj_t *row_name[ROWS_MAX];
    lv_obj_t *row_badge[ROWS_MAX];
    int rows_built;
    /* The picker's rows, by the key each adds. */
    char pick_key[ROWS_MAX][RIFT_KEY_HEX];
    int pick_count;
    lv_obj_t *list_note;
    lv_obj_t *bar_edit;
    lv_obj_t *undo;
    lv_obj_t *clear;
    lv_obj_t *picker;
    lv_obj_t *bar_cut;
    lv_obj_t *cut;
    lv_obj_t *keep;
    lv_obj_t *bar_go;
    lv_obj_t *cancel;
    lv_obj_t *use;
    lv_obj_t *legend;

    int picking;   /* the list is the repeaters to add, not the route */
    int cursor;    /* the picker row a key would add, or -1 */
    int warn;      /* the status line carries the warn colour */
    int marks;
    int lines;
};

static struct rift_route *route_of(const struct rift_app *a)
{
    return (struct rift_route *)&a->model.route;
}

int rift_route_view_active(const struct rift_app *app)
{
    return app && app->section == RIFT_SEC_MAP && app->model.route.draft.active;
}

lv_obj_t *rift_route_view_panel(const struct rift_route_view *v)
{
    return v ? v->panel : NULL;
}

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

/* ---- leaving -------------------------------------------------------------------- */

/* Back to the conversation the builder was opened from, its composer and
 * what is typed in it as they were. */
static void back_to_thread(struct rift_app *a, const char *to)
{
    char key[RIFT_KEY_HEX];

    snprintf(key, sizeof(key), "%s", to);
    rift_app_open_conversation(a, key);
    if (a->section != RIFT_SEC_COMMS) {
        rift_app_show_section(a, RIFT_SEC_COMMS);
    }
}

static void cancel_builder(struct rift_app *a)
{
    char to[RIFT_KEY_HEX];

    /* Nothing is staged and nothing is sent: a route staged before stays
     * as it was, and the contact's learned route was never touched. */
    snprintf(to, sizeof(to), "%s", a->model.route.draft.to);
    rift_route_close(route_of(a));
    back_to_thread(a, to);
}

static void use_route(struct rift_app *a)
{
    char to[RIFT_KEY_HEX];

    snprintf(to, sizeof(to), "%s", a->model.route.draft.to);
    if (rift_route_use(route_of(a), &a->model) == 0) {
        back_to_thread(a, to);
    }
}

/* ---- presses ---------------------------------------------------------------------- */

static void edited(struct rift_route_view *v)
{
    v->cursor = -1;
    rift_app_refresh(v->app);
}

static void on_undo(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    rift_route_undo(route_of(v->app));
    edited(v);
}

static void on_clear(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    rift_route_clear(route_of(v->app));
    edited(v);
}

static void on_picker(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    v->picking = !v->picking;
    lv_obj_scroll_to_y(v->body, 0, LV_ANIM_OFF); /* the other list, from its top */
    edited(v);
}

static void on_cut(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    /* The second press of two: the row picked was the first, and this
     * button says which waypoints go. */
    rift_route_cut(route_of(v->app), v->app->model.route.draft.marked);
    edited(v);
}

static void on_keep(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    rift_route_mark(route_of(v->app), -1);
    edited(v);
}

static void on_cancel(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    cancel_builder(v->app);
}

static void on_use(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);

    use_route(v->app);
}

static void on_row(lv_event_t *e)
{
    struct rift_route_view *v = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    struct rift_route *r = route_of(v->app);
    int i;

    for (i = 0; i < v->rows_built && v->row[i] != target; i++) {
    }
    if (i >= v->rows_built) {
        return;
    }
    if (v->picking) {
        if (i < v->pick_count) {
            rift_route_add(r, &v->app->model, v->pick_key[i]);
        }
    } else {
        /* A waypoint picked in the list: the first of the two presses that
         * cut the route there. Picked again, it is put back. */
        rift_route_mark(r, r->draft.marked == i ? -1 : i);
    }
    edited(v);
}

void rift_route_view_tap(struct rift_route_view *v, int index)
{
    struct rift_route *r;
    const struct rift_node *n;
    char label[RIFT_LABEL_MAX];
    int at;

    if (!v || index < 0 || index >= v->app->model.node_count) {
        return;
    }
    r = route_of(v->app);
    n = &v->app->model.nodes[index];
    at = rift_route_index(r, n->key);
    if (at >= 0) {
        /* A relay already in the route, tapped on the map: picked in the
         * list, as if its row had been - the same waypoint, the same number. */
        rift_route_mark(r, r->draft.marked == at ? -1 : at);
    } else if (strcmp(n->key, r->draft.to) == 0) {
        rift_fmt_label(n, label, sizeof(label));
        snprintf(r->draft.note, sizeof(r->draft.note),
                 "%s is the recipient: the route ends there.", label);
    } else {
        rift_route_add(r, &v->app->model, n->key);
    }
    edited(v);
}

int rift_route_view_key(struct rift_app *app, uint32_t key)
{
    struct rift_route_view *v;
    struct rift_route *r;

    v = rift_map_view_route(app);
    if (!rift_route_view_active(app) || !v) {
        return 0;
    }
    r = route_of(app);
    switch (key) {
    case LV_KEY_ESC:
        cancel_builder(app);
        return 1;
    case LV_KEY_BACKSPACE:
        rift_route_undo(r);
        break;
    case LV_KEY_DEL:
        if (r->draft.marked >= 0) {
            rift_route_cut(r, r->draft.marked);
        }
        break;
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
        v->picking = key == LV_KEY_RIGHT;
        v->cursor = -1;
        break;
    case LV_KEY_UP:
    case LV_KEY_DOWN: {
        int n = v->picking ? v->pick_count : r->draft.count;
        int at = v->picking ? v->cursor : r->draft.marked;

        if (n == 0) {
            return 1;
        }
        at = key == LV_KEY_DOWN ? (at + 1 < n ? at + 1 : n - 1) : (at > 0 ? at - 1 : 0);
        if (v->picking) {
            v->cursor = at;
        } else {
            rift_route_mark(r, at);
        }
        rift_app_refresh(app);
        if (at < v->rows_built) {
            lv_obj_scroll_to_view(v->row[at], LV_ANIM_OFF); /* the row the key is on, in view */
        }
        return 1;
    }
    case LV_KEY_ENTER:
        if (v->picking && v->cursor >= 0 && v->cursor < v->pick_count) {
            rift_route_add(r, &app->model, v->pick_key[v->cursor]);
            v->cursor = -1;
            break;
        }
        use_route(app);
        return 1;
    default:
        return 0;
    }
    edited(v);
    return 1;
}

/* ---- building ---------------------------------------------------------------------- */

struct rift_route_view *rift_route_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_route_view *v = calloc(1, sizeof(*v));

    if (!v) {
        return NULL;
    }
    v->app = app;
    v->parent = parent;
    v->cursor = -1;
    return v;
}

static void apply_shape(struct rift_route_view *v);

/* Built the first time a draft is open, not with MAP: MAP draws a thousand
 * nodes in one object (tests/rift_app_test.c counts them), and a reader who
 * never opens the builder pays nothing for it. */
static void build(struct rift_route_view *v)
{
    /* The panel is a fixed share of MAP (rift_route_view_shape): what it
     * says scrolls in its body, and the actions stay under it, always in
     * reach - USE ROUTE is never scrolled away. The panel itself lets its
     * caption overhang the top rule (rift_panel), so the clipping is the
     * body's. */
    v->panel = rift_panel(v->parent, "ROUTE BUILDER");
    v->body = lv_obj_create(v->panel);
    lv_obj_remove_style_all(v->body);
    lv_obj_set_width(v->body, LV_PCT(100));
    lv_obj_set_flex_grow(v->body, 1);
    lv_obj_set_flex_flow(v->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->body, 8, 0);
    lv_obj_set_scroll_dir(v->body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->body, LV_SCROLLBAR_MODE_AUTO);
    /* What matters first: who, how, the route in words, what the service
     * said, the route as a list; then the recipient's key and the caveat. */
    v->title = wrapping(v->body, POS_STYLE_ROW_TITLE);
    v->mode = wrapping(v->body, POS_STYLE_CAPTION);
    pos_style_add(v->mode, POS_STYLE_ACCENT_TEXT, 0);
    v->chain = wrapping(v->body, POS_STYLE_CAPTION);
    v->status = wrapping(v->body, POS_STYLE_CAPTION);

    v->list = lv_obj_create(v->body);
    lv_obj_remove_style_all(v->list);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_height(v->list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->list, ROW_GAP, 0);
    lv_obj_remove_flag(v->list, LV_OBJ_FLAG_SCROLLABLE);
    v->list_note = wrapping(v->body, POS_STYLE_CAPTION);
    v->who = wrapping(v->body, POS_STYLE_CAPTION);
    v->legend = wrapping(v->body, POS_STYLE_TEXT_MUTED);
    lv_label_set_text(v->legend,
                      "Lines show the route you are asking for, not radio links anyone has "
                      "seen, and not delivery: only an ACK says a message arrived. Choosing "
                      "and checking a route sends nothing.");

    v->bar_edit = bar(v->panel);
    v->undo = rift_action(v->bar_edit, "UNDO", 0, 1, on_undo, v);
    v->clear = rift_action(v->bar_edit, "CLEAR", 0, 1, on_clear, v);
    v->picker = rift_action(v->bar_edit, "RELAYS", 0, 1, on_picker, v);
    v->bar_cut = bar(v->panel);
    v->cut = rift_action(v->bar_cut, "REMOVE", 1, 1, on_cut, v);
    v->keep = rift_action(v->bar_cut, "KEEP", 0, 1, on_keep, v);
    v->bar_go = bar(v->panel);
    v->cancel = rift_action(v->bar_go, "CANCEL", 0, 1, on_cancel, v);
    v->use = rift_action(v->bar_go, "USE ROUTE", 1, 0, on_use, v);
    lv_obj_add_flag(v->panel, LV_OBJ_FLAG_HIDDEN);
    apply_shape(v);
}

void rift_route_view_destroy(struct rift_route_view *v)
{
    free(v); /* the objects are MAP's, deleted with it */
}

static void apply_shape(struct rift_route_view *v)
{
    if (!v->panel) {
        return;
    }
    /* Beside the map, its whole height; or under it, a fixed share of MAP,
     * so the map keeps the rest whatever the route's length. A fixed size
     * rather than its content's: MAP's root is sized, and a child sized by
     * its content inside a flex share is a size LVGL may never settle
     * (rift_comms.c, 2026-09-28). */
    if (v->wide) {
        lv_obj_set_size(v->panel, PANEL_W_WIDE, LV_PCT(100));
    } else {
        lv_obj_set_size(v->panel, LV_PCT(100), LV_PCT(PANEL_H_PCT));
    }
}

void rift_route_view_shape(struct rift_route_view *v, int wide)
{
    if (v) {
        v->wide = wide;
        apply_shape(v);
    }
}

/* ---- painting ----------------------------------------------------------------------- */

static void set_warn(struct rift_route_view *v, int warn)
{
    if (warn == v->warn) {
        return;
    }
    if (warn) {
        pos_style_add(v->status, POS_STYLE_STATUS_WARN_TEXT, 0);
    } else {
        lv_obj_remove_style(v->status, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    }
    v->warn = warn;
}

static void ensure_rows(struct rift_route_view *v, int n)
{
    while (v->rows_built < n && v->rows_built < ROWS_MAX) {
        int i = v->rows_built;
        lv_obj_t *b = lv_button_create(v->list);

        /* A 56 px row on a slab, its columns in their own roles' colours:
         * a button's fill would be a theme's action colour (Neon's is
         * magenta) under words that are not drawn for it. */
        lv_obj_remove_style_all(b);
        pos_style_add(b, POS_STYLE_SLAB, 0);
        pos_style_add(b, POS_STYLE_DIVIDER, 0);
        pos_style_add(b, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_size(b, LV_PCT(100), RIFT_TOUCH_H);
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, 10, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, v);
        v->row_num[i] = rift_cell(b, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(v->row_num[i], LV_SIZE_CONTENT);
        pos_style_add(v->row_num[i], POS_STYLE_ACCENT_TEXT, 0);
        v->row_name[i] = rift_cell(b, POS_STYLE_ROW_TITLE, 1, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_flex_grow(v->row_name[i], 1);
        v->row_badge[i] = rift_cell(b, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(v->row_badge[i], LV_SIZE_CONTENT);
        v->row[i] = b;
        v->rows_built++;
    }
}

static void paint_row(struct rift_route_view *v, int i, const char *num, const char *name,
                      const char *badge, int selected)
{
    rift_label_set(v->row_num[i], num);
    rift_label_set(v->row_badge[i], badge);
    rift_cell_set_text_fit(v->row_name[i], name);
    if (selected != lv_obj_has_state(v->row[i], LV_STATE_CHECKED)) {
        if (selected) {
            lv_obj_add_state(v->row[i], LV_STATE_CHECKED);
            pos_style_add(v->row[i], POS_STYLE_SELECTED, 0);
        } else {
            lv_obj_remove_state(v->row[i], LV_STATE_CHECKED);
            lv_obj_remove_style(v->row[i], pos_style(POS_STYLE_SELECTED), 0);
        }
    }
    show(v->row[i], 1);
}

/* One waypoint's row: "1", "RPT-NORD", "a1 · ON MAP" - the number on its
 * marker, the name, and the hash MeshCore will carry with whether the map
 * can show it. A picker row's number is "+". */
static void paint_waypoint(struct rift_route_view *v, int i, int number, const char *key,
                           int selected)
{
    const struct rift_node *n = rift_model_find(&v->app->model, key);
    char num[12];
    char name[RIFT_LABEL_MAX];
    char badge[48];

    if (number > 0) {
        snprintf(num, sizeof(num), "%d", number);
    } else {
        snprintf(num, sizeof(num), "+");
    }
    if (!n) {
        snprintf(name, sizeof(name), "%.8s?", key);
        snprintf(badge, sizeof(badge), "LEFT THE NODE LIST");
    } else {
        rift_fmt_label(n, name, sizeof(name));
        snprintf(badge, sizeof(badge), "%s" RIFT_SEP "%s", n->hash,
                 n->have_location ? "ON MAP" : "NO LOCATION");
    }
    paint_row(v, i, num, name, badge, selected);
}

static void paint_list(struct rift_route_view *v)
{
    const struct rift_model *m = &v->app->model;
    const struct rift_route *r = &m->route;
    char text[96];
    int shown = 0;
    int more = 0;
    int i;

    if (v->picking) {
        v->pick_count = 0;
        for (i = 0; i < m->node_count; i++) {
            if (rift_route_eligible(r, m, m->nodes[i].key, NULL, 0) != 0) {
                continue;
            }
            if (v->pick_count >= ROWS_MAX) {
                more++;
                continue;
            }
            snprintf(v->pick_key[v->pick_count], sizeof(v->pick_key[0]), "%s", m->nodes[i].key);
            v->pick_count++;
        }
        ensure_rows(v, v->pick_count);
        lv_obj_update_layout(v->panel); /* the names are fitted to settled widths */
        for (i = 0; i < v->pick_count && i < v->rows_built; i++) {
            paint_waypoint(v, i, 0, v->pick_key[i], i == v->cursor);
        }
        shown = i;
        if (v->pick_count == 0) {
            rift_label_set(v->list_note, "No other repeater is known that this route could take. "
                                         "Repeaters appear here as their adverts are heard.");
        } else if (more) {
            snprintf(text, sizeof(text), "%d more not listed: tap them on the map.", more);
            rift_label_set(v->list_note, text);
        } else {
            rift_label_set(v->list_note, "Tap a repeater to add it after the last relay. "
                                         "Those with no location can only be added here.");
        }
    } else {
        ensure_rows(v, r->draft.count);
        lv_obj_update_layout(v->panel);
        for (i = 0; i < r->draft.count && i < v->rows_built; i++) {
            paint_waypoint(v, i, i + 1, r->draft.via[i], i == r->draft.marked);
        }
        shown = i;
        rift_label_set(v->list_note,
                       r->draft.count ? ""
                                      : "No relay yet. Tap repeaters on the map in the order the "
                                        "message should take, or RELAYS for the list. With "
                                        "none, the message goes DIRECT to the recipient only.");
    }
    show(v->list_note, lv_label_get_text(v->list_note)[0] != '\0');
    for (i = shown; i < v->rows_built; i++) {
        show(v->row[i], 0);
    }
}

static void paint_status(struct rift_route_view *v)
{
    const struct rift_model *m = &v->app->model;
    const struct rift_route *r = &m->route;
    const struct rift_route_draft *d = &r->draft;
    char text[RIFT_ROUTE_WHY_MAX + 96];
    int missing = rift_route_missing(r, m);
    int warn = 1;

    if (!rift_ipc_connected(&v->app->ipc)) {
        snprintf(text, sizeof(text), "meshcored is not answering; the route is checked when it "
                                     "is back. Nothing is sent from here.");
    } else if (!r->caps_known) {
        snprintf(text, sizeof(text), "Asking meshcored whether it takes a chosen route\xE2\x80\xA6");
        warn = 0;
    } else if (!rift_route_supported(r)) {
        snprintf(text, sizeof(text), "This meshcored does not take a chosen route: only AUTO is "
                                     "possible. CANCEL to go back.");
    } else if (missing) {
        snprintf(text, sizeof(text), "%d waypoint%s left the node list: remove %s to go on.",
                 missing, missing == 1 ? "" : "s", missing == 1 ? "it" : "them");
    } else if (d->note[0]) {
        snprintf(text, sizeof(text), "%s", d->note);
    } else if (d->check == RIFT_ROUTE_VALID) {
        char hashes[40];

        if (d->enc.hash_bytes > 0) {
            snprintf(hashes, sizeof(hashes), RIFT_SEP "%d-BYTE HASHES", d->enc.hash_bytes);
        } else {
            hashes[0] = '\0';
        }
        snprintf(text, sizeof(text),
                 "CHECKED BY MESHCORED%s" RIFT_SEP "%d PATH BYTE%s" RIFT_SEP "NOTHING SENT", hashes,
                 d->enc.path_bytes, d->enc.path_bytes == 1 ? "" : "S");
        warn = 0;
    } else if (d->check == RIFT_ROUTE_REFUSED) {
        snprintf(text, sizeof(text), "REFUSED BY MESHCORED: %s", d->why);
    } else if (d->check == RIFT_ROUTE_UNANSWERED) {
        snprintf(text, sizeof(text), "%s", d->why);
    } else {
        snprintf(text, sizeof(text), "Checking the route with meshcored\xE2\x80\xA6");
        warn = 0;
    }
    rift_label_set(v->status, text);
    set_warn(v, warn);
}

unsigned rift_route_view_refresh(struct rift_route_view *v)
{
    const struct rift_model *m;
    const struct rift_route *r;
    const struct rift_route_draft *d;
    const struct rift_node *to;
    char text[RIFT_CHAIN_MAX + 64];
    char label[RIFT_LABEL_MAX];
    char count[16];
    int usable;

    if (!v) {
        return 0;
    }
    m = &v->app->model;
    r = &m->route;
    d = &r->draft;
    if (!rift_route_view_active(v->app)) {
        if (v->panel) {
            show(v->panel, 0);
        }
        v->picking = 0;
        v->cursor = -1;
        return 0;
    }
    /* Every edit is checked as it is made. A question to the service, never
     * a transmission (docs/api/mesh.md, mesh.path_preview). */
    if (rift_route_wants_check(r)) {
        rift_ipc_path_preview(&v->app->ipc);
    }
    if (!v->panel) {
        build(v);
    }
    if (lv_obj_has_flag(v->panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_scroll_to_y(v->body, 0, LV_ANIM_OFF); /* a new draft is read from its top */
        show(v->panel, 1);
    }
    to = rift_model_find(m, d->to);
    if (to) {
        rift_fmt_label(to, label, sizeof(label));
    } else {
        snprintf(label, sizeof(label), "%.8s?", d->to);
    }
    snprintf(text, sizeof(text), "TO %s", label);
    rift_label_set(v->title, text);
    snprintf(text, sizeof(text),
             "RECIPIENT, FIXED BY THE CONVERSATION" RIFT_SEP "%.12s\xE2\x80\xA6" RIFT_SEP "%s", d->to,
             !to ? "NOT IN THE NODE LIST" : to->have_location ? "ON MAP" : "NO LOCATION");
    rift_label_set(v->who, text);
    if (d->count == 0) {
        snprintf(text, sizeof(text), "EXPLICIT DIRECT" RIFT_SEP "NO RELAY: STRAIGHT TO %s, "
                                     "NEVER FLOODED", label);
    } else {
        snprintf(text, sizeof(text), "EXPLICIT DIRECT" RIFT_SEP "%s",
                 rift_route_relays_word(d->count, count, sizeof(count)));
    }
    rift_label_set(v->mode, text);
    rift_route_chain(m, d->to, (const char(*)[RIFT_ROUTE_KEY])d->via, d->count, text, sizeof(text));
    rift_label_set(v->chain, text);
    paint_status(v);
    paint_list(v);

    rift_label_set(lv_obj_get_child(v->picker, 0), v->picking ? "ROUTE" : "RELAYS");
    rift_action_set_enabled(v->undo, 0, d->count > 0);
    rift_action_set_enabled(v->clear, 0, d->count > 0);
    if (d->marked >= 0 && !v->picking) {
        if (d->marked == d->count - 1) {
            snprintf(text, sizeof(text), "REMOVE %d", d->marked + 1);
        } else {
            snprintf(text, sizeof(text), "REMOVE %d-%d", d->marked + 1, d->count);
        }
        rift_label_set(lv_obj_get_child(v->cut, 0), text);
    }
    show(v->bar_cut, d->marked >= 0 && !v->picking);
    show(v->bar_edit, !(d->marked >= 0 && !v->picking));
    usable = rift_route_usable(r, m);
    rift_label_set(lv_obj_get_child(v->use, 0), d->count ? "USE ROUTE" : "USE DIRECT");
    rift_action_set_enabled(v->use, 1, usable);
    return (d->gen << 8) ^ ((unsigned)d->check << 4) ^ (unsigned)(d->marked + 1) ^
           ((unsigned)usable << 7);
}

void rift_route_view_draw(struct rift_route_view *v, lv_layer_t *layer, const lv_area_t *ar,
                          const struct rift_map_view *view, lv_obj_t *canvas)
{
    if (v) {
        rift_route_draw(v->app, layer, ar, view, canvas, &v->marks, &v->lines);
    }
}

int rift_route_view_drawn_marks(const struct rift_route_view *v)
{
    return v ? v->marks : 0;
}

int rift_route_view_drawn_lines(const struct rift_route_view *v)
{
    return v ? v->lines : 0;
}
