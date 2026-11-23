/*
 * gatewayd: the allowlist. See gw_policy.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_policy.h"

#include "gw.h"
#include "gw_capture.h"
#include "gw_util.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

/* ---- result filters (docs/api/gateway.md §6) --------------------------- */

static cJSON *keep(const cJSON *src, const char *const *names)
{
    cJSON *out = cJSON_CreateObject();
    size_t i;

    if (out == NULL || !cJSON_IsObject(src)) {
        cJSON_Delete(out);
        return NULL;
    }
    for (i = 0; names[i] != NULL; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, names[i]);

        if (v != NULL) {
            cJSON *copy = cJSON_Duplicate(v, 1);

            if (copy == NULL || !cJSON_AddItemToObject(out, names[i], copy)) {
                cJSON_Delete(copy);
                cJSON_Delete(out);
                return NULL;
            }
        }
    }
    return out;
}

/* Replaces the array `name` of obj by one holding only `names` of each
 * element. An element that is not an object is dropped. */
static int keep_in_array(cJSON *obj, const char *name, const char *const *names)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, name);
    cJSON *out, *el;

    if (arr == NULL) {
        return 0;
    }
    if (!cJSON_IsArray(arr)) {
        cJSON_DeleteItemFromObjectCaseSensitive(obj, name);
        return 0;
    }
    out = cJSON_CreateArray();
    if (out == NULL) {
        return -1;
    }
    cJSON_ArrayForEach(el, arr) {
        cJSON *k = cJSON_IsObject(el) ? keep(el, names) : NULL;

        if (k != NULL && !cJSON_AddItemToArray(out, k)) {
            cJSON_Delete(k);
            cJSON_Delete(out);
            return -1;
        }
    }
    cJSON_ReplaceItemInObjectCaseSensitive(obj, name, out);
    return 0;
}

static cJSON *filter_system_info(const cJSON *r)
{
    static const char *const names[] = {"version", "build", "model", "kernel", "hostname",
                                        "cpus", "os", "api_version", NULL};

    return keep(r, names);
}

