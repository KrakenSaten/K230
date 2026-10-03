/*
 * The conversation list. See rift_conv_list.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_conv_list.h"

#include "pocketui.h"
#include "pos_styles.h"
#include "rift_widgets.h"
#include "rift_emoji_style.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rows in the pool: enough for the tallest pane a list can have (portrait
 * with nothing open, about 25 rows) with a margin each side. */
#define POOL_MAX 40
/* A conversation row. It was one 36 px data row - a name in the row-title
 * type sharing the width with a preview in the 14 px caption - which is the
 * density of a table and too small for the one list a reader picks from by
 * name. Now the name is in the title type on a line that is its own, and in
 * portrait the preview is in body type on a second line under it, so neither
 * is cut short to make room for the other. Landscape's narrow list has no
 * preview (the thread is beside it): one line, a touch target tall. Every
 * height follows the text size (DS §46). */
#define CONV_ROW_MIN_H RIFT_TOUCH_H
#define CONV_ROW_AIR 16
#define CONV_LINE_AIR 6
#define CONV_ROLE_NAME POS_STYLE_TITLE
#define CONV_ROLE_PREVIEW POS_STYLE_TEXT_SECONDARY
#define BIND_MARGIN (2 * rift_conv_row_h(l->wide))
#define COL_GAP 8
#define COL_ROUTE 72
#define COL_HEARD 44
#define PULSE_PULL (5 - COL_GAP)
/* The selected row, as NODES draws it: a slab 2 px taller than a row, inset
 * 8, with 2 px of air for the focus outline LVGL draws outside the box. */
#define SELECTED_INSET_H 8
#define SELECTED_INSET_V 2
#define SELECTED_AIR 2
#define SELECTED_H (rift_conv_row_h(l->wide) + 2 * SELECTED_INSET_V + 2 * SELECTED_AIR)

/* The line with the name on it, and the preview's line under it. */
static int32_t name_line_h(int wide)
{
    if (wide) {
        return LV_MAX(CONV_ROW_MIN_H, pocketui_role_line_height(CONV_ROLE_NAME) + CONV_ROW_AIR);
    }
    return pocketui_role_line_height(CONV_ROLE_NAME) + CONV_LINE_AIR;
}

static int32_t preview_line_h(void)
{
    return pocketui_role_line_height(CONV_ROLE_PREVIEW) + CONV_LINE_AIR;
}

int32_t rift_conv_row_h(int wide)
{
    return wide ? name_line_h(1) : LV_MAX(CONV_ROW_MIN_H, name_line_h(0) + preview_line_h());
}

struct conv_row {
    lv_obj_t *slot;
    lv_obj_t *line;
    lv_obj_t *below; /* the preview's line, portrait only */
    lv_obj_t *ident; /* the identity mark (DS §37.3) */
    lv_obj_t *glyph;
    lv_obj_t *name;
    lv_obj_t *preview;
    lv_obj_t *pill;
    lv_obj_t *pulse;
    lv_obj_t *heard;
    lv_obj_t *route;
    char key[RIFT_KEY_HEX]; /* "" is a free row */
    int item;               /* its place in the order */
    int selected;
    struct rift_conv_list *owner;
};

struct rift_conv_list {
    struct rift_app *app;
    lv_obj_t *list;
    lv_obj_t *spacer;
    struct conv_row row[POOL_MAX];
    int row_count;

    /* The conversations as last given, in order, and the row each lays out
     * into: y is i rows down, plus the selected row's extra height for
     * everything under it. */
    struct rift_conv *conv; /* RIFT_MAX_CONVERSATIONS, on the heap */
    int count;
    int sel;                /* the open conversation's place, or -1 */
    int64_t now;
    int32_t total_h;
    int wide;
    int binding;
};

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
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static void copy_key(char *dst, size_t dst_len, const char *src)
{
    if (dst && dst_len > 0) {
        snprintf(dst, dst_len, "%.*s", (int)(dst_len - 1), src ? src : "");
    }
}

/* ---- rows ------------------------------------------------------------------ */

static void on_conv_row(lv_event_t *e)
{
    const struct conv_row *r = lv_event_get_user_data(e);

    if (r->key[0]) {
        rift_app_open_conversation(r->owner->app, r->key);
    }
}

