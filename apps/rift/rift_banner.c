/*
 * RIFT's message banners: a notice on whatever Doors is showing when a
 * direct message, or a message on a channel this node holds, has just
 * arrived (rift_app.h rift_app_banner_pass).
 *
 * What counts as "just arrived" is the model's answer, the same one the
 * sounds use (rift_arrivals.c): an incoming message from a live event, new
 * to this run - never history, a snapshot, a replay, a retransmission or
 * this device's own. This file only reads the two counters, on its own
 * marks, so a banner and a sound never take an arrival from each other.
 *
 * Unlike the sounds, a banner is raised with no screen of RIFT's up: that
 * is what it is for. It is not raised for the conversation the reader has
 * open in front of them, nor for a channel they muted. Several arrivals in
 * one pass are one banner for the newest direct message and one for the
 * newest channel message; the shell queues them (shell_notice.h).
 *
 * It sends nothing and changes no message: the unread mark is still the
 * thread's to clear.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_app.h"

#include "app.h"
#include "rift_emoji_style.h"

#include <stdio.h>
#include <string.h>

/* The conversation the last tapped banner was about. One is enough: a tap
 * is acted on at once (shell_notice.c), before another can be tapped. */
static char open_key[RIFT_KEY_HEX];
/* The keys of the banners raised lately, for their taps: a ring, so one
 * still waiting its turn keeps its own conversation. */
#define BANNER_KEYS 8
static char pending_key[BANNER_KEYS][RIFT_KEY_HEX];
static unsigned pending_at;

static void on_open(void *user)
{
    struct rift_app *a = rift_app_session();
    const char *key = user;

    if (!a || !key || !key[0]) {
        return; /* the session ended while the banner was up */
    }
    snprintf(open_key, sizeof(open_key), "%s", key);
    if (a->frame) {
        rift_app_open_conversation(a, open_key);
        return;
    }
    /* No screen: the next open is built over the session where the reader
     * left it (rift_app.c), so leave it at this conversation. */
    snprintf(a->conv, sizeof(a->conv), "%s", open_key);
    a->have_conv = 1;
    a->section = RIFT_SEC_COMMS;
    (void)pocketos_shell_open_app(RIFT_APP_ID);
}

/* The newest incoming message held for a conversation. */
static const struct rift_message *newest_in(const struct rift_model *m, const char *key)
{
    int i;

    for (i = m->msg_count - 1; i >= 0; i--) {
        if (m->msg[i].dir == RIFT_MSG_IN && strcmp(m->msg[i].conv_key, key) == 0) {
            return &m->msg[i];
        }
    }
    return NULL;
}

/* The reader is looking at this conversation: the message is on screen. */
static int in_view(const struct rift_app *a, const char *key)
{
    return a->frame && a->section == RIFT_SEC_COMMS && a->have_conv && strcmp(a->conv, key) == 0;
}

static void banner_for(struct rift_app *a, const char *key)
{
    const struct rift_model *m = &a->model;
    const struct rift_message *msg = newest_in(m, key);
    const char *name = rift_model_conv_name(m, key);
    struct pocketos_notice n;
    char source[64];
    char title[RIFT_NAME_MAX + 8];
    char body[RIFT_PREVIEW_MAX];
    char *p;

    if (!msg || in_view(a, key)) {
        return;
    }
    memset(&n, 0, sizeof(n));
    if (msg->is_channel) {
        /* The channel, then the name its sender claimed - nothing signs a
         * group frame, so it is a claim here as it is in the thread. */
        snprintf(source, sizeof(source), "RIFT" RIFT_SEP "%s",
                 name ? name : msg->have_channel_name ? msg->channel_name : "CHANNEL");
        snprintf(title, sizeof(title), "%s", msg->have_sender_name ? msg->sender_name : "Channel message");
        n.accent = (int)(rift_ident_hash(msg->have_sender_name ? msg->sender_name : key) & 0x7fff);
    } else {
        snprintf(source, sizeof(source), "RIFT" RIFT_SEP "DIRECT MESSAGE");
        if (name) {
            snprintf(title, sizeof(title), "%s", name);
        } else {
            rift_fmt_key_short(key, title, sizeof(title));
        }
        n.accent = (int)(rift_ident_hash(key) & 0x7fff);
    }
    /* The words, on one line: the two control characters the API lets
     * through become spaces, as in a conversation row's preview. */
    rift_utf8_ellipsis(body, sizeof(body), rift_msg_body(msg));
    for (p = body; *p; p++) {
        if (*p == '\n' || *p == '\t' || *p == '\r') {
            *p = ' ';
        }
    }
    pending_at = (pending_at + 1) % BANNER_KEYS;
    snprintf(pending_key[pending_at], sizeof(pending_key[pending_at]), "%s", key);
    n.app_id = RIFT_APP_ID;
    n.source = source;
    n.title = title;
    n.body = body;
    n.title_style = rift_emoji_style(POS_STYLE_ROW_TITLE);
    n.body_style = rift_emoji_style(POS_STYLE_TEXT_SECONDARY);
    n.on_open = on_open;
    n.user = pending_key[pending_at];
    pocketos_shell_notify(&n);
}

void rift_app_banner_start(struct rift_app *a)
{
    if (a) {
        a->banner_dm_seen = a->model.dm_arrivals;
        a->banner_ch_seen = a->model.ch_arrivals;
    }
}

void rift_app_banner_pass(struct rift_app *a)
{
    const struct rift_model *m;
    unsigned n;

    if (!a) {
        return;
    }
    m = &a->model;
    if (m->dm_arrivals != a->banner_dm_seen) {
        /* The counter only grows; a smaller one is a model this session was
         * not started on, and there is nothing to say. */
        if (m->dm_arrivals > a->banner_dm_seen && m->dm_last_key[0]) {
            banner_for(a, m->dm_last_key);
        }
        a->banner_dm_seen = m->dm_arrivals;
    }
    if (m->ch_arrivals != a->banner_ch_seen) {
        /* The newest arrival on a channel that is not muted. */
        for (n = m->ch_arrivals; n > a->banner_ch_seen; n--) {
            const char *conv = rift_model_ch_arrival_conv(m, n);

            if (conv && !rift_prefs_channel_muted(&a->prefs, conv)) {
                banner_for(a, conv);
                break;
            }
        }
        a->banner_ch_seen = m->ch_arrivals;
    }
}
