/*
 * The pretend Zabbix server. See zbx_fake.h.
 *
 * The data is computed, not stored: host i and problem j of a scenario come
 * from pure functions of i and j, so a 1 500-host scenario costs no memory
 * until an answer is built, and every answer is the same for the same
 * request.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "zabbix/zbx_fake.h"

#include "zabbix/zbx_api.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HOSTID_BASE 10100
#define TRIGGERID_BASE 20000
#define EVENTID_BASE 500000
#define ITEMID_BASE 40000
#define ITEMS_PER_HOST 10
#define SESSION_ID "5f2a8c01d3e94b7a9c6e0f1a2b3c4d5e"

static const char *const scenarios[] = {
    "demo", "healthy", "empty", "large", "slow", "timeout", "refused", "dns", "tls", "auth",
    "expired", "apierror", "malformed", "http500", "huge", "flap", "drop", "old", "v74", NULL,
};

const char *const *zbx_fake_scenarios(void)
{
    return scenarios;
}

bool zbx_fake_known(const char *name)
{
    int i;

    for (i = 0; name && scenarios[i]; i++) {
        if (strcmp(name, scenarios[i]) == 0) {
            return true;
        }
    }
    return false;
}

int zbx_fake_init(struct zbx_fake *f, const char *scenario)
{
    memset(f, 0, sizeof(*f));
    snprintf(f->scenario, sizeof(f->scenario), "%s", ZBX_FAKE_DEFAULT);
    return zbx_fake_set_scenario(f, scenario ? scenario : ZBX_FAKE_DEFAULT);
}

int zbx_fake_set_scenario(struct zbx_fake *f, const char *scenario)
{
    if (!zbx_fake_known(scenario)) {
        return -1;
    }
    snprintf(f->scenario, sizeof(f->scenario), "%s", scenario);
    f->requests = 0;
    f->epoch = 0;
    return 0;
}

static bool is(const struct zbx_fake *f, const char *name)
{
    return strcmp(f->scenario, name) == 0;
}

static const char *server_version(const struct zbx_fake *f)
{
    if (is(f, "old")) {
        return "6.0.48";
    }
    if (is(f, "v74")) {
        return "7.4.15";
    }
    return "7.0.31";
}

/* ---- the estate ---------------------------------------------------------------- */

struct fhost {
    int index;
    char name[160];
    int iface_type;             /* 1 agent, 2 SNMP, 0 none */
    int iface_avail;            /* 0 unknown, 1 available, 2 unavailable */
    int active;                 /* active_available */
    bool maintenance;
};

struct fprob {
    int index;
    int host;
    int severity;
    bool ack;
    long age;                   /* seconds before the epoch */
    char name[480];
};

static const char *const demo_hosts[] = {
    "app-ams-01", "app-ams-02", "app-osl-01", "backup-nyc-01", "backup-nyc-02",
    "cache-ber-01", "cache-osl-01", "db-ams-01", "db-osl-01", "db-osl-02",
    "dns-osl-01", "dns-osl-02", "edge-ams-01", "edge-osl-01", "k8s-node-ams-01",
    "k8s-node-ams-02", "k8s-node-ams-03", "k8s-node-ams-04", "mail-osl-01", "monitor-osl-01",
    "nas-osl-01", "ntp-osl-01", "printer-osl-01", "proxy-ber-01", "vpn-osl-01",
    "web-ams-01", "web-ams-02", "web-ber-01", "web-ber-02", "web-ber-03",
    "web-nyc-01", "web-osl-01", "web-osl-02", "web-osl-03", "wifi-ctrl-osl-01",
    "Zabbix server",
};
#define DEMO_HOSTS ((int)(sizeof(demo_hosts) / sizeof(demo_hosts[0])))

