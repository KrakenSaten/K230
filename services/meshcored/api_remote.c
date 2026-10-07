/*
 * meshcored: repeater discovery and the repeater session, on the mesh.* IPC
 * surface. See docs/api/mesh.md, "Repeater control", and mesh_runtime.h for
 * the protocol each method speaks.
 *
 * The same two rules as api.c: no internal structure escapes (a node is its
 * public key), and a value that is not known is absent. Every string a
 * repeater chose - an owner text, a CLI answer, a name - goes through the
 * remote-text sanitiser on its way out.
 *
 * A password passes through here and nowhere else in the service. It is
 * handed to the runtime, which copies and wipes it, and the request's own
 * copy is overwritten before this returns. It is never logged, stored or
 * echoed in an answer.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "mcd.h"

#include "mcd_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void add_key_hex(cJSON *o, const char *field, const uint8_t *key, size_t len)
{
    char hex[MCD_PUB_KEY_LEN * 2 + 1];

    if (mcd_hex_encode(key, len, hex, sizeof(hex))) {
        cJSON_AddStringToObject(o, field, hex);
    }
}

static void add_text(cJSON *o, const char *field, const char *text)
{
    char safe[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];

    mcd_text_sanitize(text, safe, sizeof(safe));
    cJSON_AddStringToObject(o, field, safe);
}

/* The node's name and type when this service holds it as a contact; absent
 * otherwise. A discovered repeater whose advert has not been heard is a key
 * and nothing more. */
static void add_contact(struct mcd *d, cJSON *o, const uint8_t *key, size_t len)
{
    struct mcd_node n;

    if (mcd_runtime_node_by_prefix(d->rt, key, len, &n) == 1) {
        if (n.name[0]) {
            add_text(o, "name", n.name);
        }
        cJSON_AddNumberToObject(o, "type", (double)n.type);
        cJSON_AddBoolToObject(o, "known", true);
    } else {
        cJSON_AddBoolToObject(o, "known", false);
    }
}

/* ---- discovery ---------------------------------------------------------- */

static void add_round(cJSON *o, const struct mcd_discover_state *s)
{
    cJSON_AddNumberToObject(o, "round", (double)s->round);
    cJSON_AddBoolToObject(o, "open", s->open);
    cJSON_AddNumberToObject(o, "window_ms", (double)MCD_DISCOVER_WINDOW_MS);
    if (s->round > 0) {
        cJSON_AddNumberToObject(o, "started_mono_ms", (double)s->started_mono_ms);
        cJSON_AddNumberToObject(o, "until_mono_ms", (double)s->until_mono_ms);
    }
}

static cJSON *discovered_json(struct mcd *d, const struct mcd_discovered *r,
                              const struct mcd_discover_state *s)
{
    cJSON *o = cJSON_CreateObject();
    char hash[3];

    add_key_hex(o, "public_key", r->public_key, MCD_PUB_KEY_LEN);
    if (mcd_hex_encode(r->public_key, 1, hash, sizeof(hash))) {
        cJSON_AddStringToObject(o, "node_hash", hash);
    }
    add_contact(d, o, r->public_key, MCD_PUB_KEY_LEN);
    cJSON_AddNumberToObject(o, "round", (double)r->round);
    /* Answered the latest round: heard directly as of that round. An answer
     * to an earlier one is kept and said to be older, never passed off as
     * current. */
    cJSON_AddBoolToObject(o, "current", r->round == s->round);
    cJSON_AddNumberToObject(o, "mono_ms", (double)r->mono_ms);
    cJSON_AddNumberToObject(o, "their_snr_db", r->their_snr_db);
    if (r->snr_known) {
        cJSON_AddNumberToObject(o, "snr_db", r->snr_db);
    }
    if (r->rssi_known) {
        cJSON_AddNumberToObject(o, "rssi_dbm", r->rssi_dbm);
    }
    return o;
}

cJSON *mcd_event_discover(struct mcd *d, const struct mcd_discovered *r,
                          const struct mcd_discover_state *s)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "reason", r ? "reply" : "closed");
    add_round(data, s);
    if (r) {
        cJSON_AddItemToObject(data, "repeater", discovered_json(d, r, s));
    }
    return pocketipc_event("mesh.discover", data);
}

