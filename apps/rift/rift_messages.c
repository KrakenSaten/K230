/*
 * RIFT's messages: the conversations, how far each has been read, and the
 * one submission that may be in flight. The other half of the model is
 * rift_model.c, which holds the service and the nodes; both write the same
 * struct rift_model and both obey the rules at the top of rift_model.h.
 *
 * Two of those rules do most of the work here:
 *
 *   - A message is keyed by meshcored's own id. mesh.message is raised when
 *     a message arrives, when one is sent, and again every time its state
 *     changes (docs/api/mesh.md), so the same message reaches this file
 *     several times on purpose. Keying on the id is what makes the third
 *     arrival an update to one row rather than a third copy of it, and it
 *     is why a message with no id is refused outright: there would be
 *     nothing to match the next one against.
 *
 *   - Nothing here decides that a message was delivered. "accepted" is not
 *     transmitted and transmitted is not acknowledged; the state is the
 *     service's word and this file only ever copies it. The one state this
 *     app owns is SENDING, which says a request is in flight and claims
 *     nothing else.
 *
 * It also holds the answer to which run of meshcored the ids it is keyed on
 * came from, because it is the only thing that needs it: the service hands
 * them out from 1 again on every run.
 *
 * It is a window, not a log. meshcored's own store does not survive its
 * restart, and this holds the newest RIFT_MAX_MESSAGES of whatever it has;
 * a conversation's unread count, its preview and its delivery tally are all
 * derived from what is still held and are honest about being bounded. Nor
 * does the id space survive a restart, which is why the window is emptied
 * when the run changes rather than merged across it - forget_old_run below.
 *
 * No LVGL and no sockets: host-tested by tests/rift_model_test.c and
 * tests/rift_comms_test.c with no display and no service.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_model.h"

#include "rift_format.h"
#include "rift_json.h"

#include <stdio.h>
#include <string.h>

/* ---- messages ----------------------------------------------------------- */

static enum rift_msg_state msg_state_from_word(const char *w)
{
    if (!w) {
        return RIFT_MSG_STATE_UNKNOWN;
    }
    if (strcmp(w, "received") == 0) {
        return RIFT_MSG_RECEIVED;
    }
    if (strcmp(w, "sent_flood") == 0) {
        return RIFT_MSG_SENT_FLOOD;
    }
    if (strcmp(w, "sent_direct") == 0) {
        return RIFT_MSG_SENT_DIRECT;
    }
    if (strcmp(w, "acked") == 0) {
        return RIFT_MSG_ACKED;
    }
    if (strcmp(w, "no_ack") == 0) {
        return RIFT_MSG_NO_ACK;
    }
    if (strcmp(w, "failed") == 0) {
        return RIFT_MSG_FAILED;
    }
    /* A state word this build does not know is kept as a word and not
     * mapped onto the nearest one this build happens to have. */
    return RIFT_MSG_STATE_UNKNOWN;
}

/* Longer than any monotonic clock reports. An uptime past this is not a
 * large uptime, it is a number that would overflow the milliseconds it is
 * turned into below. */
#define RIFT_UPTIME_MAX_S (100LL * 365 * 24 * 3600)

/* Which run of meshcored answered this status, and whether it is the one
 * that answered the last. See "which run of the service this is" in
 * rift_model.h for why the message cache needs to know, and for why one
 * test is not enough. It is here rather than beside the rest of the status
 * parsing because the cache below is the only thing that needs the answer:
 * rift_model_apply_status reads the field, this decides what it means.
 *
 * A status that says nothing about the run - no uptime, or one no clock
 * could have reached - concludes nothing, in either direction: what is
 * already held about the run stays held. */
