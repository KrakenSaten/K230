/*
 * The Zabbix JSON-RPC API for the viewer. See zbx_api.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix/zbx_api.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- versions --------------------------------------------------------------------- */

long zbx_version_num(const char *v)
{
    long part[3] = { 0, 0, 0 };
    int i = 0;
    const char *p = v;

    if (!p || !isdigit((unsigned char)*p)) {
        return -1;
    }
    while (i < 3) {
        char *end;
        long n;

        errno = 0;
        n = strtol(p, &end, 10);
        if (errno || end == p || n < 0 || n > 99) {
            return -1;
        }
        part[i++] = n;
        if (*end != '.') {
            break; /* "7.0" or "7.0.5rc1": what follows is not a number */
        }
        p = end + 1;
    }
    if (i < 2) {
        return -1;
    }
    return part[0] * 10000 + part[1] * 100 + part[2];
}

/* ---- building --------------------------------------------------------------------- */

static cJSON *envelope(const char *method, int id, cJSON **params)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);
    *params = cJSON_AddObjectToObject(root, "params");
    cJSON_AddNumberToObject(root, "id", id);
    if (!*params) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static char *finish(cJSON *root, const struct zbx_req_ctx *c)
{
    char *s;

    if (!root) {
        return NULL;
    }
    if (c && c->auth_in_body) {
        cJSON_AddStringToObject(root, "auth", c->auth_in_body);
    }
    s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void add_strings(cJSON *obj, const char *name, const char *const *v, int n)
{
    cJSON *a = cJSON_AddArrayToObject(obj, name);
    int i;

    for (i = 0; a && i < n; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateString(v[i]));
    }
}

char *zbx_req_version(int id)
{
    cJSON *params;
    cJSON *root = envelope("apiinfo.version", id, &params);

    /* apiinfo.version takes an empty array rather than an object on every
     * version; an object is accepted too, but the documented form is safer. */
    if (root) {
        cJSON_DeleteItemFromObject(root, "params");
        cJSON_AddItemToObject(root, "params", cJSON_CreateArray());
    }
    return finish(root, NULL);
}

char *zbx_req_login(int id, const char *user, const char *password)
{
    cJSON *params;
    cJSON *root = envelope("user.login", id, &params);

    if (root) {
        cJSON_AddStringToObject(params, "username", user ? user : "");
        cJSON_AddStringToObject(params, "password", password ? password : "");
    }
    return finish(root, NULL);
}

char *zbx_req_logout(const struct zbx_req_ctx *c)
{
    cJSON *params;
    cJSON *root = envelope("user.logout", c->id, &params);

    /* The documented form is an empty array, as for apiinfo.version. */
    if (root) {
        cJSON_DeleteItemFromObject(root, "params");
        cJSON_AddItemToObject(root, "params", cJSON_CreateArray());
    }
    return finish(root, c);
}

static void problem_filters(cJSON *params)
{
    /* Trigger problems (source 0, object 0), open ones only (recent false
     * is the default: resolved problems are left out), and without the
     * suppressed ones, as the frontend shows them by default. */
    cJSON_AddNumberToObject(params, "source", 0);
    cJSON_AddNumberToObject(params, "object", 0);
    cJSON_AddBoolToObject(params, "suppressed", 0);
}

char *zbx_req_problems(const struct zbx_req_ctx *c, const char *hostid, int limit)
{
    static const char *const out[] = { "eventid", "objectid", "clock", "name",
                                       "severity", "acknowledged", "suppressed" };
    cJSON *params;
    cJSON *root = envelope("problem.get", c->id, &params);

    if (root) {
        add_strings(params, "output", out, (int)(sizeof(out) / sizeof(out[0])));
        problem_filters(params);
        if (hostid) {
            add_strings(params, "hostids", &hostid, 1);
        }
        /* problem.get sorts by eventid only: newest first, then the helper
         * orders what arrived by severity. */
        cJSON_AddStringToObject(params, "sortfield", "eventid");
        cJSON_AddStringToObject(params, "sortorder", "DESC");
        cJSON_AddNumberToObject(params, "limit", limit);
    }
    return finish(root, c);
}

