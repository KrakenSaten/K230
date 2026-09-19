/*
 * RIFT's model. See rift_model.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_model.h"

#include "rift_format.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

int64_t rift_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char *rift_svc_state_word(enum rift_svc_state s)
{
    switch (s) {
    case RIFT_SVC_ABSENT:
        return "not running";
    case RIFT_SVC_STARTING:
        return "starting";
    case RIFT_SVC_WAITING_RADIOD:
        return "waiting for radiod";
    case RIFT_SVC_WAITING_LEASE:
        return "waiting for the radio";
    case RIFT_SVC_CONFIGURING:
        return "configuring";
    case RIFT_SVC_ONLINE:
        return "online";
    case RIFT_SVC_DEGRADED:
        return "degraded";
    case RIFT_SVC_ERROR:
        return "error";
    case RIFT_SVC_UNKNOWN:
    default:
        return "unknown";
    }
}

static enum rift_svc_state state_from_word(const char *w)
{
    if (!w) {
        return RIFT_SVC_UNKNOWN;
    }
    if (strcmp(w, "starting") == 0) {
        return RIFT_SVC_STARTING;
    }
    if (strcmp(w, "waiting_for_radiod") == 0) {
        return RIFT_SVC_WAITING_RADIOD;
    }
    if (strcmp(w, "waiting_for_lease") == 0) {
        return RIFT_SVC_WAITING_LEASE;
    }
    if (strcmp(w, "configuring") == 0) {
        return RIFT_SVC_CONFIGURING;
    }
    if (strcmp(w, "online") == 0) {
        return RIFT_SVC_ONLINE;
    }
    if (strcmp(w, "degraded") == 0) {
        return RIFT_SVC_DEGRADED;
    }
    if (strcmp(w, "error") == 0) {
        return RIFT_SVC_ERROR;
    }
    /* A state word this build does not know is not guessed at. */
    return RIFT_SVC_UNKNOWN;
}

/* ---- reading JSON, one field at a time --------------------------------- */

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

/* A number, only when it is one. An absent field and a field holding null,
 * a string or an object all mean "not reported", which is different from 0
 * (docs/api/mesh.md). */
static int num_of(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v || !cJSON_IsNumber(v)) {
        return 0;
    }
    *out = v->valuedouble;
    return 1;
}

static int bool_of(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!v || !cJSON_IsBool(v)) {
        return fallback;
    }
    return cJSON_IsTrue(v) ? 1 : 0;
}

static int hex_only(const char *s, size_t want_len)
{
    size_t i;

    if (!s) {
        return 0;
    }
    for (i = 0; s[i]; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) {
            return 0;
        }
    }
    return want_len == 0 ? (i > 0) : (i == want_len);
}

/* ---- the cache --------------------------------------------------------- */

void rift_model_init(struct rift_model *m)
{
    if (!m) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->state = RIFT_SVC_UNKNOWN;
}

void rift_model_service_lost(struct rift_model *m, const char *reason)
{
    if (!m) {
        return;
    }
    m->state = RIFT_SVC_ABSENT;
    rift_utf8_copy(m->reason, sizeof(m->reason), reason ? reason : "meshcored is not answering");
    m->have_state_mono = 0;
    /* The nodes stay: they are the last thing anybody told us, and an empty
     * list would say something less true than a stale one. What changes is
     * that the screen must now say they are cached (handoff §5). */
    m->stale = 1;
    m->snapshot_valid = 0;
    m->have_status = 0;
    m->have_radio_state = 0;
    m->radio_connected = 0;
    m->radio_lease_held = 0;
    m->radio_online = 0;
}

void rift_model_service_found(struct rift_model *m)
{
    if (!m) {
        return;
    }
    m->stale = 0;
    m->snapshot_valid = 0;
    if (m->state == RIFT_SVC_ABSENT) {
        m->state = RIFT_SVC_UNKNOWN;
        m->reason[0] = '\0';
    }
}

static struct rift_node *find_mut(struct rift_model *m, const char *key)
{
    int i;

