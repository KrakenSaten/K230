/*
 * RIFT's meshcored client: RESEND - a message sent again (rift_ipc.h). Kept
 * apart from rift_ipc.c, which owns the connection and the one writer of
 * mesh.send (rift_ipc_write_send), so neither file is everything.
 *
 * Reached only from a message's RESEND action in COMMS (tests/rift_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_ipc.h"

#include "rift_format.h"

int rift_ipc_resend_message(struct rift_ipc *c, int64_t message_id)
{
    char why[RIFT_TEXT_MAX];
    int64_t now;

    if (!c || !c->model) {
        return -1;
    }
    if (c->fd < 0) {
        rift_model_send_failed(c->model, "meshcored is not answering; nothing was sent");
        c->revision++;
        return -1;
    }
    now = rift_mono_ms();
    if (rift_model_resend_begin(c->model, message_id, now) != 0) {
        rift_model_send_failed(c->model, rift_model_sending(c->model)
                                             ? "one message is already on its way"
                                             : "that message cannot be sent again");
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
                               c->model->outbox.resend_id, now);
}