char *zbx_req_problem_count(const struct zbx_req_ctx *c, int severity, bool unacknowledged)
{
    cJSON *params;
    cJSON *root = envelope("problem.get", c->id, &params);

    if (root) {
        cJSON_AddBoolToObject(params, "countOutput", 1);
        problem_filters(params);
        if (unacknowledged) {
            cJSON_AddBoolToObject(params, "acknowledged", 0);
        }
        if (severity >= 0) {
            cJSON *a = cJSON_AddArrayToObject(params, "severities");

            if (a) {
                cJSON_AddItemToArray(a, cJSON_CreateNumber(severity));
            }
        }
    }
    return finish(root, c);
}

char *zbx_req_trigger_hosts(const struct zbx_req_ctx *c, const char *const *triggerids, int n)
{
    static const char *const hosts[] = { "hostid", "name" };
    static const char *const out[] = { "triggerid" };
    cJSON *params;
    cJSON *root = envelope("trigger.get", c->id, &params);

    if (root) {
        add_strings(params, "triggerids", triggerids, n);
        add_strings(params, "output", out, 1);
        add_strings(params, "selectHosts", hosts, 2);
    }
    return finish(root, c);
}

char *zbx_req_host_count(const struct zbx_req_ctx *c)
{
    cJSON *params;
    cJSON *root = envelope("host.get", c->id, &params);

    if (root) {
        cJSON_AddBoolToObject(params, "countOutput", 1);
        cJSON_AddBoolToObject(params, "monitored_hosts", 1);
    }
    return finish(root, c);
}

char *zbx_req_hosts(const struct zbx_req_ctx *c, const char *hostid, int limit)
{
    static const char *const out[] = { "hostid", "host", "name", "maintenance_status",
                                       "active_available" };
    static const char *const ifs[] = { "type", "main", "available" };
    cJSON *params;
    cJSON *root = envelope("host.get", c->id, &params);

    if (root) {
        /* active_available only where it exists: older servers refuse an
         * output field they do not know. */
        add_strings(params, "output", out, c->active_available ? 5 : 4);
        add_strings(params, "selectInterfaces", ifs, 3);
        if (hostid) {
            add_strings(params, "hostids", &hostid, 1);
        } else {
            cJSON_AddBoolToObject(params, "monitored_hosts", 1);
            cJSON_AddStringToObject(params, "sortfield", "name");
            cJSON_AddNumberToObject(params, "limit", limit);
        }
    }
    return finish(root, c);
}

static const char *const host_out[] = { "hostid", "host", "name", "maintenance_status",
                                        "active_available" };
static const char *const host_ifs[] = { "type", "main", "available" };

char *zbx_req_hosts_by_id(const struct zbx_req_ctx *c, const char *const *hostids, int n)
{
    cJSON *params;
    cJSON *root = envelope("host.get", c->id, &params);

    if (root) {
        add_strings(params, "output", host_out, c->active_available ? 5 : 4);
        add_strings(params, "selectInterfaces", host_ifs, 3);
        add_strings(params, "hostids", hostids, n);
        cJSON_AddBoolToObject(params, "monitored_hosts", 1);
    }
    return finish(root, c);
}

char *zbx_req_down_interfaces(const struct zbx_req_ctx *c, int limit)
{
    static const char *const out[] = { "hostid" };
    cJSON *params;
    cJSON *root = envelope("hostinterface.get", c->id, &params);

    if (root) {
        cJSON *filter;

        add_strings(params, "output", out, 1);
        filter = cJSON_AddObjectToObject(params, "filter");
        if (filter) {
            cJSON_AddNumberToObject(filter, "available", 2);
        }
        cJSON_AddNumberToObject(params, "limit", limit);
    }
    return finish(root, c);
}

