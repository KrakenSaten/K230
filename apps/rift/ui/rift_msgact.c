/*
 * The actions on one message. See rift_msgact.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_msgact.h"

#include "app.h"
#include "pos_styles.h"
#include "rift_comms.h"
#include "rift_emoji.h"
#include "rift_emoji_style.h"
#include "rift_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACTIONS 4

struct rift_msgact {
    struct rift_app *app;
    lv_obj_t *bar;
    lv_obj_t *what;    /* the message the actions are for, on one line */
    lv_obj_t *row;
    lv_obj_t *button[ACTIONS];
    int shown[ACTIONS];
    int64_t id;        /* 0: closed */
    char conv[RIFT_KEY_HEX];
    int key_at;        /* the action the keys are on; -1 when opened by touch */
};

static const char *const words[ACTIONS] = { "REPLY", "COPY", "RESEND", "CLOSE" };

static const struct rift_message *message_of(const struct rift_msgact *b)
{
    return b->id ? rift_model_message(&b->app->model, b->id) : NULL;
}

/* The composer a conversation is written in now: the command line's field
 * in landscape, the thread's in portrait. */
static lv_obj_t *composer_of(struct rift_app *a)
{
    return a->wide ? a->composer : rift_comms_field(a);
}

/* Words into the composer, which then takes the keys. Nothing is sent: the
 * reader reads what is there, and SEND is still theirs to press. */
static void into_composer(struct rift_app *a, const char *text, int in_front)
{
    lv_obj_t *field = composer_of(a);

    if (!field || !text || !text[0]) {
        return;
    }
    if (in_front) {
        char whole[RIFT_REPLY_PREFIX_MAX + RIFT_MSG_TEXT_MAX];
        const char *was = lv_textarea_get_text(field);

        snprintf(whole, sizeof(whole), "%s%s", text, was ? was : "");
        lv_textarea_set_text(field, whole);
        lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST);
    } else {
        lv_textarea_add_text(field, text);
    }
    a->focus_composer_pending = 1;
    /* Portrait has no keyboard but the touch one, which a tap on the field
     * brings up; the words were put there without one, so it comes up here. */
    if (!a->wide && !pocketos_shell_keyboard_visible()) {
        pocketos_shell_keyboard_show(POCKETOS_KB_DONE, NULL, a);
    }
}

static void paint_keys(struct rift_msgact *b)
{
    int i;

    for (i = 0; i < ACTIONS; i++) {
        if (i == b->key_at) {
            pos_style_add(b->button[i], POS_STYLE_SELECTED, 0);
        } else {
            lv_obj_remove_style(b->button[i], pos_style(POS_STYLE_SELECTED), 0);
        }
    }
}

static void act(struct rift_msgact *b, int which)
{
    struct rift_app *a = b->app;
    const struct rift_message *msg = message_of(b);
    int64_t id = b->id;
    int by_key = b->key_at >= 0;

    if (!msg) {
        rift_msgact_close(b);
        return;
    }
    switch (which) {
    case RIFT_MSGACT_REPLY: {
        char prefix[RIFT_REPLY_PREFIX_MAX];

        if (rift_reply_prefix(msg, prefix, sizeof(prefix)) > 0) {
            into_composer(a, prefix, 1);
        }
        break;
    }
    case RIFT_MSGACT_COPY: {
        char words_of[RIFT_MSG_TEXT_MAX];

        /* What was said, without the "<sender>: " a channel payload carries
         * (rift_msg_body): the name is not part of the words. */
        snprintf(words_of, sizeof(words_of), "%s", rift_msg_body(msg));
        into_composer(a, words_of, 0);
        break;
    }
    case RIFT_MSGACT_RESEND:
        if (rift_model_can_resend(msg)) {
            rift_comms_resend(a, id);
        }
        break;
    default:
        break;
    }
    /* Opened by key, the keys go back where they came from: the composer. */
    if (by_key) {
        a->focus_composer_pending = 1;
    }
    rift_msgact_close(b);
    a->refresh_pending = 1;
}

