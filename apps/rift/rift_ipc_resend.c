/*
 * RIFT's meshcored client: RESEND - a message sent again (rift_ipc.h). Kept
 * apart from rift_ipc.c, which owns the connection and the one writer of
 * mesh.send (rift_ipc_write_send), so neither file is everything.
 *
 * Reached only from a message's RESEND action in COMMS (tests/rift_lint.sh).
 * The answer to a mesh.send, first send or RESEND, is read here too.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_ipc.h"

#include "rift_format.h"
#include "rift_json.h"

int rift_ipc_resend_message(struct rift_ipc *c, int64_t message_id)
{
    char why[RIFT_TEXT_MAX];
    int64_t now;

    if (!c || !c->model) {
        return -1;
    }
    if (c->fd < 0) {
        rift_model_send_refused(c->model, "meshcored is not answering; nothing was sent");
        c->revision++;
        return -1;
    }
    now = rift_mono_ms();
    if (rift_model_resend_begin(c->model, message_id, now) != 0) {
        /* One already in flight is left as it is (rift_model_send_refused). */
        rift_model_send_refused(c->model, "that message cannot be sent again");
        c->revision++;
        return -1;
    }
    /* An orphan goes as a new message, so its text is checked as one. */
    if (!c->model->outbox.resend_id &&
        rift_send_text_check(c->model->outbox.text, why, sizeof(why)) != 0) {
        rift_model_send_failed(c->model, why);
        c->revision++;
        return -1;
    }
    return rift_ipc_write_send(c, c->model->outbox.conv_key, c->model->outbox.text,
                               c->model->outbox.resend_id, now, NULL);
}

void rift_ipc_send_answered(struct rift_ipc *c, const cJSON *result)
{
    /* "accepted" is always true when there is a result at all
     * (docs/api/mesh.md); what matters here is the id, because that is
     * what the message itself will arrive under. The message is not
     * created from this reply: mesh.message carries it, and keying both
     * on the id is what keeps one message one row however they race. An id
     * that is not a whole one is not known (0), never cut to another. */
    int64_t id = 0;
    const cJSON *route = cJSON_IsObject(result)
                             ? cJSON_GetObjectItemCaseSensitive(result, "route")
                             : NULL;

    if (!cJSON_IsObject(result) || !msg_id_of(result, "message_id", &id)) {
        id = 0;
    }
    rift_model_send_accepted(c->model, id,
                             (cJSON_IsString(route) && route->valuestring) ? route->valuestring
                                                                          : NULL);
}