void rift_model_note_service_run(struct rift_model *m, const cJSON *result, int64_t now_ms)
{
    int64_t uptime;
    int64_t start;
    double d;

    if (!num_of(result, "uptime_s", &d) || d < 0 || d > (double)RIFT_UPTIME_MAX_S) {
        return;
    }
    uptime = (int64_t)d;
    start = now_ms - uptime * 1000;
    if (!m->have_svc_start) {
        m->have_svc_start = 1;
        m->have_uptime = 1;
        m->svc_start_ms = start;
        m->uptime_s = uptime;
        return;
    }
    if (uptime < m->uptime_s || start > m->svc_start_ms + RIFT_SVC_RESTART_SLACK_MS) {
        /* A different process, with a message id space that has started
         * again. Counted rather than acted on here: forget_old_run below
         * empties the cache, the next time a message is applied to it, so
         * that a status and the messages that follow it cannot disagree. */
        m->svc_restarts++;
        m->svc_start_ms = start;
    } else if (start < m->svc_start_ms) {
        /* Still the same run. The uptime is truncated to whole seconds, so
         * a derived start can only be too late; the lowest seen is the
         * least wrong, and holding it keeps the comparison above honest. */
        m->svc_start_ms = start;
    }
    m->uptime_s = uptime;
    m->have_uptime = 1;
}

/* The ids in this cache belong to one run of the service, and to no other.
 * meshcored hands them out from 1 on every run and keeps no messages across
 * one (mesh.messages, "persistent": false), so when the run changes the
 * cache is emptied rather than merged into. Merging is what unit A found on
 * 2026-09-21: a new id 1 landing on top of an old id 1, a new id 2 on an old
 * id 2, and every old id with no new counterpart orphaned - a history
 * blended from two sessions, with a message the service had forgotten still
 * on the panel. The node list has been replaced outright by every snapshot
 * from the start, for the same reason.
 *
 * The read marks go with the messages. A mark is a conversation's
 * last_read_id, so an id space that has started again would mark the new
 * 1..n as already read: unread counts wrong in the direction that hides a
 * message, which is the more visible half of the same fault.
 *
 * What is deliberately NOT reset is messages_seeded. Everything the new run
 * holds arrived while this app was not watching, which is the same case as
 * a snapshot after any reconnect, and is unread for the same reason.
 *
 * Nothing here decides that the run changed; rift_model_apply_status does,
 * and this reads the count it keeps. */
static void forget_old_run(struct rift_model *m)
{
    if (m->msg_generation == m->svc_restarts) {
        return;
    }
    m->msgs_forgotten += (unsigned)m->msg_count;
    m->msg_count = 0;
    memset(m->msg, 0, sizeof(m->msg));
    m->read_mark_count = 0;
    memset(m->read_mark, 0, sizeof(m->read_mark));
    /* The highest id seen is an id in the old space too. The fingerprints
     * are not: they are about what was said, which a restart does not
     * change (rift_model.h, dm_recent_fp). */
    m->dm_high_id = 0;
    /* Nothing has been read from this run yet, which is not the same as
     * this run holding nothing. The next snapshot says which. */
    m->messages_valid = 0;
    m->msg_generation = m->svc_restarts;
}

static struct rift_message *find_msg(struct rift_model *m, int64_t id)
{
    int i;

    for (i = m->msg_count - 1; i >= 0; i--) {
        if (m->msg[i].id == id) {
            return &m->msg[i];
        }
    }
    return NULL;
}

/* The slot for this id: the one already held, or a new one in id order.
 * NULL when the cache is full of newer messages, which is the one case
 * where a message is refused rather than made room for. */
static struct rift_message *msg_slot(struct rift_model *m, int64_t id, int *is_new)
{
    struct rift_message *held = find_msg(m, id);
    int at;
    int i;