/* The demo's problems, oldest first (so eventids grow with newness). */
static const struct {
    long age;
    int severity;
    bool ack;
    const char *host;
    const char *name;
} demo_problems[] = {
    { 259200, 2, true, "web-osl-02", "Cert: SSL certificate expires soon (less than 14 days)" },
    { 93600, 2, false, "backup-nyc-01", "Load average is too high (per CPU load over 1.5 for 5m)" },
    { 18000, 3, true, "mail-osl-01", "/var: Disk space is low (used > 90%)" },
    { 7200, 3, false, "app-ams-01", "Zabbix agent is not available (for 3m)" },
    { 3780, 4, false, "web-ber-03", "High CPU utilization (over 90% for 5m)" },
    { 1500, 2, false, "k8s-node-ams-04", "Interface eth1: Link down" },
    { 720, 4, true, "db-osl-02", "MySQL: Service is down" },
    { 360, 1, false, "ntp-osl-01", "Host has been restarted (uptime < 10m)" },
    { 240, 5, false, "edge-osl-01", "Unavailable by ICMP ping" },
};
#define DEMO_PROBLEMS ((int)(sizeof(demo_problems) / sizeof(demo_problems[0])))

#define LARGE_HOSTS 1500
#define LARGE_PROBLEMS 1200
/* The host with the long name: the host of problem 1191, a recent disaster
 * ((1191 * 7) % 1500), so it is among the hosts and problems a viewer keeps.
 * Every problem j with j % 40 == 11 is a disaster with a long name. */
#define LONG_HOST 837

static int n_hosts(const struct zbx_fake *f)
{
    if (is(f, "empty")) {
        return 0;
    }
    if (is(f, "healthy")) {
        return 24;
    }
    if (is(f, "large")) {
        return LARGE_HOSTS;
    }
    return DEMO_HOSTS;
}

static int n_problems(const struct zbx_fake *f)
{
    if (is(f, "empty") || is(f, "healthy")) {
        return 0;
    }
    if (is(f, "large")) {
        return LARGE_PROBLEMS;
    }
    return DEMO_PROBLEMS;
}

static void host_at(const struct zbx_fake *f, int i, struct fhost *h)
{
    static const char *const roles[] = { "web", "db", "cache", "app", "edge", "mail",
                                         "dns", "backup", "k8s-node", "vpn", "proxy", "nas" };
    static const char *const sites[] = { "osl", "ber", "ams", "nyc", "sin", "syd" };

    memset(h, 0, sizeof(*h));
    h->index = i;
    h->iface_type = 1;
    h->iface_avail = 1;
    h->active = 1;
    if (is(f, "large")) {
        if (i == LONG_HOST) {
            snprintf(h->name, sizeof(h->name), "%s",
                     "very-long-hostname-for-testing-truncation-in-the-doors-zabbix-viewer-"
                     "number-837.datacenter-north.example.internal");
        } else {
            snprintf(h->name, sizeof(h->name), "%s-%s-%04d", roles[i % 12], sites[(i / 12) % 6], i);
        }
        if (i % 40 == 3) {
            h->iface_avail = 2;
            h->active = 2;
        } else if (i % 55 == 5) {
            h->iface_avail = 0;
            h->active = 0;
        }
        h->maintenance = i % 90 == 9;
        return;
    }
    snprintf(h->name, sizeof(h->name), "%s", demo_hosts[i]);
    if (is(f, "healthy")) {
        return;
    }
    if (strcmp(h->name, "edge-osl-01") == 0) {
        h->iface_type = 2; /* SNMP: no agent, so nothing active either */
        h->iface_avail = 2;
        h->active = 0;
    } else if (strcmp(h->name, "app-ams-01") == 0) {
        h->iface_avail = 2;
        h->active = 2;
    } else if (strcmp(h->name, "printer-osl-01") == 0) {
        h->iface_type = 2;
        h->iface_avail = 0;
        h->active = 0;
    } else if (strcmp(h->name, "backup-nyc-02") == 0) {
        h->maintenance = true;
    }
}

static int demo_host_index(const char *name)
{
    int i;

    for (i = 0; i < DEMO_HOSTS; i++) {
        if (strcmp(demo_hosts[i], name) == 0) {
            return i;
        }
    }
    return 0;
}

