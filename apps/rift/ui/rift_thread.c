/*
 * The open conversation. See rift_thread.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_thread.h"

#include "app.h"
#include "pocketui.h"
#include "pos_styles.h"
#include "rift_comms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COL_GAP 8
#define MSG_RULE_W 2
/* Between one message and the next: in portrait, where the thread has the
 * height of the panel; and in landscape, where it has 354 px under the
 * strip less the header and the command line, and every four pixels
 * between messages is a message fewer on screen (DS §37.2). */
#define MSG_GAP 6
#define MSG_GAP_WIDE 2
/* The thread's header row: a 36 px data row in portrait, where a finger
 * lands on it; the 28 px header-row height of handoff §4 in landscape,
 * where it is read and never tapped, and the 8 px are the messages'. */
#define HEAD_H_WIDE RIFT_HEADER_ROW_H
/* Between a body and a caption that shares its line. */
#define CAPTION_GAP 10
/* Above the composer. */
#define COMPOSER_GAP 8
/* SEND is an action, 56 tall like every other, and as wide as its word
 * needs - not half the row: the field is what a reader works in. */
#define SEND_W 128

/* One message: the 2 px rule that says whose it is, the body, and ONE
 * caption - age · [claimed sender] · state · evidence.
 *
 * It used to be three lines: a line of age and sender over the body, then
 * the state under it. In a direct thread the sender line said "you" or the
 * peer's name, which the rule's side and the thread's header already say;
 * folded into the caption, the same messages take about a quarter less
 * height, and a landscape pane shows a message more.
 *
 * And the caption shares the body's line when there is room for both - a
 * mesh message is usually short - and wraps under it when there is not. The
 * two are items in a wrapping row, each as wide as its words and no wider
 * than the column, so a one-line message is one line. */
struct msg_row {
    lv_obj_t *slot;
    lv_obj_t *body_row;
    lv_obj_t *rule;
    lv_obj_t *column;
    lv_obj_t *body;
    /* Who said it, on a channel: the claimed name in its identity accent
     * (DS §37.3), its own label so the accent lands on the name alone. */
    lv_obj_t *sender;
    lv_obj_t *caption;
    int32_t max_w; /* the widest either may be, as last set */
    int warn;      /* the caption carries the warn colour */
    int out;       /* laid out as ours (1, right) or theirs (0, left); -1 not yet */
    int ident_on;  /* the sender label carries an identity style */
    uint32_t ident;
};

struct rift_thread {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *head;
    lv_obj_t *glyph;
    lv_obj_t *who;
    lv_obj_t *state;
    lv_obj_t *earlier;
    lv_obj_t *scroll;
    lv_obj_t *note;

    lv_obj_t *composer;
    lv_obj_t *field;
    lv_obj_t *send;

    struct msg_row row[RIFT_THREAD_ROWS];
    int row_count;

    /* What the composer was last told. Enabling a field puts it back in the
     * focus group, and lv_group_add_obj() does that by removing and
     * re-appending it, which moves the focus off it - so saying "enabled"
     * to an already-enabled field once a refresh took the keyboard away
     * from whoever was typing. -1 is "not told yet". */
    int field_enabled;

    /* The thread as it was built: the ids, so a state change updates a row
     * in place and only a new or departed message rebuilds anything. */
    int64_t shape_id[RIFT_THREAD_ROWS];
    int shape_count;
    int shape_valid;
    char shape_peer[RIFT_KEY_HEX]; /* whose thread the rows are */
    /* The height the messages had at the last refresh, and whether the
     * reader was at the end of them. When the pane changes height - the
     * landscape composer appearing under it, the portrait keyboard coming
     * up - a thread that was being read at its end is read at its end
     * again, or the newest message is the one the change hides. A reader
     * who had scrolled back into the history is left where they were. */
    int32_t scroll_h;
    int at_end;
    int wide; /* the landscape shape: tighter rows, a header-row header */
};

/* ---- small shared bits --------------------------------------------------- */

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

/* A label as wide as its words, wrapping at a width set later (max_width,
 * in pixels: LVGL's label sizes itself against a pixel maximum, not a
 * percentage of its parent). */
