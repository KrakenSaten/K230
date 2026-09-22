/*
 * RIFT's words about messages and requests: the state of a message, the
 * caption under it, a conversation's preview line, what a reader may send,
 * and what became of an advert or a change to a node. The other half of
 * rift_format.c, under the same rules (rift_format.h): no LVGL, no cJSON,
 * no I/O, and nothing printed that the data does not support.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_format.h"

#include <stdio.h>
#include <string.h>

/* ---- messages ----------------------------------------------------------- */

void rift_fmt_msg_state(const struct rift_message *msg, char *out, size_t out_len)
{
    const char *word;

    if (!out || out_len == 0) {
        return;
    }
    if (!msg) {
        rift_utf8_copy(out, out_len, RIFT_UNKNOWN);
        return;
    }
    switch (msg->state) {
    case RIFT_MSG_SENDING:
        word = "SENDING";
        break;
    case RIFT_MSG_RECEIVED:
        word = "RECEIVED";
        break;
    case RIFT_MSG_SENT_FLOOD:
    case RIFT_MSG_SENT_DIRECT:
        word = "SENT";
        break;
    case RIFT_MSG_ACKED:
        /* The API calls it "acked"; to a reader an ACK is what delivery
         * means, and the handoff's own caption is DELIVERED. */
        word = "DELIVERED";
        break;
    case RIFT_MSG_NO_ACK:
        word = "NO ACK";
        break;
    case RIFT_MSG_FAILED:
        word = "FAILED";
        break;
    case RIFT_MSG_STATE_UNKNOWN:
    default:
        /* A state this build does not know: the service's own word, so a
         * v0 API that grows one is readable here before this app is
         * rebuilt for it. */
        if (msg->state_word[0]) {
            rift_utf8_copy(out, out_len, msg->state_word);
            return;
        }
        word = RIFT_UNKNOWN;
        break;
    }
    rift_utf8_copy(out, out_len, word);
}

int rift_msg_is_warn(const struct rift_message *msg)
{
    if (!msg) {
        return 0;
    }
    return msg->state == RIFT_MSG_NO_ACK || msg->state == RIFT_MSG_FAILED ||
           msg->state == RIFT_MSG_STATE_UNKNOWN;
}

void rift_fmt_ack(const struct rift_message *msg, char *out, size_t out_len)
{
    int64_t took;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg || !msg->have_ack_mono || !msg->have_mono) {
        return;
    }
    took = msg->ack_mono_ms - msg->mono_ms;
    if (took < 0) {
        /* An acknowledgement before the message it answers can only be a
         * fault in the stamps, and is not printed as a negative duration. */
        return;
    }
    if (took < 60000) {
        snprintf(out, out_len, "%d s", (int)((took + 500) / 1000));
        return;
    }
    snprintf(out, out_len, "%dm %02ds", (int)(took / 60000), (int)((took % 60000) / 1000));
}

void rift_fmt_msg_caption(const struct rift_message *msg, char *out, size_t out_len)
{
    char state[RIFT_MSG_STATE_MAX];
    char ack[RIFT_ACK_MAX];
    char rssi[RIFT_SIGNAL_MAX];
    char snr[RIFT_SIGNAL_MAX];
    size_t at;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg) {
        return;
    }
    rift_fmt_msg_state(msg, state, sizeof(state));
    at = (size_t)snprintf(out, out_len, "%s", state);
    if (at >= out_len) {
        return;
    }
    /* A message nothing can acknowledge.
     *
     * A MeshCore group frame is flooded and unacknowledged: there is no
     * expected_ack, no timeout and no delivery report, so an outgoing
     * channel message reaches sent_flood and stops there. The caption says
     * that outright. Leaving it at "SENT · FLOOD", the way a direct message
     * reads while it waits, would promise a second line that is never
     * coming, and a reader would learn to read a permanent "SENT" as a
     * failure. */
    if (msg->is_channel && msg->dir == RIFT_MSG_OUT && !msg->ack_expected) {
        snprintf(out + at, out_len - at, RIFT_SEP "FLOOD" RIFT_SEP "NO ACK ON CHANNELS");
        return;
    }
    if (msg->state == RIFT_MSG_ACKED) {
        rift_fmt_ack(msg, ack, sizeof(ack));
        if (ack[0]) {
            snprintf(out + at, out_len - at, RIFT_SEP "ACK %s", ack);
        }
        return;
    }
    if (msg->state == RIFT_MSG_SENT_FLOOD || msg->state == RIFT_MSG_SENT_DIRECT) {
        snprintf(out + at, out_len - at, RIFT_SEP "%s",
                 msg->state == RIFT_MSG_SENT_FLOOD ? "FLOOD" : "DIRECT");
        return;
    }
    /* What was measured for the frame this message arrived in, when the
     * service reported it. An outgoing message has nothing measured for it
     * here: what came back was an ACK, and its signal belongs to the ACK. */
    if (msg->dir != RIFT_MSG_IN) {
        return;
    }
    if (msg->have_rssi) {
        /* Labelled, unlike the NODES column of the same value: a caption has
         * no header above it to say what the number is. */
        rift_fmt_rssi(msg->rssi_dbm, 1, rssi, sizeof(rssi));
        at += (size_t)snprintf(out + at, out_len - at, RIFT_SEP "%s dBm", rssi);
        if (at >= out_len) {
            return;
        }
    }
    if (msg->have_snr) {
        rift_fmt_snr(msg->snr_db, 1, snr, sizeof(snr));
        snprintf(out + at, out_len - at, RIFT_SEP "SNR %s", snr);
    }
}