static void problem_at(const struct zbx_fake *f, int j, struct fprob *p)
{
    memset(p, 0, sizeof(*p));
    p->index = j;
    if (is(f, "large")) {
        static const int sev[20] = { 2, 2, 3, 1, 2, 4, 0, 2, 3, 2, 1, 5, 2, 3, 4, 2, 1, 3, 2, 2 };
        static const char *const names[] = {
            "High CPU utilization (over 90% for 5m)",
            "Zabbix agent is not available (for 3m)",
            "/: Disk space is low (used > 80%)",
            "Interface eth0: Link down",
            "Load average is too high (per CPU load over 1.5 for 5m)",
            "Lack of available memory (< 20M of 7.7 GB)",
            "Unavailable by ICMP ping",
            "High ICMP ping loss",
            "Nginx: Service is down",
            "Host has been restarted (uptime < 10m)",
        };
        struct fhost h;

        p->host = (j * 7) % LARGE_HOSTS;
        host_at(f, p->host, &h);
        p->severity = sev[j % 20];
        p->ack = j % 3 == 0;
        p->age = (long)(LARGE_PROBLEMS - j) * 97 + 30;
        if (j % 40 == 11) {
            snprintf(p->name, sizeof(p->name),
                     "Very long trigger name number %d on %.100s to prove that names longer than any "
                     "row can hold are cut at a character boundary and still end in dots rather "
                     "than in the middle of a multi-byte character such as \xc3\xa6\xc3\xb8\xc3\xa5 "
                     "or overflow into the next field of the protocol line",
                     j, h.name);
        } else {
            snprintf(p->name, sizeof(p->name), "%s", names[j % 10]);
        }
        return;
    }
    p->host = demo_host_index(demo_problems[j].host);
    p->severity = demo_problems[j].severity;
    p->ack = demo_problems[j].ack;
    p->age = demo_problems[j].age;
    snprintf(p->name, sizeof(p->name), "%s", demo_problems[j].name);
}

/* ---- answering ---------------------------------------------------------------------- */

static char *print(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return s;
}

static cJSON *reply(const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();

    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (cJSON_IsNumber(id)) {
        cJSON_AddNumberToObject(r, "id", id->valuedouble);
    } else {
        cJSON_AddNullToObject(r, "id");
    }
    return r;
}

static char *error(const cJSON *id, int code, const char *message, const char *data)
{
    cJSON *r = reply(id);
    cJSON *e = cJSON_AddObjectToObject(r, "error");

    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON_AddStringToObject(e, "data", data);
    return print(r);
}

static char *not_authorized(const struct zbx_fake *f, const cJSON *id)
{
    /* 6.0 spells it the British way. */
    return error(id, -32602, "Invalid params.", is(f, "old") ? "Not authorised." : "Not authorized.");
}

static void add_str(cJSON *o, const char *k, const char *v)
{
    cJSON_AddStringToObject(o, k, v);
}

static void add_num(cJSON *o, const char *k, long long v)
{
    char s[24];

    snprintf(s, sizeof(s), "%lld", v);
    cJSON_AddStringToObject(o, k, s);
}

static bool id_in(const cJSON *arr, long long id)
{
    const cJSON *e;

    if (!arr) {
        return true;
    }
    cJSON_ArrayForEach(e, arr) {
        long long v = -1;

        if (cJSON_IsString(e)) {
            v = atoll(e->valuestring);
        } else if (cJSON_IsNumber(e)) {
            v = (long long)e->valuedouble;
        }
        if (v == id) {
            return true;
        }
    }
    return false;
}

static bool in_ints(const cJSON *arr, int v)
{
    return id_in(arr, v);
}

static bool flag(const cJSON *params, const char *name)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, name);

    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0) ||
           (cJSON_IsString(v) && strcmp(v->valuestring, "0") != 0 && v->valuestring[0]);
}

static long limit_of(const cJSON *params)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "limit");

    return cJSON_IsNumber(v) && v->valuedouble > 0 ? (long)v->valuedouble : -1;
}

/* 7.0 validates problem.get strictly: a parameter it does not know is an
 * error, which is what keeps the requests of zbx_api.c honest. */