static cJSON *m_discover(struct mcd *d, int *code, char *err, size_t errlen)
{
    struct mcd_discover_state s;
    enum mcd_discover_result rc = mcd_runtime_discover(d->rt, &s);
    cJSON *o;

    switch (rc) {
    case MCD_DISCOVER_STARTED:
    case MCD_DISCOVER_BUSY:
        /* A round already open is an answer, not an error: it is collecting
         * the replies the caller wants, and nothing was sent a second time. */
        o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "started", rc == MCD_DISCOVER_STARTED);
        add_round(o, &s);
        return o;
    case MCD_DISCOVER_NO_RADIO:
        *code = POCKETIPC_ERR_BUSY;
        snprintf(err, errlen, "the radio is not available (service state %s)",
                 mcd_state_name(d->state));
        return NULL;
    case MCD_DISCOVER_FAILED:
    default:
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(err, errlen, "the MeshCore runtime had no packet free for the request");
        return NULL;
    }
}

static cJSON *m_discovered(struct mcd *d)
{
    struct mcd_discovered list[MCD_DISCOVER_MAX];
    struct mcd_discover_state s;
    int n = mcd_runtime_discovered(d->rt, list, MCD_DISCOVER_MAX);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int i;

    mcd_runtime_discover_state(d->rt, &s);
    add_round(o, &s);
    for (i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, discovered_json(d, &list[i], &s));
    }
    cJSON_AddItemToObject(o, "repeaters", arr);
    cJSON_AddNumberToObject(o, "count", (double)n);
    return o;
}

/* ---- the session -------------------------------------------------------- */

static cJSON *session_json(struct mcd *d, const struct mcd_remote_session *s)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddBoolToObject(o, "active", s->active);
    if (!s->active) {
        return o;
    }
    add_key_hex(o, "node", s->key, MCD_PUB_KEY_LEN);
    add_contact(d, o, s->key, MCD_PUB_KEY_LEN);
    cJSON_AddStringToObject(o, "login", mcd_login_state_name(s->login));
    if (s->login == MCD_LOGIN_OK) {
        /* The legacy "OK" carries no permissions: then neither is reported,
         * rather than reported as guest. */
        cJSON_AddBoolToObject(o, "legacy", s->legacy);
        if (!s->legacy) {
            cJSON_AddBoolToObject(o, "admin", s->admin);
            cJSON_AddNumberToObject(o, "permissions", (double)s->permissions);
            cJSON_AddNumberToObject(o, "acl", (double)s->acl);
            cJSON_AddNumberToObject(o, "firmware_level", (double)s->fw_level);
        }
    }
    if (s->server_clock_known) {
        cJSON_AddNumberToObject(o, "repeater_clock", (double)s->server_clock);
    }
    if (s->login_mono_ms) {
        cJSON_AddNumberToObject(o, "login_mono_ms", (double)s->login_mono_ms);
    }
    if (s->pending != MCD_REMOTE_NONE) {
        cJSON *p = cJSON_AddObjectToObject(o, "pending");

        cJSON_AddStringToObject(p, "kind", mcd_remote_kind_name(s->pending));
        cJSON_AddNumberToObject(p, "request_id", (double)s->pending_id);
        cJSON_AddNumberToObject(p, "deadline_mono_ms", (double)s->deadline_mono_ms);
    }
    cJSON_AddNumberToObject(o, "stale_replies", (double)s->stale_replies);
    cJSON_AddNumberToObject(o, "malformed_replies", (double)s->malformed_replies);
    return o;
}

static cJSON *stats_json(const struct mcd_repeater_stats *st)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "battery_mv", (double)st->batt_milli_volts);
    cJSON_AddNumberToObject(o, "tx_queue", (double)st->tx_queue_len);
    cJSON_AddNumberToObject(o, "noise_floor_dbm", (double)st->noise_floor);
    cJSON_AddNumberToObject(o, "last_rssi_dbm", (double)st->last_rssi);
    cJSON_AddNumberToObject(o, "last_snr_db", (double)st->last_snr_x4 / 4.0);
    cJSON_AddNumberToObject(o, "packets_recv", (double)st->packets_recv);
    cJSON_AddNumberToObject(o, "packets_sent", (double)st->packets_sent);
    cJSON_AddNumberToObject(o, "air_time_s", (double)st->air_time_secs);
    cJSON_AddNumberToObject(o, "uptime_s", (double)st->up_time_secs);
    cJSON_AddNumberToObject(o, "sent_flood", (double)st->sent_flood);
    cJSON_AddNumberToObject(o, "sent_direct", (double)st->sent_direct);
    cJSON_AddNumberToObject(o, "recv_flood", (double)st->recv_flood);
    cJSON_AddNumberToObject(o, "recv_direct", (double)st->recv_direct);
    cJSON_AddNumberToObject(o, "err_events", (double)st->err_events);
    /* The two later tiers only when the reply reached them: an older
     * repeater does not send them, and a 0 would be a count nobody made. */
    if (st->have_dups) {
        cJSON_AddNumberToObject(o, "direct_dups", (double)st->direct_dups);
        cJSON_AddNumberToObject(o, "flood_dups", (double)st->flood_dups);
    }
    if (st->have_rx_air) {
        cJSON_AddNumberToObject(o, "rx_air_time_s", (double)st->rx_air_time_secs);
        cJSON_AddNumberToObject(o, "recv_errors", (double)st->recv_errors);
    }
    return o;
}

