/*
 * meshcored: the mesh.* IPC surface. See docs/api/mesh.md.
 *
 * Protocol-oriented, not screen-oriented. The methods name what a MeshCore
 * service knows - its identity, the nodes it has heard, the messages it has
 * exchanged, the state of its radio relationship - and say nothing about how
 * any of it is drawn. There is no RIFT vocabulary here, no layout, no
 * ordering for a list widget and no pagination shaped around a screen
 * height: a client asks for what it needs and decides all of that itself.
 *
 * Two rules the result shapes follow throughout:
 *
 *  - No internal structure escapes. Every value is a copy of a plain field,
 *    and a node is named by its public key, which is a fact about the node
 *    rather than an index into a table this service may reorder.
 *  - A value that is not known is absent. An RSSI nobody measured is not
 *    reported as 0, a hop count that was never learned is not reported as 0
 *    hops, and a radio state nobody has told us is not reported at all.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "mcd.h"

#include "mcd_util.h"
#include "radio_link.h"

#include "pocketlog/pocketlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shared shapes ------------------------------------------------------ */

static void add_key(cJSON *o, const char *field, const uint8_t *key, size_t len)
{
    char hex[MCD_PUB_KEY_LEN * 2 + 1];

    if (mcd_hex_encode(key, len, hex, sizeof(hex))) {
        cJSON_AddStringToObject(o, field, hex);
    }
}

static cJSON *node_json(const struct mcd_node *n)
{
    cJSON *o = cJSON_CreateObject();
    char hash[3];

    add_key(o, "public_key", n->public_key, MCD_PUB_KEY_LEN);
    /* The node hash is the first byte of the public key, which is what
     * MeshCore routes on. Given by name so a client does not have to know
     * that, and does not have to slice the string itself. */
    if (mcd_hex_encode(n->public_key, 1, hash, sizeof(hash))) {
        cJSON_AddStringToObject(o, "node_hash", hash);
    }
    cJSON_AddStringToObject(o, "name", n->name);
    cJSON_AddNumberToObject(o, "type", (double)n->type);
    cJSON_AddBoolToObject(o, "path_known", n->path_known);
    if (n->path_known) {
        char path_hex[MCD_MAX_PATH * 2 + 1];

        cJSON_AddNumberToObject(o, "hops", (double)n->path_hops);
        /* Direct means zero relays between here and there. */
        cJSON_AddBoolToObject(o, "direct", n->path_hops == 0);
        if (n->path_bytes > 0 &&
            mcd_hex_encode(n->path, n->path_bytes, path_hex, sizeof(path_hex))) {
            cJSON_AddStringToObject(o, "path_hex", path_hex);
        }
    }
    if (n->last_advert_timestamp != 0) {
        cJSON_AddNumberToObject(o, "last_advert_timestamp", (double)n->last_advert_timestamp);
    }
    if (n->last_heard_known) {
        cJSON_AddNumberToObject(o, "last_heard_mono_ms", (double)n->last_heard_mono_ms);
    }
    if (n->last_snr_known) {
        cJSON_AddNumberToObject(o, "last_snr_db", n->last_snr_db);
    }
    if (n->last_rssi_known) {
        cJSON_AddNumberToObject(o, "last_rssi_dbm", n->last_rssi_dbm);
    }
    return o;
}

static cJSON *message_json(const struct mcd_message *m)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "id", (double)m->id);
    cJSON_AddStringToObject(o, "direction", m->outgoing ? "out" : "in");
    add_key(o, "peer_public_key", m->peer_key, MCD_PUB_KEY_LEN);
    cJSON_AddStringToObject(o, "peer_name", m->peer_name);
    cJSON_AddStringToObject(o, "text", m->text);
    cJSON_AddNumberToObject(o, "timestamp", (double)m->timestamp);
    cJSON_AddNumberToObject(o, "mono_ms", (double)m->mono_ms);
    cJSON_AddStringToObject(o, "state", mcd_msg_state_name(m->state));
    if (m->ack_known) {
        cJSON_AddNumberToObject(o, "ack_mono_ms", (double)m->ack_mono_ms);
    }
    if (m->snr_known) {
        cJSON_AddNumberToObject(o, "snr_db", m->snr_db);
    }
    if (m->rssi_known) {
        cJSON_AddNumberToObject(o, "rssi_dbm", m->rssi_dbm);
    }
    return o;
}