static const char *unknown_param(const cJSON *params, const char *const *allowed)
{
    const cJSON *e;

    cJSON_ArrayForEach(e, params) {
        int i;
        bool ok = false;

        for (i = 0; allowed[i]; i++) {
            if (strcmp(e->string, allowed[i]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return e->string;
        }
    }
    return NULL;
}

static char *problem_get(struct zbx_fake *f, const cJSON *id, const cJSON *params)
{
    static const char *const allowed[] = {
        "eventids", "groupids", "hostids", "objectids", "source", "object", "acknowledged",
        "suppressed", "symptom", "severities", "evaltype", "tags", "recent", "eventid_from",
        "eventid_till", "time_from", "time_till", "selectAcknowledges", "selectTags",
        "selectSuppressionData", "countOutput", "editable", "excludeSearch", "filter", "limit",
        "output", "preservekeys", "search", "searchByAny", "searchWildcardsEnabled", "sortfield",
        "sortorder", "startSearch", NULL,
    };
    const cJSON *hostids = cJSON_GetObjectItemCaseSensitive(params, "hostids");
    const cJSON *sevs = cJSON_GetObjectItemCaseSensitive(params, "severities");
    const char *bad = unknown_param(params, allowed);
    long limit = limit_of(params);
    int n = n_problems(f);
    cJSON *r;
    cJSON *arr;
    int j;
    int count = 0;

    if (bad) {
        char data[160];

        snprintf(data, sizeof(data), "Invalid parameter \"/\": unexpected parameter \"%s\".", bad);
        return error(id, -32602, "Invalid params.", data);
    }
    if (is(f, "apierror")) {
        return error(id, -32500, "Application error.", "No permissions to call \"problem.get\".");
    }
    if (is(f, "malformed")) {
        return strdup("{\"jsonrpc\":\"2.0\",\"result\":[{\"eventid\":\"1\",\"objectid\":\"2\",\"cl");
    }
    r = reply(id);
    if (flag(params, "countOutput")) {
        const cJSON *ack = cJSON_GetObjectItemCaseSensitive(params, "acknowledged");
        bool unack_only = ack && !flag(params, "acknowledged");

        for (j = 0; j < n; j++) {
            struct fprob p;

            problem_at(f, j, &p);
            if (id_in(hostids, HOSTID_BASE + p.host) && (!sevs || in_ints(sevs, p.severity)) &&
                (!unack_only || !p.ack)) {
                count++;
            }
        }
        add_num(r, "result", count);
        return print(r);
    }
    arr = cJSON_AddArrayToObject(r, "result");
    if (is(f, "huge")) {
        /* Past ZBX_RESPONSE_MAX however it is counted. */
        for (j = 0; j < 14000; j++) {
            cJSON *o = cJSON_CreateObject();

            add_num(o, "eventid", EVENTID_BASE + j);
            add_str(o, "name", "padding padding padding padding padding padding padding padding "
                               "padding padding padding padding padding padding padding padding "
                               "padding padding padding padding padding padding padding padding");
            cJSON_AddItemToArray(arr, o);
        }
        return print(r);
    }
    /* Newest (highest eventid) first, as sortorder DESC asks. */
    for (j = n - 1; j >= 0 && (limit < 0 || count < limit); j--) {
        struct fprob p;
        cJSON *o;

        problem_at(f, j, &p);
        if (!id_in(hostids, HOSTID_BASE + p.host) || (sevs && !in_ints(sevs, p.severity))) {
            continue;
        }
        o = cJSON_CreateObject();
        add_num(o, "eventid", EVENTID_BASE + j);
        add_num(o, "objectid", TRIGGERID_BASE + j);
        add_num(o, "clock", f->epoch - p.age > 0 ? f->epoch - p.age : 1);
        add_str(o, "name", p.name);
        add_num(o, "severity", p.severity);
        add_num(o, "acknowledged", p.ack ? 1 : 0);
        add_num(o, "suppressed", 0);
        cJSON_AddItemToArray(arr, o);
        count++;
    }
    return print(r);
}

static char *trigger_get(struct zbx_fake *f, const cJSON *id, const cJSON *params)
{
    const cJSON *tids = cJSON_GetObjectItemCaseSensitive(params, "triggerids");
    int n = n_problems(f);
    cJSON *r = reply(id);
    cJSON *arr = cJSON_AddArrayToObject(r, "result");
    int j;

    for (j = 0; j < n; j++) {
        struct fprob p;
        struct fhost h;
        cJSON *o;
        cJSON *hs;
        cJSON *ho;

        if (!id_in(tids, TRIGGERID_BASE + j)) {
            continue;
        }
        problem_at(f, j, &p);
        host_at(f, p.host, &h);
        o = cJSON_CreateObject();
        add_num(o, "triggerid", TRIGGERID_BASE + j);
        hs = cJSON_AddArrayToObject(o, "hosts");
        ho = cJSON_CreateObject();
        add_num(ho, "hostid", HOSTID_BASE + p.host);
        add_str(ho, "name", h.name);
        cJSON_AddItemToArray(hs, ho);
        cJSON_AddItemToArray(arr, o);
    }
    return print(r);
}

static int name_cmp(const void *a, const void *b, void *f)
{
    struct fhost ha;
    struct fhost hb;

    host_at(f, *(const int *)a, &ha);
    host_at(f, *(const int *)b, &hb);
    return strcasecmp(ha.name, hb.name);
}

static cJSON *host_object(const struct zbx_fake *f, const struct fhost *h, bool active)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *ifs;

    add_num(o, "hostid", HOSTID_BASE + h->index);
    add_str(o, "host", h->name);
    add_str(o, "name", h->name);
    add_num(o, "maintenance_status", h->maintenance ? 1 : 0);
    if (active) {
        add_num(o, "active_available", h->active);
    }
    ifs = cJSON_AddArrayToObject(o, "interfaces");
    if (h->iface_type) {
        cJSON *i = cJSON_CreateObject();

        add_num(i, "type", h->iface_type);
        add_num(i, "main", 1);
        add_num(i, "available", h->iface_avail);
        cJSON_AddItemToArray(ifs, i);
    }
    (void)f;
    return o;
}

static bool output_has(const cJSON *params, const char *field)
{
    const cJSON *out = cJSON_GetObjectItemCaseSensitive(params, "output");
    const cJSON *e;

    cJSON_ArrayForEach(e, out) {
        if (cJSON_IsString(e) && strcmp(e->valuestring, field) == 0) {
            return true;
        }
    }
    return false;
}

static char *host_get(struct zbx_fake *f, const cJSON *id, const cJSON *params)
{
    const cJSON *hostids = cJSON_GetObjectItemCaseSensitive(params, "hostids");
    bool active = output_has(params, "active_available");
    long limit = limit_of(params);
    int n = n_hosts(f);
    cJSON *r;
    cJSON *arr;
    int *order;
    int i;
    int count = 0;

    if (active && zbx_version_num(server_version(f)) < ZBX_API_ACTIVE_AVAIL_VERSION) {
        return error(id, -32602, "Invalid params.",
                     "Invalid parameter \"/output/5\": value must be one of \"hostid\", "
                     "\"proxy_hostid\", \"host\", \"status\", \"ipmi_authtype\", ...");
    }
    r = reply(id);
    if (flag(params, "countOutput")) {
        add_num(r, "result", n);
        return print(r);
    }
    arr = cJSON_AddArrayToObject(r, "result");
    order = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!order) {
        cJSON_Delete(r);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        order[i] = i;
    }
    qsort_r(order, (size_t)n, sizeof(int), name_cmp, f);
    for (i = 0; i < n && (limit < 0 || count < limit); i++) {
        struct fhost h;

        if (!id_in(hostids, HOSTID_BASE + order[i])) {
            continue;
        }
        host_at(f, order[i], &h);
        cJSON_AddItemToArray(arr, host_object(f, &h, active));
        count++;
    }
    free(order);
    return print(r);
}

