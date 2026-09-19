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
 * It is a window, not a log. meshcored's own store does not survive its
 * restart, and this holds the newest RIFT_MAX_MESSAGES of whatever it has;
 * a conversation's unread count, its preview and its delivery tally are all
 * derived from what is still held and are honest about being bounded.
 *
 * No LVGL and no sockets: host-tested by tests/rift_model_test.c and
 * tests/rift_comms_test.c with no display and no service.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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
 * was taken, -1 when it was not a message this model will hold. */
int rift_model_apply_message(struct rift_model *m, const cJSON *o)
{
    const char *peer;
    const char *dir;
    const char *text;
    const char *state;
    const char *name;
    struct rift_message *msg;
    double d;
    int64_t id;
    int is_new;

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
    peer = str_of(o, "peer_public_key");
    dir = str_of(o, "direction");
    text = str_of(o, "text");
    if (!hex_only(peer, 64) || !dir || !text) {
        return -1;
    }
    if (strcmp(dir, "in") != 0 && strcmp(dir, "out") != 0) {
        return -1;
    }
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
    snprintf(msg->peer_key, sizeof(msg->peer_key), "%s", peer);
    name = str_of(o, "peer_name");
    if (name && name[0]) {
        rift_utf8_copy(msg->peer_name, sizeof(msg->peer_name), name);
        msg->have_peer_name = 1;
    }
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
    return 0;
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
        mk = mark_for(m, m->msg[i].peer_key, 1);
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
    if (seeding) {
        seed_read_marks(m);
        m->messages_seeded = 1;
    }
    m->stale = 0;
    return 0;
}

int rift_model_mark_read(struct rift_model *m, const char *peer_key)
{
    struct rift_read_mark *mk;
    int64_t newest = 0;
    int cleared;
    int i;

    if (!m || !peer_key || !peer_key[0]) {
        return 0;
    }
    cleared = rift_model_unread(m, peer_key);
    if (cleared == 0) {
        return 0;
    }
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].dir != RIFT_MSG_IN || strcmp(m->msg[i].peer_key, peer_key) != 0) {
            continue;
        }
        if (m->msg[i].id > newest) {
            newest = m->msg[i].id;
        }
    }
    if (newest == 0) {
        return 0;
    }
    mk = mark_for(m, peer_key, 1);
    if (!mk) {
        return 0;
    }
    mk->last_read_id = newest;
    return cleared;
}

int rift_model_unread(const struct rift_model *m, const char *peer_key)
{
    int64_t mark;
    int n = 0;
    int i;

    if (!m || !peer_key || !peer_key[0]) {
        return 0;
    }
    mark = read_id_of(m, peer_key);
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].dir == RIFT_MSG_IN && m->msg[i].id > mark &&
            strcmp(m->msg[i].peer_key, peer_key) == 0) {
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
        if (m->msg[i].id > read_id_of(m, m->msg[i].peer_key)) {
            n++;
        }
    }
    return n;
}

const char *rift_model_peer_name(const struct rift_model *m, const char *peer_key)
{
    const struct rift_node *n;
    const char *name = NULL;
    int i;

    if (!m || !peer_key || !peer_key[0]) {
        return NULL;
    }
    /* The newest message that carried a name wins: it is what the service
     * called the peer most recently. */
    for (i = 0; i < m->msg_count; i++) {
        if (m->msg[i].have_peer_name && m->msg[i].peer_name[0] &&
            strcmp(m->msg[i].peer_key, peer_key) == 0) {
            name = m->msg[i].peer_name;
        }
    }
    if (name) {
        return name;
    }
    n = rift_model_find(m, peer_key);
    if (n && n->have_name && n->name[0]) {
        return n->name;
    }
    return NULL;
}

/* ---- conversations -------------------------------------------------------- */

int rift_model_conversations(const struct rift_model *m, struct rift_conv *out, int max)
{
    int n = 0;
    int i;
    int j;

    if (!m || !out || max <= 0) {
        return 0;
    }
    for (i = 0; i < m->msg_count; i++) {
        const struct rift_message *msg = &m->msg[i];
        struct rift_conv *c = NULL;

        for (j = 0; j < n; j++) {
            if (strcmp(out[j].key, msg->peer_key) == 0) {
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
            snprintf(c->key, sizeof(c->key), "%s", msg->peer_key);
        }
        c->total++;
        if (msg->dir == RIFT_MSG_OUT) {
            c->outgoing++;
            if (msg->state == RIFT_MSG_ACKED) {
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
        }
    }
    for (i = 0; i < n; i++) {
        const char *name = rift_model_peer_name(m, out[i].key);

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

int rift_model_thread(const struct rift_model *m, const char *peer_key,
                      const struct rift_message **out, int max, int *older)
{
    int total = 0;
    int skip;
    int n = 0;
    int i;

    if (older) {
        *older = 0;
    }
    if (!m || !out || !peer_key || !peer_key[0] || max <= 0) {
        return 0;
    }
    for (i = 0; i < m->msg_count; i++) {
        if (strcmp(m->msg[i].peer_key, peer_key) == 0) {
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
        if (strcmp(m->msg[i].peer_key, peer_key) != 0) {
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

int rift_model_sending(const struct rift_model *m)
{
    return m && m->outbox.active;
}

int rift_model_send_begin(struct rift_model *m, const char *peer_key, const char *text,
                          int64_t now_ms)
{
    if (!m || !peer_key || !hex_only(peer_key, 64) || !text || !text[0]) {
        return -1;
    }
    if (m->outbox.active) {
        return -1;
    }
    memset(&m->outbox, 0, sizeof(m->outbox));
    m->outbox.active = 1;
    snprintf(m->outbox.peer_key, sizeof(m->outbox.peer_key), "%s", peer_key);
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
    snprintf(m->outbox.error, sizeof(m->outbox.error), "%s", error ? error : "refused");
}

void rift_model_send_clear(struct rift_model *m)
{
    if (!m) {
        return;
    }
    memset(&m->outbox, 0, sizeof(m->outbox));
}
