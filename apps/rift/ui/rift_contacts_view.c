/*
 * CONTACTS. See rift_contacts_view.h.
 *
 * The list is virtual, as NODES' is (ui/rift_nodes.c): a spacer as tall as
 * every row would be and a small pool of rows placed over what is on
 * screen, so a thousand contacts cost the objects a screenful does.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_contacts_view.h"

#include "app.h"
#include "pocketui.h"
#include "pos_styles.h"
#include "rift_contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL 32
#define COL_GAP 8
#define COL_TYPE 96
#define COL_KEY 96
#define COL_HEARD 48
#define FILTER_W 120
#define FIELD_H_WIDE 36

struct contact_row {
    lv_obj_t *slot;
    lv_obj_t *name;
    lv_obj_t *type;
    lv_obj_t *key;
    lv_obj_t *heard;
    int item;     /* where in the list it is bound, -1 for none */
    int selected;
    struct rift_contacts_view *v;
};

struct rift_contacts_view {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *head;
    lv_obj_t *title;
    lv_obj_t *count;
    lv_obj_t *bar;
    lv_obj_t *field;
    lv_obj_t *all;
    lv_obj_t *recent;
    lv_obj_t *list;
    lv_obj_t *spacer;
    lv_obj_t *note;
    struct contact_row row[POOL];
    int rows;
    /* The list as last ordered: pointers into the model's node table, which
     * does not move (rift_model.h), and redone on every refresh. */
    const struct rift_node *item[RIFT_MAX_NODES];
    int n;
    int sel;       /* the selected item, -1 for none */
    int filter_drawn;
};

static struct rift_contacts_view *view_of(const struct rift_app *app)
{
    return app ? app->contacts : NULL;
}

/* ---- rows ----------------------------------------------------------------- */

static void open_item(struct rift_contacts_view *v, int i)
{
    struct rift_app *a = v->app;
    const struct rift_node *n;

    if (i < 0 || i >= v->n) {
        return;
    }
    n = v->item[i];
    v->sel = i;
    snprintf(a->contacts_sel, sizeof(a->contacts_sel), "%s", n->key);
    /* A repeater or a sensor takes no direct message (rift_model.h,
     * rift_node_can_message): selecting it says so, and opens nothing. */
    if (!rift_node_can_message(n)) {
        a->refresh_pending = 1;
        return;
    }
    rift_app_open_conversation(a, n->key);
}

static void on_row(lv_event_t *e)
{
    struct contact_row *r = lv_event_get_user_data(e);

    open_item(r->v, r->item);
}

static lv_obj_t *cell(lv_obj_t *parent, enum pos_style_role role, int32_t w, lv_text_align_t al)
{
    lv_obj_t *c = rift_cell(parent, role, w, al);

    lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    return c;
}

static void build_row(struct rift_contacts_view *v)
{
    struct contact_row *r = &v->row[v->rows];

    memset(r, 0, sizeof(*r));
    r->v = v;
    r->item = -1;
    r->slot = lv_obj_create(v->list);
    lv_obj_remove_style_all(r->slot);
    lv_obj_add_flag(r->slot, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(r->slot, LV_PCT(100), RIFT_ROW_H);
    lv_obj_set_flex_flow(r->slot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r->slot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r->slot, COL_GAP, 0);
    lv_obj_set_style_pad_hor(r->slot, 4, 0);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_SCROLLABLE);
    pos_style_add(r->slot, POS_STYLE_DIVIDER, 0);
    pos_style_add(r->slot, POS_STYLE_SLAB_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r->slot, on_row, LV_EVENT_CLICKED, r);
    r->name = cell(r->slot, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->name, 1);
    lv_obj_set_width(r->name, 1);
    r->type = cell(r->slot, POS_STYLE_CAPTION, COL_TYPE, LV_TEXT_ALIGN_LEFT);
    r->key = cell(r->slot, POS_STYLE_CAPTION, COL_KEY, LV_TEXT_ALIGN_LEFT);
    r->heard = cell(r->slot, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    lv_obj_add_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
    v->rows++;
}

static void bind_row(struct contact_row *r, int item, int64_t now)
{
    struct rift_contacts_view *v = r->v;
    const struct rift_node *n;
    char key[16];
    char heard[RIFT_AGE_MAX];
    const char *type;

    r->item = item;
    if (item < 0 || item >= v->n) {
        lv_obj_add_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
        r->item = -1;
        return;
    }
    n = v->item[item];
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(r->slot, (int32_t)item * RIFT_ROW_H);
    {
        char name[RIFT_NAME_MAX + 8];

        if (n->have_name && n->name[0]) {
            rift_text_shown(n->name, name, sizeof(name));
        } else {
            snprintf(name, sizeof(name), "%.2s", n->key);
        }
        /* Fitted to its column, with an ellipsis when it is longer. */
        rift_cell_set_text_fit(r->name, name);
    }
    /* Repeaters stay repeaters: the kind is said on every row, in the
     * advertised type's word, never guessed from a name. */
    type = rift_type_word(n->type, n->have_type);
    {
        char word[16];
        size_t i;

        snprintf(word, sizeof(word), "%s", type ? type : RIFT_UNKNOWN);
        for (i = 0; word[i]; i++) {
            word[i] = (word[i] >= 'a' && word[i] <= 'z') ? (char)(word[i] - 'a' + 'A') : word[i];
        }
        rift_label_set(r->type, word);
    }
    snprintf(key, sizeof(key), "%.8s", n->key);
    {
        size_t i;

        for (i = 0; key[i]; i++) {
            key[i] = (key[i] >= 'a' && key[i] <= 'f') ? (char)(key[i] - 'a' + 'A') : key[i];
        }
    }
    rift_label_set(r->key, key);
    rift_fmt_age(now - n->heard_mono_ms, n->have_heard, heard, sizeof(heard));
    rift_label_set(r->heard, heard);
    if ((item == v->sel) != r->selected) {
        r->selected = (item == v->sel);
        if (r->selected) {
            pos_style_add(r->slot, POS_STYLE_SELECTED, 0);
        } else {
            lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SELECTED), 0);
        }
    }
}