char *zbx_req_items(const struct zbx_req_ctx *c, const char *hostid, int limit)
{
    static const char *const out[] = { "itemid", "name", "key_", "lastvalue",
                                       "lastclock", "units", "value_type" };
    cJSON *params;
    cJSON *root = envelope("item.get", c->id, &params);

    if (root) {
        add_strings(params, "hostids", &hostid, 1);
        add_strings(params, "output", out, (int)(sizeof(out) / sizeof(out[0])));
        cJSON_AddBoolToObject(params, "monitored", 1);
        cJSON_AddStringToObject(params, "sortfield", "name");
        cJSON_AddNumberToObject(params, "limit", limit);
    }
    return finish(root, c);
}

/* ---- replies --------------------------------------------------------------------- */

static bool contains_ci(const char *hay, const char *needle)
{
    return hay && strcasestr(hay, needle) != NULL;
}

bool zbx_error_is_auth(long code, const char *message, const char *data)
{
    static const char *const words[] = {
        "not authori",           /* "Not authorised." (6.0), "Not authorized." (7.x) */
        "session terminated",    /* "Session terminated, re-login, please." */
        "re-login",
        "api token expired",
        "incorrect user name or password",
        "login name or password is incorrect",
        "temporarily blocked",
    };
    size_t i;

    (void)code; /* -32602 and -32500 both carry these; the text decides */
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (contains_ci(message, words[i]) || contains_ci(data, words[i])) {
            return true;
        }
    }
    return false;
}

static enum zbx_err malformed(struct zbx_reply *r, const char *why)
{
    snprintf(r->text, sizeof(r->text), "%s", why);
    r->err = ZBX_ERR_MALFORMED;
    return r->err;
}

enum zbx_err zbx_reply_parse(struct zbx_reply *r, const char *body, size_t len, int id)
{
    const cJSON *v;
    const cJSON *err;

    memset(r, 0, sizeof(*r));
    if (!body || len == 0) {
        return malformed(r, "empty answer");
    }
    r->root = cJSON_ParseWithLength(body, len);
    if (!r->root || !cJSON_IsObject(r->root)) {
        return malformed(r, "not a JSON object");
    }
    v = cJSON_GetObjectItemCaseSensitive(r->root, "jsonrpc");
    if (!cJSON_IsString(v) || strcmp(v->valuestring, "2.0") != 0) {
        return malformed(r, "not JSON-RPC 2.0");
    }
    err = cJSON_GetObjectItemCaseSensitive(r->root, "error");
    if (err) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(err, "code");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(err, "data");
        const char *m = cJSON_IsString(msg) ? msg->valuestring : "";
        const char *d = cJSON_IsString(data) ? data->valuestring : "";

        if (!cJSON_IsObject(err) || !cJSON_IsNumber(code)) {
            return malformed(r, "error without a code");
        }
        r->code = (long)code->valuedouble;
        {
            char joined[ZBX_TEXT_MAX * 2];

            snprintf(joined, sizeof(joined), "%s%s%s", m, *m && *d ? " " : "", d);
            zbx_copy_text(r->text, sizeof(r->text), joined);
        }
        r->err = zbx_error_is_auth(r->code, m, d) ? ZBX_ERR_AUTH : ZBX_ERR_API;
        return r->err;
    }
    /* An error answer may carry a null id (a request the server could not
     * read); a result must answer this request. */
    v = cJSON_GetObjectItemCaseSensitive(r->root, "id");
    if (!cJSON_IsNumber(v) || (int)v->valuedouble != id) {
        return malformed(r, "answer to another request");
    }
    r->result = cJSON_GetObjectItemCaseSensitive(r->root, "result");
    if (!r->result) {
        return malformed(r, "no result");
    }
    r->err = ZBX_ERR_NONE;
    return r->err;
}

void zbx_reply_free(struct zbx_reply *r)
{
    if (r && r->root) {
        cJSON_Delete(r->root);
    }
    if (r) {
        r->root = NULL;
        r->result = NULL;
    }
}

/* ---- members ----------------------------------------------------------------------- */

/* A member as text: strings as they are, numbers printed. NULL when absent
 * or of another type. */
static const char *member_text(const cJSON *obj, const char *name, char *tmp, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);

    if (cJSON_IsString(v) && v->valuestring) {
        return v->valuestring;
    }
    if (cJSON_IsNumber(v)) {
        snprintf(tmp, len, "%.0f", v->valuedouble);
        return tmp;
    }
    return NULL;
}

