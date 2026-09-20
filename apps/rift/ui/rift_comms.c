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
#define SELECTED_INSET 12
/* handoff §9: COMMS is 372 list / 560 thread / 300 route. */
#define LIST_W_WIDE 372
#define CTX_W_WIDE 300
/* Portrait stacks them, and gives the list a fixed slice: a list that grew
 * with the number of conversations would push the thread off the screen. */
#define LIST_H_PORTRAIT 268

struct conv_row {
    lv_obj_t *slot;
    lv_obj_t *line;
    lv_obj_t *glyph;
    lv_obj_t *name;
    lv_obj_t *preview;
    lv_obj_t *pill;
    lv_obj_t *route;
    char key[RIFT_KEY_HEX];
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

    /* what the built list was chosen from */
    char shape_key[RIFT_MAX_CONVERSATIONS][RIFT_KEY_HEX];
    int shape_count;
    char shape_open[RIFT_KEY_HEX];
    int shape_wide;
    int shape_valid;

    /* the order of the last refresh, so a key press can step through it */
    char order_key[RIFT_MAX_CONVERSATIONS][RIFT_KEY_HEX];
    int order_count;
};

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
    name = rift_model_peer_name(&app->model, peer);
    if (name && name[0]) {
        rift_utf8_ellipsis(out, out_len, name);
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

static void build_conv_row(struct rift_comms *v, const struct rift_conv *c, int selected)
{
    struct conv_row *r = &v->row[v->row_count];

    memset(r, 0, sizeof(*r));
    r->owner = v;
    copy_key(r->key, sizeof(r->key), c->key);

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
    r->route = rift_cell(r->line, POS_STYLE_CAPTION, COL_ROUTE, LV_TEXT_ALIGN_RIGHT);
    v->row_count++;
}

static void update_conv_row(struct rift_comms *v, struct conv_row *r, const struct rift_conv *c,
                            int64_t now)
{
    struct rift_app *a = v->app;
    const struct rift_node *n = rift_model_find(&a->model, c->key);
    char text[RIFT_PREVIEW_MAX];

    rift_glyph_set(r->glyph, rift_app_glyph(n, now));
    if (c->have_name && c->name[0]) {
        rift_cell_set_text_fit(r->name, c->name);
    } else {
        /* A peer with no name anywhere is named by the hash MeshCore routes
         * on, never by an empty row. */
        snprintf(text, sizeof(text), "%.2s", c->key);
        rift_cell_set_text_fit(r->name, text);
    }
    rift_fmt_preview(c->newest, text, sizeof(text));
    rift_cell_set_text_fit(r->preview, text);
    /* The pill and the preview's visibility were set before the layout that
     * preceded this loop; setting them again here would be setting them
     * after the widths they decide have already been used. */
    if (!n) {
        /* A conversation with a peer the contact table no longer holds. */
        lv_label_set_text(r->route, RIFT_UNKNOWN);
    } else if (rift_link_of(n) == RIFT_LINK_DIRECT) {
        lv_label_set_text(r->route, "DIRECT");
    } else if (rift_link_of(n) == RIFT_LINK_UNKNOWN) {
        lv_label_set_text(r->route, "NO PATH");
    } else {
        rift_fmt_hops(n, text, sizeof(text));
        lv_label_set_text_fmt(r->route, "%s HOPS", text);
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
    lv_obj_add_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
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
        lv_obj_set_height(v->pane_thread, LV_PCT(100));
        lv_obj_set_flex_grow(v->pane_thread, 1);
        lv_obj_remove_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_width(v->pane_list, LV_PCT(100));
        lv_obj_set_height(v->pane_list, LIST_H_PORTRAIT);
        lv_obj_set_flex_grow(v->pane_list, 0);
        lv_obj_set_width(v->pane_thread, LV_PCT(100));
        lv_obj_set_flex_grow(v->pane_thread, 1);
        lv_obj_add_flag(v->pane_ctx, LV_OBJ_FLAG_HIDDEN);
    }
    rift_thread_shape(v->thread, app->wide);
    v->shape_valid = 0;
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
    int changed;
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
            const char *nm = rift_model_peer_name(m, peer);

            for (i = count; i > 0; i--) {
                conv[i] = conv[i - 1];
            }
            memset(&conv[0], 0, sizeof(conv[0]));
            copy_key(conv[0].key, sizeof(conv[0].key), peer);
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

    changed = !v->shape_valid || count != v->shape_count || app->wide != v->shape_wide ||
              strcmp(peer ? peer : "", v->shape_open) != 0;
    for (i = 0; !changed && i < count; i++) {
        if (strcmp(conv[i].key, v->shape_key[i]) != 0) {
            changed = 1;
        }
    }
    if (changed) {
        lv_obj_clean(v->list);
        v->row_count = 0;
        for (i = 0; i < count && v->row_count < RIFT_MAX_CONVERSATIONS; i++) {
            build_conv_row(v, &conv[i], peer && strcmp(conv[i].key, peer) == 0);
        }
        v->shape_valid = 1;
        v->shape_count = count;
        v->shape_wide = app->wide;
        copy_key(v->shape_open, sizeof(v->shape_open), peer);
        for (i = 0; i < count; i++) {
            copy_key(v->shape_key[i], sizeof(v->shape_key[i]), conv[i].key);
        }
    }
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
    lv_obj_update_layout(v->pane_list);
    for (i = 0; i < v->row_count && i < count; i++) {
        update_conv_row(v, &v->row[i], &conv[i], now);
    }

    /* The list's own note. Channels are named here rather than left to be
     * noticed as an absence: the approved design puts them in this list, so
     * a reader who knows the design would otherwise be looking for a
     * feature and finding a short list. */
    if (count == 0) {
        if (!m->messages_valid) {
            lv_label_set_text(v->note, "Waiting for meshcored.");
        } else {
            lv_label_set_text(v->note,
                              "No messages yet. Channels are not in the radio service, so this "
                              "list holds direct conversations only.");
        }
    } else if (m->stale) {
        lv_label_set_text_fmt(v->note, "%d conversation%s, cached: meshcored is not answering.",
                              count, count == 1 ? "" : "s");
    } else {
        lv_label_set_text_fmt(v->note,
                              "%d conversation%s" RIFT_SEP "direct only: channels are not in "
                              "the radio service",
                              count, count == 1 ? "" : "s");
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