static cJSON *profile_json(const struct mcd_profile *p)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "frequency_mhz", p->frequency_mhz);
    cJSON_AddNumberToObject(o, "bandwidth_khz", p->bandwidth_khz);
    cJSON_AddNumberToObject(o, "spreading_factor", p->spreading_factor);
    cJSON_AddNumberToObject(o, "coding_rate", p->coding_rate);
    cJSON_AddNumberToObject(o, "sync_word", p->sync_word);
    cJSON_AddNumberToObject(o, "preamble_length", p->preamble_length);
    cJSON_AddNumberToObject(o, "tx_power_dbm", p->tx_power_dbm);
    cJSON_AddBoolToObject(o, "crc", p->crc);
    return o;
}

/* ---- events ------------------------------------------------------------- */

cJSON *mcd_event_state(const struct mcd *d)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "state", mcd_state_name(d->state));
    cJSON_AddStringToObject(data, "reason", d->state_reason);
    cJSON_AddNumberToObject(data, "mono_ms", (double)mcd_mono_ms());
    return pocketipc_event("mesh.state", data);
}

cJSON *mcd_event_node(const struct mcd_node *n, const char *reason)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "reason", reason);
    cJSON_AddItemToObject(data, "node", node_json(n));
    return pocketipc_event("mesh.node", data);
}

cJSON *mcd_event_message(const struct mcd_message *m)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddItemToObject(data, "message", message_json(m));
    return pocketipc_event("mesh.message", data);
}

cJSON *mcd_event_activity_rx(const struct mcd_rx_meta *meta, int bytes, const char *outcome)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "kind", "rx");
    cJSON_AddStringToObject(data, "payload_type", outcome);
    if (bytes > 0) {
        cJSON_AddNumberToObject(data, "bytes", (double)bytes);
    }
    cJSON_AddNumberToObject(data, "mono_ms", (double)meta->mono_ms);
    if (meta->rssi_known) {
        cJSON_AddNumberToObject(data, "rssi_dbm", meta->rssi_dbm);
    }
    if (meta->snr_known) {
        cJSON_AddNumberToObject(data, "snr_db", meta->snr_db);
    }
    if (meta->freq_error_known) {
        cJSON_AddNumberToObject(data, "frequency_error_hz", meta->freq_error_hz);
    }
    return pocketipc_event("mesh.activity", data);
}

cJSON *mcd_event_activity_tx(uint64_t submit_id, int bytes, const char *result, uint64_t mono_ms)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "kind", "tx");
    cJSON_AddNumberToObject(data, "submit_id", (double)submit_id);
    cJSON_AddStringToObject(data, "result", result);
    if (bytes >= 0) {
        cJSON_AddNumberToObject(data, "bytes", (double)bytes);
    }
    cJSON_AddNumberToObject(data, "mono_ms", (double)mono_ms);
    return pocketipc_event("mesh.activity", data);
}

/* ---- methods ------------------------------------------------------------ */

static cJSON *m_info(struct mcd *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *proto = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "service", MCD_SERVICE_NAME);
    cJSON_AddNumberToObject(o, "api_version", MCD_API_VERSION);
    cJSON_AddStringToObject(o, "version", pocketlog_version());
    cJSON_AddStringToObject(o, "build", pocketlog_build_id());
    cJSON_AddStringToObject(o, "protocol", "meshcore");
    /* Which revision of the MeshCore sources this speaks. A service whose
     * whole purpose is another project's wire format has to be able to say
     * what it was built from; these are the commits protocols/meshcore pins
     * and tools/meshcore-frame was built from for the accepted P0 gate. */
    cJSON_AddStringToObject(proto, "rift_commit", mcd_runtime_rift_commit());
    cJSON_AddStringToObject(proto, "crypto_commit", mcd_runtime_crypto_commit());
    cJSON_AddItemToObject(o, "source", proto);
    cJSON_AddStringToObject(o, "radiod_socket", d->cfg.radiod_socket);
    cJSON_AddItemToObject(o, "requested_profile", profile_json(&d->cfg.profile));
    return o;
}