static bool member_ll(const cJSON *obj, const char *name, long long *out)
{
    char tmp[32];
    const char *s = member_text(obj, name, tmp, sizeof(tmp));
    char *end;
    long long v;

    if (!s || !*s) {
        return false;
    }
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno || *end) {
        return false;
    }
    *out = v;
    return true;
}

static bool member_id(const cJSON *obj, const char *name, char *out, size_t len)
{
    char tmp[32];
    const char *s = member_text(obj, name, tmp, sizeof(tmp));

    if (!s || !*s || strlen(s) >= len) {
        return false;
    }
    snprintf(out, len, "%s", s);
    return true;
}

/* ---- results ----------------------------------------------------------------------- */

int zbx_parse_version(const cJSON *result, char *out, size_t len)
{
    if (!cJSON_IsString(result) || zbx_version_num(result->valuestring) < 0) {
        return -1;
    }
    zbx_copy_text(out, len, result->valuestring);
    return 0;
}

int zbx_parse_login(const cJSON *result, char *session, size_t len)
{
    /* A string; with userData requested it would be an object, which this
     * client never asks for. */
    if (!cJSON_IsString(result) || !result->valuestring[0] || strlen(result->valuestring) >= len) {
        return -1;
    }
    snprintf(session, len, "%s", result->valuestring);
    return 0;
}