static void build_row(struct rift_conv_list *l)
{
    struct conv_row *r = &l->row[l->row_count];

    memset(r, 0, sizeof(*r));
    r->owner = l;
    r->item = -1;

    r->slot = lv_obj_create(l->list);
    lv_obj_remove_style_all(r->slot);
    lv_obj_set_width(r->slot, LV_PCT(100));
    lv_obj_set_height(r->slot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r->slot, LV_OBJ_FLAG_HIDDEN);

    r->line = dense_row(r->slot, name_line_h(l->wide));
    /* A tap opens the conversation and does nothing else: choosing where a
     * message would go is not sending one (RIFT-DEV-1). */
    lv_obj_add_flag(r->line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->line, on_conv_row, LV_EVENT_CLICKED, r);

    /* The identity mark first: who this conversation is, in their accent,
     * before the glyph that says how they are reached. */
    r->ident = rift_vrule(r->line, RIFT_IDENT_W);
    rift_vrule_set(r->ident, RIFT_TONE_NONE);
    r->glyph = rift_glyph_create(r->line);
    r->name = rift_cell(r->line, CONV_ROLE_NAME, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->name, 1);
    lv_obj_set_width(r->name, 1);
    r->pill = rift_unread_pill(r->line);
    r->heard = rift_cell(r->line, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    r->pulse = rift_pulse_create(r->line);
    lv_obj_set_style_margin_left(r->pulse, PULSE_PULL, 0);
    r->route = rift_cell(r->line, POS_STYLE_CAPTION, COL_ROUTE, LV_TEXT_ALIGN_RIGHT);
    rift_conv_cols_widen(r->heard, r->route);
    /* The preview's own line, under the name and starting where the name
     * does. The same tap as the line above it: the row is one target. */
    r->below = dense_row(r->slot, preview_line_h());
    lv_obj_set_style_pad_left(r->below, RIFT_IDENT_W + COL_GAP + RIFT_GLYPH_BOX + COL_GAP, 0);
    lv_obj_add_flag(r->below, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->below, on_conv_row, LV_EVENT_CLICKED, r);
    r->preview = rift_cell(r->below, CONV_ROLE_PREVIEW, 0, LV_TEXT_ALIGN_LEFT);
    rift_emoji_style_add(r->preview, CONV_ROLE_PREVIEW);
    lv_obj_set_flex_grow(r->preview, 1);
    lv_obj_set_width(r->preview, 1);
    l->row_count++;
}

/* Each column's widest words: its header and the longest value update_row
 * writes into it. */
void rift_conv_cols_widen(lv_obj_t *heard, lv_obj_t *route)
{
    rift_cell_widen(heard, "HEARD");
    rift_cell_widen(heard, ">99d");
    rift_cell_widen(route, "ROUTE");
    rift_cell_widen(route, "NO PATH");
    rift_cell_widen(route, "64 HOPS");
}

static void unbind(struct conv_row *r)
{
    r->key[0] = '\0';
    r->item = -1;
    lv_obj_add_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
}

static void select_row(struct conv_row *r, int selected)
{
    if (selected == r->selected) {
        return;
    }
    if (selected) {
        pos_style_add(r->slot, POS_STYLE_SLAB, 0);
        pos_style_add(r->slot, POS_STYLE_SELECTED, 0);
        lv_obj_set_style_pad_hor(r->slot, SELECTED_INSET_H, 0);
        lv_obj_set_style_pad_ver(r->slot, SELECTED_INSET_V, 0);
    } else {
        lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SLAB), 0);
        lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SELECTED), 0);
        lv_obj_set_style_pad_hor(r->slot, 0, 0);
        lv_obj_set_style_pad_ver(r->slot, 0, 0);
    }
    r->selected = selected;
}