static cJSON *m_status(struct mcd *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *radio = cJSON_CreateObject();
    cJSON *counters = cJSON_CreateObject();
    struct mcd_runtime_stats st;
    uint64_t now = mcd_mono_ms();

    mcd_runtime_stats(d->rt, &st);

    cJSON_AddStringToObject(o, "state", mcd_state_name(d->state));
    cJSON_AddStringToObject(o, "reason", d->state_reason);
    cJSON_AddNumberToObject(o, "state_since_mono_ms", (double)d->state_since_ms);
    cJSON_AddNumberToObject(o, "uptime_s", (double)((now - d->start_ms) / 1000u));

    cJSON_AddBoolToObject(radio, "connected", mcd_link_connected(d->link));
    cJSON_AddBoolToObject(radio, "lease_held", mcd_link_lease_held(d->link));
    if (mcd_link_lease_held(d->link)) {
        cJSON_AddNumberToObject(radio, "lease_owner_id",
                                (double)mcd_link_lease_owner_id(d->link));
    }
    cJSON_AddBoolToObject(radio, "online", mcd_runtime_radio_online(d->rt));
    /* Absent until radiod has said so: a state nobody reported is not "off". */
    if (d->radio_state_known) {
        cJSON_AddStringToObject(radio, "radio_state", d->radio_state);
    }
    if (d->profile_applied) {
        cJSON_AddItemToObject(radio, "profile", profile_json(&d->applied));
    }
    cJSON_AddItemToObject(o, "radio", radio);

    cJSON_AddNumberToObject(counters, "rx_events", (double)d->counters.rx_events);
    cJSON_AddNumberToObject(counters, "rx_delivered", (double)d->counters.rx_delivered);
    cJSON_AddNumberToObject(counters, "rx_rejected", (double)d->counters.rx_rejected);
    cJSON_AddNumberToObject(counters, "rx_dropped", (double)d->counters.rx_dropped);
    cJSON_AddNumberToObject(counters, "tx_submitted", (double)d->counters.tx_submitted);
    cJSON_AddNumberToObject(counters, "tx_accepted", (double)d->counters.tx_accepted);
    cJSON_AddNumberToObject(counters, "tx_refused", (double)d->counters.tx_refused);
    cJSON_AddNumberToObject(counters, "tx_ok", (double)d->counters.tx_ok);
    cJSON_AddNumberToObject(counters, "tx_rx_resume_failed",
                            (double)d->counters.tx_rx_resume_failed);
    cJSON_AddNumberToObject(counters, "tx_failed", (double)d->counters.tx_failed);
    cJSON_AddNumberToObject(counters, "tx_unknown", (double)d->counters.tx_unknown);
    cJSON_AddNumberToObject(counters, "tx_done_unmatched",
                            (double)d->counters.tx_done_unmatched);
    cJSON_AddNumberToObject(counters, "radiod_connects", (double)d->counters.radiod_connects);
    cJSON_AddNumberToObject(counters, "radiod_disconnects",
                            (double)d->counters.radiod_disconnects);
    cJSON_AddNumberToObject(counters, "lease_acquired", (double)d->counters.lease_acquired);
    cJSON_AddNumberToObject(counters, "lease_refused", (double)d->counters.lease_refused);
    cJSON_AddNumberToObject(counters, "lease_lost", (double)d->counters.lease_lost);
    cJSON_AddNumberToObject(counters, "sent_flood", (double)st.sent_flood);
    cJSON_AddNumberToObject(counters, "sent_direct", (double)st.sent_direct);
    cJSON_AddNumberToObject(counters, "recv_flood", (double)st.recv_flood);
    cJSON_AddNumberToObject(counters, "recv_direct", (double)st.recv_direct);
    cJSON_AddNumberToObject(counters, "path_payloads_refused",
                            (double)st.path_payloads_refused);
    cJSON_AddItemToObject(o, "counters", counters);

    cJSON_AddNumberToObject(o, "nodes", (double)st.contacts);
    cJSON_AddNumberToObject(o, "messages", (double)mcd_runtime_message_count(d->rt));
    cJSON_AddNumberToObject(o, "packets_free", (double)st.packets_free);
    cJSON_AddNumberToObject(o, "packets_total", (double)st.packets_total);
    return o;
}

static cJSON *m_identity(struct mcd *d)
{
    cJSON *o = cJSON_CreateObject();
    uint8_t key[MCD_PUB_KEY_LEN];
    char name[MCD_NAME_MAX];
    char hash[3];

    mcd_runtime_identity(d->rt, key, name, sizeof(name));
    add_key(o, "public_key", key, sizeof(key));
    if (mcd_hex_encode(key, 1, hash, sizeof(hash))) {
        cJSON_AddStringToObject(o, "node_hash", hash);
    }
    cJSON_AddStringToObject(o, "name", name);
    /* The private half is never reported, by any method, at any verbosity. */
    return o;
}