/* hostinterface.get: only what the viewer asks - the hostids of interfaces
 * filtered on "available" (2 unavailable, 1 available, 0 unknown). */
static char *hostinterface_get(struct zbx_fake *f, const cJSON *id, const cJSON *params)
{
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(params, "filter");
    const cJSON *av = cJSON_GetObjectItemCaseSensitive(filter, "available");
    long limit = limit_of(params);
    long want = -1;
    int n = n_hosts(f);
    cJSON *r = reply(id);
    cJSON *arr = cJSON_AddArrayToObject(r, "result");
    int count = 0;
    int i;

    if (cJSON_IsNumber(av)) {
        want = (long)av->valuedouble;
    } else if (cJSON_IsString(av)) {
        want = atol(av->valuestring);
    }
    for (i = 0; i < n && (limit < 0 || count < limit); i++) {
        struct fhost h;
        cJSON *o;

        host_at(f, i, &h);
        if (!h.iface_type || (want >= 0 && h.iface_avail != want)) {
            continue;
        }
        o = cJSON_CreateObject();
        add_num(o, "hostid", HOSTID_BASE + i);
        cJSON_AddItemToArray(arr, o);
        count++;
    }
    return print(r);
}

static char *item_get(struct zbx_fake *f, const cJSON *id, const cJSON *params)
{
    const cJSON *hostids = cJSON_GetObjectItemCaseSensitive(params, "hostids");
    int n = n_hosts(f);
    cJSON *r = reply(id);
    cJSON *arr = cJSON_AddArrayToObject(r, "result");
    int i;

    for (i = 0; i < n; i++) {
        struct fhost h;
        long long hid = HOSTID_BASE + i;
        bool down;
        char v[10][48];
        const char *key[10] = { "agent.ping", "system.cpu.util", "system.cpu.load[all,avg1]",
                                "vm.memory.utilization", "vfs.fs.dependent.size[/,pused]",
                                "system.uptime", "net.if.in[\"eth0\"]", "icmppingsec",
                                "system.hostname", "vm.memory.size[total]" };
        const char *name[10] = { "Zabbix agent ping", "CPU utilization", "Load average (1m avg)",
                                 "Memory utilization", "/: Space utilization", "System uptime",
                                 "Interface eth0: Bits received", "ICMP response time",
                                 "System name", "Total memory" };
        const char *units[10] = { "", "%", "", "%", "%", "uptime", "bps", "s", "", "B" };
        const int vt[10] = { 3, 0, 0, 0, 0, 3, 3, 0, 1, 3 };
        int k;

        if (!hostids || !id_in(hostids, hid)) {
            continue;
        }
        host_at(f, i, &h);
        down = h.iface_avail == 2;
        snprintf(v[0], sizeof(v[0]), "%d", down ? 0 : 1);
        snprintf(v[1], sizeof(v[1]), "%d.%d", 5 + (int)(hid * 37 % 90), (int)(hid % 10));
        snprintf(v[2], sizeof(v[2]), "%.2f", (double)(hid % 7) * 0.37);
        snprintf(v[3], sizeof(v[3]), "%d.%d", 30 + (int)(hid * 13 % 60), (int)(hid % 7));
        snprintf(v[4], sizeof(v[4]), "%d.%d", 20 + (int)(hid * 7 % 70), (int)(hid % 9));
        snprintf(v[5], sizeof(v[5]), "%lld", 86400LL * (hid % 40) + 3600LL * (hid % 24));
        snprintf(v[6], sizeof(v[6]), "%lld", hid * 12345 % 90000000);
        snprintf(v[7], sizeof(v[7]), "%.4f", 0.0012 + (double)(hid % 9) * 0.0007);
        snprintf(v[8], sizeof(v[8]), "%.47s", h.name);
        snprintf(v[9], sizeof(v[9]), "%lld", 8589934592LL);
        for (k = 0; k < ITEMS_PER_HOST; k++) {
            cJSON *o = cJSON_CreateObject();

            add_num(o, "itemid", ITEMID_BASE + (long long)i * ITEMS_PER_HOST + k);
            add_str(o, "name", name[k]);
            add_str(o, "key_", key[k]);
            add_str(o, "lastvalue", down && k > 0 && k < 8 ? "0" : v[k]);
            /* A host that is down stopped reporting a while ago. */
            add_num(o, "lastclock", down && k > 0 && k < 8 ? 0 : f->epoch - 30 - k);
            add_str(o, "units", units[k]);
            add_num(o, "value_type", vt[k]);
            cJSON_AddItemToArray(arr, o);
        }
    }
    return print(r);
}