static void on_action(lv_event_t *e)
{
    struct rift_msgact *b = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target_obj(e);
    int i;

    for (i = 0; i < ACTIONS; i++) {
        if (b->button[i] == target) {
            act(b, i);
            return;
        }
    }
}

struct rift_msgact *rift_msgact_create(struct rift_app *app, lv_obj_t *parent)
{
    struct rift_msgact *b = calloc(1, sizeof(*b));
    int i;

    if (!b) {
        return NULL;
    }
    b->app = app;
    b->key_at = -1;
    b->bar = lv_obj_create(parent);
    lv_obj_remove_style_all(b->bar);
    lv_obj_set_width(b->bar, LV_PCT(100));
    lv_obj_set_height(b->bar, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b->bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(b->bar, 4, 0);
    lv_obj_set_style_pad_row(b->bar, 4, 0);
    lv_obj_remove_flag(b->bar, LV_OBJ_FLAG_SCROLLABLE);
    rift_rule(b->bar);
    b->what = lv_label_create(b->bar);
    lv_obj_remove_style_all(b->what);
    pos_style_add(b->what, POS_STYLE_CAPTION, 0);
    /* The preview is folded for the colour emoji font (rift_fmt_preview):
     * drawn with it, as the thread's lines are, not as a box. */
    rift_emoji_style_add(b->what, POS_STYLE_CAPTION);
    lv_obj_set_width(b->what, LV_PCT(100));
    lv_label_set_long_mode(b->what, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(b->what, "");
    b->row = lv_obj_create(b->bar);
    lv_obj_remove_style_all(b->row);
    lv_obj_set_width(b->row, LV_PCT(100));
    lv_obj_set_height(b->row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b->row, 8, 0);
    lv_obj_remove_flag(b->row, LV_OBJ_FLAG_SCROLLABLE);
    for (i = 0; i < ACTIONS; i++) {
        b->button[i] = rift_action(b->row, words[i], i == RIFT_MSGACT_RESEND, 1, on_action, b);
    }
    lv_obj_add_flag(b->bar, LV_OBJ_FLAG_HIDDEN);
    return b;
}

void rift_msgact_destroy(struct rift_msgact *b)
{
    /* The objects are the thread's children and go with it. */
    free(b);
}

int64_t rift_msgact_id(const struct rift_msgact *b)
{
    return b ? b->id : 0;
}

int rift_msgact_by_key(const struct rift_msgact *b)
{
    return b && b->id && b->key_at >= 0;
}

lv_obj_t *rift_msgact_button(const struct rift_msgact *b, int which)
{
    if (!b || !b->id || which < 0 || which >= ACTIONS || !b->shown[which]) {
        return NULL;
    }
    return b->button[which];
}

/* The actions this message has, and the line saying which message it is. */
static void paint(struct rift_msgact *b, const struct rift_message *msg)
{
    char line[RIFT_PREVIEW_MAX + RIFT_MSG_STATE_MAX + 8];
    char preview[RIFT_PREVIEW_MAX];
    int i;

    b->shown[RIFT_MSGACT_REPLY] = rift_reply_possible(msg);
    b->shown[RIFT_MSGACT_COPY] = 1;
    b->shown[RIFT_MSGACT_RESEND] = rift_model_can_resend(msg);
    b->shown[RIFT_MSGACT_CLOSE] = 1;
    for (i = 0; i < ACTIONS; i++) {
        if (b->shown[i]) {
            lv_obj_remove_flag(b->button[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(b->button[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (b->key_at >= 0 && !b->shown[b->key_at]) {
        b->key_at = RIFT_MSGACT_COPY;
    }
    paint_keys(b);
    rift_fmt_preview(msg, preview, sizeof(preview));
    if (rift_model_can_resend(msg)) {
        char state[RIFT_MSG_STATE_MAX];

        rift_fmt_msg_state(msg, state, sizeof(state));
        snprintf(line, sizeof(line), "%s" RIFT_SEP "%s", state, preview);
    } else {
        snprintf(line, sizeof(line), "%s", preview);
    }
    rift_label_set(b->what, line);
}

void rift_msgact_open(struct rift_msgact *b, int64_t id, const char *conv, int by_key)
{
    const struct rift_message *msg;

    if (!b || !conv) {
        return;
    }
    msg = rift_model_message(&b->app->model, id);
    if (!msg || strcmp(msg->conv_key, conv) != 0) {
        return;
    }
    b->id = id;
    snprintf(b->conv, sizeof(b->conv), "%s", conv);
    /* The keys start on the action the message is most likely held for. */
    b->key_at = !by_key                    ? -1
                : rift_model_can_resend(msg) ? RIFT_MSGACT_RESEND
                : rift_reply_possible(msg)   ? RIFT_MSGACT_REPLY
                                             : RIFT_MSGACT_COPY;
    paint(b, msg);
    lv_obj_remove_flag(b->bar, LV_OBJ_FLAG_HIDDEN);
}

void rift_msgact_close(struct rift_msgact *b)
{
    if (!b) {
        return;
    }
    b->id = 0;
    b->key_at = -1;
    b->conv[0] = '\0';
    lv_obj_add_flag(b->bar, LV_OBJ_FLAG_HIDDEN);
}

void rift_msgact_refresh(struct rift_msgact *b, const char *conv)
{
    const struct rift_message *msg;

    if (!b || !b->id) {
        return;
    }
    msg = message_of(b);
    if (!msg || !conv || strcmp(conv, b->conv) != 0) {
        rift_msgact_close(b);
        return;
    }
    paint(b, msg);
}

int rift_msgact_key(struct rift_msgact *b, uint32_t key, const int64_t *ids, int count)
{
    int at = -1;
    int i;

    if (!b || !b->id) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (ids[i] == b->id) {
            at = i;
        }
    }
    if (b->key_at < 0) {
        b->key_at = RIFT_MSGACT_COPY;
    }
    switch (key) {
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        if (at >= 0) {
            int to = at + (key == LV_KEY_UP ? -1 : 1);

            if (to >= 0 && to < count) {
                char conv[RIFT_KEY_HEX];

                /* A copy: open writes b->conv from what it is given. */
                snprintf(conv, sizeof(conv), "%s", b->conv);
                rift_msgact_open(b, ids[to], conv, 1);
            }
        }
        b->app->refresh_pending = 1;
        return 1;
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
        for (i = 1; i <= ACTIONS; i++) {
            int to = (b->key_at + (key == LV_KEY_LEFT ? ACTIONS - i : i)) % ACTIONS;

            if (b->shown[to]) {
                b->key_at = to;
                break;
            }
        }
        paint_keys(b);
        return 1;
    case LV_KEY_ENTER:
        act(b, b->key_at);
        return 1;
    case LV_KEY_ESC:
        rift_msgact_close(b);
        b->app->focus_composer_pending = 1;
        b->app->refresh_pending = 1;
        return 1;
    default:
        return 1; /* the bar has the keys while it is open */
    }
}

const char *rift_msgact_quote(lv_obj_t *quote, const char *body, int *ident_on, uint32_t *ident)
{
    struct rift_reply reply;

    if (!rift_reply_parse(body, &reply)) {
        lv_obj_add_flag(quote, LV_OBJ_FLAG_HIDDEN);
        return body;
    }
    {
        /* "↳ Anna?: Are you coming up?" - the name as a claim, as every
         * channel name is shown, in the quoted person's identity accent. */
        char line[RIFT_REPLY_NAME_MAX + sizeof(reply.quote) + 16];
        char folded[sizeof(line)];
        uint32_t want = rift_ident_hash(reply.name);

        snprintf(line, sizeof(line), "\xE2\x86\xB3 %s?%s%s", reply.name,
                 reply.quote[0] ? ": " : "", reply.quote);
        rift_emoji_fold(line, folded, sizeof(folded));
        rift_label_set(quote, folded);
        lv_obj_remove_flag(quote, LV_OBJ_FLAG_HIDDEN);
        if (!*ident_on || *ident != want) {
            if (*ident_on) {
                lv_obj_remove_style(quote, pos_style_identity(*ident), 0);
            }
            lv_obj_add_style(quote, pos_style_identity(want), 0);
            *ident_on = 1;
            *ident = want;
        }
    }
    return reply.rest;
}