    *is_new = 0;
    if (held) {
        return held;
    }
    if (m->msg_count >= RIFT_MAX_MESSAGES) {
        /* Full. The oldest goes, unless the newcomer is older than it: a
         * cache that evicted a newer message to hold an older one would
         * show a thread that walks backwards. */
        if (id <= m->msg[0].id) {
            m->msgs_dropped++;
            return NULL;
        }
        memmove(&m->msg[0], &m->msg[1], sizeof(m->msg[0]) * (size_t)(RIFT_MAX_MESSAGES - 1));
        m->msg_count--;
        m->msgs_dropped++;
    }
    /* Kept in id order, which is the order they happened: the service's ids
     * count upwards and are never reused while it runs. The common case is
     * an append, so this walks from the end. */
    for (at = m->msg_count; at > 0 && m->msg[at - 1].id > id; at--) {
        ;
    }
    for (i = m->msg_count; i > at; i--) {
        m->msg[i] = m->msg[i - 1];
    }
    memset(&m->msg[at], 0, sizeof(m->msg[at]));
    m->msg_count++;
    *is_new = 1;
    return &m->msg[at];
}

/* One message object, in the shape of docs/api/mesh.md. Returns 0 when it
 * was taken, -1 when it was not a message this model will hold. On 0, *out
 * is the message as filed and *fresh says whether its id was new to the
 * window. */
int rift_model_file_message(struct rift_model *m, const cJSON *o, struct rift_message **out,
                            int *fresh)
{
    const char *peer;
    const char *dir;
    const char *text;
    const char *state;
    const char *name;
    const char *kind;
    struct rift_message *msg;
    double d;
    int64_t id;
    int is_new;
    int is_channel;
    int slot = 0;

    if (!cJSON_IsObject(o)) {
        return -1;
    }
    if (!num_of(o, "id", &d) || d < 1) {
        /* The id is the identity a duplicate is matched on. Without one
         * there is no way to tell an update from a second copy, so the
         * message is refused rather than held as an unmatchable row. */
        return -1;
    }
    id = (int64_t)d;
    dir = str_of(o, "direction");
    text = str_of(o, "text");
    if (!dir || !text) {
        return -1;
    }
    if (strcmp(dir, "in") != 0 && strcmp(dir, "out") != 0) {
        return -1;
    }
    /* Which of the two kinds this is. The service says so outright
     * (docs/api/mesh.md, mesh.messages); anything that is not the word
     * "channel" is read as a direct message, which is also what a service
     * that predates the field produces. */
    kind = str_of(o, "kind");
    is_channel = kind && strcmp(kind, "channel") == 0;
    peer = str_of(o, "peer_public_key");
    if (is_channel) {
        /* A channel message has a slot and no peer. A slot this build
         * cannot hold is refused whole rather than shown in a conversation
         * a reader cannot open. */
        if (!num_of(o, "channel", &d) || d < 0 || d >= (double)RIFT_MAX_CHANNELS ||
            d != (double)(int)d) {
            return -1;
        }
        slot = (int)d;
    } else if (!hex_only(peer, 64)) {
        return -1;
    }
    /* After the message has been found acceptable and before it is filed:
     * a message that is refused must change nothing, and a message that is
     * kept must never be filed beside ids from an older run. */
    forget_old_run(m);
    msg = msg_slot(m, id, &is_new);
    if (!msg) {
        return -1;
    }
    if (!is_new) {
        m->msgs_duplicate++;
    }
    msg->id = id;
    msg->seq = ++m->msg_seq;
    msg->dir = (strcmp(dir, "out") == 0) ? RIFT_MSG_OUT : RIFT_MSG_IN;
    msg->is_channel = is_channel;
    if (is_channel) {
        msg->channel_slot = slot;
        name = str_of(o, "channel_name");
        if (name && name[0]) {
            rift_utf8_copy(msg->channel_name, sizeof(msg->channel_name), name);
            msg->have_channel_name = 1;
        }
        name = str_of(o, "channel_hash");
        if (hex_only(name, 2)) {
            snprintf(msg->channel_hash, sizeof(msg->channel_hash), "%s", name);
            msg->have_channel_hash = 1;
        }
        /* Filed under the channel it was on, not the slot it was in: the
         * slot may hold a different channel by the time anyone reads it. */
        rift_channel_key(slot, msg->have_channel_hash ? msg->channel_hash : NULL,
                         msg->have_channel_name ? msg->channel_name : NULL, msg->conv_key,
                         sizeof(msg->conv_key));
        /* The name the sender claimed inside the payload. Kept as a claim:
         * nothing signs a group frame, and no screen may present this the
         * way it presents a peer_name, which came with a public key. */
        name = str_of(o, "sender_name");
        if (name && name[0]) {
            rift_utf8_copy(msg->sender_name, sizeof(msg->sender_name), name);
            msg->have_sender_name = 1;
        }
    } else {
        snprintf(msg->peer_key, sizeof(msg->peer_key), "%s", peer);
        snprintf(msg->conv_key, sizeof(msg->conv_key), "%s", peer);
        name = str_of(o, "peer_name");
        if (name && name[0]) {
            rift_utf8_copy(msg->peer_name, sizeof(msg->peer_name), name);
            msg->have_peer_name = 1;
        }
    }
    /* Whether an acknowledgement can ever arrive. The service says so; the
     * fallback is what the protocol gives - an outgoing direct message is
     * acknowledged and nothing else is - so a service that did not say it
     * still cannot make this app draw a delivery that cannot happen. */
    msg->ack_expected = bool_of(o, "ack_expected",
                                !is_channel && msg->dir == RIFT_MSG_OUT);
    /* The body is remote text: meshcored has already made it well-formed
     * UTF-8 with no control characters but newline and tab (docs/api/mesh.md,
     * "Remote text"). This keeps that true when it is longer than the field
     * that holds it, by cutting on a character boundary. */
    rift_utf8_copy(msg->text, sizeof(msg->text), text);
    state = str_of(o, "state");
    msg->state = msg_state_from_word(state);
    snprintf(msg->state_word, sizeof(msg->state_word), "%s", state ? state : "");

    msg->have_timestamp = num_of(o, "timestamp", &d);
    if (msg->have_timestamp) {
        msg->timestamp = (int64_t)d;
    }
    msg->have_mono = num_of(o, "mono_ms", &d);
    if (msg->have_mono) {
        msg->mono_ms = (int64_t)d;
    }
    msg->have_ack_mono = num_of(o, "ack_mono_ms", &d);
    if (msg->have_ack_mono) {
        msg->ack_mono_ms = (int64_t)d;
    }
    msg->have_snr = num_of(o, "snr_db", &d);
    if (msg->have_snr) {
        msg->snr_db = d;
    }
    msg->have_rssi = num_of(o, "rssi_dbm", &d);
    if (msg->have_rssi) {
        msg->rssi_dbm = d;
    }
    m->msgs_applied++;
    if (out) {
        *out = msg;
    }
    if (fresh) {
        *fresh = is_new;
    }
    return 0;
}