static cJSON *filter_system_status(const cJSON *r)
{
    static const char *const top[] = {"uptime_s", "load", "cpu_percent", "memory", "temperature_c",
                                      "clock_set", "storage", "network", "power", "services",
                                      "api_version", NULL};
    /* No MAC address: it identifies the unit to anyone who has seen it. */
    static const char *const net[] = {"name", "operstate", "carrier", "ipv4", NULL};
    static const char *const svc[] = {"name", "running", "crashloop", "restarts", NULL};
    cJSON *out = keep(r, top);

    if (out != NULL && (keep_in_array(out, "network", net) != 0 || keep_in_array(out, "services", svc) != 0)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static cJSON *filter_wifi_status(const cJSON *r)
{
    /* No BSSID: it locates the network. */
    static const char *const names[] = {"state", "reason", "ssid", "signal_dbm", "bars", "ipv4",
                                        "security", "api_version", NULL};

    return keep(r, names);
}

static cJSON *filter_whole(const cJSON *r)
{
    return cJSON_IsObject(r) ? cJSON_Duplicate(r, 1) : NULL;
}

/* ---- the mesh group (docs/api/gateway.md §7) ---------------------------- */
/*
 * Every result of meshcored is reduced to the fields docs/api/mesh.md
 * documents, by name. meshcored reports no channel key and no identity key
 * (ADR-019 rule 26); the lists below are what holds that here too, whatever
 * a later meshcored adds to a result, and gw_scrub runs behind them.
 */
static const char *const NODE_FIELDS[] = {
    "public_key", "node_hash", "name", "type", "path_known", "hops", "direct", "path_hex",
    "last_advert_timestamp", "last_heard_mono_ms", "last_snr_db", "last_rssi_dbm", "advert_hops",
    "advert_mono_ms", "lat", "lon", NULL};
static const char *const CHANNEL_FIELDS[] = {
    "channel", "name", "channel_hash", "key_bits", "well_known", "text_limit", "ack_expected", NULL};
static const char *const MESSAGE_FIELDS[] = {
    "id", "direction", "kind", "peer_public_key", "peer_name", "channel", "channel_name", "channel_hash",
    "sender_name", "text", "timestamp", "mono_ms", "state", "ack_expected", "ack_mono_ms", "attempts",
    "route", "path_hex", "path_hops", "transmitted", "heard_back", "heard_back_hops", "heard_back_mono_ms",
    "snr_db", "rssi_dbm", NULL};

cJSON *gw_filter_node(const cJSON *node)
{
    cJSON *out = keep(node, NODE_FIELDS);

    gw_scrub(out);
    return out;
}

cJSON *gw_filter_channel(const cJSON *channel)
{
    cJSON *out = keep(channel, CHANNEL_FIELDS);

    gw_scrub(out);
    return out;
}

cJSON *gw_filter_message(const cJSON *message)
{
    cJSON *out = keep(message, MESSAGE_FIELDS);

    gw_scrub(out);
    return out;
}

static cJSON *filter_mesh_identity(const cJSON *r)
{
    static const char *const names[] = {"public_key", "node_hash", "name", "name_source", "name_max", NULL};

    return keep(r, names);
}

static cJSON *filter_mesh_nodes(const cJSON *r)
{
    static const char *const top[] = {"nodes", NULL};
    cJSON *out = keep(r, top);

    if (out != NULL && (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(out, "nodes")) ||
                        keep_in_array(out, "nodes", NODE_FIELDS) != 0)) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static cJSON *filter_mesh_channels(const cJSON *r)
{
    static const char *const top[] = {"channels", "count", "max", "persistent", NULL};
    cJSON *out = keep(r, top);

    if (out != NULL && keep_in_array(out, "channels", CHANNEL_FIELDS) != 0) {
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

static cJSON *filter_path_preview(const cJSON *r)
{
    static const char *const names[] = {"valid", "route", "relays", "hash_bytes", "path_bytes", "path_hex",
                                        "transmitted", "link_evidence_verified", NULL};

    return keep(r, names);
}

/* {reason, <member>} with the member reduced by `filter`. */
static cJSON *event_with(const cJSON *data, const char *member, cJSON *(*filter)(const cJSON *))
{
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(data, "reason");
    cJSON *out, *inner;

    if (!cJSON_IsString(reason) || strlen(reason->valuestring) > 16) {
        return NULL;
    }
    inner = filter(cJSON_GetObjectItemCaseSensitive(data, member));
    out = cJSON_CreateObject();
    if (inner == NULL || out == NULL || cJSON_AddStringToObject(out, "reason", reason->valuestring) == NULL ||
        !cJSON_AddItemToObject(out, member, inner)) {
        cJSON_Delete(inner);
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

cJSON *gw_filter_event(const char *name, const cJSON *data)
{
    static const char *const state[] = {"state", "reason", "mono_ms", NULL};

    if (name == NULL || !cJSON_IsObject(data)) {
        return NULL;
    }
    if (strcmp(name, "mesh.node") == 0) {
        return event_with(data, "node", gw_filter_node);
    }
    if (strcmp(name, "mesh.channel") == 0) {
        return event_with(data, "channel", gw_filter_channel);
    }
    if (strcmp(name, "mesh.state") == 0) {
        cJSON *out = keep(data, state);

        gw_scrub(out);
        return out;
    }
    return NULL;
}

/* ---- parameters --------------------------------------------------------- */

/* True when every member of params is one of names. */
static bool only_members(const cJSON *params, const char *const *names)
{
    const cJSON *el;
    size_t i;

    if (params == NULL) {
        return true;
    }
    cJSON_ArrayForEach(el, params) {
        for (i = 0; names[i] != NULL; i++) {
            if (el->string != NULL && strcmp(el->string, names[i]) == 0) {
                break;
            }
        }
        if (names[i] == NULL) {
            return false;
        }
    }
    return true;
}

/* A whole number from lo to hi. */
static bool whole(const cJSON *v, double lo, double hi, double *out)
{
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= lo && v->valuedouble <= hi) ||
        v->valuedouble != floor(v->valuedouble)) {
        return false;
    }
    *out = v->valuedouble;
    return true;
}

/* An even number of hex digits, min to max of them. */
static bool hex_string(const cJSON *v, size_t min, size_t max)
{
    size_t i, n;

    if (!cJSON_IsString(v)) {
        return false;
    }
    n = strlen(v->valuestring);
    if (n < min || n > max || (n & 1u) != 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)v->valuestring[i])) {
            return false;
        }
    }
    return true;
}

/* mesh.node {node}: a public key or a prefix of one. */
static int params_node(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"node", NULL};
    const cJSON *node = cJSON_GetObjectItemCaseSensitive(in, "node");

    *out = NULL;
    if (!only_members(in, names) || !hex_string(node, 2, 64)) {
        return -1;
    }
    *out = cJSON_CreateObject();
    if (*out == NULL || cJSON_AddStringToObject(*out, "node", node->valuestring) == NULL) {
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

/* mesh.path_preview {to, via[]}: whole public keys, at most 63 relays. */
static int params_path_preview(const cJSON *in, cJSON **out)
{
    static const char *const names[] = {"to", "via", NULL};
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(in, "to");
    const cJSON *via = cJSON_GetObjectItemCaseSensitive(in, "via");
    const cJSON *el;
    cJSON *arr;

    *out = NULL;
    if (!only_members(in, names) || !hex_string(to, 64, 64) || !cJSON_IsArray(via) ||
        cJSON_GetArraySize(via) > 63) {
        return -1;
    }
    cJSON_ArrayForEach(el, via) {
        if (!hex_string(el, 64, 64)) {
            return -1;
        }
    }
    *out = cJSON_CreateObject();
    arr = cJSON_CreateArray();
    if (*out == NULL || arr == NULL || cJSON_AddStringToObject(*out, "to", to->valuestring) == NULL) {
        cJSON_Delete(arr);
        cJSON_Delete(*out);
        *out = NULL;
        return -1;
    }
    cJSON_AddItemToObject(*out, "via", arr);
    cJSON_ArrayForEach(el, via) {
        cJSON *k = cJSON_CreateString(el->valuestring);

        if (k == NULL || !cJSON_AddItemToArray(arr, k)) {
            cJSON_Delete(k);
            cJSON_Delete(*out);
            *out = NULL;
            return -1;
        }
    }
    return 0;
}

int gw_page_params(const cJSON *params, int *offset, int *limit)
{
    static const char *const names[] = {"offset", "limit", NULL};
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(params, "offset");
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(params, "limit");
    double v;

    *offset = 0;
    *limit = GW_NODES_LIMIT;
    if (!only_members(params, names)) {
        return -1;
    }
    if (o != NULL) {
        if (!whole(o, 0, GW_NODES_OFFSET_MAX, &v)) {
            return -1;
        }
        *offset = (int)v;
    }
    if (l != NULL) {
        if (!whole(l, 1, GW_NODES_LIMIT, &v)) {
            return -1;
        }
        *limit = (int)v;
    }
    return 0;
}

cJSON *gw_nodes_page(const cJSON *filtered, int offset, int limit, const char *run_id)
{
    const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(filtered, "nodes");
    const cJSON *el;
    cJSON *out = cJSON_CreateObject(), *arr = cJSON_CreateArray();
    int i = 0, n = 0;

    if (out == NULL || arr == NULL || !cJSON_IsArray(nodes)) {
        cJSON_Delete(out);
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON_AddItemToObject(out, "nodes", arr);
    cJSON_ArrayForEach(el, nodes) {
        if (i >= offset && n < limit) {
            cJSON *copy = cJSON_Duplicate(el, 1);

            if (copy == NULL || !cJSON_AddItemToArray(arr, copy)) {
                cJSON_Delete(copy);
                cJSON_Delete(out);
                return NULL;
            }
            n++;
        }
        i++;
    }
    cJSON_AddNumberToObject(out, "count", n);
    cJSON_AddNumberToObject(out, "total", i);
    cJSON_AddStringToObject(out, "run_id", run_id != NULL ? run_id : "");
    return out;
}

int gw_sync_params(const cJSON *params, bool *have_after, uint64_t *after_seq, int *limit)
{
    static const char *const names[] = {"after_seq", "limit", NULL};
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(params, "after_seq");
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(params, "limit");
    double v;

    *have_after = false;
    *after_seq = 0;
    *limit = GW_SYNC_LIMIT;
    if (!only_members(params, names)) {
        return -1;
    }
    if (a != NULL) {
        if (!whole(a, 0, 9007199254740992.0, &v)) {
            return -1;
        }
        *have_after = true;
        *after_seq = (uint64_t)v;
    }
    if (l != NULL) {
        if (!whole(l, 1, GW_SYNC_LIMIT, &v)) {
            return -1;
        }
        *limit = (int)v;
    }
    return 0;
}

/* ---- topics ------------------------------------------------------------- */

static const struct {
    unsigned bit;
    const char *name;
    unsigned scope;
} TOPICS[] = {
    {GW_T_MESH, "mesh", GW_S_MESH_READ},
    {GW_T_MESH_ACTIVITY, "mesh.activity", GW_S_MESH_READ},
    {GW_T_CAPTURE, "capture", GW_S_CAPTURE_VIEW},
};
#define TOPIC_COUNT (sizeof(TOPICS) / sizeof(TOPICS[0]))

unsigned gw_topics_allowed(unsigned mask, unsigned scopes)
{
    unsigned out = 0;
    size_t i;

    for (i = 0; i < TOPIC_COUNT; i++) {
        if ((mask & TOPICS[i].bit) != 0 && (scopes & TOPICS[i].scope) == TOPICS[i].scope) {
            out |= TOPICS[i].bit;
        }
    }
    return out;
}

int gw_topics_parse(const cJSON *params, unsigned scopes, unsigned *mask)
{
    static const char *const names[] = {"topics", NULL};
    const cJSON *topics = cJSON_GetObjectItemCaseSensitive(params, "topics");
    const cJSON *el;
    unsigned m = 0;
    size_t i;

    if (!only_members(params, names) || !cJSON_IsArray(topics) || cJSON_GetArraySize(topics) > 8) {
        return -1;
    }
    cJSON_ArrayForEach(el, topics) {
        if (!cJSON_IsString(el)) {
            return -1;
        }
        for (i = 0; i < TOPIC_COUNT; i++) {
            if (strcmp(TOPICS[i].name, el->valuestring) == 0) {
                m |= TOPICS[i].bit;
            }
        }
    }
    *mask = gw_topics_allowed(m, scopes);
    return 0;
}

cJSON *gw_topics_json(unsigned mask)
{
    cJSON *arr = cJSON_CreateArray();
    size_t i;

    for (i = 0; arr != NULL && i < TOPIC_COUNT; i++) {
        if ((mask & TOPICS[i].bit) != 0) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(TOPICS[i].name));
        }
    }
    return arr;
}

/* ---- the table --------------------------------------------------------- */
/*
 * Every method a network client can reach. A method that is not here does
 * not exist for it (code 1), and tests/gatewayd_lint.sh holds that nothing
 * that changes the unit (ADR-019 rule 25) and no shell.* method is ever
 * added. M2 carries the session group and device status, M4.1 the reading
 * of the mesh; sending and the capture group come with their own
 * milestones, their scopes are defined and stored now.
 */
static const struct gw_method METHODS[] = {
    {"gateway.hello", GW_K_HELLO, GW_W_PAIRED | GW_W_CANDIDATE, 0, NULL, NULL, NULL},
    {"gateway.pair", GW_K_PAIR, GW_W_CANDIDATE, 0, NULL, NULL, NULL},
    {"gateway.ping", GW_K_PING, GW_W_PAIRED, 0, NULL, NULL, NULL},
    {"gateway.unpair", GW_K_UNPAIR, GW_W_PAIRED, 0, NULL, NULL, NULL},
    {"system.info", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "sysd", filter_system_info, NULL},
    {"system.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "sysd", filter_system_status, NULL},
    {"wifi.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "netd", filter_wifi_status, NULL},
    {"radio.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "radiod", filter_whole, NULL},
    {"mesh.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "meshcored", filter_whole, NULL},
    /* M4.1: reading the mesh. Every row below gateway.subscribe needs
     * mesh_read; none changes the node and none transmits
     * (mesh.path_preview creates no packet: docs/api/mesh.md). */
    {"gateway.subscribe", GW_K_SUBSCRIBE, GW_W_PAIRED, 0, NULL, NULL, NULL},
    {"mesh.identity", GW_K_MESH, GW_W_PAIRED, GW_S_MESH_READ, "meshcored", filter_mesh_identity, NULL},
    {"mesh.nodes", GW_K_NODES, GW_W_PAIRED, GW_S_MESH_READ, "meshcored", filter_mesh_nodes, NULL},
    {"mesh.node", GW_K_MESH, GW_W_PAIRED, GW_S_MESH_READ, "meshcored", gw_filter_node, params_node},
    {"mesh.channels", GW_K_MESH, GW_W_PAIRED, GW_S_MESH_READ, "meshcored", filter_mesh_channels, NULL},
    {"mesh.path_preview", GW_K_MESH, GW_W_PAIRED, GW_S_MESH_READ, "meshcored", filter_path_preview,
     params_path_preview},
    {"mesh.sync", GW_K_SYNC, GW_W_PAIRED, GW_S_MESH_READ, NULL, NULL, NULL},
    /* Sending (M4.2): its own scope, which mesh_read does not give. What a
     * client sent is checked and written again by gw_send.c. */
    {"mesh.send", GW_K_SEND, GW_W_PAIRED, GW_S_MESH_SEND, "meshcored", NULL, NULL},
    {"mesh.resend", GW_K_RESEND, GW_W_PAIRED, GW_S_MESH_SEND, "meshcored", NULL, NULL},
    /* PacketShark's capture (ADR-016 amendment 1; ADR-019 rule 6): these
     * four and no other. They are served by the PacketShark app on the
     * shell's socket and are no shell.* method. Reading needs capture_view;
     * holding, stopping and asking for a start need capture_control, which
     * is only ever held with capture_view. Asking starts nothing: a person
     * taps START on the unit. capture.request_cancel is not here and never
     * will be: the gateway calls it for itself (main.c cap_cancel). */
    {"capture.status", GW_K_CAPTURE, GW_W_PAIRED, GW_S_CAPTURE_VIEW, "shell", gw_capture_status, NULL},
    {"capture.hold", GW_K_CAPTURE, GW_W_PAIRED, GW_S_CAPTURE_CONTROL, "shell", gw_capture_command,
     gw_capture_params_hold},
    {"capture.stop", GW_K_CAPTURE, GW_W_PAIRED, GW_S_CAPTURE_CONTROL, "shell", gw_capture_command, NULL},
    {"capture.request_start", GW_K_CAPTURE, GW_W_PAIRED, GW_S_CAPTURE_CONTROL, "shell", gw_capture_shown,
     gw_capture_params_ask},
};

const struct gw_method *gw_methods(size_t *count)
{
    *count = sizeof(METHODS) / sizeof(METHODS[0]);
    return METHODS;
}

const struct gw_method *gw_method_find(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); i++) {
        if (strcmp(METHODS[i].name, name) == 0) {
            return &METHODS[i];
        }
    }
    return NULL;
}

enum gw_verdict gw_policy_check(const struct gw_method *m, enum gw_who who, unsigned scopes)
{
    /* A method this caller may never call is indistinguishable from one
     * that does not exist. */
    if (m == NULL || (m->who & (unsigned)who) == 0) {
        return GW_V_UNKNOWN;
    }
    if (m->scope != 0 && (scopes & m->scope) != m->scope) {
        return GW_V_FORBIDDEN;
    }
    return GW_V_OK;
}

/* ---- scopes ------------------------------------------------------------ */

static const struct {
    unsigned bit;
    const char *name;
} SCOPES[] = {
    {GW_S_STATUS, "status"},
    {GW_S_MESH_READ, "mesh_read"},
    {GW_S_MESH_SEND, "mesh_send"},
    {GW_S_CAPTURE_VIEW, "capture_view"},
    {GW_S_CAPTURE_CONTROL, "capture_control"},
};
#define SCOPE_COUNT (sizeof(SCOPES) / sizeof(SCOPES[0]))

const char *gw_scope_name(unsigned bit)
{
    size_t i;

    for (i = 0; i < SCOPE_COUNT; i++) {
        if (SCOPES[i].bit == bit) {
            return SCOPES[i].name;
        }
    }
    return NULL;
}

unsigned gw_scope_parse(const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < SCOPE_COUNT; i++) {
        if (strcmp(SCOPES[i].name, name) == 0) {
            return SCOPES[i].bit;
        }
    }
    return 0;
}

bool gw_scopes_valid(unsigned mask)
{
    if ((mask & ~(unsigned)GW_S_ALL) != 0) {
        return false;
    }
    if ((mask & GW_S_MESH_SEND) != 0 && (mask & GW_S_MESH_READ) == 0) {
        return false;
    }
    if ((mask & GW_S_CAPTURE_CONTROL) != 0 && (mask & GW_S_CAPTURE_VIEW) == 0) {
        return false;
    }
    return true;
}

cJSON *gw_scopes_json(unsigned mask)
{
    cJSON *arr = cJSON_CreateArray();
    size_t i;

    for (i = 0; arr != NULL && i < SCOPE_COUNT; i++) {
        if ((mask & SCOPES[i].bit) != 0) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(SCOPES[i].name));
        }
    }
    return arr;
}