static lv_obj_t *fit_label(lv_obj_t *parent, enum pos_style_role role)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
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

/* ---- the composer -------------------------------------------------------- */

const char *rift_thread_refusal(const struct rift_app *app)
{
    const struct rift_model *m;

    if (!app) {
        return "No conversation is open.";
    }
    m = &app->model;
    if (!rift_comms_open_peer(app)) {
        return "Choose a conversation to write to.";
    }
    if (m->stale || m->state == RIFT_SVC_ABSENT) {
        return "meshcored is not answering.";
    }
    if (m->have_status && !m->radio_online) {
        /* The service is there and its radio is not usable. mesh.send would
         * be refused with error 5 (docs/api/mesh.md); saying so first is
         * better than sending something to be refused. */
        return "The radio is not ready to send.";
    }
    /* A channel conversation whose channel is gone - left, or its slot
     * taken by another channel since. Its messages are still shown; there
     * is nowhere to write, and the slot's new owner is not it. Until the
     * list has been read nothing is known either way, and nothing is sent. */
    if (rift_key_is_channel(rift_comms_open_peer(app)) >= 0 &&
        !rift_model_key_channel(m, rift_comms_open_peer(app))) {
        return m->channels_valid ? "This channel is not joined any more."
                                 : "The channel list has not been read yet.";
    }
    if (rift_model_sending(m)) {
        return "One message is on its way.";
    }
    return NULL;
}

const char *rift_thread_composer_text(const struct rift_thread *t)
{
    return (t && t->field) ? lv_textarea_get_text(t->field) : NULL;
}

void rift_thread_composer_clear(struct rift_thread *t)
{
    if (t && t->field) {
        lv_textarea_set_text(t->field, "");
    }
}

static void on_send(lv_event_t *e)
{
    struct rift_thread *t = lv_event_get_user_data(e);

    rift_comms_submit(t->app, lv_textarea_get_text(t->field));
}

static void on_field_clicked(lv_event_t *e)
{
    struct rift_thread *t = lv_event_get_user_data(e);

    /* Portrait only: there is no physical keyboard, so the Doors touch
     * keyboard comes up on focus (handoff §10). In landscape the base is
     * attached and no software keyboard is ever shown. */
    if (t->app->wide) {
        return;
    }
    if (!pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, t->app);
    }
}

/* The keyboard's Done, and Enter on a physical one: a single-line field
 * raises READY either way (DS §17.4), and committing a composer means
 * sending. */
static void on_field_ready(lv_event_t *e)
{
    struct rift_thread *t = lv_event_get_user_data(e);

    rift_comms_submit(t->app, lv_textarea_get_text(t->field));
}

static void build_composer(struct rift_thread *t)
{
    t->composer = lv_obj_create(t->root);
    lv_obj_remove_style_all(t->composer);
    lv_obj_set_width(t->composer, LV_PCT(100));
    lv_obj_set_height(t->composer, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t->composer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t->composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t->composer, 12, 0);
    lv_obj_set_style_pad_top(t->composer, COMPOSER_GAP, 0);
    lv_obj_remove_flag(t->composer, LV_OBJ_FLAG_SCROLLABLE);

    t->field = pocketui_text_field(t->composer, "Message", true);
    if (t->field) {
        /* The field is returned; its wrapper is what sits in the flex row
         * (pocketui.h), so the grow goes on the parent. */
        lv_obj_set_flex_grow(lv_obj_get_parent(t->field), 1);
        lv_obj_add_event_cb(t->field, on_field_clicked, LV_EVENT_CLICKED, t);
        lv_obj_add_event_cb(t->field, on_field_ready, LV_EVENT_READY, t);
    }
    t->send = rift_action(t->composer, "SEND", 1, 1, on_send, t);
    if (t->send) {
        lv_obj_set_flex_grow(t->send, 0);
        lv_obj_set_width(t->send, SEND_W);
    }
}

/* ---- message rows -------------------------------------------------------- */