int rift_model_apply_message(struct rift_model *m, const cJSON *o)
{
    return rift_model_file_message(m, o, NULL, NULL);
}

/* ---- how far a conversation has been read -------------------------------- */

static struct rift_read_mark *mark_for(struct rift_model *m, const char *key, int create)
{
    int oldest = 0;
    int i;

    for (i = 0; i < m->read_mark_count; i++) {
        if (strcmp(m->read_mark[i].key, key) == 0) {
            return &m->read_mark[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (m->read_mark_count < RIFT_MAX_CONVERSATIONS) {
        struct rift_read_mark *mk = &m->read_mark[m->read_mark_count++];

        memset(mk, 0, sizeof(*mk));
        snprintf(mk->key, sizeof(mk->key), "%s", key);
        return mk;
    }
    /* Full. The conversation read longest ago gives up its mark and goes
     * back to being wholly unread, which overstates rather than hides. */
    for (i = 1; i < m->read_mark_count; i++) {
        if (m->read_mark[i].last_read_id < m->read_mark[oldest].last_read_id) {
            oldest = i;
        }
    }
    memset(&m->read_mark[oldest], 0, sizeof(m->read_mark[oldest]));
    snprintf(m->read_mark[oldest].key, sizeof(m->read_mark[oldest].key), "%s", key);
    return &m->read_mark[oldest];
}

static int64_t read_id_of(const struct rift_model *m, const char *key)
{
    int i;

    for (i = 0; i < m->read_mark_count; i++) {
        if (strcmp(m->read_mark[i].key, key) == 0) {
            return m->read_mark[i].last_read_id;
        }
    }
    return 0;
}

/* Every conversation is read up to the newest message it holds. Used for
 * the first snapshot of a session, which is history rather than news. */
static void seed_read_marks(struct rift_model *m)
{
    int i;

    for (i = 0; i < m->msg_count; i++) {
        struct rift_read_mark *mk;

        if (m->msg[i].dir != RIFT_MSG_IN) {
            continue;
        }
        mk = mark_for(m, m->msg[i].conv_key, 1);
        if (mk && m->msg[i].id > mk->last_read_id) {
            mk->last_read_id = m->msg[i].id;
        }
    }
}

int rift_model_apply_messages(struct rift_model *m, const cJSON *result)
{
    const cJSON *arr;
    const cJSON *it;
    double d;
    int seeding;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(result, "messages");
    if (!cJSON_IsArray(arr)) {
        return -1;
    }
    /* Here as well as in apply_message, because a snapshot that is empty -
     * a service that has just restarted and heard nothing yet - would
     * otherwise leave the whole of the previous run's cache standing. */
    forget_old_run(m);
    seeding = !m->messages_seeded;
    cJSON_ArrayForEach (it, arr) {
        /* One bad entry inside an otherwise good snapshot is refused on its
         * own and counted; the rest of the snapshot is still the truth. */
        if (rift_model_apply_message(m, it) != 0) {
            m->events_malformed++;
        }
    }
    m->have_messages_reported = num_of(result, "total", &d);
    if (m->have_messages_reported) {
        m->messages_reported = (int)d;
    }
    m->messages_persistent = bool_of(result, "persistent", 0);
    m->messages_valid = 1;
    /* A snapshot is history, however recent: nothing in it is an arrival,
     * and nothing at or below its newest incoming id will be one either
     * when the same message comes round again as an event. */
    {
        int i;

        for (i = 0; i < m->msg_count; i++) {
            if (m->msg[i].dir == RIFT_MSG_IN && !m->msg[i].is_channel &&
                m->msg[i].id > m->dm_high_id) {
                m->dm_high_id = m->msg[i].id;
            }
        }
    }
    if (seeding) {
        seed_read_marks(m);
        m->messages_seeded = 1;
    }
    m->stale = 0;
    return 0;
}

int rift_model_mark_read(struct rift_model *m, const char *conv_key)
{
    struct rift_read_mark *mk;
    int64_t newest = 0;
    int cleared;
    int i;

    if (!m || !conv_key || !conv_key[0]) {
        return 0;
    }
    cleared = rift_model_unread(m, conv_key);
    if (cleared == 0) {
        return 0;
    }
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].dir != RIFT_MSG_IN || strcmp(m->msg[i].conv_key, conv_key) != 0) {
            continue;
        }
        if (m->msg[i].id > newest) {
            newest = m->msg[i].id;
        }
    }
    if (newest == 0) {
        return 0;
    }
    mk = mark_for(m, conv_key, 1);
    if (!mk) {
        return 0;
    }
    mk->last_read_id = newest;
    return cleared;
}