static void shape_row(struct conv_row *r, int wide)
{
    /* Landscape is the narrow list: the name, the pill and the age. The
     * preview and the route are the thread's header's there. */
    if (wide) {
        lv_obj_add_flag(r->below, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(r->route, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(r->below, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(r->route, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_height(r->line, name_line_h(wide));
}

static void update_row(struct rift_conv_list *l, struct conv_row *r, const struct rift_conv *c)
{
    struct rift_app *a = l->app;
    const struct rift_node *n = c->is_channel ? NULL : rift_model_find(&a->model, c->key);
    int64_t now = l->now;
    char text[RIFT_PREVIEW_MAX];

    rift_glyph_set(r->glyph, c->is_channel ? RIFT_GLYPH_CHANNEL : rift_app_glyph(n, now));
    /* The identity mark (DS §37.3). A channel is known by the hash that
     * goes on the air, so the same channel is the same colour on every
     * device that holds its key; a peer by their public key. A
     * conversation is with somebody by definition, so every peer gets one
     * - whether the table still holds them, and whether it ever said what
     * they are - except a peer it says is a repeater or a sensor. */
    if (c->is_channel) {
        const struct rift_channel *ch = rift_model_key_channel(&a->model, c->key);

        rift_vrule_set_identity(r->ident, rift_ident_hash(ch && ch->have_hash ? ch->hash
                                                          : c->have_name ? c->name
                                                                         : c->key));
    } else if (!n || !n->have_type || rift_ident_for_type(n->type, n->have_type)) {
        rift_vrule_set_identity(r->ident, rift_ident_hash(c->key));
    } else {
        rift_vrule_set(r->ident, RIFT_TONE_NONE);
    }
    rift_unread_pill_set(r->pill, c->unread);
    if (c->have_name && c->name[0]) {
        rift_cell_set_text_fit(r->name, c->name);
    } else if (c->is_channel) {
        snprintf(text, sizeof(text), "CHANNEL %d", c->channel_slot);
        rift_cell_set_text_fit(r->name, text);
    } else {
        /* A peer with no name anywhere is named by the hash MeshCore routes
         * on, never by an empty row. */
        snprintf(text, sizeof(text), "%.2s", c->key);
        rift_cell_set_text_fit(r->name, text);
    }
    if (!l->wide) {
        rift_fmt_preview(c->newest, text, sizeof(text));
        rift_cell_set_text_fit(r->preview, text);
    }
    {
        int64_t heard_ms = 0;
        int heard = rift_model_conv_heard(&a->model, c, &heard_ms);

        rift_pulse_set(r->pulse, rift_pulse_of(now - heard_ms, heard));
        rift_fmt_age(now - heard_ms, heard, text, sizeof(text));
        rift_label_set(r->heard, text);
    }
    if (l->wide) {
        return;
    }
    if (c->is_channel) {
        /* A channel has no path and cannot have one: a group frame is
         * flooded to everyone who holds the key. FLOOD is the whole truth
         * about how it travels. */
        rift_label_set(r->route, "FLOOD");
    } else if (!n) {
        rift_label_set(r->route, RIFT_UNKNOWN);
    } else if (rift_link_of(n) == RIFT_LINK_DIRECT) {
        rift_label_set(r->route, "DIRECT");
    } else if (rift_link_of(n) == RIFT_LINK_UNKNOWN) {
        rift_label_set(r->route, "NO PATH");
    } else {
        char hops[RIFT_HOPS_MAX];

        rift_fmt_hops(n, hops, sizeof(hops));
        snprintf(text, sizeof(text), "%s HOPS", hops);
        rift_label_set(r->route, text);
    }
}

/* ---- the virtual list --------------------------------------------------------- */

static int32_t item_y(const struct rift_conv_list *l, int i)
{
    int32_t y = (int32_t)i * rift_conv_row_h(l->wide);

    if (l->sel >= 0 && i > l->sel) {
        y += SELECTED_H - rift_conv_row_h(l->wide);
    }
    return y;
}

static int32_t item_h(const struct rift_conv_list *l, int i)
{
    return i == l->sel ? SELECTED_H : rift_conv_row_h(l->wide);
}

/* Bind the pool to the part of the list on screen plus a margin, plus the
 * open row wherever it is. Rows already showing a conversation still
 * wanted keep it; the rest are rebound. every says whether kept rows are
 * refreshed too (a repaint) or only the newly bound (a scroll). */
static void bind_window(struct rift_conv_list *l, int every)
{
    int32_t top = lv_obj_get_scroll_y(l->list) - BIND_MARGIN;
    int32_t bottom = lv_obj_get_scroll_y(l->list) + lv_obj_get_height(l->list) + BIND_MARGIN;
    static char want[RIFT_MAX_CONVERSATIONS];
    int fresh_rows = 0;
    int i;
    int j;

    l->binding = 1;
    memset(want, 0, sizeof(want));
    for (i = 0; i < l->count; i++) {
        int32_t y = item_y(l, i);

        if ((y + item_h(l, i) > top && y < bottom) || i == l->sel) {
            want[i] = 1;
        }
    }
    for (j = 0; j < l->row_count; j++) {
        struct conv_row *r = &l->row[j];
        int kept = -1;

        if (!r->key[0]) {
            continue;
        }
        /* Where it was is where it most likely still is; a re-ordering is
         * the only thing that moves it, and then the walk finds it. */
        if (r->item >= 0 && r->item < l->count && want[r->item] == 1 &&
            strcmp(l->conv[r->item].key, r->key) == 0) {
            kept = r->item;
        } else {
            for (i = 0; i < l->count; i++) {
                if (want[i] == 1 && strcmp(l->conv[i].key, r->key) == 0) {
                    kept = i;
                    break;
                }
            }
        }
        if (kept < 0) {
            unbind(r);
            continue;
        }
        want[kept] = 2;
        r->item = kept;
    }
    for (i = 0; i < l->count; i++) {
        struct conv_row *r = NULL;

        if (want[i] != 1) {
            continue;
        }
        for (j = 0; j < l->row_count; j++) {
            if (!l->row[j].key[0]) {
                r = &l->row[j];
                break;
            }
        }
        if (!r && l->row_count < POOL_MAX) {
            r = &l->row[l->row_count];
            build_row(l);
            shape_row(r, l->wide);
        }
        if (!r) {
            break; /* the pool is spent; the rest is off screen */
        }
        copy_key(r->key, sizeof(r->key), l->conv[i].key);
        r->item = i;
        want[i] = 3;
    }
    /* Placed and styled first, then one layout if a new row needs its
     * widths, then filled in: a name is fitted to the width its row has. */
    for (j = 0; j < l->row_count; j++) {
        struct conv_row *r = &l->row[j];
        int32_t y;

        if (!r->key[0]) {
            continue;
        }
        select_row(r, r->item == l->sel);
        /* The row's heights follow the text size, which can change under a
         * row already built. The slot is as tall as a row says it is, so a
         * row shorter than a touch target's height still takes that. */
        if (lv_obj_get_style_height(r->line, LV_PART_MAIN) != name_line_h(l->wide)) {
            lv_obj_set_height(r->line, name_line_h(l->wide));
        }
        if (lv_obj_get_style_height(r->below, LV_PART_MAIN) != preview_line_h()) {
            lv_obj_set_height(r->below, preview_line_h());
        }
        lv_obj_set_style_min_height(r->slot, rift_conv_row_h(l->wide), 0);
        y = item_y(l, r->item) + (r->item == l->sel ? SELECTED_AIR : 0);
        if (lv_obj_get_y(r->slot) != y || lv_obj_get_x(r->slot) != 0) {
            lv_obj_set_pos(r->slot, 0, y);
        }
        lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_HIDDEN);
        if (want[r->item] == 3 && lv_obj_get_width(r->name) <= 1) {
            fresh_rows = 1;
        }
    }
    if (fresh_rows) {
        lv_obj_update_layout(l->list);
    }
    for (j = 0; j < l->row_count; j++) {
        struct conv_row *r = &l->row[j];

        if (r->key[0] && (every || want[r->item] == 3)) {
            update_row(l, r, &l->conv[r->item]);
        }
    }
    l->binding = 0;
}

static void on_list_scroll(lv_event_t *e)
{
    struct rift_conv_list *l = lv_event_get_user_data(e);

    if (l->binding) {
        return;
    }
    bind_window(l, 0);
    l->app->refresh_pending = 1;
}

static void clamp_scroll(struct rift_conv_list *l)
{
    int32_t most = l->total_h - lv_obj_get_height(l->list);

    if (most < 0) {
        most = 0;
    }
    if (lv_obj_get_scroll_y(l->list) > most) {
        lv_obj_scroll_to_y(l->list, most, LV_ANIM_OFF);
    }
}

static void reveal_selected(struct rift_conv_list *l)
{
    int32_t y = lv_obj_get_scroll_y(l->list);
    int32_t h = lv_obj_get_height(l->list);
    int32_t iy;
    int32_t ih;

    if (l->sel < 0) {
        return;
    }
    iy = item_y(l, l->sel);
    ih = item_h(l, l->sel);
    if (iy < y) {
        lv_obj_scroll_to_y(l->list, iy, LV_ANIM_OFF);
    } else if (iy + ih > y + h) {
        lv_obj_scroll_to_y(l->list, iy + ih - h, LV_ANIM_OFF);
    }
}

/* ---- the public entry points ------------------------------------------------- */

struct rift_conv_list *rift_conv_list_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_conv_list *l = calloc(1, sizeof(*l));

    if (!l) {
        return NULL;
    }
    l->conv = calloc(RIFT_MAX_CONVERSATIONS, sizeof(*l->conv));
    if (!l->conv) {
        free(l);
        return NULL;
    }
    l->app = app;
    l->sel = -1;
    l->list = lv_obj_create(parent);
    lv_obj_remove_style_all(l->list);
    lv_obj_set_width(l->list, LV_PCT(100));
    lv_obj_set_flex_grow(l->list, 1);
    lv_obj_set_scroll_dir(l->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l->list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(l->list, on_list_scroll, LV_EVENT_SCROLL, l);
    /* The spacer is what gives the list its height to scroll: one pixel at
     * the bottom of where every row would be. */
    l->spacer = lv_obj_create(l->list);
    lv_obj_remove_style_all(l->spacer);
    lv_obj_set_size(l->spacer, 1, 1);
    lv_obj_remove_flag(l->spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(l->spacer, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

void rift_conv_list_destroy(struct rift_conv_list *l)
{
    if (!l) {
        return;
    }
    /* The objects go with the section container the shell deletes; a
     * scroll event on the way down must not reach a freed block. */
    if (l->list) {
        lv_obj_remove_event_cb_with_user_data(l->list, on_list_scroll, l);
    }
    free(l->conv);
    free(l);
}

lv_obj_t *rift_conv_list_obj(struct rift_conv_list *l)
{
    return l ? l->list : NULL;
}

void rift_conv_list_shape(struct rift_conv_list *l, int wide)
{
    int i;

    if (!l) {
        return;
    }
    l->wide = wide;
    for (i = 0; i < l->row_count; i++) {
        shape_row(&l->row[i], wide);
        unbind(&l->row[i]);
    }
}

void rift_conv_list_refresh(struct rift_conv_list *l, const struct rift_conv *conv, int count,
                            const char *open, int64_t now, int reveal)
{
    int i;

    if (!l) {
        return;
    }
    if (count > RIFT_MAX_CONVERSATIONS) {
        count = RIFT_MAX_CONVERSATIONS;
    }
    if (count > 0) {
        memcpy(l->conv, conv, sizeof(*conv) * (size_t)count);
    }
    l->count = count;
    l->now = now;
    l->sel = -1;
    for (i = 0; open && i < count; i++) {
        if (strcmp(l->conv[i].key, open) == 0) {
            l->sel = i;
            break;
        }
    }
    l->total_h = count > 0 ? item_y(l, count - 1) + item_h(l, count - 1) : 0;
    lv_obj_set_pos(l->spacer, 0, l->total_h > 0 ? l->total_h - 1 : 0);
    lv_obj_update_layout(l->list);
    l->binding = 1;
    clamp_scroll(l);
    if (reveal) {
        reveal_selected(l);
    }
    l->binding = 0;
    bind_window(l, 1);
}

int rift_conv_list_rows_built(const struct rift_conv_list *l)
{
    return l ? l->row_count : 0;
}

int32_t rift_conv_list_total_h(const struct rift_conv_list *l)
{
    return l ? l->total_h : 0;
}
