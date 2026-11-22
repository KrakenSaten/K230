/*
 * gatewayd: the allowlist. See gw_policy.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_policy.h"

#include "gw.h"
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

/* ---- the table --------------------------------------------------------- */
/*
 * Every method a network client can reach. A method that is not here does
 * not exist for it (code 1), and tests/gatewayd_lint.sh holds that nothing
 * that changes the unit (ADR-019 rule 25) and no shell.* method is ever
 * added. M2 carries the session group and device status; the mesh and
 * capture groups come with their own milestones, their scopes are defined
 * and stored now.
 */
static const struct gw_method METHODS[] = {
    {"gateway.hello", GW_K_HELLO, GW_W_PAIRED | GW_W_CANDIDATE, 0, NULL, NULL},
    {"gateway.pair", GW_K_PAIR, GW_W_CANDIDATE, 0, NULL, NULL},
    {"gateway.ping", GW_K_PING, GW_W_PAIRED, 0, NULL, NULL},
    {"gateway.unpair", GW_K_UNPAIR, GW_W_PAIRED, 0, NULL, NULL},
    {"system.info", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "sysd", filter_system_info},
    {"system.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "sysd", filter_system_status},
    {"wifi.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "netd", filter_wifi_status},
    {"radio.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "radiod", filter_whole},
    {"mesh.status", GW_K_UPSTREAM, GW_W_PAIRED, GW_S_STATUS, "meshcored", filter_whole},
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
    if (strcmp(lower, "public_key") == 0 || strcmp(lower, "key_bits") == 0) {
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