/* The credential a server of this version reads, or NULL. The header wins
 * from 6.4; before that only the member counts. */
static const char *credential(const struct zbx_fake *f, const char *bearer, const cJSON *auth)
{
    long v = zbx_version_num(server_version(f));
    const char *member = cJSON_IsString(auth) ? auth->valuestring : NULL;

    if (v >= ZBX_API_BEARER_VERSION && bearer && *bearer) {
        return bearer;
    }
    return member && *member ? member : NULL;
}

static char *answer_rpc(struct zbx_fake *f, const char *bearer, const char *body)
{
    cJSON *req = cJSON_Parse(body ? body : "");
    const cJSON *id;
    const cJSON *method;
    const cJSON *params;
    const cJSON *auth;
    long v = zbx_version_num(server_version(f));
    const char *m;
    char *out;

    if (!req || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        return error(NULL, -32700, "Parse error.", "Invalid JSON. An error occurred on the server "
                                                   "while parsing the JSON text.");
    }
    id = cJSON_GetObjectItemCaseSensitive(req, "id");
    method = cJSON_GetObjectItemCaseSensitive(req, "method");
    params = cJSON_GetObjectItemCaseSensitive(req, "params");
    auth = cJSON_GetObjectItemCaseSensitive(req, "auth");
    m = cJSON_IsString(method) ? method->valuestring : "";

    if (auth && v >= ZBX_API_NO_AUTH_MEMBER_VERSION) {
        out = error(id, -32602, "Invalid params.",
                    "Invalid parameter \"/\": unexpected parameter \"auth\".");
    } else if (strcmp(m, "apiinfo.version") == 0) {
        if (auth) {
            out = error(id, -32602, "Invalid params.",
                        "The \"apiinfo.version\" method must be called without the \"auth\" "
                        "parameter.");
        } else if (bearer && v >= 70400) {
            out = error(id, -32602, "Invalid params.",
                        "The \"apiinfo.version\" method must be called without authorization "
                        "header.");
        } else {
            cJSON *r = reply(id);

            cJSON_AddStringToObject(r, "result", server_version(f));
            out = print(r);
        }
    } else if (strcmp(m, "user.login") == 0) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(params, "username");

        if (!u && v < ZBX_API_BEARER_VERSION) {
            u = cJSON_GetObjectItemCaseSensitive(params, "user");
        }
        if (is(f, "auth") || !cJSON_IsString(u) || strcmp(u->valuestring, ZBX_FAKE_USER) != 0) {
            out = error(id, -32500, "Application error.",
                        "Incorrect user name or password or account is temporarily blocked.");
        } else {
            cJSON *r = reply(id);

            cJSON_AddStringToObject(r, "result", SESSION_ID);
            out = print(r);
        }
    } else if (!credential(f, bearer, auth) || is(f, "auth")) {
        out = not_authorized(f, id);
    } else if (is(f, "expired")) {
        out = error(id, -32500, "Application error.", "API token expired.");
    } else if (!cJSON_IsObject(params)) {
        out = error(id, -32602, "Invalid params.", "Invalid parameter \"/\": an array is expected.");
    } else if (strcmp(m, "problem.get") == 0) {
        out = problem_get(f, id, params);
    } else if (strcmp(m, "trigger.get") == 0) {
        out = trigger_get(f, id, params);
    } else if (strcmp(m, "host.get") == 0) {
        out = host_get(f, id, params);
    } else if (strcmp(m, "item.get") == 0) {
        out = item_get(f, id, params);
    } else if (strcmp(m, "hostinterface.get") == 0) {
        out = hostinterface_get(f, id, params);
    } else {
        out = error(id, -32601, "Method not found.", "Incorrect API \"x\".");
    }
    cJSON_Delete(req);
    return out;
}