static cJSON *neighbours_json(struct mcd *d, const struct mcd_remote_reply *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int i;

    cJSON_AddNumberToObject(o, "total", (double)r->neighbours_total);
    for (i = 0; i < r->neighbour_count; i++) {
        const struct mcd_neighbour *nb = &r->neighbours[i];
        cJSON *e = cJSON_CreateObject();

        add_key_hex(e, "prefix", nb->prefix, MCD_REMOTE_NEIGHBOUR_PREFIX);
        /* Named only when the prefix names exactly one node this service
         * holds: the repeater sent six bytes of a key, not a name. */
        add_contact(d, e, nb->prefix, MCD_REMOTE_NEIGHBOUR_PREFIX);
        cJSON_AddNumberToObject(e, "heard_s_ago", (double)nb->heard_secs_ago);
        cJSON_AddNumberToObject(e, "snr_db", (double)nb->snr_x4 / 4.0);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(o, "entries", arr);
    return o;
}

/* GET_OWNER_INFO's "<firmware>\n<name>\n<owner>", split where it splits. */
static cJSON *owner_json(const char *text)
{
    cJSON *o = cJSON_CreateObject();
    char buf[MCD_MAX_TEXT + 1];
    char *name;
    char *owner = NULL;

    snprintf(buf, sizeof(buf), "%s", text);
    name = strchr(buf, '\n');
    if (name) {
        *name++ = '\0';
        owner = strchr(name, '\n');
        if (owner) {
            *owner++ = '\0';
        }
    }
    add_text(o, "firmware", buf);
    if (name) {
        add_text(o, "name", name);
    }
    if (owner) {
        add_text(o, "owner", owner);
    }
    return o;
}

static cJSON *reply_json(struct mcd *d, const struct mcd_remote_reply *r)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddNumberToObject(o, "request_id", (double)r->request_id);
    cJSON_AddStringToObject(o, "kind", mcd_remote_kind_name(r->kind));
    cJSON_AddStringToObject(o, "outcome", mcd_remote_outcome_name(r->outcome));
    add_key_hex(o, "node", r->key, MCD_PUB_KEY_LEN);
    cJSON_AddNumberToObject(o, "mono_ms", (double)r->mono_ms);
    if (r->late) {
        cJSON_AddBoolToObject(o, "late", true);
    }
    if (r->snr_known) {
        cJSON_AddNumberToObject(o, "snr_db", r->snr_db);
    }
    if (r->rssi_known) {
        cJSON_AddNumberToObject(o, "rssi_dbm", r->rssi_dbm);
    }
    if (r->outcome != MCD_REMOTE_REPLIED) {
        return o;
    }
    switch (r->kind) {
    case MCD_REMOTE_STATUS:
        cJSON_AddItemToObject(o, "status", stats_json(&r->stats));
        break;
    case MCD_REMOTE_NEIGHBOURS:
        cJSON_AddItemToObject(o, "neighbours", neighbours_json(d, r));
        break;
    case MCD_REMOTE_OWNER:
        cJSON_AddItemToObject(o, "owner", owner_json(r->text));
        break;
    case MCD_REMOTE_CLI:
        add_text(o, "text", r->text);
        break;
    default:
        break;
    }
    return o;
}

cJSON *mcd_event_remote(struct mcd *d, const struct mcd_remote_reply *r,
                        const struct mcd_remote_session *s)
{
    cJSON *data = cJSON_CreateObject();

    cJSON_AddItemToObject(data, "reply", reply_json(d, r));
    cJSON_AddItemToObject(data, "session", session_json(d, s));
    return pocketipc_event("mesh.remote", data);
}