    for (i = 0; i < m->node_count; i++) {
        if (strcmp(m->nodes[i].key, key) == 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

const struct rift_node *rift_model_find(const struct rift_model *m, const char *key)
{
    int i;

    if (!m || !key) {
        return NULL;
    }
    for (i = 0; i < m->node_count; i++) {
        if (strcmp(m->nodes[i].key, key) == 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

/* The slot to reuse when the cache is full: the one heard longest ago, and
 * a node never heard before one that was. Never the node the caller is
 * about to write. */
static int evict_index(const struct rift_model *m)
{
    int worst = 0;
    int i;

    for (i = 1; i < m->node_count; i++) {
        const struct rift_node *a = &m->nodes[worst];
        const struct rift_node *b = &m->nodes[i];

        if (!b->have_heard && a->have_heard) {
            worst = i;
        } else if (b->have_heard == a->have_heard && b->have_heard &&
                   b->heard_mono_ms < a->heard_mono_ms) {
            worst = i;
        }
    }
    return worst;
}

static struct rift_node *slot_for(struct rift_model *m, const char *key)
{
    struct rift_node *n = find_mut(m, key);
    int at;

    if (n) {
        return n;
    }
    if (m->node_count < RIFT_MAX_NODES) {
        at = m->node_count++;
    } else {
        at = evict_index(m);
        m->nodes_dropped++;
    }
    n = &m->nodes[at];
    memset(n, 0, sizeof(*n));
    snprintf(n->key, sizeof(n->key), "%s", key);
    return n;
}

/* Remember a path this app has just observed, newest first, and only when
 * it differs from the newest one already held: a node adverting the same
 * path every few minutes is not a history of changes. */
static void record_path(struct rift_node *n, int have_mono, int64_t mono_ms)
{
    int i;

    if (n->hist_count > 0) {
        const struct rift_path_obs *newest = &n->hist[0];

        if (newest->path_known == n->path_known && newest->hops == n->hops &&
            strcmp(newest->path_hex, n->path_hex) == 0) {
            return;
        }
    }
    for (i = RIFT_PATH_HISTORY - 1; i > 0; i--) {
        n->hist[i] = n->hist[i - 1];
    }
    memset(&n->hist[0], 0, sizeof(n->hist[0]));
    n->hist[0].have_mono = have_mono;
    n->hist[0].mono_ms = mono_ms;
    n->hist[0].path_known = n->path_known;
    n->hist[0].hops = n->hops;
    snprintf(n->hist[0].path_hex, sizeof(n->hist[0].path_hex), "%s", n->path_hex);
    if (n->hist_count < RIFT_PATH_HISTORY) {
        n->hist_count++;
    }
}

/* Read one node object into the cache. Returns 0, or -1 when the object is
 * not a node this model will take - which is not the same as a node with
 * missing values, every one of which is allowed and kept as missing. */
static int apply_node(struct rift_model *m, const cJSON *o)
{
    const char *key;
    const char *hash;
    const char *name;
    const char *path_hex;
    struct rift_node *n;
    double d;

    if (!cJSON_IsObject(o)) {
        return -1;
    }
    key = str_of(o, "public_key");
    /* The key is the identity. Without a whole, well-formed one there is
     * nothing to file this under and nothing to ask mesh.node about, so the
     * object is refused rather than filed under a guess. */
    if (!hex_only(key, 64)) {
        return -1;
    }
    n = slot_for(m, key);
    hash = str_of(o, "node_hash");
    if (hex_only(hash, 2)) {
        snprintf(n->hash, sizeof(n->hash), "%s", hash);
    } else if (!n->hash[0]) {
        /* The API always sends it; derive it rather than show nothing. */
        n->hash[0] = key[0];
        n->hash[1] = key[1];
        n->hash[2] = '\0';
    }
    name = str_of(o, "name");
    if (name && name[0]) {
        n->have_name = 1;
        rift_utf8_copy(n->name, sizeof(n->name), name);
    } else if (name) {
        /* An empty name is a node that has not said what it is called. */
        n->have_name = 0;
        n->name[0] = '\0';
    }
    if (num_of(o, "type", &d)) {
        n->have_type = 1;
        n->type = (int)d;
    }
    n->path_known = bool_of(o, "path_known", n->path_known);
    if (n->path_known) {
        if (num_of(o, "hops", &d)) {
            n->hops = (int)d;
            if (n->hops < 0) {
                n->hops = 0;
            }
        }
        n->direct = bool_of(o, "direct", n->hops == 0);
        path_hex = str_of(o, "path_hex");
        if (path_hex) {
            snprintf(n->path_hex, sizeof(n->path_hex), "%s", path_hex);
        } else {
            /* path_known with no bytes: the hops are real and unnamed. */
            n->path_hex[0] = '\0';
        }
    } else {
        n->hops = 0;
        n->direct = 0;
        n->path_hex[0] = '\0';
    }
    if (num_of(o, "last_advert_timestamp", &d)) {
        n->have_advert = 1;
        n->advert_timestamp = (int64_t)d;
    }
    if (num_of(o, "last_heard_mono_ms", &d)) {
        n->have_heard = 1;
        n->heard_mono_ms = (int64_t)d;
    }
    /* Signal is per observation, and an observation that carried none
     * leaves the last known value alone: the API records "heard with the
     * signal unknown" rather than with somebody else's signal, and
     * forgetting a measurement that was made would be the same mistake in
     * the other direction. */
    if (num_of(o, "last_snr_db", &d)) {
        n->have_snr = 1;
        n->snr_db = d;
    }
    if (num_of(o, "last_rssi_dbm", &d)) {
        n->have_rssi = 1;
        n->rssi_dbm = d;
    }
    n->seq = ++m->seq;
    return 0;
}

int rift_model_apply_nodes(struct rift_model *m, const cJSON *result)
{
    const cJSON *arr;
    const cJSON *item;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    arr = cJSON_GetObjectItemCaseSensitive(result, "nodes");
    if (!cJSON_IsArray(arr)) {
        return -1;
    }
    /* A snapshot replaces the list: a node the service no longer holds has
     * been forgotten there, and keeping it here would be this app inventing
     * a node nobody can be asked about. */
    m->node_count = 0;
    memset(m->nodes, 0, sizeof(m->nodes));
    cJSON_ArrayForEach (item, arr) {
        if (apply_node(m, item) != 0) {
            m->events_malformed++;
        }
    }
    m->snapshot_valid = 1;
    m->have_snapshot_mono = 1;
    m->snapshot_mono_ms = rift_mono_ms();
    m->stale = 0;
    return 0;
}

int rift_model_apply_identity(struct rift_model *m, const cJSON *result)
{
    const char *key;
    const char *hash;
    const char *name;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    key = str_of(result, "public_key");
    if (!hex_only(key, 64)) {
        return -1;
    }
    snprintf(m->self_key, sizeof(m->self_key), "%s", key);
    hash = str_of(result, "node_hash");
    if (hex_only(hash, 2)) {
        snprintf(m->self_hash, sizeof(m->self_hash), "%s", hash);
    } else {
        m->self_hash[0] = key[0];
        m->self_hash[1] = key[1];
        m->self_hash[2] = '\0';
    }
    name = str_of(result, "name");
    rift_utf8_copy(m->self_name, sizeof(m->self_name), name ? name : "");
    m->have_identity = 1;
    return 0;
}

int rift_model_apply_info(struct rift_model *m, const cJSON *result)
{
    const char *s;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    s = str_of(result, "version");
    snprintf(m->version, sizeof(m->version), "%s", s ? s : "");
    s = str_of(result, "build");
    snprintf(m->build, sizeof(m->build), "%s", s ? s : "");
    s = str_of(result, "protocol");
    snprintf(m->protocol, sizeof(m->protocol), "%s", s ? s : "");
    m->have_info = 1;
    return 0;
}

int rift_model_apply_status(struct rift_model *m, const cJSON *result)
{
    const cJSON *radio;
    const cJSON *counters;
    const char *s;
    double d;

    if (!m || !cJSON_IsObject(result)) {
        return -1;
    }
    s = str_of(result, "state");
    if (!s) {
        return -1;
    }
    m->state = state_from_word(s);
    s = str_of(result, "reason");
    rift_utf8_copy(m->reason, sizeof(m->reason), s ? s : "");
    if (num_of(result, "state_since_mono_ms", &d)) {
        m->have_state_mono = 1;
        m->state_mono_ms = (int64_t)d;
    }
    radio = cJSON_GetObjectItemCaseSensitive(result, "radio");
    if (cJSON_IsObject(radio)) {
        m->radio_connected = bool_of(radio, "connected", 0);
        m->radio_lease_held = bool_of(radio, "lease_held", 0);
        m->radio_online = bool_of(radio, "online", 0);
        s = str_of(radio, "radio_state");
        if (s) {
            m->have_radio_state = 1;
            snprintf(m->radio_state, sizeof(m->radio_state), "%s", s);
        } else {
            /* Absent until radiod has said, and absent is not "off". */
            m->have_radio_state = 0;
            m->radio_state[0] = '\0';
        }
    }
    if (num_of(result, "nodes", &d)) {
        m->have_nodes_reported = 1;
        m->nodes_reported = (int)d;
    }
    s = str_of(result, "state_fault");
    if (s && s[0]) {
        m->have_state_fault = 1;
        rift_utf8_copy(m->state_fault, sizeof(m->state_fault), s);
    } else {
        m->have_state_fault = 0;
        m->state_fault[0] = '\0';
    }
    counters = cJSON_GetObjectItemCaseSensitive(result, "counters");
    if (cJSON_IsObject(counters)) {
        m->have_counters = 1;
        if (num_of(counters, "rx_events", &d)) {
            m->rx_events = (unsigned)d;
        }
        if (num_of(counters, "rx_delivered", &d)) {
            m->rx_delivered = (unsigned)d;
        }
        if (num_of(counters, "nodes_unretained", &d)) {
            m->nodes_unretained = (unsigned)d;
        }
    }
    m->have_status = 1;
    m->stale = 0;
    return 0;
}

/* ---- events ------------------------------------------------------------ */

static void push_activity(struct rift_model *m, const struct rift_activity *a)
{
    m->activity_head = (m->activity_head + 1) % RIFT_MAX_ACTIVITY;
    m->activity[m->activity_head] = *a;
    if (m->activity_count < RIFT_MAX_ACTIVITY) {
        m->activity_count++;
    }
    m->activity_total++;
}

const struct rift_activity *rift_model_activity_at(const struct rift_model *m, int i)
{
    int at;

    if (!m || i < 0 || i >= m->activity_count) {
        return NULL;
    }
    at = m->activity_head - i;
    while (at < 0) {
        at += RIFT_MAX_ACTIVITY;
    }
    return &m->activity[at];
}

static int apply_activity(struct rift_model *m, const cJSON *data)
{
    struct rift_activity a;
    const char *kind = str_of(data, "kind");
    const char *word;
    double d;

    if (!kind) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    if (strcmp(kind, "rx") == 0) {
        a.kind = RIFT_ACT_RX;
        word = str_of(data, "payload_type");
    } else if (strcmp(kind, "tx") == 0) {
        a.kind = RIFT_ACT_TX;
        /* "A client that treats a tx activity as 'a packet went out' will
         * be wrong for three of those five results" (docs/api/mesh.md), so
         * the result is what is shown, never the kind. */
        word = str_of(data, "result");
    } else {
        return -1;
    }
    rift_utf8_copy(a.word, sizeof(a.word), word ? word : RIFT_UNKNOWN);
    if (num_of(data, "bytes", &d)) {
        a.have_bytes = 1;
        a.bytes = (int)d;
    }
    if (num_of(data, "mono_ms", &d)) {
        a.have_mono = 1;
        a.mono_ms = (int64_t)d;
    }
    if (num_of(data, "rssi_dbm", &d)) {
        a.have_rssi = 1;
        a.rssi_dbm = d;
    }
    if (num_of(data, "snr_db", &d)) {
        a.have_snr = 1;
        a.snr_db = d;
    }
    push_activity(m, &a);
    return 0;
}

int rift_model_apply_event(struct rift_model *m, const char *name, const cJSON *data)
{
    if (!m || !name) {
        return -1;
    }
    if (!cJSON_IsObject(data)) {
        m->events_malformed++;
        return -1;
    }
    if (strcmp(name, "mesh.state") == 0) {
        const char *s = str_of(data, "state");
        double d;

        if (!s) {
            m->events_malformed++;
            return -1;
        }
        m->state = state_from_word(s);
        s = str_of(data, "reason");
        rift_utf8_copy(m->reason, sizeof(m->reason), s ? s : "");
        if (num_of(data, "mono_ms", &d)) {
            m->have_state_mono = 1;
            m->state_mono_ms = (int64_t)d;
        }
        m->stale = 0;
        m->events_applied++;
        return 0;
    }
    if (strcmp(name, "mesh.node") == 0) {
        const cJSON *node = cJSON_GetObjectItemCaseSensitive(data, "node");
        const char *reason = str_of(data, "reason");
        const char *key = str_of(node, "public_key");
        struct rift_node *n;

        if (!cJSON_IsObject(node) || !hex_only(key, 64)) {
            m->events_malformed++;
            return -1;
        }
        /* An event for a node already held updates that node. It never adds
         * a second row for it: slot_for keys on the public key, which is a
         * fact about the node rather than a position in a list the service
         * may reorder. */
        if (apply_node(m, node) != 0) {
            m->events_malformed++;
            return -1;
        }
        n = find_mut(m, key);
        if (n) {
            n->observations++;
            record_path(n, n->have_heard, n->heard_mono_ms);
        }
        (void)reason;
        m->stale = 0;
        m->events_applied++;
        return 0;
    }
    if (strcmp(name, "mesh.activity") == 0) {
        if (apply_activity(m, data) != 0) {
            m->events_malformed++;
            return -1;
        }
        m->stale = 0;
        m->events_applied++;
        return 0;
    }
    /* mesh.message is v0 API and is COMMS, which this phase does not draw.
     * An event nothing here consumes is not a fault; it is simply not ours
     * and is not counted as malformed. */
    if (strcmp(name, "mesh.message") == 0) {
        return 0;
    }
    m->events_malformed++;
    return -1;
}

/* ---- order ------------------------------------------------------------- */

/* Three groups, in this order: heard within the stale boundary (newest
 * first), heard longer ago (newest first), never heard at all. */
static int group_of(const struct rift_node *n, int64_t now_ms)
{
    if (!n->have_heard) {
        return 2;
    }
    return rift_node_is_stale(n, now_ms) ? 1 : 0;
}

static int before(const struct rift_node *a, const struct rift_node *b, int64_t now_ms)
{
    int ga = group_of(a, now_ms);
    int gb = group_of(b, now_ms);

    if (ga != gb) {
        return ga < gb;
    }
    if (a->have_heard && b->have_heard && a->heard_mono_ms != b->heard_mono_ms) {
        return a->heard_mono_ms > b->heard_mono_ms;
    }
    /* A total order, so a list that is rebuilt every second does not
     * reshuffle rows whose ages are equal. */
    return strcmp(a->key, b->key) < 0;
}

int rift_model_order(const struct rift_model *m, int64_t now_ms, const struct rift_node **out,
                     int max)
{
    int n = 0;
    int i;
    int j;

    if (!m || !out || max <= 0) {
        return 0;
    }
    for (i = 0; i < m->node_count && n < max; i++) {
        const struct rift_node *node = &m->nodes[i];

        for (j = n; j > 0 && before(node, out[j - 1], now_ms); j--) {
            out[j] = out[j - 1];
        }
        out[j] = node;
        n++;
    }
    return n;
}

int rift_model_fresh_count(const struct rift_model *m, int64_t now_ms)
{
    int n = 0;
    int i;

    if (!m) {
        return 0;
    }
    for (i = 0; i < m->node_count; i++) {
        if (group_of(&m->nodes[i], now_ms) == 0) {
            n++;
        }
    }
    return n;
}

const char *rift_model_name_for_hash(const struct rift_model *m, const char *hash_hex)
{
    const struct rift_node *hit = NULL;
    size_t len;
    int i;

    if (!m || !hash_hex) {
        return NULL;
    }
    len = strlen(hash_hex);
    if (len == 0 || len > RIFT_KEY_HEX - 1) {
        return NULL;
    }
    if (m->have_identity && strncasecmp(m->self_key, hash_hex, len) == 0) {
        return m->self_name[0] ? m->self_name : NULL;
    }
    for (i = 0; i < m->node_count; i++) {
        if (strncasecmp(m->nodes[i].key, hash_hex, len) != 0) {
            continue;
        }
        if (hit) {
            /* Two nodes share this prefix. mesh.node refuses an ambiguous
             * prefix rather than answering one (docs/api/mesh.md) and so
             * does this: a guessed name on a hop is a wrong route drawn
             * confidently. */
            return NULL;
        }
        hit = &m->nodes[i];
    }
    if (!hit || !hit->have_name || !hit->name[0]) {
        return NULL;
    }
    return hit->name;
}