int zbx_parse_count(const cJSON *result, int *out)
{
    char *end;
    long long v;

    if (cJSON_IsNumber(result)) {
        v = (long long)result->valuedouble;
    } else if (cJSON_IsString(result) && result->valuestring[0]) {
        errno = 0;
        v = strtoll(result->valuestring, &end, 10);
        if (errno || *end) {
            return -1;
        }
    } else {
        return -1;
    }
    if (v < 0 || v > 100000000) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

int zbx_parse_problems(const cJSON *result, struct zbx_problem *p, int max, int *n)
{
    const cJSON *e;
    int k = 0;

    *n = 0;
    if (!cJSON_IsArray(result)) {
        return -1;
    }
    cJSON_ArrayForEach(e, result) {
        struct zbx_problem q;
        long long sev;
        long long ack;
        long long supp = 0;
        long long clock;
        char tmp[32];
        const char *name;

        if (!cJSON_IsObject(e)) {
            return -1;
        }
        memset(&q, 0, sizeof(q));
        name = member_text(e, "name", tmp, sizeof(tmp));
        if (!member_id(e, "eventid", q.eventid, sizeof(q.eventid)) ||
            !member_id(e, "objectid", q.objectid, sizeof(q.objectid)) ||
            !member_ll(e, "clock", &clock) || !member_ll(e, "severity", &sev) ||
            !member_ll(e, "acknowledged", &ack) || !name) {
            return -1;
        }
        (void)member_ll(e, "suppressed", &supp);
        if (supp != 0) {
            /* Asked for without them; dropped here too, because what
             * "suppressed": false filters is not documented (ZABBIX.md). */
            continue;
        }
        q.severity = (int8_t)zbx_severity_clamp(sev < 0 ? 0 : sev);
        q.acknowledged = ack != 0;
        q.suppressed = supp != 0;
        q.clock = clock;
        zbx_copy_text(q.name, sizeof(q.name), name);
        if (k < max) {
            p[k++] = q;
        }
        (*n)++;
    }
    return 0;
}

int zbx_parse_trigger_hosts(const cJSON *result, struct zbx_problem *p, int n)
{
    const cJSON *t;

    if (!cJSON_IsArray(result)) {
        return -1;
    }
    cJSON_ArrayForEach(t, result) {
        char tid[ZBX_ID_MAX];
        const cJSON *hosts;
        const cJSON *h;
        int i;

        if (!cJSON_IsObject(t) || !member_id(t, "triggerid", tid, sizeof(tid))) {
            return -1;
        }
        hosts = cJSON_GetObjectItemCaseSensitive(t, "hosts");
        if (!cJSON_IsArray(hosts)) {
            return -1;
        }
        h = cJSON_GetArrayItem(hosts, 0);
        if (!h) {
            continue; /* a trigger with no host the user may see */
        }
        for (i = 0; i < n; i++) {
            char tmp[32];
            const char *name;

            if (strcmp(p[i].objectid, tid) != 0) {
                continue;
            }
            name = member_text(h, "name", tmp, sizeof(tmp));
            if (!cJSON_IsObject(h) || !member_id(h, "hostid", p[i].hostid, sizeof(p[i].hostid)) ||
                !name) {
                return -1;
            }
            zbx_copy_text(p[i].host, sizeof(p[i].host), name);
        }
    }
    return 0;
}

int zbx_parse_hosts(const cJSON *result, struct zbx_host *h, int max, int *n)
{
    const cJSON *e;
    int k = 0;

    *n = 0;
    if (!cJSON_IsArray(result)) {
        return -1;
    }
    cJSON_ArrayForEach(e, result) {
        struct zbx_host x;
        char tmp[32];
        char tmp2[32];
        const char *name;
        const char *tech;
        const cJSON *ifs;
        const cJSON *itf;
        long long maint = 0;
        long long active;
        bool seen_up = false;

        if (!cJSON_IsObject(e)) {
            return -1;
        }
        memset(&x, 0, sizeof(x));
        x.max_severity = ZBX_SEV_NONE;
        if (!member_id(e, "hostid", x.hostid, sizeof(x.hostid))) {
            return -1;
        }
        name = member_text(e, "name", tmp, sizeof(tmp));
        tech = member_text(e, "host", tmp2, sizeof(tmp2));
        if (!name && !tech) {
            return -1;
        }
        /* The visible name, or the technical one when there is none. */
        zbx_copy_text(x.name, sizeof(x.name), name && *name ? name : tech);
        (void)member_ll(e, "maintenance_status", &maint);
        x.maintenance = maint == 1;
        x.avail = ZBX_AVAIL_UNKNOWN;
        ifs = cJSON_GetObjectItemCaseSensitive(e, "interfaces");
        if (ifs && !cJSON_IsArray(ifs)) {
            return -1;
        }
        cJSON_ArrayForEach(itf, ifs) {
            long long a;

            if (!cJSON_IsObject(itf) || !member_ll(itf, "available", &a)) {
                return -1;
            }
            x.avail = zbx_avail_combine(x.avail, (long)a, &seen_up);
        }
        if (member_ll(e, "active_available", &active)) {
            x.avail = zbx_avail_combine(x.avail, (long)active, &seen_up);
        }
        if (k < max) {
            h[k++] = x;
        }
        (*n)++;
    }
    return 0;
}

int zbx_parse_hostids(const cJSON *result, char (*ids)[ZBX_ID_MAX], int max, int *n, int *seen)
{
    const cJSON *e;

    *seen = 0;
    if (!cJSON_IsArray(result)) {
        return -1;
    }
    cJSON_ArrayForEach(e, result) {
        char id[ZBX_ID_MAX];
        int i;

        if (!cJSON_IsObject(e) || !member_id(e, "hostid", id, sizeof(id))) {
            return -1;
        }
        (*seen)++;
        for (i = 0; i < *n && strcmp(ids[i], id) != 0; i++) {
        }
        if (i == *n && *n < max) {
            memcpy(ids[(*n)++], id, sizeof(id));
        }
    }
    return 0;
}

/* The latest values worth a line on a small screen, best first. A key
 * matches a pattern when it is the pattern, or when the pattern has no '['
 * and the key is the pattern followed by one. These are the keys of the
 * stock "Linux by Zabbix agent", "Windows by Zabbix agent" and ICMP
 * templates. */
static const char *const key_priority[] = {
    "agent.ping",
    "icmpping",
    "system.cpu.util",
    "system.cpu.load[all,avg1]",
    "vm.memory.utilization",
    "vm.memory.util",
    "vfs.fs.dependent.size[/,pused]",
    "vfs.fs.size[/,pused]",
    "system.uptime",
    "system.net.uptime",
    "icmppingsec",
    "icmppingloss",
    "system.swap.size[,pfree]",
    "proc.num",
};
#define KEY_RANK_NONE 1000

static int key_rank(const char *key)
{
    size_t i;

    for (i = 0; key && i < sizeof(key_priority) / sizeof(key_priority[0]); i++) {
        const char *pat = key_priority[i];
        size_t n = strlen(pat);

        if (strcmp(key, pat) == 0) {
            return (int)i;
        }
        if (!strchr(pat, '[') && strncmp(key, pat, n) == 0 && key[n] == '[' &&
            strcmp(pat, "icmpping") != 0) {
            return (int)i;
        }
    }
    return KEY_RANK_NONE;
}

struct ranked_item {
    struct zbx_item item;
    int rank;
};

static int ranked_cmp(const void *pa, const void *pb)
{
    const struct ranked_item *a = pa;
    const struct ranked_item *b = pb;

    if (a->rank != b->rank) {
        return a->rank < b->rank ? -1 : 1;
    }
    return strcasecmp(a->item.name, b->item.name);
}

int zbx_parse_items(const cJSON *result, struct zbx_item *it, int max, int *n)
{
    struct ranked_item *all;
    const cJSON *e;
    int count = 0;
    int size;
    int i;

    *n = 0;
    if (!cJSON_IsArray(result)) {
        return -1;
    }
    size = cJSON_GetArraySize(result);
    if (size > ZBX_ITEM_FETCH) {
        size = ZBX_ITEM_FETCH;
    }
    all = calloc(size > 0 ? (size_t)size : 1, sizeof(*all));
    if (!all) {
        return -1;
    }
    cJSON_ArrayForEach(e, result) {
        struct ranked_item r;
        char t1[32];
        char t2[32];
        char t3[32];
        char t4[32];
        const char *name;
        const char *key;
        const char *value;
        const char *units;
        long long clock;
        long long vt = 4;

        if (!cJSON_IsObject(e)) {
            free(all);
            return -1;
        }
        memset(&r, 0, sizeof(r));
        name = member_text(e, "name", t1, sizeof(t1));
        key = member_text(e, "key_", t2, sizeof(t2));
        value = member_text(e, "lastvalue", t3, sizeof(t3));
        units = member_text(e, "units", t4, sizeof(t4));
        if (!member_id(e, "itemid", r.item.itemid, sizeof(r.item.itemid)) || !name || !key ||
            !value || !member_ll(e, "lastclock", &clock)) {
            free(all);
            return -1;
        }
        (void)member_ll(e, "value_type", &vt);
        if (clock <= 0 || count >= size) {
            continue; /* never collected: nothing to show */
        }
        r.rank = key_rank(key);
        r.item.clock = clock;
        zbx_copy_text(r.item.name, sizeof(r.item.name), name);
        zbx_copy_text(r.item.units, sizeof(r.item.units), units ? units : "");
        zbx_format_value(r.item.value, sizeof(r.item.value), key, value, units ? units : "",
                         (int)vt);
        all[count++] = r;
    }
    qsort(all, (size_t)count, sizeof(*all), ranked_cmp);
    for (i = 0; i < count && i < max; i++) {
        it[i] = all[i].item;
    }
    *n = i;
    free(all);
    return 0;
}

/* ---- values ----------------------------------------------------------------------- */

static void trim_number(char *out, size_t len, double v)
{
    double a = fabs(v);

    if (a >= 1000 || a == floor(a)) {
        snprintf(out, len, "%.0f", v);
    } else if (a >= 100) {
        snprintf(out, len, "%.1f", v);
    } else {
        snprintf(out, len, "%.2f", v);
    }
    /* "12.50" -> "12.5", "3.00" -> "3" */
    if (strchr(out, '.')) {
        size_t n = strlen(out);

        while (n > 0 && out[n - 1] == '0') {
            out[--n] = '\0';
        }
        if (n > 0 && out[n - 1] == '.') {
            out[--n] = '\0';
        }
    }
}

static void scaled(char *out, size_t len, double v, double base, const char *const *unit, int units)
{
    char num[32];
    int i = 0;

    while (fabs(v) >= base && i < units - 1) {
        v /= base;
        i++;
    }
    trim_number(num, sizeof(num), v);
    snprintf(out, len, "%s %s", num, unit[i]);
}

static bool key_is(const char *key, const char *pat)
{
    size_t n = strlen(pat);

    return key && strncmp(key, pat, n) == 0 && (key[n] == '\0' || key[n] == '[');
}

void zbx_format_value(char *out, size_t len, const char *key, const char *value,
                      const char *units, int value_type)
{
    char *end;
    double v;

    if (!out || len == 0) {
        return;
    }
    if (!value) {
        value = "";
    }
    if (value_type != 0 && value_type != 3) {
        zbx_copy_text(out, len, value); /* text, log, character */
        return;
    }
    errno = 0;
    v = strtod(value, &end);
    if (errno || end == value || *end) {
        zbx_copy_text(out, len, value);
        return;
    }
    if (key_is(key, "agent.ping") || key_is(key, "icmpping")) {
        snprintf(out, len, "%s", v >= 1 ? "Up" : "Down");
        return;
    }
    if (!units) {
        units = "";
    }
    if (strcmp(units, "B") == 0 || strcmp(units, "Bps") == 0) {
        static const char *const b[] = { "B", "KB", "MB", "GB", "TB", "PB" };
        static const char *const bps[] = { "Bps", "KBps", "MBps", "GBps", "TBps", "PBps" };

        scaled(out, len, v, 1024, units[1] ? bps : b, 6);
        return;
    }
    if (strcmp(units, "bps") == 0) {
        static const char *const bits[] = { "bps", "Kbps", "Mbps", "Gbps", "Tbps" };

        scaled(out, len, v, 1000, bits, 5);
        return;
    }
    if (strcmp(units, "uptime") == 0 || (strcmp(units, "s") == 0 && key && strstr(key, "uptime"))) {
        long long s = (long long)v;

        if (s >= 86400) {
            snprintf(out, len, "%lldd %lldh", s / 86400, (s % 86400) / 3600);
        } else if (s >= 3600) {
            snprintf(out, len, "%lldh %lldm", s / 3600, (s % 3600) / 60);
        } else {
            snprintf(out, len, "%lldm", s / 60);
        }
        return;
    }
    if (strcmp(units, "s") == 0 && fabs(v) < 1 && v != 0) {
        char num[32];

        trim_number(num, sizeof(num), v * 1000);
        snprintf(out, len, "%s ms", num);
        return;
    }
    {
        char num[32];

        trim_number(num, sizeof(num), v);
        if (units[0] && strcmp(units, "unixtime") != 0) {
            snprintf(out, len, "%s %s", num, units);
        } else {
            snprintf(out, len, "%s", num);
        }
    }
}

long long zbx_http_date(const char *s)
{
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char wday[4];
    char mon[4];
    struct tm tm;
    int d;
    int y;
    int hh;
    int mm;
    int ss;
    int i;
    time_t t;

    if (!s) {
        return -1;
    }
    while (*s == ' ') {
        s++;
    }
    if (sscanf(s, "%3s %d %3s %d %d:%d:%d GMT", wday, &d, mon, &y, &hh, &mm, &ss) != 7 ||
        strlen(wday) != 3) {
        /* "Sun," is read as "Sun" followed by a stray comma: try that form. */
        if (sscanf(s, "%3[A-Za-z], %d %3s %d %d:%d:%d GMT", wday, &d, mon, &y, &hh, &mm, &ss) != 7) {
            return -1;
        }
    }
    for (i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) {
            break;
        }
    }
    if (i == 12 || d < 1 || d > 31 || y < 1970 || y > 9999 || hh > 23 || mm > 59 || ss > 60 ||
        hh < 0 || mm < 0 || ss < 0) {
        return -1;
    }
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = i;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    t = timegm(&tm);
    return t == (time_t)-1 ? -1 : (long long)t;
}