static int params_node(const cJSON *params, uint8_t key[MCD_PUB_KEY_LEN], char *err,
                       size_t errlen)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "node");

    if (!cJSON_IsString(v) || v->valuestring == NULL ||
        strlen(v->valuestring) != MCD_PUB_KEY_LEN * 2 ||
        mcd_key_prefix_parse(v->valuestring, key, MCD_PUB_KEY_LEN) != MCD_PUB_KEY_LEN) {
        snprintf(err, errlen, "node must be a whole public key, 64 hex characters");
        return -1;
    }
    return 0;
}

/* The answer to an accepted request, or the refusal in words. */
static cJSON *answer(struct mcd *d, enum mcd_remote_result rc, uint64_t id, uint32_t wait,
                     int *code, char *err, size_t errlen)
{
    cJSON *o;

    switch (rc) {
    case MCD_REMOTE_ACCEPTED_FLOOD:
    case MCD_REMOTE_ACCEPTED_DIRECT:
        o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "accepted", true);
        cJSON_AddNumberToObject(o, "request_id", (double)id);
        cJSON_AddStringToObject(o, "route",
                                rc == MCD_REMOTE_ACCEPTED_DIRECT ? "direct" : "flood");
        cJSON_AddNumberToObject(o, "wait_ms", (double)wait);
        return o;
    case MCD_REMOTE_BUSY:
        *code = POCKETIPC_ERR_BUSY;
        snprintf(err, errlen, "a repeater request is already waiting for its answer");
        return NULL;
    case MCD_REMOTE_NO_RADIO:
        *code = POCKETIPC_ERR_BUSY;
        snprintf(err, errlen, "the radio is not available (service state %s)",
                 mcd_state_name(d->state));
        return NULL;
    case MCD_REMOTE_NO_CONTACT:
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "no node with that public key is held; its advert has not been "
                              "heard");
        return NULL;
    case MCD_REMOTE_NOT_LOGGED_IN:
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "not logged in to that node");
        return NULL;
    case MCD_REMOTE_NOT_ADMIN:
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "logged in as a guest; the repeater answers commands only for "
                              "admin");
        return NULL;
    case MCD_REMOTE_BAD_TEXT:
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "that is not one line of 1 to %d printable bytes", MCD_MAX_TEXT);
        return NULL;
    case MCD_REMOTE_FAILED:
    default:
        *code = POCKETIPC_ERR_BACKEND;
        snprintf(err, errlen, "the MeshCore runtime could not build the request");
        return NULL;
    }
}

/* The request's own copy of a password, overwritten in place before the
 * request is freed: whatever pocketipc does with the frame afterwards, the
 * string this service parsed out of it no longer holds the password. */
static void wipe_string(cJSON *s)
{
    volatile char *p;

    if (!cJSON_IsString(s) || s->valuestring == NULL) {
        return;
    }
    for (p = s->valuestring; *p; p++) {
        *p = '\0';
    }
}

static cJSON *m_login(struct mcd *d, cJSON *params, int *code, char *err, size_t errlen)
{
    cJSON *jpw = cJSON_GetObjectItemCaseSensitive(params, "password");
    uint8_t key[MCD_PUB_KEY_LEN];
    enum mcd_remote_result rc;
    uint64_t id = 0;
    uint32_t wait = 0;

    if (params_node(params, key, err, errlen) != 0) {
        wipe_string(jpw);
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        return NULL;
    }
    if (!cJSON_IsString(jpw) || jpw->valuestring == NULL) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "password must be a string (empty to be admitted by the "
                              "repeater's access list)");
        return NULL;
    }
    if (strlen(jpw->valuestring) > MCD_REMOTE_PASSWORD_MAX) {
        rc = MCD_REMOTE_BAD_TEXT;
    } else {
        rc = mcd_runtime_remote_login(d->rt, key, jpw->valuestring, &id, &wait);
    }
    wipe_string(jpw);
    if (rc == MCD_REMOTE_BAD_TEXT) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "password must be at most %d printable bytes on one line",
                 MCD_REMOTE_PASSWORD_MAX);
        return NULL;
    }
    return answer(d, rc, id, wait, code, err, errlen);
}