void rift_fmt_preview(const struct rift_message *msg, char *out, size_t out_len)
{
    char folded[RIFT_MSG_TEXT_MAX];
    size_t i;

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg) {
        return;
    }
    /* Newline and tab are the two control characters the API lets through
     * (docs/api/mesh.md, "Remote text"). A row is one line, so they become
     * spaces here rather than being left to the label to interpret. */
    {
        const char *body = rift_msg_body(msg);

        for (i = 0; i + 1 < sizeof(folded) && body[i]; i++) {
            char c = body[i];

            folded[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        }
        folded[i] = '\0';
    }
    {
        /* Built whole and then cut on a character boundary: a prefix put in
         * front of remote text must not push its last character in half. */
        char whole[RIFT_NAME_MAX + 4 + RIFT_MSG_TEXT_MAX];

        if (msg->dir == RIFT_MSG_OUT) {
            snprintf(whole, sizeof(whole), "you: %s", folded);
        } else if (msg->is_channel && msg->have_sender_name && msg->sender_name[0]) {
            /* The name a channel sender claimed, marked as a claim with the
             * same "?" the thread uses: nothing signs a group frame. */
            snprintf(whole, sizeof(whole), "%s?: %s", msg->sender_name, folded);
        } else {
            snprintf(whole, sizeof(whole), "%s", folded);
        }
        rift_utf8_copy(out, out_len, whole);
    }
}

const char *rift_msg_body(const struct rift_message *msg)
{
    size_t n;

    if (!msg) {
        return "";
    }
    /* A channel payload is "<sender>: <body>" - MeshCore writes the name
     * into it (docs/api/mesh.md) - and the thread already says who with the
     * claim marker. Printing the prefix again under it said the name twice,
     * and on this node's own messages put its name in front of every line
     * the reader wrote. The prefix is taken off only when it is exactly the
     * sender_name the service parsed and ": "; anything else is shown as
     * it came, because a guess at where a name ends is a guess at what was
     * said. */
    if (!msg->is_channel || !msg->have_sender_name || !msg->sender_name[0]) {
        return msg->text;
    }
    n = strlen(msg->sender_name);
    if (strncmp(msg->text, msg->sender_name, n) != 0 || msg->text[n] != ':' ||
        msg->text[n + 1] != ' ') {
        return msg->text;
    }
    return msg->text + n + 2;
}