/* Bind the pool to what is on screen now. */
static void bind_visible(struct rift_contacts_view *v)
{
    int32_t top = lv_obj_get_scroll_y(v->list);
    int32_t h = lv_obj_get_height(v->list);
    int first = top > 0 ? (int)(top / RIFT_ROW_H) : 0;
    int want = (int)(h / RIFT_ROW_H) + 2;
    int64_t now = rift_app_now(v->app);
    int k;

    if (want > POOL) {
        want = POOL;
    }
    if (v->rows < want) {
        while (v->rows < want) {
            build_row(v);
        }
        /* New rows laid out before they are filled: a name is fitted to
         * the width its column settles at, not to nothing. */
        lv_obj_update_layout(v->list);
    }
    for (k = 0; k < v->rows; k++) {
        bind_row(&v->row[k], k < want ? first + k : -1, now);
    }
}

static void on_scroll(lv_event_t *e)
{
    bind_visible(lv_event_get_user_data(e));
}

/* ---- the bar: search and the two filters ------------------------------------ */

static void on_changed(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    const char *text = lv_textarea_get_text(lv_event_get_target_obj(e));
    char kept[4 * RIFT_QUERY_MAX];
    size_t i;
    size_t o = 0;

    /* What the field took in before any callback saw it - a tab, an Esc - is
     * not part of a name anybody is looking for. */
    for (i = 0; text && text[i] && o + 1 < sizeof(kept); i++) {
        unsigned char c = (unsigned char)text[i];

        if (c >= 0x20 && c != 0x7F) {
            kept[o++] = (char)c;
        }
    }
    kept[o] = '\0';
    rift_utf8_copy(a->contacts_query, sizeof(a->contacts_query), kept);
    a->refresh_pending = 1;
}

static void on_field_clicked(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (a->wide || lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, a);
    }
}

static void on_field_ready(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);

    if (pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_hide();
    }
    a->focus_list_pending = 1;
    a->refresh_pending = 1;
}

/* Esc clears the search; on an empty one it gives the list the keys. */
static void on_field_key(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_contacts_view *v = view_of(a);
    int typed;

    if (lv_event_get_key(e) != LV_KEY_ESC || !v) {
        return;
    }
    typed = a->contacts_query[0] != '\0';
    lv_textarea_set_text(v->field, "");
    a->contacts_query[0] = '\0';
    if (!typed) {
        a->focus_list_pending = 1;
    }
    a->refresh_pending = 1;
}

static void on_filter(lv_event_t *e)
{
    struct rift_app *a = lv_event_get_user_data(e);
    struct rift_contacts_view *v = view_of(a);

    if (!v) {
        return;
    }
    a->contacts_recent = (lv_event_get_target_obj(e) == v->recent);
    v->sel = -1;
    a->contacts_sel[0] = '\0';
    lv_obj_scroll_to_y(v->list, 0, LV_ANIM_OFF);
    a->refresh_pending = 1;
}

static void on_back(lv_event_t *e)
{
    rift_app_show_section(lv_event_get_user_data(e), RIFT_SEC_COMMS);
}

/* ---- the public entry points ------------------------------------------------- */