int rift_model_unread(const struct rift_model *m, const char *conv_key)
{
    int64_t mark;
    int n = 0;
    int i;

    if (!m || !conv_key || !conv_key[0]) {
        return 0;
    }
    mark = read_id_of(m, conv_key);
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].dir == RIFT_MSG_IN && m->msg[i].id > mark &&
            strcmp(m->msg[i].conv_key, conv_key) == 0) {
            n++;
        }
    }
    return n;
}

int rift_model_unread_total(const struct rift_model *m)
{
    int n = 0;
    int i;

    if (!m) {
        return 0;
    }
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].dir != RIFT_MSG_IN) {
            continue;
        }
        if (m->msg[i].id > read_id_of(m, m->msg[i].conv_key)) {
            n++;
        }
    }
    return n;
}

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

/* ---- conversations -------------------------------------------------------- */

/* Which conversations make the list when there are more than it holds: the
 * ones whose newest message is newest. The window can hold messages from
 * more peers than a list has rows (RIFT_MAX_MESSAGES against max), and
 * filling the list in the order messages are held - oldest first - kept
 * the conversations nobody had spoken in for longest and left out the ones
 * that had just spoken. keep[i] is set for every message whose conversation
 * is in. */
static void newest_conversations(const struct rift_model *m, int max, char *keep)
{
    const char *key[RIFT_MAX_MESSAGES];
    int distinct = 0;
    int i;
    int j;

    /* Newest first, so a key is first met at its newest message, and the
     * first max keys met are the max newest conversations. */
    for (i = m->msg_count - 1; i >= 0; i--) {
        for (j = 0; j < distinct; j++) {
            if (strcmp(key[j], m->msg[i].conv_key) == 0) {
                break;
            }
        }
        if (j == distinct) {
            key[distinct++] = m->msg[i].conv_key;
        }
        keep[i] = j < max;
    }
}