void rift_fmt_msg_meta(const struct rift_message *msg, int64_t now_ms, char *out,
                       size_t out_len)
{
    char age[RIFT_AGE_MAX];
    char caption[RIFT_MSG_CAPTION_MAX];
    char who[RIFT_NAME_MAX + 2];

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!msg) {
        return;
    }
    /* An age, not a time of day: this board has no clock that survives a
     * power cut, and a message's own timestamp is the sender's clock. */
    rift_fmt_age(now_ms - msg->mono_ms, msg->have_mono, age, sizeof(age));
    rift_fmt_msg_caption(msg, caption, sizeof(caption));
    /* Who spoke is said here only where the thread does not already say it.
     * In a direct thread there are two parties: the header names the peer
     * and the side of the rule says which of the two a line is. On a channel
     * anyone holding the key may speak, so an incoming line names the
     * sender it claims to be - marked as a claim, because nothing signs a
     * group frame. */
    if (msg->is_channel && msg->dir == RIFT_MSG_IN) {
        if (msg->have_sender_name && msg->sender_name[0]) {
            snprintf(who, sizeof(who), "%s?", msg->sender_name);
        } else {
            snprintf(who, sizeof(who), "UNNAMED");
        }
        snprintf(out, out_len, "%s" RIFT_SEP "%s" RIFT_SEP "%s", age, who, caption);
        return;
    }
    snprintf(out, out_len, "%s" RIFT_SEP "%s", age, caption);
}

void rift_fmt_action(const struct rift_action_state *s, int64_t now_ms, char *out,
                     size_t out_len)
{
    const char *what;
    char age[RIFT_AGE_MAX];

    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!s || s->kind == RIFT_ACTION_NONE || (!s->active && !s->done && !s->failed)) {
        return;
    }
    switch (s->kind) {
    case RIFT_ACTION_ADVERT_NEAR:
        what = "ZERO-HOP ADVERT";
        break;
    case RIFT_ACTION_ADVERT_MESH:
        what = "FLOOD ADVERT";
        break;
    case RIFT_ACTION_FORGET:
        what = "FORGET";
        break;
    case RIFT_ACTION_RESET_PATH:
        what = "RE-ROUTE";
        break;
    case RIFT_ACTION_NONE:
    default:
        return;
    }
    if (s->active) {
        /* Asked, and not answered: nothing more is claimed. */
        snprintf(out, out_len, "%s" RIFT_SEP "ASKED" RIFT_ELLIPSIS, what);
        return;
    }
    if (s->failed) {
        /* Refused is not the same answer as no answer: a request whose
         * connection went before it was answered may well have happened,
         * and "NOT DONE" in front of "may or may not" would contradict it. */
        snprintf(out, out_len, "%s" RIFT_SEP "%s: %s", what,
                 s->unknown ? "NO ANSWER" : "NOT DONE", s->error);
        return;
    }
    rift_fmt_age(now_ms - s->mono_ms, s->have_mono, age, sizeof(age));
    switch (s->kind) {
    case RIFT_ACTION_ADVERT_NEAR:
    case RIFT_ACTION_ADVERT_MESH:
        /* Accepted, and said as accepted: the service queued it. Whether it
         * went out is the activity feed's to say, in the service's words. */
        snprintf(out, out_len, "%s" RIFT_SEP "ACCEPTED %s AGO", what, age);
        break;
    case RIFT_ACTION_FORGET:
        snprintf(out, out_len, "%s FORGOTTEN %s AGO" RIFT_SEP "BACK WHEN IT ADVERTS",
                 s->label[0] ? s->label : "NODE", age);
        break;
    case RIFT_ACTION_RESET_PATH:
    default:
        snprintf(out, out_len, "ROUTE FORGOTTEN %s AGO" RIFT_SEP "NEXT MESSAGE FLOODS", age);
        break;
    }
}

size_t rift_send_text_bytes(const char *text)
{
    return text ? strlen(text) : 0;
}

int rift_send_text_check(const char *text, char *why, size_t why_len)
{
    size_t len = rift_send_text_bytes(text);
    size_t i;

    if (why && why_len) {
        why[0] = '\0';
    }
    if (len == 0) {
        if (why) {
            rift_utf8_copy(why, why_len, "Nothing to send.");
        }
        return -1;
    }
    if (len > RIFT_SEND_TEXT_MAX) {
        if (why) {
            snprintf(why, why_len, "%u bytes; a message takes %d.", (unsigned)len,
                     RIFT_SEND_TEXT_MAX);
        }
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c == '\n' || c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7F) {
            /* meshcored refuses these rather than sanitising them on the
             * way out: a caller sending a control character has made a
             * mistake and is told so (docs/api/mesh.md). Same rule here, so
             * the reader is told before the request goes. */
            if (why) {
                rift_utf8_copy(why, why_len, "That text has a control character in it.");
            }
            return -1;
        }
    }
    return 0;
}