void zbx_fake_answer(struct zbx_fake *f, const char *bearer, const char *body, int64_t now,
                     struct zbx_fake_answer *a)
{
    unsigned r = f->requests++;

    memset(a, 0, sizeof(*a));
    if (f->epoch == 0) {
        f->epoch = now > 0 ? now : 1;
    }
    if (is(f, "timeout")) {
        a->fault = ZBX_FAKE_HANG;
        return;
    }
    if (is(f, "refused") || (is(f, "flap") && r % 16 >= 12) || (is(f, "drop") && r >= 12)) {
        a->fault = ZBX_FAKE_REFUSED;
        return;
    }
    if (is(f, "dns")) {
        a->fault = ZBX_FAKE_DNS;
        return;
    }
    if (is(f, "tls")) {
        a->fault = ZBX_FAKE_TLS;
        return;
    }
    if (is(f, "slow")) {
        a->delay_ms = 4000;
    }
    if (is(f, "http500")) {
        a->status = 500;
        a->body = strdup("<html><body><h1>500 Internal Server Error</h1></body></html>");
    } else {
        a->status = 200;
        a->body = answer_rpc(f, bearer, body);
    }
    a->len = a->body ? strlen(a->body) : 0;
    if (!a->body) {
        a->status = 500;
    }
}

