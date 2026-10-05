/*
 * The words under the open conversation: why the composer cannot send, what
 * became of the last send, and what an empty thread means. Split out of
 * rift_thread.c, which draws the messages, so neither file is everything.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_thread.h"

#include "pos_styles.h"
#include "rift_comms.h"

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
    if (rift_key_is_channel(rift_comms_open_peer(app)) < 0) {
        const char *why =
            rift_node_no_message_why(rift_model_find(m, rift_comms_open_peer(app)));

        if (why) {
            return why;
        }
    }
    if (rift_model_sending(m)) {
        return "One message is on its way.";
    }
    return NULL;
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
void rift_thread_note_paint(struct rift_app *app, lv_obj_t *note, const char *peer, int shown)
{
    const struct rift_model *m = &app->model;
    const char *refusal = rift_thread_refusal(app);

    lv_obj_remove_style(note, pos_style(POS_STYLE_STATUS_WARN_TEXT), 0);
    if (m->outbox.failed && m->outbox.error[0]) {
        /* "Not sent" only when the service said no: a submission whose
         * connection went before the answer may well have gone out. */
        lv_label_set_text_fmt(note, "%s: %s", m->outbox.unknown ? "No answer" : "Not sent",
                              m->outbox.error);
        pos_style_add(note, POS_STYLE_STATUS_WARN_TEXT, 0);
    } else if (rift_model_sending(m)) {
        lv_label_set_text(note, "Sending\xE2\x80\xA6");
    } else if (refusal) {
        lv_label_set_text(note, refusal);
    } else if (peer && shown == 0) {
        if (rift_key_is_channel(peer) >= 0) {
            lv_label_set_text(note,
                              "Nothing on this channel yet. Anyone holding the same key can "
                              "read what you send, and nothing will acknowledge it.");
        } else if (m->messages_valid && !m->messages_persistent) {
            lv_label_set_text(note, "Nothing said yet, or nothing since the radio service "
                                       "last started: it keeps no history across a restart.");
        } else {
            lv_label_set_text(note, "Nothing said yet.");
        }
    } else {
        lv_label_set_text(note, "");
    }
    if (lv_label_get_text(note)[0]) {
        lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN);
    }
}