lv_obj_t *rift_contacts_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_contacts_view *v = calloc(1, sizeof(*v));
    lv_obj_t *back;

    if (!v) {
        return NULL;
    }
    app->contacts = v;
    v->app = app;
    v->sel = -1;
    v->filter_drawn = -1;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(v->root, RIFT_PAD, 0);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);

    v->head = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->head);
    lv_obj_set_width(v->head, LV_PCT(100));
    lv_obj_set_height(v->head, rift_header_row_h());
    lv_obj_set_flex_flow(v->head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(v->head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(v->head, COL_GAP, 0);
    lv_obj_remove_flag(v->head, LV_OBJ_FLAG_SCROLLABLE);
    back = rift_cell(v->head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    pos_style_add(back, POS_STYLE_ACCENT_TEXT, 0);
    lv_label_set_text(back, "\xE2\x80\xB9 COMMS");
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, app);
    v->title = rift_cell(v->head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(v->title, 1);
    lv_obj_set_width(v->title, 1);
    lv_label_set_text(v->title, "CONTACTS");
    v->count = rift_cell(v->head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_RIGHT);
    rift_rule(v->root);

    v->bar = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->bar);
    lv_obj_set_width(v->bar, LV_PCT(100));
    lv_obj_set_height(v->bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(v->bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(v->bar, 12, 0);
    lv_obj_set_style_pad_ver(v->bar, 6, 0);
    lv_obj_remove_flag(v->bar, LV_OBJ_FLAG_SCROLLABLE);
    v->field = pocketui_text_field(v->bar, "Search: name or key", true);
    if (v->field) {
        lv_obj_set_flex_grow(lv_obj_get_parent(v->field), 1);
        lv_textarea_set_max_length(v->field, RIFT_QUERY_MAX - 1);
        lv_obj_add_event_cb(v->field, on_changed, LV_EVENT_VALUE_CHANGED, app);
        lv_obj_add_event_cb(v->field, on_field_clicked, LV_EVENT_CLICKED, app);
        lv_obj_add_event_cb(v->field, on_field_ready, LV_EVENT_READY, app);
        lv_obj_add_event_cb(v->field, on_field_key, LV_EVENT_KEY, app);
    }
    v->all = rift_action(v->bar, "ALL", 1, 1, on_filter, app);
    lv_obj_set_flex_grow(v->all, 0);
    lv_obj_set_width(v->all, FILTER_W);
    v->recent = rift_action(v->bar, "RECENT", 0, 1, on_filter, app);
    lv_obj_set_flex_grow(v->recent, 0);
    lv_obj_set_width(v->recent, FILTER_W);

    v->list = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->list);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(v->list, on_scroll, LV_EVENT_SCROLL, v);
    v->spacer = lv_obj_create(v->list);
    lv_obj_remove_style_all(v->spacer);
    lv_obj_set_size(v->spacer, 1, 0);
    lv_obj_remove_flag(v->spacer, LV_OBJ_FLAG_CLICKABLE);

    v->note = lv_label_create(v->root);
    lv_obj_remove_style_all(v->note);
    pos_style_add(v->note, POS_STYLE_CAPTION, 0);
    lv_obj_set_width(v->note, LV_PCT(100));
    lv_label_set_long_mode(v->note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v->note, "");
    return v->root;
}

void rift_contacts_view_destroy(struct rift_app *app)
{
    struct rift_contacts_view *v = view_of(app);

    if (!v) {
        return;
    }
    /* The objects are the section container's and go with it. */
    free(v);
    app->contacts = NULL;
}

void rift_contacts_view_shape(struct rift_app *app)
{
    struct rift_contacts_view *v = view_of(app);
    int32_t h;

    if (!v) {
        return;
    }
    h = app->wide ? RIFT_ROW_H : RIFT_TOUCH_H;
    lv_obj_set_style_pad_ver(v->bar, app->wide ? 2 : 6, 0);
    lv_obj_set_height(v->all, h);
    lv_obj_set_height(v->recent, h);
    if (v->field) {
        lv_obj_set_height(v->field, app->wide ? FIELD_H_WIDE : 64);
        lv_obj_set_style_pad_ver(v->field, app->wide ? 4 : 16, 0);
    }
}

void rift_contacts_view_refresh(struct rift_app *app)
{
    struct rift_contacts_view *v = view_of(app);
    const struct rift_model *m;
    int i;

    if (!v) {
        return;
    }
    m = &app->model;
    v->n = rift_contacts_list(m, app->contacts_recent ? RIFT_CONTACTS_RECENT : RIFT_CONTACTS_ALL,
                              app->contacts_query, v->item, RIFT_MAX_NODES);
    /* The selection follows its contact, not its place: a node heard again
     * does not move it, and one filtered out loses it. */
    v->sel = -1;
    for (i = 0; app->contacts_sel[0] && i < v->n; i++) {
        if (strcmp(v->item[i]->key, app->contacts_sel) == 0) {
            v->sel = i;
            break;
        }
    }
    if (v->filter_drawn != app->contacts_recent) {
        v->filter_drawn = app->contacts_recent;
        /* The filter in force is the primary button; the other is not. */
        rift_action_set_enabled(v->all, 1, 0);
        rift_action_set_enabled(v->all, !app->contacts_recent, 1);
        rift_action_set_enabled(v->recent, 1, 0);
        rift_action_set_enabled(v->recent, app->contacts_recent, 1);
    }
    if (v->field && strcmp(lv_textarea_get_text(v->field), app->contacts_query) != 0 &&
        !lv_obj_has_state(v->field, LV_STATE_FOCUSED)) {
        lv_textarea_set_text(v->field, app->contacts_query);
    }
    /* How many the table holds, and how many of them are shown: the
     * service's own count where it has said, which is the stored table. */
    if (m->have_nodes_reported) {
        lv_label_set_text_fmt(v->count, "%d OF %d", v->n, m->nodes_reported);
    } else {
        lv_label_set_text_fmt(v->count, "%d", v->n);
    }
    lv_obj_set_height(v->spacer, (int32_t)v->n * RIFT_ROW_H);
    {
        const struct rift_node *s = v->sel >= 0 ? v->item[v->sel] : NULL;
        const char *why = s ? rift_node_no_message_why(s) : NULL;

        if (why) {
            lv_label_set_text(v->note, why);
        } else if (v->n == 0 && app->contacts_query[0]) {
            lv_label_set_text(v->note, "No contact answers that: a name, or two or more hex "
                                       "characters of a key.");
        } else if (v->n == 0) {
            lv_label_set_text(v->note, app->contacts_recent
                                           ? "No direct conversation held yet."
                                           : "No contacts yet: a node becomes one when its "
                                             "advert is heard.");
        } else if (rift_model_unretained_recent(m) > 0) {
            lv_label_set_text(v->note, "The contact table is full: new nodes are not being "
                                       "kept. Forget one in NODES to make room.");
        } else {
            lv_label_set_text(v->note, "");
        }
    }
    lv_obj_update_layout(v->root);
    bind_visible(v);
}

int rift_contacts_view_key(struct rift_app *app, uint32_t key)
{
    struct rift_contacts_view *v = view_of(app);
    int to;

    if (!v || v->n == 0) {
        return 0;
    }
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        to = v->sel < 0 ? 0 : v->sel + (key == LV_KEY_UP ? -1 : 1);
        if (to < 0) {
            to = 0;
        }
        if (to >= v->n) {
            to = v->n - 1;
        }
        v->sel = to;
        snprintf(app->contacts_sel, sizeof(app->contacts_sel), "%s", v->item[to]->key);
        {
            int32_t y = (int32_t)to * RIFT_ROW_H;
            int32_t top = lv_obj_get_scroll_y(v->list);
            int32_t h = lv_obj_get_height(v->list);

            if (y < top) {
                lv_obj_scroll_to_y(v->list, y, LV_ANIM_OFF);
            } else if (y + RIFT_ROW_H > top + h) {
                lv_obj_scroll_to_y(v->list, y + RIFT_ROW_H - h, LV_ANIM_OFF);
            }
        }
        app->refresh_pending = 1;
        return 1;
    case LV_KEY_ENTER:
        open_item(v, v->sel < 0 ? 0 : v->sel);
        return 1;
    default:
        return 0;
    }
}

int rift_contacts_view_count(const struct rift_app *app)
{
    const struct rift_contacts_view *v = view_of(app);

    return v ? v->n : 0;
}

const char *rift_contacts_view_key_at(const struct rift_app *app, int i)
{
    const struct rift_contacts_view *v = view_of(app);

    return (v && i >= 0 && i < v->n) ? v->item[i]->key : NULL;
}

int rift_contacts_view_rows_built(const struct rift_app *app)
{
    const struct rift_contacts_view *v = view_of(app);

    return v ? v->rows : 0;
}

lv_obj_t *rift_contacts_view_field(const struct rift_app *app)
{
    const struct rift_contacts_view *v = view_of(app);

    return v ? v->field : NULL;
}

lv_obj_t *rift_contacts_view_filter_button(const struct rift_app *app, int recent)
{
    const struct rift_contacts_view *v = view_of(app);

    return v ? (recent ? v->recent : v->all) : NULL;
}

const char *rift_contacts_view_note(const struct rift_app *app)
{
    const struct rift_contacts_view *v = view_of(app);

    return v ? lv_label_get_text(v->note) : NULL;
}
