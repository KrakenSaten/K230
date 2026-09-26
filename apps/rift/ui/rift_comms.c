/*
 * COMMS. See rift_comms.h.
 *
 * This file owns the conversation list and the three panes; the open
 * conversation is rift_thread.c, the way NODES keeps its detail in
 * rift_detail.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_comms.h"

#include "pos_styles.h"
#include "rift_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Column widths, chosen the way NODES chose its: the widest value each one
 * holds, in Mono 14, with room to spare in Outdoor. Everything but the name
 * and the preview is fixed, so a row cannot reflow as its values change. */
#define COL_GAP 8
#define COL_ROUTE 72
#define COL_HEARD 44
#define PULSE_PULL (5 - COL_GAP) /* the pulse 5 px after its age, as in NODES */
/* The selected row, as NODES draws it: a slab 2 px taller than a row, inset
 * 8, with 2 px of air for the focus outline LVGL draws outside the box. */
#define SELECTED_INSET_H 8
#define SELECTED_INSET_V 2
#define SELECTED_AIR 2
/* handoff §9: COMMS is 372 list / 560 thread / 300 route. */
#define LIST_W_WIDE 372
#define CTX_W_WIDE 300
/* Portrait stacks them. The list is as tall as its rows, and no taller than
 * this many while a conversation is open - enough to switch between the
 * last few without scrolling, and the rest of the height is the thread's.
 * With nothing open the list may take everything but THREAD_MIN_H: there is
 * no thread to read, only a composer saying what it is waiting for. It used
 * to be a fixed 268 px slice, which with two conversations was 150 px of
 * nothing between the list and the thread. */
#define PORTRAIT_OPEN_ROWS 5
#define THREAD_MIN_H 168

struct conv_row {
    lv_obj_t *slot;
    lv_obj_t *line;
    lv_obj_t *glyph;
    lv_obj_t *name;
    lv_obj_t *preview;
    lv_obj_t *pill;
    lv_obj_t *pulse;
    lv_obj_t *heard;
    lv_obj_t *route;
    char key[RIFT_KEY_HEX];
    int selected;
    struct rift_comms *owner;
};

struct rift_comms {
    struct rift_app *app;
    lv_obj_t *root;

    lv_obj_t *pane_list;
    lv_obj_t *head;
    lv_obj_t *list;
    lv_obj_t *note;
    struct conv_row row[RIFT_MAX_CONVERSATIONS];
    int row_count;

    lv_obj_t *pane_thread;
    struct rift_thread *thread;

    lv_obj_t *pane_ctx;
    lv_obj_t *ctx_chain;
    lv_obj_t *ctx_stats;
    lv_obj_t *ctx_tally;
    lv_obj_t *ctx_history;

    /* which conversation the list last revealed */
    char shape_open[RIFT_KEY_HEX];
    int shape_valid;

    /* the order of the last refresh, so a key press can step through it */
    char order_key[RIFT_MAX_CONVERSATIONS][RIFT_KEY_HEX];
    int order_count;
};

