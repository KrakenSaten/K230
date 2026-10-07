/*
 * RIFT's threads: one conversation's messages in the order a thread is read,
 * the name a conversation is shown under, and a message taken again to be
 * sent again (RESEND). Split out of rift_messages.c,
 * which holds the window they are read from, so neither file is everything;
 * the rules at the top of that file and of rift_model.h hold here too.
 *
 * No LVGL and no sockets: host-tested by tests/rift_model_test.c and
 * tests/rift_comms_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_model.h"

#include <stdio.h>
#include <string.h>

const char *rift_model_conv_name(const struct rift_model *m, const char *conv_key)
{
    const struct rift_node *n;
    const char *name = NULL;
    int slot;
    int i;

    if (!m || !conv_key || !conv_key[0]) {
        return NULL;
    }
    slot = rift_key_is_channel(conv_key);
    if (slot >= 0) {
        /* A channel's name is this node's own for it - it is never on the
         * air, so there is nobody else's to prefer. The list is the first
         * source because it is what mesh.channels last said; a message's
         * copy is the fallback for a channel that has since been left but
         * whose messages are still held. The list counts only while its slot
         * still holds this channel: once another channel has taken the slot,
         * its name is somebody else's. */
        const struct rift_channel *ch = rift_model_key_channel(m, conv_key);

        if (ch && ch->have_name && ch->name[0]) {
            return ch->name;
        }
        for (i = 0; i < m->msg_count; i++) {
            if (m->msg[i].have_channel_name && m->msg[i].channel_name[0] &&
                strcmp(m->msg[i].conv_key, conv_key) == 0) {
                name = m->msg[i].channel_name;
            }
        }
        return name;
    }
    /* The newest message that carried a name wins: it is what the service
     * called the peer most recently. */
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].have_peer_name && m->msg[i].peer_name[0] &&
            strcmp(m->msg[i].peer_key, conv_key) == 0) {
            name = m->msg[i].peer_name;
        }
    }
    if (name) {
        return name;
    }
    n = rift_model_find(m, conv_key);
    if (n && n->have_name && n->name[0]) {
        return n->name;
    }
    return NULL;
}

int rift_model_thread(const struct rift_model *m, const char *conv_key,
                      const struct rift_message **out, int max, int *older)
{
    int total = 0;
    int skip;
    int n = 0;
    int i;

    if (older) {
        *older = 0;
    }
    if (!m || !out || !conv_key || !conv_key[0] || max <= 0) {
        return 0;
    }
    for (i = 0; i < m->msg_count; i++) {
        if (strcmp(m->msg[i].conv_key, conv_key) == 0) {
            total++;
        }
    }
    /* A thread longer than the window is read from its end: the newest max,
     * still oldest first, and the number left off the front is reported so
     * the screen can say there are earlier ones rather than imply there are
     * not. */
    skip = total > max ? total - max : 0;
    if (older) {
        *older = skip;
    }
    for (i = 0; i < m->msg_count && n < max; i++) {
        if (strcmp(m->msg[i].conv_key, conv_key) != 0) {
            continue;
        }
        if (skip > 0) {
            skip--;
            continue;
        }
        out[n++] = &m->msg[i];
    }
    return n;
}

/* ---- sending again ---------------------------------------------------------- */

const struct rift_message *rift_model_message(const struct rift_model *m, int64_t id)
{
    int i;

    if (!m) {
        return NULL;
    }
    for (i = m->msg_count - 1; i >= 0; i--) {
        if (m->msg[i].id == id) {
            return &m->msg[i];
        }
    }
    return NULL;
}

int rift_model_can_resend(const struct rift_message *msg)
{
    if (!msg || msg->dir != RIFT_MSG_OUT || msg->is_channel || !msg->ack_expected) {
        return 0;
    }
    return msg->orphan || msg->state == RIFT_MSG_NO_ACK || msg->state == RIFT_MSG_FAILED;
}

int rift_model_resend_begin(struct rift_model *m, int64_t id, int64_t now_ms)
{
    const struct rift_message *msg = rift_model_message(m, id);

    if (!msg || !rift_model_can_resend(msg) || m->outbox.active) {
        return -1;
    }
    /* The same shape as a first send - written, then answered or refused -
     * so the thread says "Sending" and the composer waits the same way. The
     * text is the message's own, never the composer's: a RESEND sends what
     * was sent, and nothing a reader is typing now. */
    memset(&m->outbox, 0, sizeof(m->outbox));
    m->outbox.active = 1;
    snprintf(m->outbox.conv_key, sizeof(m->outbox.conv_key), "%s", msg->conv_key);
    snprintf(m->outbox.text, sizeof(m->outbox.text), "%s", msg->text);
    m->outbox.have_submitted = 1;
    m->outbox.submitted_mono_ms = now_ms;
    if (msg->orphan) {
        m->outbox.replaces_id = msg->id;
    } else {
        m->outbox.resend_id = msg->id;
    }
    return 0;
}