int gw_scopes_from_json(const cJSON *arr, unsigned *mask)
{
    const cJSON *el;
    unsigned m = 0;

    if (!cJSON_IsArray(arr)) {
        return -1;
    }
    cJSON_ArrayForEach(el, arr) {
        unsigned bit = cJSON_IsString(el) ? gw_scope_parse(el->valuestring) : 0;

        if (bit == 0 || (m & bit) != 0) {
            return -1;
        }
        m |= bit;
    }
    if (!gw_scopes_valid(m)) {
        return -1;
    }
    *mask = m;
    return 0;
}

/* ---- requests ---------------------------------------------------------- */

int gw_request_parse(const uint8_t *p, size_t n, cJSON **root, const cJSON **id, const char **method,
                     const cJSON **params)
{
    cJSON *r;
    const cJSON *i, *m, *pr;

    *root = NULL;
    /* cJSON does not check its input's encoding, and a NUL would end its
     * view of the frame early. */
    if (n == 0 || memchr(p, '\0', n) != NULL || !gw_utf8_ok(p, n)) {
        return -1;
    }
    r = cJSON_ParseWithLength((const char *)p, n);
    if (r == NULL) {
        return -1;
    }
    i = cJSON_GetObjectItemCaseSensitive(r, "id");
    m = cJSON_GetObjectItemCaseSensitive(r, "method");
    pr = cJSON_GetObjectItemCaseSensitive(r, "params");
    if (!cJSON_IsObject(r) || !cJSON_IsNumber(i) || !isfinite(i->valuedouble) ||
        i->valuedouble != floor(i->valuedouble) || fabs(i->valuedouble) > 9007199254740992.0 ||
        !cJSON_IsString(m) || m->valuestring[0] == '\0' || strlen(m->valuestring) > GW_METHOD_MAX ||
        (pr != NULL && !cJSON_IsObject(pr))) {
        cJSON_Delete(r);
        return -1;
    }
    *root = r;
    *id = i;
    *method = m->valuestring;
    *params = pr;
    return 0;
}