/* Show the list's note only when there is something in it. */
static void note_shown(lv_obj_t *note, int shown)
{
    if (shown) {
        lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- small shared bits ---------------------------------------------------- */

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
    /* A layout box takes no taps, so it cannot swallow the one meant for the
     * row under it (RIFT-DEV-1 wants the whole row width as the hit area). */
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* Copy a public key into a field of the same fixed size.
 *
 * A plain "%s" here draws -Wformat-truncation: the source is a char array
 * inside a bigger object - one conversation in an array of them - and the
 * compiler has to assume a string in it might not be terminated before the
 * end of that object, which is thousands of bytes. The precision says what
 * the field actually is, which is both true and what the compiler needs. */
static void copy_key(char *dst, size_t dst_len, const char *src)
{
    if (dst && dst_len > 0) {
        snprintf(dst, dst_len, "%.*s", (int)(dst_len - 1), src ? src : "");
    }
}

static lv_obj_t *wrap_label(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

const char *rift_comms_open_peer(const struct rift_app *app)
{
    if (!app || !app->have_conv || !app->conv[0]) {
        return NULL;
    }
    return app->conv;
}

void rift_comms_target_label(const struct rift_app *app, char *out, size_t out_len)
{
    const char *peer;
    const char *name;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    peer = rift_comms_open_peer(app);
    if (!peer) {
        return;
    }
    name = rift_model_conv_name(&app->model, peer);
    if (name && name[0]) {
        rift_utf8_ellipsis(out, out_len, name);
        return;
    }
    if (rift_key_is_channel(peer) >= 0) {
        /* A channel with no name: the slot is what it is addressed by. */
        snprintf(out, out_len, "CHANNEL %d", rift_key_is_channel(peer));
        return;
    }
    /* No name anywhere: the node hash is what MeshCore routes on, and is the
     * only other thing that identifies this peer to a reader. */
    snprintf(out, out_len, "%.2s", peer);
}

void rift_comms_submit(struct rift_app *app, const char *text)
{
    const char *peer;
    const char *refusal;

    if (!app) {
        return;
    }
    peer = rift_comms_open_peer(app);
    refusal = rift_thread_refusal(app);
    if (refusal) {
        rift_model_send_failed(&app->model, refusal);
        rift_app_refresh(app);
        return;
    }
    if (!text || !text[0]) {
        /* Nothing typed is not a failure worth a caption; it is nothing to
         * do. */
        return;
    }
    rift_model_send_clear(&app->model);
    if (rift_ipc_send_message(&app->ipc, peer, text) == 0) {
        /* The field is emptied only once the request has been written. A
         * composer that cleared itself and then failed would have thrown
         * away what the reader typed. */
        if (app->comms) {
            rift_thread_composer_clear(app->comms->thread);
        }
        if (app->composer) {
            lv_textarea_set_text(app->composer, "");
        }
    }
    rift_app_refresh(app);
}

/* ---- the conversation list -------------------------------------------------- */

static void on_conv_row(lv_event_t *e)
{
    const struct conv_row *r = lv_event_get_user_data(e);

    rift_app_open_conversation(r->owner->app, r->key);
}

/* A row, built once. Which conversation it shows is set on every refresh
 * (bind_conv_row): a new message re-orders the list, and re-ordering used to
 * delete and rebuild every row in it; now it only rebinds them. */
static void build_conv_row(struct rift_comms *v)
{
    struct conv_row *r = &v->row[v->row_count];

    memset(r, 0, sizeof(*r));
    r->owner = v;

    r->slot = lv_obj_create(v->list);
    lv_obj_remove_style_all(r->slot);
    lv_obj_set_width(r->slot, LV_PCT(100));
    lv_obj_set_height(r->slot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_CLICKABLE);

    r->line = dense_row(r->slot, RIFT_ROW_H);
    /* A tap opens the conversation and does nothing else: choosing where a
     * message would go is not sending one (RIFT-DEV-1). */
    lv_obj_add_flag(r->line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->line, on_conv_row, LV_EVENT_CLICKED, r);

    r->glyph = rift_glyph_create(r->line);
    r->name = rift_cell(r->line, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->name, 1);
    lv_obj_set_width(r->name, 1);
    r->preview = rift_cell(r->line, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(r->preview, 2);
    lv_obj_set_width(r->preview, 1);
    r->pill = rift_unread_pill(r->line);
    /* When the other side was last heard from, and the same age bucketed
     * (rift_model_conv_heard, rift_pulse_of), the pulse close after the age
     * as NODES draws it. */
    r->heard = rift_cell(r->line, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    r->pulse = rift_pulse_create(r->line);
    lv_obj_set_style_margin_left(r->pulse, PULSE_PULL, 0);
    r->route = rift_cell(r->line, POS_STYLE_CAPTION, COL_ROUTE, LV_TEXT_ALIGN_RIGHT);
    v->row_count++;
}

/* Point a row at a conversation, and give it the selection's look when it
 * is the open one. */
static void bind_conv_row(struct conv_row *r, const struct rift_conv *c, int selected)
{
    copy_key(r->key, sizeof(r->key), c->key);
    if (selected == r->selected) {
        return;
    }
    if (selected) {
        pos_style_add(r->slot, POS_STYLE_SLAB, 0);
        pos_style_add(r->slot, POS_STYLE_SELECTED, 0);
        lv_obj_set_style_pad_hor(r->slot, SELECTED_INSET_H, 0);
        lv_obj_set_style_pad_ver(r->slot, SELECTED_INSET_V, 0);
        lv_obj_set_style_margin_ver(r->slot, SELECTED_AIR, 0);
    } else {
        lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SLAB), 0);
        lv_obj_remove_style(r->slot, pos_style(POS_STYLE_SELECTED), 0);
        lv_obj_set_style_pad_hor(r->slot, 0, 0);
        lv_obj_set_style_pad_ver(r->slot, 0, 0);
        lv_obj_set_style_margin_ver(r->slot, 0, 0);
    }
    r->selected = selected;
}

static void update_conv_row(struct rift_comms *v, struct conv_row *r, const struct rift_conv *c,
                            int64_t now)
{
    struct rift_app *a = v->app;
    const struct rift_node *n = c->is_channel ? NULL : rift_model_find(&a->model, c->key);
    char text[RIFT_PREVIEW_MAX];

    rift_glyph_set(r->glyph, c->is_channel ? RIFT_GLYPH_CHANNEL : rift_app_glyph(n, now));
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
    rift_fmt_preview(c->newest, text, sizeof(text));
    rift_cell_set_text_fit(r->preview, text);
    {
        int64_t heard_ms = 0;
        int heard = rift_model_conv_heard(&a->model, c, &heard_ms);

        rift_pulse_set(r->pulse, rift_pulse_of(now - heard_ms, heard));
        rift_fmt_age(now - heard_ms, heard, text, sizeof(text));
        rift_label_set(r->heard, text);
    }
    /* The pill and the preview's visibility were set before the layout that
     * preceded this loop; setting them again here would be setting them
     * after the widths they decide have already been used. */
    if (c->is_channel) {
        /* A channel has no path and cannot have one: a group frame is
         * flooded to everyone who holds the key, and there is no peer for a
         * route to lead to. FLOOD is the whole truth about how it travels. */
        rift_label_set(r->route, "FLOOD");
    } else if (!n) {
        /* A conversation with a peer the contact table no longer holds. */
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

/* ---- the landscape route pane ------------------------------------------------ */

static void build_ctx(struct rift_comms *v)
{
    lv_obj_t *panel;

    v->pane_ctx = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->pane_ctx);
    /* A DS panel: the hairline in `line` is what divides the panes, and the
     * panel role is where that hairline is defined. */
    pos_style_add(v->pane_ctx, POS_STYLE_PANEL, 0);
    lv_obj_set_style_pad_all(v->pane_ctx, RIFT_PANE_PAD, 0);
    lv_obj_set_width(v->pane_ctx, CTX_W_WIDE);
    lv_obj_set_height(v->pane_ctx, LV_PCT(100));
    lv_obj_set_flex_flow(v->pane_ctx, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->pane_ctx, 8, 0);
    lv_obj_set_scroll_dir(v->pane_ctx, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->pane_ctx, LV_SCROLLBAR_MODE_AUTO);

    panel = rift_panel(v->pane_ctx, "ROUTE");
    v->ctx_chain = wrap_label(panel, POS_STYLE_CAPTION);
    v->ctx_stats = wrap_label(v->pane_ctx, POS_STYLE_CAPTION);
    v->ctx_tally = wrap_label(v->pane_ctx, POS_STYLE_CAPTION);
    v->ctx_history = wrap_label(v->pane_ctx, POS_STYLE_TEXT_MUTED);
    lv_obj_add_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
}

/* The one thing about a thread's history that looking at it does not tell
 * you. Said once, in the route pane where there is room, rather than under
 * every thread in two lines (mesh.messages, "persistent": false). */
static const char *history_note(const struct rift_model *m)
{
    if (m->messages_valid && !m->messages_persistent) {
        return "This history is the radio service's, and does not survive a restart of it.";
    }
    return "";
}

static void refresh_ctx(struct rift_comms *v, const struct rift_conv *conv, int64_t now)
{
    struct rift_app *a = v->app;
    const struct rift_model *m = &a->model;
    const char *peer = rift_comms_open_peer(a);
    const struct rift_node *n = peer ? rift_model_find(m, peer) : NULL;
    char chain[RIFT_CHAIN_MAX];
    char label[RIFT_LABEL_MAX];
    char rssi[RIFT_SIGNAL_MAX];
    char snr[RIFT_SIGNAL_MAX];
    char heard[RIFT_AGE_MAX];
    struct rift_path p;

    if (!v->pane_ctx) {
        return;
    }
    if (!peer) {
        lv_label_set_text(v->ctx_chain, "No conversation is open.");
        lv_label_set_text(v->ctx_stats, "");
        lv_label_set_text(v->ctx_tally, "");
        lv_label_set_text(v->ctx_history, history_note(m));
        return;
    }
    if (rift_key_is_channel(peer) >= 0) {
        const struct rift_channel *ch = rift_model_key_channel(m, peer);

        /* A channel has no route and no signal of its own. What it has is a
         * key, the one-byte hash that key derives, and the fact that every
         * message on it is flooded to whoever holds the same bytes. Drawing
         * a hop chain here would be drawing a path that does not exist. */
        lv_label_set_text(v->ctx_chain, "No route: a channel is a shared key. Every node "
                                        "holding it hears a message; nothing acknowledges "
                                        "one.");
        if (ch && ch->have_hash) {
            if (ch->have_key_bits) {
                lv_label_set_text_fmt(v->ctx_stats, "HASH %s" RIFT_SEP "%d-BIT KEY",
                                      ch->hash, ch->key_bits);
            } else {
                lv_label_set_text_fmt(v->ctx_stats, "HASH %s", ch->hash);
            }
        } else {
            lv_label_set_text(v->ctx_stats, "");
        }
        if (conv && conv->outgoing > 0) {
            /* No DELIVERED and no NO ACK: neither is a number this protocol
             * can produce for a channel. */
            lv_label_set_text_fmt(v->ctx_tally,
                                  "%d SENT" RIFT_SEP "NOTHING ACKNOWLEDGES A CHANNEL",
                                  conv->outgoing);
        } else {
            lv_label_set_text(v->ctx_tally, "Nothing sent on this channel yet.");
        }
        lv_label_set_text(v->ctx_history, history_note(m));
        return;
    }
    if (!n) {
        lv_label_set_text(v->ctx_chain,
                          "This peer is not in the node list, so no route to it is known.");
        lv_label_set_text(v->ctx_stats, "");
    } else {
        const char *self = (m->have_identity && m->self_name[0]) ? m->self_name : "this device";

        if (rift_path_parse(n, &p) != 0) {
            memset(&p, 0, sizeof(p));
        }
        rift_fmt_label(n, label, sizeof(label));
        rift_path_chain(self, &p, label, rift_app_resolve, a, chain, sizeof(chain));
        lv_label_set_text(v->ctx_chain, chain);
        rift_fmt_rssi(n->rssi_dbm, n->have_rssi, rssi, sizeof(rssi));
        rift_fmt_snr(n->snr_db, n->have_snr, snr, sizeof(snr));
        rift_fmt_age(now - n->heard_mono_ms, n->have_heard, heard, sizeof(heard));
        lv_label_set_text_fmt(v->ctx_stats, "HEARD %s" RIFT_SEP "LAST HOP %s" RIFT_SEP "SNR %s",
                              heard, rssi, snr);
    }
    /* The delivery tally is this app's own arithmetic over the messages it
     * still holds. meshcored keeps no such count, and the window is
     * bounded, so it is what RIFT has seen and not a total. */
    if (conv && conv->outgoing > 0) {
        lv_label_set_text_fmt(v->ctx_tally,
                              "OF %d SENT" RIFT_SEP "%d DELIVERED" RIFT_SEP "%d NO ACK" RIFT_SEP
                              "%d FAILED",
                              conv->outgoing, conv->acked, conv->no_ack, conv->failed);
    } else {
        lv_label_set_text(v->ctx_tally, "Nothing sent to this peer yet.");
    }
    lv_label_set_text(v->ctx_history, history_note(m));
}

/* ---- layout -------------------------------------------------------------------- */

void rift_comms_shape(struct rift_app *app)
{
    struct rift_comms *v = app ? app->comms : NULL;

    if (!v) {
        return;
    }
    lv_obj_set_flex_flow(v->root, app->wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    if (app->wide) {
        lv_obj_set_width(v->pane_list, LIST_W_WIDE);
        lv_obj_set_height(v->pane_list, LV_PCT(100));
        lv_obj_set_flex_grow(v->pane_list, 0);
        lv_obj_set_flex_grow(v->list, 1);
        lv_obj_set_height(v->pane_thread, LV_PCT(100));
        lv_obj_set_flex_grow(v->pane_thread, 1);
        lv_obj_remove_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* As tall as what is in it; the list inside is sized to its rows on
         * every refresh (size_portrait_list). */
        lv_obj_set_width(v->pane_list, LV_PCT(100));
        lv_obj_set_height(v->pane_list, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(v->pane_list, 0);
        lv_obj_set_flex_grow(v->list, 0);
        lv_obj_set_width(v->pane_thread, LV_PCT(100));
        lv_obj_set_flex_grow(v->pane_thread, 1);
        lv_obj_add_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    }
    rift_thread_shape(v->thread, app->wide);
    v->shape_valid = 0;
}

/* Portrait: the list as tall as its rows, up to PORTRAIT_OPEN_ROWS while a
 * thread is open, or up to all but THREAD_MIN_H of the section when none
 * is. What does not fit scrolls, and the selected row is scrolled into
 * view as it always was. */
static void size_portrait_list(struct rift_comms *v, int count, int open)
{
    int32_t rows_h = (int32_t)count * RIFT_ROW_H;
    int32_t cap;

    if (open && count > 0) {
        rows_h += 2 * (SELECTED_INSET_V + SELECTED_AIR);
        cap = PORTRAIT_OPEN_ROWS * RIFT_ROW_H + 2 * (SELECTED_INSET_V + SELECTED_AIR);
    } else {
        cap = lv_obj_get_height(v->root) - RIFT_HEADER_ROW_H - 1 - THREAD_MIN_H;
        if (cap < RIFT_ROW_H) {
            cap = RIFT_ROW_H;
        }
    }
    if (rows_h > cap) {
        rows_h = cap;
    }
    if (lv_obj_get_height(v->list) != rows_h ||
        lv_obj_get_style_height(v->list, LV_PART_MAIN) != rows_h) {
        lv_obj_set_height(v->list, rows_h);
    }
}

/* ---- create, destroy, refresh --------------------------------------------------- */

lv_obj_t *rift_comms_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_comms *v = calloc(1, sizeof(*v));
    lv_obj_t *head;
    lv_obj_t *cell;

    if (!v) {
        return NULL;
    }
    app->comms = v;
    v->app = app;

    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    lv_obj_set_size(v->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);

    v->pane_list = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->pane_list);
    lv_obj_set_width(v->pane_list, LV_PCT(100));
    lv_obj_set_flex_flow(v->pane_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(v->pane_list, RIFT_PAD, 0);
    lv_obj_remove_flag(v->pane_list, LV_OBJ_FLAG_SCROLLABLE);

    head = dense_row(v->pane_list, RIFT_HEADER_ROW_H);
    v->head = head;
    cell = rift_cell(head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(cell, 1);
    lv_label_set_text(cell, "CONVERSATIONS");
    cell = rift_cell(head, POS_STYLE_CAPTION, COL_HEARD, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(cell, "HEARD");
    cell = rift_cell(head, POS_STYLE_CAPTION, RIFT_PULSE_W, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_margin_left(cell, PULSE_PULL, 0);
    cell = rift_cell(head, POS_STYLE_CAPTION, COL_ROUTE, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(cell, "ROUTE");
    rift_rule(v->pane_list);

    v->list = lv_obj_create(v->pane_list);
    lv_obj_remove_style_all(v->list);
    lv_obj_set_width(v->list, LV_PCT(100));
    lv_obj_set_flex_grow(v->list, 1);
    lv_obj_set_flex_flow(v->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v->list, LV_SCROLLBAR_MODE_AUTO);

    v->note = wrap_label(v->pane_list, POS_STYLE_CAPTION);

    v->pane_thread = lv_obj_create(v->root);
    lv_obj_remove_style_all(v->pane_thread);
    lv_obj_set_width(v->pane_thread, LV_PCT(100));
    lv_obj_set_flex_grow(v->pane_thread, 1);
    lv_obj_remove_flag(v->pane_thread, LV_OBJ_FLAG_SCROLLABLE);
    v->thread = rift_thread_create(app, v->pane_thread);

    build_ctx(v);
    return v->root;
}

void rift_comms_destroy(struct rift_app *app)
{
    struct rift_comms *v = app ? app->comms : NULL;

    if (!v) {
        return;
    }
    rift_thread_destroy(v->thread);
    /* Every object is a child of the section container and is deleted with
     * it by the shell; the private block is this app's to release. */
    free(v);
    app->comms = NULL;
}

void rift_comms_refresh(struct rift_app *app)
{
    struct rift_comms *v = app ? app->comms : NULL;
    struct rift_conv conv[RIFT_MAX_CONVERSATIONS];
    const struct rift_conv *open_conv = NULL;
    const struct rift_model *m;
    const char *peer;
    int64_t now;
    int count;
    int reveal;
    int i;

    if (!v) {
        return;
    }
    m = &app->model;
    now = rift_app_now(app);
    peer = rift_comms_open_peer(app);
    /* Read before drawing, not after.
     *
     * This refresh is about to put the open thread on screen, so by the time
     * anything is visible it has been read - and the conversation row, the
     * unread pill and the count on the COMMS tab are all drawn from the same
     * numbers in this one pass. Marking afterwards left the badge for the
     * peer whose thread was open showing on the frame that opened it, and
     * cleared it on whichever frame came next, which made the whole screen
     * depend on when the next repaint happened to land. */
    if (peer) {
        rift_model_mark_read(&app->model, peer);
    }
    count = rift_model_conversations(m, conv, RIFT_MAX_CONVERSATIONS);

    /* Every channel the service holds is a row, whether or not anything has
     * been said on it.
     *
     * The model does not call a channel with no messages a conversation -
     * it holds no messages, and inventing history is what this app must not
     * do - but a joined channel that is invisible until somebody speaks is
     * a channel nobody can be the first to speak on. So the channels are
     * added here, as rows with no preview, no unread and a total of zero,
     * which is exactly what they are. They go after the conversations that
     * do hold messages, because those have something to show. */
    for (i = 0; i < m->channel_count && count < RIFT_MAX_CONVERSATIONS; i++) {
        const struct rift_channel *ch = &m->channels[i];
        char key[RIFT_KEY_HEX];
        int held = 0;
        int j;

        rift_channel_conv_key(ch, key, sizeof(key));
        if (!key[0]) {
            continue;
        }
        for (j = 0; j < count; j++) {
            if (strcmp(conv[j].key, key) == 0) {
                held = 1;
            }
        }
        if (held) {
            continue;
        }
        memset(&conv[count], 0, sizeof(conv[count]));
        copy_key(conv[count].key, sizeof(conv[count].key), key);
        conv[count].is_channel = 1;
        conv[count].channel_slot = ch->slot;
        if (ch->have_name && ch->name[0]) {
            rift_utf8_copy(conv[count].name, sizeof(conv[count].name), ch->name);
            conv[count].have_name = 1;
        }
        count++;
    }

    /* A conversation can be open before anything has been said in it:
     * NODES' MESSAGE points the composer at a peer that may have no
     * messages at all. The model does not call that a conversation - it
     * holds no messages, and inventing one there would be inventing history
     * - but the list has to show it, because it is where what you type is
     * going. Nothing is fabricated: no preview, no unread, a total of zero.
     * It goes first because it is the one being written to. */
    if (peer && count < RIFT_MAX_CONVERSATIONS) {
        int held = 0;

        for (i = 0; i < count; i++) {
            if (strcmp(conv[i].key, peer) == 0) {
                held = 1;
            }
        }
        if (!held) {
            const char *nm = rift_model_conv_name(m, peer);
            int slot = rift_key_is_channel(peer);

            for (i = count; i > 0; i--) {
                conv[i] = conv[i - 1];
            }
            memset(&conv[0], 0, sizeof(conv[0]));
            copy_key(conv[0].key, sizeof(conv[0].key), peer);
            if (slot >= 0) {
                conv[0].is_channel = 1;
                conv[0].channel_slot = slot;
            }
            if (nm && nm[0]) {
                rift_utf8_copy(conv[0].name, sizeof(conv[0].name), nm);
                conv[0].have_name = 1;
            }
            count++;
        }
    }

    v->order_count = count;
    for (i = 0; i < count; i++) {
        copy_key(v->order_key[i], sizeof(v->order_key[i]), conv[i].key);
        if (peer && strcmp(conv[i].key, peer) == 0) {
            open_conv = &conv[i];
        }
    }

    reveal = !v->shape_valid || strcmp(peer ? peer : "", v->shape_open) != 0;
    /* One row per conversation, kept: rows are added or taken away only when
     * the number of conversations changes, and otherwise rebound in place.
     * A re-ordering - every new message is one - moves no object, so the
     * list keeps its place and costs a rebind rather than a rebuild. */
    while (v->row_count < count && v->row_count < RIFT_MAX_CONVERSATIONS) {
        build_conv_row(v);
    }
    while (v->row_count > count) {
        v->row_count--;
        lv_obj_delete(v->row[v->row_count].slot);
        memset(&v->row[v->row_count], 0, sizeof(v->row[v->row_count]));
    }
    for (i = 0; i < v->row_count && i < count; i++) {
        bind_conv_row(&v->row[i], &conv[i], peer && strcmp(conv[i].key, peer) == 0);
    }
    v->shape_valid = 1;
    copy_key(v->shape_open, sizeof(v->shape_open), peer);
    /* The unread pill is content-sized and sits in the same row as the name
     * and the preview, both of which are fitted to what is left. So every
     * pill is sized first, the pane is laid out once, and only then is any
     * text fitted - the same two-pass rule as NODES, and for the same
     * reason: otherwise the result depends on which refresh this is. */
    for (i = 0; i < v->row_count && i < count; i++) {
        rift_unread_pill_set(v->row[i].pill, conv[i].unread);
        if (app->wide) {
            lv_obj_add_flag(v->row[i].preview, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(v->row[i].preview, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* The list's own note, only for what the rows cannot say themselves. A
     * count of rows is not that - they are on screen - and that a channel
     * is unacknowledged is said under every message sent on one. */
    if (m->have_channel_fault) {
        /* Said before anything else about this list, because it is the one
         * thing here the mesh cannot put right: a channel key is not
         * re-advertised by anybody. */
        lv_label_set_text_fmt(v->note,
                              "The radio service could not read its stored channels: %s",
                              m->channel_fault);
        note_shown(v->note, 1);
    } else if (count == 0) {
        if (!m->messages_valid || !m->channels_valid) {
            lv_label_set_text(v->note, "Waiting for meshcored.");
        } else {
            lv_label_set_text(v->note,
                              "No conversations and no channels. A channel is joined with its "
                              "key on the radio service, not here; a conversation is started "
                              "from a node's MESSAGE.");
        }
        note_shown(v->note, 1);
    } else if (m->stale) {
        lv_label_set_text_fmt(v->note, "%d row%s, cached: meshcored is not answering.", count,
                              count == 1 ? "" : "s");
        note_shown(v->note, 1);
    } else {
        note_shown(v->note, 0);
    }
    if (!app->wide) {
        size_portrait_list(v, count, peer != NULL);
    }
    lv_obj_update_layout(v->pane_list);
    /* Fewer rows than before under a reader who had scrolled: LVGL leaves the
     * offset past the end of what is left, so bring it back. */
    if (lv_obj_get_scroll_bottom(v->list) < 0) {
        int32_t y = lv_obj_get_scroll_y(v->list) + lv_obj_get_scroll_bottom(v->list);

        lv_obj_scroll_to_y(v->list, y > 0 ? y : 0, LV_ANIM_OFF);
    }
    for (i = 0; i < v->row_count && i < count; i++) {
        update_conv_row(v, &v->row[i], &conv[i], now);
    }
    if (reveal && peer) {
        for (i = 0; i < v->row_count; i++) {
            if (strcmp(v->row[i].key, peer) == 0) {
                lv_obj_scroll_to_view(v->row[i].slot, LV_ANIM_OFF);
                break;
            }
        }
    }

    rift_thread_refresh(v->thread, peer, open_conv);
    if (app->wide) {
        refresh_ctx(v, open_conv, now);
    }
}

/* ---- keys ------------------------------------------------------------------------ */

int rift_comms_key(struct rift_app *app, uint32_t key)
{
    struct rift_comms *v = app ? app->comms : NULL;
    int at = -1;
    int i;

    if (!v) {
        return 0;
    }
    /* While the composer holds focus the arrows belong to it: they move a
     * caret through what is being typed. TAB is what moves between the two
     * panes (handoff §9), and rift_app owns it, because the landscape
     * composer is the command line. */
    if (app->composer_focused || v->order_count == 0) {
        return 0;
    }
    for (i = 0; i < v->order_count; i++) {
        if (app->have_conv && strcmp(v->order_key[i], app->conv) == 0) {
            at = i;
            break;
        }
    }
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_LEFT:
        at = at <= 0 ? 0 : at - 1;
        rift_app_open_conversation(app, v->order_key[at]);
        return 1;
    case LV_KEY_DOWN:
    case LV_KEY_RIGHT:
        at = (at < 0 || at + 1 >= v->order_count) ? (at < 0 ? 0 : at) : at + 1;
        rift_app_open_conversation(app, v->order_key[at]);
        return 1;
    default:
        return 0;
    }
}