static cJSON *m_request(struct mcd *d, const cJSON *params, int *code, char *err, size_t errlen)
{
    const cJSON *jkind = cJSON_GetObjectItemCaseSensitive(params, "kind");
    uint8_t key[MCD_PUB_KEY_LEN];
    enum mcd_remote_kind kind;
    enum mcd_remote_result rc;
    uint64_t id = 0;
    uint32_t wait = 0;

    if (params_node(params, key, err, errlen) != 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        return NULL;
    }
    if (!cJSON_IsString(jkind) || jkind->valuestring == NULL) {
        kind = MCD_REMOTE_NONE;
    } else if (strcmp(jkind->valuestring, "status") == 0) {
        kind = MCD_REMOTE_STATUS;
    } else if (strcmp(jkind->valuestring, "neighbours") == 0) {
        kind = MCD_REMOTE_NEIGHBOURS;
    } else if (strcmp(jkind->valuestring, "owner") == 0) {
        kind = MCD_REMOTE_OWNER;
    } else {
        kind = MCD_REMOTE_NONE;
    }
    if (kind == MCD_REMOTE_NONE) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "kind must be \"status\", \"neighbours\" or \"owner\"");
        return NULL;
    }
    rc = mcd_runtime_remote_ask(d->rt, key, kind, &id, &wait);
    return answer(d, rc, id, wait, code, err, errlen);
}

static cJSON *m_cli(struct mcd *d, const cJSON *params, int *code, char *err, size_t errlen)
{
    const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(params, "command");
    uint8_t key[MCD_PUB_KEY_LEN];
    enum mcd_remote_result rc;
    uint64_t id = 0;
    uint32_t wait = 0;

    if (params_node(params, key, err, errlen) != 0) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        return NULL;
    }
    if (!cJSON_IsString(jcmd) || jcmd->valuestring == NULL) {
        *code = POCKETIPC_ERR_INVALID_PARAMS;
        snprintf(err, errlen, "command must be a string");
        return NULL;
    }
    rc = mcd_runtime_remote_cli(d->rt, key, jcmd->valuestring, &id, &wait);
    return answer(d, rc, id, wait, code, err, errlen);
}

static cJSON *m_logout(struct mcd *d, const cJSON *params, int *code, char *err, size_t errlen)
{
    const cJSON *jnode = cJSON_GetObjectItemCaseSensitive(params, "node");
    struct mcd_remote_session s;
    uint8_t key[MCD_PUB_KEY_LEN];
    bool ended;
    cJSON *o;

    /* Without a node, whichever session there is: what a client starting
     * afresh asks, so a session it can no longer show does not outlive it. */
    if (jnode != NULL) {
        if (params_node(params, key, err, errlen) != 0) {
            *code = POCKETIPC_ERR_INVALID_PARAMS;
            return NULL;
        }
        ended = mcd_runtime_remote_logout(d->rt, key);
    } else {
        ended = mcd_runtime_remote_logout(d->rt, NULL);
    }
    mcd_runtime_remote_session(d->rt, &s);
    o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "logged_out", ended);
    /* Nothing is transmitted: MeshCore has no logout on the air. */
    cJSON_AddBoolToObject(o, "transmitted", false);
    cJSON_AddItemToObject(o, "session", session_json(d, &s));
    return o;
}

cJSON *mcd_remote_method(struct mcd *d, const char *name, cJSON *params, int *code, char *err,
                         size_t errlen, bool *handled)
{
    struct mcd_remote_session s;

    *handled = true;
    if (strcmp(name, "mesh.discover") == 0) {
        return m_discover(d, code, err, errlen);
    }
    if (strcmp(name, "mesh.discovered") == 0) {
        return m_discovered(d);
    }
    if (strcmp(name, "mesh.remote_login") == 0) {
        return m_login(d, params, code, err, errlen);
    }
    if (strcmp(name, "mesh.remote_request") == 0) {
        return m_request(d, params, code, err, errlen);
    }
    if (strcmp(name, "mesh.remote_cli") == 0) {
        return m_cli(d, params, code, err, errlen);
    }
    if (strcmp(name, "mesh.remote_logout") == 0) {
        return m_logout(d, params, code, err, errlen);
    }
    if (strcmp(name, "mesh.remote_session") == 0) {
        mcd_runtime_remote_session(d->rt, &s);
        return session_json(d, &s);
    }
    *handled = false;
    return NULL;
}