static cJSON *m_nodes(struct mcd *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int n = mcd_runtime_node_count(d->rt);
    int i;

    for (i = 0; i < n; i++) {
        struct mcd_node node;

        if (mcd_runtime_node_at(d->rt, i, &node)) {
            cJSON_AddItemToArray(arr, node_json(&node));
        }
    }
    cJSON_AddItemToObject(o, "nodes", arr);
    cJSON_AddNumberToObject(o, "count", (double)n);
    return o;
}

/* Take a public-key prefix from a request. Returns the byte count, or -1
 * with a message written to err. */
static int params_key(const cJSON *params, const char *field, uint8_t *dst, size_t dst_len,
                      char *err, size_t errlen)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, field);
    int n;

    if (!cJSON_IsString(v) || v->valuestring == NULL) {
        snprintf(err, errlen, "%s must be a hex public key or prefix", field);
        return -1;
    }
    n = mcd_key_prefix_parse(v->valuestring, dst, dst_len);
    if (n <= 0) {
        snprintf(err, errlen,
                 "%s must be 2 to 64 hex characters, an even number of them", field);
        return -1;
    }
    return n;
}

static cJSON *m_node(struct mcd *d, const cJSON *params, int *code, char *err, size_t errlen)
{
    uint8_t prefix[MCD_PUB_KEY_LEN];
    struct mcd_node node;
    int n = params_key(params, "node", prefix, sizeof(prefix), err, errlen);
    int rc;

    if (n < 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        return NULL;
    }
    rc = mcd_runtime_node_by_prefix(d->rt, prefix, (size_t)n, &node);
    if (rc == 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "no node with that public key prefix");
        return NULL;
    }
    if (rc < 0) {
        /* Answering with one of them would be a guess about which node the
         * caller meant, and the answer is a key exchange partner. */
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "that prefix matches more than one node; give more of the key");
        return NULL;
    }
    return node_json(&node);
}

static cJSON *m_messages(struct mcd *d, const cJSON *params)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int total = mcd_runtime_message_count(d->rt);
    int limit = total;
    int start;
    int i;
    const cJSON *jl = cJSON_GetObjectItemCaseSensitive(params, "limit");

    if (cJSON_IsNumber(jl)) {
        double d = jl->valuedouble;
        /* Clamped, not cast: converting a double outside int's range is
         * undefined, and this one came from a client. */
        int v = (d >= 0.0 && d < (double)total) ? (int)d : total;

        if (v < limit) {
            limit = v;
        }
    }
    /* The newest `limit`, oldest first: a client that asks for ten wants the
     * last ten, and wants to read them in the order they happened. */
    start = total - limit;
    for (i = start; i < total; i++) {
        struct mcd_message m;

        if (mcd_runtime_message_at(d->rt, i, &m)) {
            cJSON_AddItemToArray(arr, message_json(&m));
        }
    }
    cJSON_AddItemToObject(o, "messages", arr);
    cJSON_AddNumberToObject(o, "count", (double)limit);
    cJSON_AddNumberToObject(o, "total", (double)total);
    /* Said plainly rather than left to be discovered: this list does not
     * survive a restart in this phase. */
    cJSON_AddBoolToObject(o, "persistent", false);
    return o;
}