static void build_row(struct rift_thread *t)
{
    struct msg_row *r = &t->row[t->row_count];

    memset(r, 0, sizeof(*r));
    r->slot = lv_obj_create(t->scroll);
    lv_obj_remove_style_all(r->slot);
    lv_obj_set_width(r->slot, LV_PCT(100));
    lv_obj_set_height(r->slot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(r->slot, t->wide ? MSG_GAP_WIDE : MSG_GAP, 0);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->slot, LV_OBJ_FLAG_CLICKABLE);

    r->body_row = lv_obj_create(r->slot);
    lv_obj_remove_style_all(r->body_row);
    lv_obj_set_width(r->body_row, LV_PCT(100));
    lv_obj_set_height(r->body_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->body_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r->body_row, COL_GAP, 0);
    lv_obj_remove_flag(r->body_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->body_row, LV_OBJ_FLAG_CLICKABLE);

    /* The 2 px rule of handoff §6, and the body beside it. Which side it is
     * on is set per message, because that is what says who spoke - there
     * are no bubbles in this design. */
    r->rule = rift_vrule(r->body_row, MSG_RULE_W);
    r->column = lv_obj_create(r->body_row);
    lv_obj_remove_style_all(r->column);
    lv_obj_set_flex_grow(r->column, 1);
    lv_obj_set_width(r->column, 1);
    lv_obj_set_height(r->column, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->column, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r->column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(r->column, CAPTION_GAP, 0);
    lv_obj_remove_flag(r->column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r->column, LV_OBJ_FLAG_CLICKABLE);
    r->body = fit_label(r->column, POS_STYLE_TEXT_PRIMARY);
    r->sender = fit_label(r->column, POS_STYLE_CAPTION);
    lv_obj_add_flag(r->sender, LV_OBJ_FLAG_HIDDEN);
    r->caption = fit_label(r->column, POS_STYLE_CAPTION);
    r->out = -1; /* neither side yet: the first update sets it */
    t->row_count++;
}

static void update_row(struct rift_thread *t, struct msg_row *r,
                       const struct rift_message *msg, int64_t now, int32_t max_w)
{
    struct rift_app *a = t->app;
    const struct rift_node *n = msg->is_channel ? NULL
                                                : rift_model_find(&a->model, msg->peer_key);
    char text[RIFT_MSG_META_MAX];
    int out = (msg->dir == RIFT_MSG_OUT);
    lv_text_align_t align = out ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;

    /* The widest a body or a caption may be is the column: either wraps
     * there, and together they share a line only when both fit. */
    if (max_w > 0 && max_w != r->max_w) {
        lv_obj_set_style_max_width(r->body, max_w, 0);
        lv_obj_set_style_max_width(r->sender, max_w, 0);
        lv_obj_set_style_max_width(r->caption, max_w, 0);
        r->max_w = max_w;
    }
    /* Ours start at the right, theirs at the left, whether the two share a
     * line or not; and own messages carry the rule on the right, received
     * ones on the left. Set when the side changes and not on every repaint:
     * each of these re-lays the row out, and a thread repaints every second. */
    if (out != r->out) {
        lv_obj_set_flex_align(r->column, out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_END, out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
        lv_obj_set_style_text_align(r->body, align, 0);
        lv_obj_set_style_text_align(r->sender, align, 0);
        lv_obj_set_style_text_align(r->caption, align, 0);
        lv_obj_move_to_index(r->rule, out ? 1 : 0);
        r->out = out;
    }

    /* The body is what was said: on a channel, without the "<sender>: "
     * MeshCore writes into the payload, because the caption below names the
     * sender - as a claim, with a trailing "?", since nothing signs a group
     * frame and anyone holding the key can send any name. */
    rift_label_set(r->body, rift_msg_body(msg));
    /* An age, not a time of day. The design's mock reads "11:32"; this board
     * has no clock that survives a power cut (docs/hardware/T-DISPLAY-K230.md)
     * and a message's own timestamp is the *sender's* clock (docs/api/mesh.md),
     * so neither is a local wall time this app could honestly print. */
    {
        /* Who spoke, on a channel, as its own label in the sender's identity
         * accent (DS §37.3): the same claimed name is the same colour on
         * every line and in the conversation list, so a busy channel can be
         * read by who is talking. The "?" stays: nothing signs a group
         * frame. The words are what say who; the colour only agrees. */
        char who[RIFT_NAME_MAX + 2];

        rift_fmt_msg_meta_split(msg, now, who, sizeof(who), text, sizeof(text));
        rift_label_set(r->caption, text);
        rift_label_set(r->sender, who);
        if (who[0]) {
            uint32_t ident = rift_ident_hash(msg->sender_name);
            int named = msg->have_sender_name && msg->sender_name[0];

            lv_obj_remove_flag(r->sender, LV_OBJ_FLAG_HIDDEN);
            if (named && (!r->ident_on || r->ident != ident)) {
                if (r->ident_on) {
                    lv_obj_remove_style(r->sender, pos_style_identity(r->ident), 0);
                }
                lv_obj_add_style(r->sender, pos_style_identity(ident), 0);
                r->ident_on = 1;
                r->ident = ident;
            } else if (!named && r->ident_on) {
                lv_obj_remove_style(r->sender, pos_style_identity(r->ident), 0);
                r->ident_on = 0;
            }
        } else {
            lv_obj_add_flag(r->sender, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Colour never carries a state on its own (handoff §5): the caption
     * already says NO ACK or FAILED in words, and this is the warn colour
     * on top of the word. */
    if (rift_msg_is_warn(msg) != r->warn) {
        r->warn = rift_msg_is_warn(msg);
        if (r->warn) {
            pos_style_add(r->caption, POS_STYLE_STATUS_WARN_TEXT, 0);
        } else {
            lv_obj_remove_style(r->caption, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
        }
    }

    /* The rule's tone: accent_primary for ours; radio_rx for theirs when the
     * peer was heard direct, text_secondary otherwise. */
    if (out) {
        rift_vrule_set(r->rule, RIFT_TONE_ACCENT);
    } else if (msg->is_channel && msg->have_sender_name && msg->sender_name[0]) {
        /* A channel message was heard from nobody in particular - a group
         * frame carries no sender - so it never gets the radio_rx tone that
         * says "this peer was heard direct". Its rule is the claimed
         * sender's identity accent instead, the same one the name beside it
         * carries (DS §37.3). */
        rift_vrule_set_identity(r->rule, rift_ident_hash(msg->sender_name));
    } else {
        rift_vrule_set(r->rule,
                       (!msg->is_channel && n && rift_link_of(n) == RIFT_LINK_DIRECT)
                           ? RIFT_TONE_RX
                           : RIFT_TONE_SECONDARY);
    }
}

/* The note under the thread, only when there is something to say: what
 * became of the last send, what the composer cannot do, or that nothing has
 * been said yet. The rest of the time its lines are the thread's.
 *
 * What used to fill it otherwise - a delivery tally, and that the history
 * does not survive the service's restart - is said once where there is
 * room: every message carries its own state, the landscape route pane keeps
 * the tally and the caveat, and an empty thread, the one place the caveat
 * changes what a reader expects, says it here.
 *
 * Decided before the messages are laid out: whether it is shown changes how
 * tall they are, and the thread is scrolled to its end against that. */
static void paint_note(struct rift_thread *t, const char *peer, int shown)
{
    const struct rift_model *m = &t->app->model;
    const char *refusal = rift_thread_refusal(t->app);

    lv_obj_remove_style(t->note, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    if (m->outbox.failed && m->outbox.error[0]) {
        /* "Not sent" only when the service said no: a submission whose
         * connection went before the answer may well have gone out. */
        lv_label_set_text_fmt(t->note, "%s: %s", m->outbox.unknown ? "No answer" : "Not sent",
                              m->outbox.error);
        pos_style_add(t->note, POS_STYLE_STATUS_WARN_TEXT, 0);
    } else if (rift_model_sending(m)) {
        lv_label_set_text(t->note, "Sending\xE2\x80\xA6");
    } else if (refusal) {
        lv_label_set_text(t->note, refusal);
    } else if (peer && shown == 0) {
        if (rift_key_is_channel(peer) >= 0) {
            lv_label_set_text(t->note,
                              "Nothing on this channel yet. Anyone holding the same key can "
                              "read what you send, and nothing will acknowledge it.");
        } else if (m->messages_valid && !m->messages_persistent) {
            lv_label_set_text(t->note, "Nothing said yet, or nothing since the radio service "
                                       "last started: it keeps no history across a restart.");
        } else {
            lv_label_set_text(t->note, "Nothing said yet.");
        }
    } else {
        lv_label_set_text(t->note, "");
    }
    if (lv_label_get_text(t->note)[0]) {
        lv_obj_remove_flag(t->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(t->note, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Whether the thread now is the thread as built, moved along: the same ids
 * with some gone off the front and some added at the end. Returns how many
 * went off the front, or -1 when it is anything else - a message missing
 * from the middle, an older one arriving late, a window that got shorter. */
static int shifted_by(const struct rift_thread *t, const struct rift_message *const *thread,
                      int shown)
{
    int drop;
    int i;

    if (t->row_count != t->shape_count) {
        return -1;
    }
    if (t->shape_count == 0) {
        return 0;
    }
    if (shown == 0) {
        return -1;
    }
    for (drop = 0; drop < t->shape_count; drop++) {
        if (t->shape_id[drop] == thread[0]->id) {
            break;
        }
    }
    if (drop == t->shape_count) {
        /* Nothing in common: every old row goes, and that is a rebuild. */
        return -1;
    }
    if (t->shape_count - drop > shown) {
        return -1;
    }
    for (i = 0; i < t->shape_count - drop; i++) {
        if (t->shape_id[drop + i] != thread[i]->id) {
            return -1;
        }
    }
    return drop;
}

/* ---- the public entry points ---------------------------------------------- */

struct rift_thread *rift_thread_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_thread *t = calloc(1, sizeof(*t));

    if (!t) {
        return NULL;
    }
    t->app = app;

    t->root = lv_obj_create(parent);
    lv_obj_remove_style_all(t->root);
    lv_obj_set_size(t->root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(t->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(t->root, RIFT_PAD, 0);
    lv_obj_remove_flag(t->root, LV_OBJ_FLAG_SCROLLABLE);

    t->head = dense_row(t->root, RIFT_ROW_H);
    t->glyph = rift_glyph_create(t->head);
    t->who = rift_cell(t->head, POS_STYLE_ROW_TITLE, 0, LV_TEXT_ALIGN_LEFT);
    t->state = rift_cell(t->head, POS_STYLE_CAPTION, 0, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_flex_grow(t->state, 1);
    lv_obj_set_width(t->state, 1);
    t->earlier = rift_cell(t->head, POS_STYLE_CAPTION, 96, LV_TEXT_ALIGN_RIGHT);

    t->scroll = lv_obj_create(t->root);
    lv_obj_remove_style_all(t->scroll);
    lv_obj_set_width(t->scroll, LV_PCT(100));
    lv_obj_set_flex_grow(t->scroll, 1);
    lv_obj_set_flex_flow(t->scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(t->scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(t->scroll, LV_SCROLLBAR_MODE_AUTO);

    t->note = wrap_label(t->root, POS_STYLE_CAPTION);
    /* Not 0 and not 1: nothing has been said to the field yet, so the first
     * refresh sets it whichever way it goes. */
    t->field_enabled = -1;
    t->at_end = 1;
    build_composer(t);
    return t;
}

void rift_thread_destroy(struct rift_thread *t)
{
    /* Every object is a child of the pane the shell deletes; the private
     * block is this app's to release. */
    free(t);
}

lv_obj_t *rift_thread_root(struct rift_thread *t)
{
    return t ? t->root : NULL;
}

void rift_thread_shape(struct rift_thread *t, int wide)
{
    if (!t) {
        return;
    }
    if (wide) {
        lv_obj_add_flag(t->composer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(t->composer, LV_OBJ_FLAG_HIDDEN);
    }
    /* The landscape shape is the denser one (DS §37.2): the header at the
     * header-row height and the messages closer together. The rows built
     * so far take the new gap here; the ones built later take it as they
     * are built. */
    t->wide = wide;
    lv_obj_set_height(t->head, wide ? HEAD_H_WIDE : RIFT_ROW_H);
    for (int i = 0; i < t->row_count; i++) {
        lv_obj_set_style_pad_bottom(t->row[i].slot, wide ? MSG_GAP_WIDE : MSG_GAP, 0);
    }
    t->shape_valid = 0;
}

void rift_thread_refresh(struct rift_thread *t, const char *peer, const struct rift_conv *conv)
{
    const struct rift_message *thread[RIFT_THREAD_ROWS];
    struct rift_app *a;
    const struct rift_model *m;
    const char *refusal;
    int64_t now;
    int32_t removed_h = 0;
    int older = 0;
    int shown = 0;
    int same_peer;
    int changed;
    int at_end;
    int rebuilt = 0;
    int i;

    if (!t) {
        return;
    }
    a = t->app;
    m = &a->model;
    now = rift_app_now(a);
    if (peer) {
        shown = rift_model_thread(m, peer, thread, RIFT_THREAD_ROWS, &older);
    }

    /* Where the reader is now, before anything below moves it: at the end,
     * or back in the history. When the pane has changed height since the
     * last refresh the offset says nothing about that - the change moved it
     * - and what the last refresh saw is the answer instead. */
    {
        int32_t h0 = lv_obj_get_height(t->scroll);

        at_end = h0 != t->scroll_h ? t->at_end
                                   : lv_obj_get_scroll_bottom(t->scroll) <= RIFT_CAPTION_H;
    }
    same_peer = t->shape_valid && strcmp(peer ? peer : "", t->shape_peer) == 0;
    changed = !same_peer || shown != t->shape_count;
    for (i = 0; !changed && i < shown; i++) {
        if (thread[i]->id != t->shape_id[i]) {
            changed = 1;
        }
    }
    if (changed) {
        int drop = same_peer ? shifted_by(t, thread, shown) : -1;

        if (drop >= 0) {
            /* The same thread, moved on: the oldest `drop` went off the
             * front of the window and the rest are appended. Only those are
             * deleted and built - one of each for a message arriving in a
             * full window - and the rows in between keep their objects. */
            for (i = 0; i < drop; i++) {
                removed_h += lv_obj_get_height(t->row[i].slot);
                lv_obj_delete(t->row[i].slot);
            }
            memmove(&t->row[0], &t->row[drop], sizeof(t->row[0]) * (size_t)(t->row_count - drop));
            t->row_count -= drop;
            while (t->row_count < shown && t->row_count < RIFT_THREAD_ROWS) {
                build_row(t);
            }
        } else {
            /* Another conversation, or this one changed in the middle: built
             * again from nothing, and read from its end. */
            lv_obj_clean(t->scroll);
            t->row_count = 0;
            for (i = 0; i < shown && t->row_count < RIFT_THREAD_ROWS; i++) {
                build_row(t);
            }
            rebuilt = 1;
        }
        t->shape_valid = 1;
        t->shape_count = shown;
        snprintf(t->shape_peer, sizeof(t->shape_peer), "%s", peer ? peer : "");
        for (i = 0; i < shown; i++) {
            t->shape_id[i] = thread[i]->id;
        }
    }
    (void)conv;
    paint_note(t, peer, shown);
    /* Settle the pane before the rows are filled in and the thread scrolled
     * to its end: a row built in this pass has not been laid out, and
     * anything measured against an unsettled pane would depend on which
     * refresh this is. */
    lv_obj_update_layout(t->root);
    {
        int32_t max_w = lv_obj_get_content_width(t->scroll) - MSG_RULE_W - COL_GAP;

        for (i = 0; i < t->row_count && i < shown; i++) {
            update_row(t, &t->row[i], thread[i], now, max_w);
        }
    }
    {
        int32_t h = lv_obj_get_height(t->scroll);

        /* A thread is read at its end: when another one is opened, when a
         * message arrives while the reader is at the end, and when the pane
         * changes height under a reader who was at the end of it. A reader
         * who has scrolled back into the history is left there - a message
         * arriving no longer pulls them down to it; the rows that went off
         * the top are taken off the offset, so the lines they were reading
         * stay where they were. */
        if ((rebuilt || (changed && at_end) || (h != t->scroll_h && at_end)) && shown > 0) {
            lv_obj_update_layout(t->scroll);
            lv_obj_scroll_to_y(t->scroll, LV_COORD_MAX, LV_ANIM_OFF);
        } else if (changed && removed_h > 0) {
            int32_t y = lv_obj_get_scroll_y(t->scroll) - removed_h;

            lv_obj_update_layout(t->scroll);
            lv_obj_scroll_to_y(t->scroll, y > 0 ? y : 0, LV_ANIM_OFF);
        }
        t->scroll_h = h;
        /* Within a caption's height of the end counts as at the end. */
        t->at_end = lv_obj_get_scroll_bottom(t->scroll) <= RIFT_CAPTION_H;
    }

    /* The header: who, and how this peer is reached. The route is the
     * peer's, which is a fact the API does report - unlike a route for an
     * individual message, which it does not. */
    if (!peer) {
        lv_label_set_text(t->who, "");
        lv_label_set_text(t->state, "No conversation is open.");
        lv_label_set_text(t->earlier, "");
        rift_glyph_set(t->glyph, RIFT_GLYPH_UNKNOWN);
    } else {
        int slot = rift_key_is_channel(peer);
        const struct rift_node *n = slot >= 0 ? NULL : rift_model_find(m, peer);
        char label[RIFT_STATE_MAX];

        rift_comms_target_label(a, label, sizeof(label));
        lv_label_set_text(t->who, label);
        rift_glyph_set(t->glyph, slot >= 0 ? RIFT_GLYPH_CHANNEL : rift_app_glyph(n, now));
        if (slot >= 0) {
            const struct rift_channel *ch = rift_model_key_channel(m, peer);

            /* A channel has no route: a group frame is flooded to whoever
             * holds the key. The hash is what actually goes on the air, and
             * is the one fact here that identifies the channel to the mesh. */
            if (!ch) {
                /* Messages on a channel the service no longer holds. The
                 * messages happened; there is nowhere to write now. */
                lv_label_set_text(t->state, "LEFT" RIFT_SEP "NOT JOINED ANY MORE");
            } else if (ch->have_hash) {
                lv_label_set_text_fmt(t->state, "CHANNEL" RIFT_SEP "HASH %s" RIFT_SEP "FLOOD",
                                      ch->hash);
            } else {
                lv_label_set_text(t->state, "CHANNEL" RIFT_SEP "FLOOD");
            }
        } else if (n) {
            rift_fmt_state(n, label, sizeof(label));
            lv_label_set_text(t->state, label);
        } else {
            /* Messages from a peer the contact table no longer holds. The
             * messages are real; the route is not knowable. */
            lv_label_set_text(t->state, "NOT IN THE NODE LIST");
        }
        if (older > 0) {
            lv_label_set_text_fmt(t->earlier, "%d EARLIER", older);
        } else {
            lv_label_set_text(t->earlier, "");
        }
    }

    refusal = rift_thread_refusal(a);

    /* The composer: usable only when there is somewhere for a message to go
     * and a service to take it. */
    if (t->field) {
        int want = (refusal == NULL);

        /* Only on a change. This is called on every refresh, and every
         * refresh that re-enabled an already-enabled field stole the focus
         * from it (see field_enabled above): typing survived only until the
         * next repaint. */
        if (want != t->field_enabled) {
            t->field_enabled = want;
            pocketui_text_field_set_enabled(t->field, want != 0);
        }
    }
    if (t->send) {
        if (refusal) {
            pos_style_add(t->send, POS_STYLE_BUTTON_DISABLED, 0);
            lv_obj_remove_flag(t->send, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_remove_style(t->send, pos_style(POS_STYLE_BUTTON_DISABLED), 0);
            lv_obj_add_flag(t->send, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}