bool gw_params_empty(const cJSON *params)
{
    return params == NULL || (cJSON_IsObject(params) && params->child == NULL);
}

/* ---- the last line: nothing named for a secret leaves ------------------ */

static bool has_word(const char *lower, const char *word)
{
    return strstr(lower, word) != NULL;
}

static bool secret_name(const char *name)
{
    char lower[64];
    size_t i, n = strlen(name);

    if (n >= sizeof(lower)) {
        n = sizeof(lower) - 1;
    }
    for (i = 0; i < n; i++) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    lower[n] = '\0';
    if (strcmp(lower, "public_key") == 0 || strcmp(lower, "peer_public_key") == 0 ||
        strcmp(lower, "key_bits") == 0) {
        return false;
    }
    return has_word(lower, "secret") || has_word(lower, "passphrase") || has_word(lower, "password") ||
           has_word(lower, "private") || has_word(lower, "token") || has_word(lower, "psk") ||
           has_word(lower, "key");
}

void gw_scrub(cJSON *node)
{
    cJSON *child, *next;

    if (node == NULL) {
        return;
    }
    for (child = node->child; child != NULL; child = next) {
        next = child->next;
        if (cJSON_IsObject(node) && child->string != NULL && secret_name(child->string)) {
            cJSON_Delete(cJSON_DetachItemViaPointer(node, child));
            continue;
        }
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            gw_scrub(child);
        }
    }
}

cJSON *gw_filter_result(const struct gw_method *m, const cJSON *result)
{
    cJSON *out;

    if (m->filter == NULL || !cJSON_IsObject(result)) {
        return NULL;
    }
    out = m->filter(result);
    gw_scrub(out);
    return out;
}