static cJSON *m_send(struct mcd *d, const cJSON *params, int *code, char *err, size_t errlen)
{
    uint8_t prefix[MCD_PUB_KEY_LEN];
    const cJSON *jtext = cJSON_GetObjectItemCaseSensitive(params, "text");
    uint64_t msg_id = 0;
    uint32_t est = 0;
    enum mcd_send_result rc;
    int n = params_key(params, "to", prefix, sizeof(prefix), err, errlen);

    if (n < 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        return NULL;
    }
    if (!cJSON_IsString(jtext) || jtext->valuestring == NULL) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "text must be a string");
        return NULL;
    }
    if (!mcd_text_acceptable(jtext->valuestring, MCD_MAX_TEXT)) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen,
                 "text must be 1 to %d bytes and hold no control characters", MCD_MAX_TEXT);
        return NULL;
    }

    rc = mcd_runtime_send_text(d->rt, prefix, (size_t)n, jtext->valuestring, &msg_id, &est);
    switch (rc) {
    case MCD_SEND_ACCEPTED_FLOOD:
    case MCD_SEND_ACCEPTED_DIRECT: {
        cJSON *o = cJSON_CreateObject();

        cJSON_AddBoolToObject(o, "accepted", true);
        cJSON_AddNumberToObject(o, "message_id", (double)msg_id);
        cJSON_AddStringToObject(o, "route",
                                rc == MCD_SEND_ACCEPTED_DIRECT ? "direct" : "flood");
        cJSON_AddNumberToObject(o, "ack_timeout_ms", (double)est);
        /* Accepted by the protocol core, which is not the same as
         * transmitted: the frame is queued for the dispatcher, goes to
         * radiod as an asynchronous transmit, and its outcome arrives as a
         * mesh.activity event and in the message's own state. */
        return o;
    }
    case MCD_SEND_NO_RADIO:
        *code = POCKETIPC_ERR_BUSY;
        snprintf(err, errlen, "the radio is not available (service state %s)",
                 mcd_state_name(d->state));
        return NULL;
    case MCD_SEND_NO_CONTACT:
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "no single node matches that public key prefix");
        return NULL;
    case MCD_SEND_TOO_LONG:
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "the text does not fit a MeshCore message");
        return NULL;
    case MCD_SEND_FAILED:
    default:
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(err, errlen, "the MeshCore runtime could not build that message");
        return NULL;
    }
}

static cJSON *m_advert(struct mcd *d, int *code, char *err, size_t errlen)
{
    cJSON *o;

    if (!mcd_runtime_send_advert(d->rt)) {
        if (!mcd_runtime_radio_online(d->rt)) {
            *code = POCKETIPC_ERR_BUSY;
            snprintf(err, errlen, "the radio is not available (service state %s)",
                     mcd_state_name(d->state));
        } else {
            *code = POCKETIPC_ERR_BACKEND;
            snprintf(err, errlen, "the MeshCore runtime could not build an advert");
        }
        return NULL;
    }
    o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", true);
    return o;
}

/* ---- dispatch ----------------------------------------------------------- */

void mcd_handle_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req,
                        void *user)
{
    struct mcd *d = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    cJSON *result = NULL;
    int code = 0;
    char err[192] = "";
    const char *name;

    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                              "method must be a string"));
        return;
    }
    name = method->valuestring;
    /* params, when given, must be an object. A string or an array here would
     * otherwise be read field by field as an object with no fields, and a
     * caller's mistake would look like a caller's omission. */
    if (params != NULL && !cJSON_IsObject(params)) {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_INVALID_PARAMS,
                                                              "params must be an object"));
        return;
    }

    if (strcmp(name, "mesh.info") == 0) {
        result = m_info(d);
    } else if (strcmp(name, "mesh.status") == 0) {
        result = m_status(d);
    } else if (strcmp(name, "mesh.identity") == 0) {
        result = m_identity(d);
    } else if (strcmp(name, "mesh.nodes") == 0) {
        result = m_nodes(d);
    } else if (strcmp(name, "mesh.node") == 0) {
        result = m_node(d, params, &code, err, sizeof(err));
    } else if (strcmp(name, "mesh.messages") == 0) {
        result = m_messages(d, params);
    } else if (strcmp(name, "mesh.send") == 0) {
        result = m_send(d, params, &code, err, sizeof(err));
    } else if (strcmp(name, "mesh.advert") == 0) {
        result = m_advert(d, &code, err, sizeof(err));
    } else if (strcmp(name, "mesh.subscribe") == 0) {
        pocketipc_client_set_subscribed(c, true);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", true);
    } else if (strcmp(name, "mesh.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, false);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "subscribed", false);
    } else {
        pocketipc_server_reply(s, c, pocketipc_error_response(id, POCKETIPC_ERR_UNKNOWN_METHOD,
                                                              "unknown method"));
        return;
    }

    if (result) {
        pocketipc_server_reply(s, c, pocketipc_response(id, result));
    } else {
        pocketipc_server_reply(s, c,
                               pocketipc_error_response(id,
                                                        code ? code : POCKETIPC_ERR_BACKEND,
                                                        err[0] ? err : "failed"));
    }
}