/* ---- as a transport ------------------------------------------------------------------ */

static void sleep_ms(long ms)
{
    struct timespec ts;

    if (ms <= 0) {
        return;
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static enum zbx_err fake_post(struct zbx_transport *t, const struct zbx_http_req *req,
                              struct zbx_http_resp *resp)
{
    struct zbx_fake *f = t->ctx;
    struct zbx_fake_answer a;
    size_t max = req->max_bytes ? req->max_bytes : ZBX_RESPONSE_MAX;

    memset(resp, 0, sizeof(*resp));
    resp->date = -1;
    zbx_fake_answer(f, req->bearer, req->body, (int64_t)time(NULL), &a);
    switch (a.fault) {
    case ZBX_FAKE_HANG:
        if (f->realtime) {
            sleep_ms(req->timeout_ms);
        }
        resp->err = ZBX_ERR_TIMEOUT;
        snprintf(resp->text, sizeof(resp->text), "Operation timed out after %d milliseconds",
                 req->timeout_ms);
        return resp->err;
    case ZBX_FAKE_REFUSED:
        resp->err = ZBX_ERR_CONNECT;
        snprintf(resp->text, sizeof(resp->text), "Failed to connect: Connection refused");
        return resp->err;
    case ZBX_FAKE_DNS:
        resp->err = ZBX_ERR_DNS;
        snprintf(resp->text, sizeof(resp->text), "Could not resolve host: zabbix.invalid");
        return resp->err;
    case ZBX_FAKE_TLS:
        resp->err = ZBX_ERR_TLS;
        snprintf(resp->text, sizeof(resp->text),
                 "SSL certificate problem: self-signed certificate");
        return resp->err;
    case ZBX_FAKE_OK:
    default:
        break;
    }
    if (f->realtime) {
        sleep_ms(a.delay_ms < req->timeout_ms ? a.delay_ms : req->timeout_ms);
    }
    if (a.delay_ms >= req->timeout_ms) {
        free(a.body);
        resp->err = ZBX_ERR_TIMEOUT;
        snprintf(resp->text, sizeof(resp->text), "Operation timed out after %d milliseconds",
                 req->timeout_ms);
        return resp->err;
    }
    resp->status = a.status;
    resp->date = f->epoch > 1 ? (long long)time(NULL) : -1;
    if (a.len > max) {
        free(a.body);
        resp->err = ZBX_ERR_TOO_LARGE;
        snprintf(resp->text, sizeof(resp->text), "answer over %zu KB", max / 1024);
        return resp->err;
    }
    if (a.status != 200) {
        free(a.body);
        resp->err = ZBX_ERR_HTTP;
        snprintf(resp->text, sizeof(resp->text), "HTTP %d", a.status);
        return resp->err;
    }
    resp->body = a.body;
    resp->len = a.len;
    resp->err = ZBX_ERR_NONE;
    return resp->err;
}

static void fake_close(struct zbx_transport *t)
{
    (void)t;
}

void zbx_transport_fake(struct zbx_transport *t, struct zbx_fake *f)
{
    memset(t, 0, sizeof(*t));
    t->ctx = f;
    t->post = fake_post;
    t->close = fake_close;
    t->name = "fake";
}