int rift_model_conversations(const struct rift_model *m, struct rift_conv *out, int max)
{
    char keep[RIFT_MAX_MESSAGES];
    int n = 0;
    int i;
    int j;

    if (!m || !out || max <= 0) {
        return 0;
    }
    newest_conversations(m, max, keep);
    for (i = 0; i < m->msg_count; i++) {
        const struct rift_message *msg = &m->msg[i];
        struct rift_conv *c = NULL;

        if (!keep[i]) {
            continue;
        }
        for (j = 0; j < n; j++) {
            if (strcmp(out[j].key, msg->conv_key) == 0) {
                c = &out[j];
                break;
            }
        }
        if (!c) {
            if (n >= max) {
                continue;
            }
            c = &out[n++];
            memset(c, 0, sizeof(*c));
            snprintf(c->key, sizeof(c->key), "%s", msg->conv_key);
            c->is_channel = msg->is_channel;
            c->channel_slot = msg->channel_slot;
        }
        c->total++;
        if (msg->dir == RIFT_MSG_OUT) {
            c->outgoing++;
            if (!msg->ack_expected) {
                /* Nothing will ever acknowledge this one, so it is counted
                 * apart rather than in with the messages that could have
                 * been delivered and were not. A tally that put a channel
                 * message in no_ack would be reporting a failure the
                 * protocol never promised to avoid. */
                c->unacknowledgeable++;
            } else if (msg->state == RIFT_MSG_ACKED) {
                c->acked++;
            } else if (msg->state == RIFT_MSG_NO_ACK) {
                c->no_ack++;
            } else if (msg->state == RIFT_MSG_FAILED) {
                c->failed++;
            }
        }
        /* The messages are held in id order, so the last one seen for a
         * peer is its newest and is the preview. */
        c->newest = msg;
        if (msg->have_mono) {
            c->have_newest_mono = 1;
            c->newest_mono_ms = msg->mono_ms;
            if (msg->dir == RIFT_MSG_IN) {
                c->have_last_in_mono = 1;
                c->last_in_mono_ms = msg->mono_ms;
            }
        }
    }
    for (i = 0; i < n; i++) {
        const char *name = rift_model_conv_name(m, out[i].key);

        if (name) {
            rift_utf8_copy(out[i].name, sizeof(out[i].name), name);
            out[i].have_name = 1;
        }
        out[i].unread = rift_model_unread(m, out[i].key);
    }
    /* Newest conversation first. A conversation whose messages carry no
     * monotonic stamp sorts on the newest id it holds, so the order is
     * total either way and does not flicker between refreshes. */
    for (i = 1; i < n; i++) {
        struct rift_conv tmp = out[i];

        for (j = i; j > 0; j--) {
            const struct rift_conv *a = &tmp;
            const struct rift_conv *b = &out[j - 1];
            int newer;

            if (a->have_newest_mono && b->have_newest_mono &&
                a->newest_mono_ms != b->newest_mono_ms) {
                newer = a->newest_mono_ms > b->newest_mono_ms;
            } else if (a->newest && b->newest && a->newest->id != b->newest->id) {
                newer = a->newest->id > b->newest->id;
            } else {
                newer = strcmp(a->key, b->key) < 0;
            }
            if (!newer) {
                break;
            }
            out[j] = out[j - 1];
        }
        out[j] = tmp;
    }
    return n;
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

/* ---- sending -------------------------------------------------------------- */

int rift_model_text_limit(const struct rift_model *m, const char *conv_key)
{
    int slot;

    if (!m || !conv_key || !conv_key[0]) {
        return 0;
    }
    slot = rift_key_is_channel(conv_key);
    if (slot < 0) {
        /* A direct message: mesh.send takes 1 to 160 bytes, which is a
         * constant of the API rather than something the service reports. */
        return RIFT_SEND_TEXT_MAX;
    }
    {
        const struct rift_channel *ch = rift_model_key_channel(m, conv_key);

        /* A channel's limit is shorter, and by how much depends on this
         * node's own name, which is why it comes from the service rather
         * than being worked out here. Until mesh.channels has said, 0 means
         * "not known" and the composer does not enforce a number it guessed. */
        if (ch && ch->have_text_limit) {
            return ch->text_limit;
        }
    }
    return 0;
}

int rift_model_sending(const struct rift_model *m)
{
    return m && m->outbox.active;
}

int rift_model_send_begin(struct rift_model *m, const char *conv_key, const char *text,
                          int64_t now_ms)
{
    int limit;

    if (!m || !conv_key || !text || !text[0]) {
        return -1;
    }
    /* Either a peer's public key or a channel this build can name. Anything
     * else is not a destination, and a request built from it would be a
     * request meshcored refuses after the fact. */
    if (rift_key_is_channel(conv_key) < 0 && !hex_only(conv_key, 64)) {
        return -1;
    }
    /* A channel is written to only while the key still names the channel
     * that is joined. mesh.send addresses a slot, and a slot that has been
     * emptied and taken by another channel would carry a reply written in
     * the old conversation to the new channel's audience. */
    if (rift_key_is_channel(conv_key) >= 0 && !rift_model_key_channel(m, conv_key)) {
        return -1;
    }
    if (m->outbox.active) {
        return -1;
    }
    limit = rift_model_text_limit(m, conv_key);
    if (limit > 0 && (int)strlen(text) > limit) {
        return -1;
    }
    memset(&m->outbox, 0, sizeof(m->outbox));
    m->outbox.active = 1;
    snprintf(m->outbox.conv_key, sizeof(m->outbox.conv_key), "%s", conv_key);
    rift_utf8_copy(m->outbox.text, sizeof(m->outbox.text), text);
    m->outbox.have_submitted = 1;
    m->outbox.submitted_mono_ms = now_ms;
    return 0;
}

void rift_model_send_accepted(struct rift_model *m, int64_t message_id, const char *route)
{
    if (!m || !m->outbox.active) {
        return;
    }
    m->outbox.message_id = message_id;
    if (route) {
        snprintf(m->outbox.route, sizeof(m->outbox.route), "%s", route);
    }
    /* The submission is done with. The message itself is meshcored's now:
     * it arrives as mesh.message keyed by this id, and its state is the
     * service's to report. Nothing here marks it sent. */
    m->outbox.active = 0;
}

void rift_model_send_failed(struct rift_model *m, const char *error)
{
    if (!m) {
        return;
    }
    m->outbox.active = 0;
    m->outbox.failed = 1;
    m->outbox.unknown = 0;
    snprintf(m->outbox.error, sizeof(m->outbox.error), "%s", error ? error : "refused");
}

void rift_model_send_clear(struct rift_model *m)
{
    if (!m) {
        return;
    }
    memset(&m->outbox, 0, sizeof(m->outbox));
}
