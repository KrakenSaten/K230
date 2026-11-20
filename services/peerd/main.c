/*
 * peerd: authenticated, sealed datagrams between Doors devices on an IP
 * network (docs/decisions/ADR-017, docs/api/peer.md). It is to the local
 * network what meshcored's app datagrams are to LoRa: it carries opaque
 * packets between identities and knows nothing about what is in them.
 *
 * It is silent and closed until asked: no socket is bound while it is not
 * visible and has no kept peer. A state it cannot trust (peer_store.h) is a
 * named fault, and it then serves peer.status and nothing else.
 *
 * This file is the pocketipc surface and the loop. The engine is
 * peer_core.c, the sockets peer_net.c, the files peer_store.c, and
 * libsodium is called from peer_crypto.c alone.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "peer_core.h"
#include "peer_net.h"
#include "peer_store.h"
#include "pocketipc/pocketipc.h"
#include "pocketipc/server.h"
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PEERD_API_VERSION 0
#define PEERD_DEFAULT_PORT 47230
#define PEERD_EST_TIMEOUT_MS 500
#define PEERD_TABLES_MAX 16
#define IFACE_REFRESH_MS 5000
#define SOCKET_RETRY_MS 2000
#define RX_BATCH 64

struct app {
    struct pocketipc_server *server;
    struct pcore *core;
    struct ps_boot boot;
    char dir[400];
    char run_id[17];
    uint16_t port;
    int fd;
    int64_t next_open;
    int64_t next_ifaces;
    struct pcore_iface ifs[PCORE_IFACES];
    int n_ifs;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- hex and addresses ----------------------------------------------------------- */

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/* Exactly max_bytes..min_bytes of hex. Returns the byte count, or -1. */
static int from_hex(const char *in, uint8_t *out, size_t min_bytes, size_t max_bytes)
{
    size_t len = strlen(in);
    size_t i;

    if (len % 2 != 0 || len / 2 < min_bytes || len / 2 > max_bytes) {
        return -1;
    }
    for (i = 0; i < len / 2; i++) {
        int hi = hex_val(in[2 * i]);
        int lo = hex_val(in[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(len / 2);
}

static void addr_text(uint32_t a, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255),
             (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

/* Dotted IPv4: four decimal numbers of one to three digits, three dots, and
 * nothing else - no sign, no space, no leading or trailing anything. */
static int addr_parse(const char *s, uint32_t *out)
{
    uint32_t addr = 0;
    int part;

    if (!s) {
        return -1;
    }
    for (part = 0; part < 4; part++) {
        unsigned v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (unsigned)(*s - '0');
            s++;
            digits++;
        }
        if (digits == 0 || v > 255 || *s != (part < 3 ? '.' : '\0')) {
            return -1;
        }
        s++;
        addr = (addr << 8) | v;
    }
    *out = addr;
    return 0;
}

/* ---- the engine's callbacks --------------------------------------------------------- */

static int cb_send(void *user, uint32_t dst, uint16_t port, const uint8_t *buf, size_t n, int bcast)
{
    struct app *a = user;

    return pnet_send(a->fd, dst, port, buf, n, bcast);
}

static uint32_t cb_source_for(void *user, uint32_t dst)
{
    struct app *a = user;

    return pnet_source_for(dst, a->port);
}

static int cb_kept_save(void *user, const struct ps_kept *recs, int n)
{
    struct app *a = user;

    return ps_kept_save(a->dir, recs, n);
}

static cJSON *dgram_json(const struct pcore_dgram *d)
{
    cJSON *o = cJSON_CreateObject();
    char key[2 * PC_KEY + 1];
    char *hex = malloc(2 * (size_t)d->len + 1);

    to_hex(d->from, PC_KEY, key);
    cJSON_AddNumberToObject(o, "id", (double)d->id);
    cJSON_AddNumberToObject(o, "port", d->port);
    cJSON_AddStringToObject(o, "from", key);
    if (hex) {
        to_hex(d->data, d->len, hex);
        cJSON_AddStringToObject(o, "payload_hex", hex);
        free(hex);
    }
    cJSON_AddStringToObject(o, "route", "lan");
    cJSON_AddNumberToObject(o, "mono_ms", (double)d->mono_ms);
    return o;
}

static void cb_delivered(void *user, const struct pcore_dgram *d)
{
    struct app *a = user;

    if (a->server) {
        pocketipc_server_broadcast(a->server, pocketipc_event("peer.app", dgram_json(d)));
    }
}

/* ---- requests ------------------------------------------------------------------------- */

static void reply_error(struct app *a, struct pocketipc_client *c, const cJSON *id, int code,
                        const char *msg)
{
    pocketipc_server_reply(a->server, c, pocketipc_error_response(id, code, msg));
}

static const cJSON *param(const cJSON *params, const char *key)
{
    return params && cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, key) : NULL;
}

/* Every key of params must be one of allowed: a request that is not fully
 * understood is not acted on. */
static int only_keys(const cJSON *params, const char *const allowed[], char *bad, size_t n)
{
    const cJSON *item;

    if (!params || cJSON_IsNull(params)) {
        return 0;
    }
    if (!cJSON_IsObject(params)) {
        snprintf(bad, n, "params must be an object");
        return -1;
    }
    cJSON_ArrayForEach(item, params) {
        size_t i;
        int ok = 0;

        for (i = 0; allowed[i]; i++) {
            ok |= item->string && strcmp(item->string, allowed[i]) == 0;
        }
        if (!ok) {
            snprintf(bad, n, "unknown parameter %.40s", item->string ? item->string : "?");
            return -1;
        }
    }
    return 0;
}

/* An integer in lo..hi. 0, or -1 when absent, not a whole number or out of range. */
static int int_param(const cJSON *params, const char *key, long lo, long hi, long *out)
{
    const cJSON *v = param(params, key);

    /* The range is checked on the double, before any conversion: a number
     * no long can hold (1e300, an infinity, a NaN) must not reach the cast. */
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= (double)lo && v->valuedouble <= (double)hi)) {
        return -1;
    }
    *out = (long)v->valuedouble;
    return (double)*out == v->valuedouble ? 0 : -1;
}

static int key_param(const cJSON *params, const char *name, uint8_t key[PC_KEY])
{
    const cJSON *v = param(params, name);

    return cJSON_IsString(v) && from_hex(v->valuestring, key, PC_KEY, PC_KEY) == PC_KEY ? 0 : -1;
}

/* What a failed engine call is, as a pocketipc error. */
static int rc_error(int rc, const char **msg)
{
    switch (rc) {
    case PCORE_E_PARAM:
        *msg = "a value out of range, this device's own key, or a key that cannot be used";
        return POCKETIPC_ERR_INVALID_PARAMS;
    case PCORE_E_UNKNOWN:
        *msg = "not a node this service holds";
        return POCKETIPC_ERR_INVALID_PARAMS;
    case PCORE_E_NO_ROUTE:
        *msg = "no address for that node yet; it is being looked for";
        return POCKETIPC_ERR_BUSY;
    case PCORE_E_FULL:
        *msg = "no room: four kept peers or four probes are the most";
        return POCKETIPC_ERR_POLICY;
    case PCORE_E_CLOSED:
        *msg = "not visible, and that node is not a kept peer";
        return POCKETIPC_ERR_POLICY;
    case PCORE_E_STORE:
        *msg = "the kept list could not be written";
        return POCKETIPC_ERR_BACKEND;
    default:
        *msg = "the datagram could not be sent";
        return POCKETIPC_ERR_BACKEND;
    }
}

static void fingerprint(const uint8_t key[PC_KEY], char out[20])
{
    /* Sixteen hex digits in groups of four: what two people compare (ADR-017). */
    snprintf(out, 20, "%02x%02x %02x%02x %02x%02x %02x%02x", key[0], key[1], key[2], key[3], key[4],
             key[5], key[6], key[7]);
}

static cJSON *status_json(struct app *a, int64_t now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *ifs = cJSON_AddArrayToObject(o, "interfaces");
    char text[20];
    int i;

    cJSON_AddStringToObject(o, "state", a->core ? "ready" : ps_fault_name(a->boot.fault));
    cJSON_AddStringToObject(o, "run_id", a->run_id);
    cJSON_AddNumberToObject(o, "port", a->port);
    cJSON_AddBoolToObject(o, "socket", a->fd >= 0);
    cJSON_AddBoolToObject(o, "kept_fault", a->boot.kept_fault);
    if (!a->core) {
        return o;
    }
    cJSON_AddBoolToObject(o, "visible", pcore_visible(a->core, now));
    cJSON_AddNumberToObject(o, "epoch", (double)pcore_epoch(a->core));
    cJSON_AddBoolToObject(o, "kept_write_pending", pcore_kept_write_pending(a->core));
    for (i = 0; i < a->n_ifs && a->fd >= 0; i++) {
        cJSON *e = cJSON_CreateObject();

        addr_text(a->ifs[i].addr, text, sizeof(text));
        cJSON_AddStringToObject(e, "address", text);
        addr_text(a->ifs[i].bcast, text, sizeof(text));
        cJSON_AddStringToObject(e, "broadcast", text);
        cJSON_AddItemToArray(ifs, e);
    }
    {
        cJSON *cnt = cJSON_AddObjectToObject(o, "counters");

        for (i = 0; i < PCORE_C_COUNT; i++) {
            cJSON_AddNumberToObject(cnt, pcore_counter_name((enum pcore_count)i),
                                    (double)pcore_counter(a->core, (enum pcore_count)i));
        }
    }
    return o;
}

static cJSON *nodes_json(struct app *a, int64_t now, int tables_only)
{
    static struct pcore_node_info info[PCORE_NODES];
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, tables_only ? "tables" : "nodes");
    int n = pcore_nodes(a->core, info, PCORE_NODES, now);
    int count = 0;
    int i;

    for (i = 0; i < n; i++) {
        cJSON *e;
        char key[2 * PC_KEY + 1];
        char text[20];

        if (tables_only && (!info[i].table || count >= PEERD_TABLES_MAX)) {
            continue;
        }
        e = cJSON_CreateObject();
        to_hex(info[i].key, PC_KEY, key);
        cJSON_AddStringToObject(e, tables_only ? "host" : "key", key);
        cJSON_AddStringToObject(e, "name", info[i].name);
        fingerprint(info[i].key, text);
        cJSON_AddStringToObject(e, "fingerprint", text);
        if (tables_only) {
            cJSON_AddNumberToObject(e, "port", info[i].t.port);
            cJSON_AddNumberToObject(e, "proto_min", info[i].t.proto_min);
            cJSON_AddNumberToObject(e, "proto_max", info[i].t.proto_max);
            cJSON_AddNumberToObject(e, "seats", info[i].t.seats);
            cJSON_AddNumberToObject(e, "tag", (double)info[i].t.tag);
        } else {
            if (info[i].addr) {
                addr_text(info[i].addr, text, sizeof(text));
                cJSON_AddStringToObject(e, "address", text);
            }
            if (info[i].heard_ms >= 0) {
                cJSON_AddNumberToObject(e, "heard_ms_ago", (double)(now - info[i].heard_ms));
            }
            cJSON_AddBoolToObject(e, "kept", info[i].kept);
            /* A listed node is a claim until it has been shown live. */
            cJSON_AddBoolToObject(e, "proved", info[i].proved);
            cJSON_AddStringToObject(e, "route", "lan");
        }
        cJSON_AddItemToArray(arr, e);
        count++;
    }
    cJSON_AddNumberToObject(o, "count", count);
    return o;
}

static void on_request(struct pocketipc_server *s, struct pocketipc_client *c, cJSON *req, void *user)
{
    static const char *const no_keys[] = { NULL };
    static const char *const name_keys[] = { "name", NULL };
    static const char *const visible_keys[] = { "visible", NULL };
    static const char *const announce_keys[] = { "port", "proto_min", "proto_max", "seats", NULL };
    static const char *const probe_keys[] = { "address", "port", NULL };
    static const char *const key_keys[] = { "key", NULL };
    static const char *const send_keys[] = { "to", "port", "payload_hex", NULL };
    static const char *const inbox_keys[] = { "port", "after_id", NULL };
    struct app *a = user;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *mth = cJSON_GetObjectItemCaseSensitive(req, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    const char *method = cJSON_IsString(mth) ? mth->valuestring : NULL;
    const char *msg = "invalid parameters";
    int64_t now = mono_ms();
    cJSON *result = NULL;
    char err[120];
    uint8_t key[PC_KEY];
    long v;
    int rc;

    (void)s;
    err[0] = '\0';
    if (!method) {
        reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS, "missing method");
        return;
    }
    LOG_DEBUG("fd %d %s", pocketipc_client_fd(c), method);

    if (strcmp(method, "peer.info") == 0) {
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "service", "peerd");
        cJSON_AddNumberToObject(result, "api", PEERD_API_VERSION);
        cJSON_AddNumberToObject(result, "wire", PW_VERSION);
        cJSON_AddStringToObject(result, "version", POCKETOS_VERSION);
        cJSON_AddStringToObject(result, "build", POCKETOS_BUILD_ID);
    } else if (strcmp(method, "peer.status") == 0) {
        result = status_json(a, now);
    } else if (strcmp(method, "peer.subscribe") == 0 || strcmp(method, "peer.unsubscribe") == 0) {
        pocketipc_client_set_subscribed(c, method[5] == 's');
        result = cJSON_CreateObject();
    } else if (!a->core) {
        /* A state that cannot be trusted: say which, and do nothing else. */
        snprintf(err, sizeof(err), "peerd is closed: %s", ps_fault_name(a->boot.fault));
        reply_error(a, c, id, POCKETIPC_ERR_BACKEND, err);
        return;
    } else if (strcmp(method, "peer.identity") == 0) {
        char hex[2 * PC_KEY + 1];
        char fp[20];

        result = cJSON_CreateObject();
        to_hex(a->boot.id.pk, PC_KEY, hex);
        fingerprint(a->boot.id.pk, fp);
        cJSON_AddStringToObject(result, "key", hex);
        cJSON_AddStringToObject(result, "fingerprint", fp);
        cJSON_AddStringToObject(result, "name", pcore_name(a->core));
    } else if (strcmp(method, "peer.set_name") == 0) {
        const cJSON *name = param(params, "name");

        if (only_keys(params, name_keys, err, sizeof(err)) < 0 || !cJSON_IsString(name) ||
            pcore_set_name(a->core, name->valuestring) != PCORE_OK) {
            msg = err[0] ? err : "name must be up to 24 bytes of text with no control character";
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS, msg);
            return;
        }
        if (ps_name_save(a->dir, pcore_name(a->core)) != 0) {
            LOG_WARN("the name could not be stored; it holds for this run");
        }
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "name", pcore_name(a->core));
    } else if (strcmp(method, "peer.set_visible") == 0) {
        const cJSON *on = param(params, "visible");

        if (only_keys(params, visible_keys, err, sizeof(err)) < 0 || !cJSON_IsBool(on)) {
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS,
                        err[0] ? err : "visible must be true or false");
            return;
        }
        pcore_set_visible(a->core, cJSON_IsTrue(on), now);
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "visible", pcore_visible(a->core, now));
        cJSON_AddNumberToObject(result, "lease_ms", cJSON_IsTrue(on) ? PCORE_LEASE_MS : 0);
    } else if (strcmp(method, "peer.nodes") == 0 || strcmp(method, "peer.tables") == 0) {
        result = nodes_json(a, now, method[5] == 't');
    } else if (strcmp(method, "peer.announce") == 0) {
        long pmin;
        long pmax;
        long seats;
        uint32_t tag = 0;

        if (only_keys(params, announce_keys, err, sizeof(err)) < 0 ||
            int_param(params, "port", PW_PORT_MIN, PW_PORT_MAX, &v) < 0 ||
            int_param(params, "proto_min", 0, 255, &pmin) < 0 ||
            int_param(params, "proto_max", 0, 255, &pmax) < 0 ||
            int_param(params, "seats", 1, 255, &seats) < 0 ||
            pcore_announce(a->core, (uint8_t)v, (uint8_t)pmin, (uint8_t)pmax, (uint8_t)seats, &tag) !=
                PCORE_OK) {
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS,
                        err[0] ? err : "port 1..15, proto_min <= proto_max, seats 1..255");
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "tag", (double)tag);
    } else if (strcmp(method, "peer.withdraw") == 0) {
        if (only_keys(params, no_keys, err, sizeof(err)) < 0) {
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS, err);
            return;
        }
        pcore_withdraw(a->core);
        result = cJSON_CreateObject();
    } else if (strcmp(method, "peer.probe") == 0) {
        const cJSON *addr = param(params, "address");
        uint32_t ip = 0;
        long port = a->port;

        if (only_keys(params, probe_keys, err, sizeof(err)) < 0 || !cJSON_IsString(addr) ||
            addr_parse(addr->valuestring, &ip) < 0 ||
            (param(params, "port") && int_param(params, "port", 1, 65535, &port) < 0)) {
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS,
                        err[0] ? err : "address must be dotted IPv4; port 1..65535");
            return;
        }
        rc = pcore_probe(a->core, ip, (uint16_t)port, now);
        if (rc <= 0) {
            reply_error(a, c, id, rc_error(rc, &msg), msg);
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "probe_id", rc);
    } else if (strcmp(method, "peer.probes") == 0) {
        struct pcore_probe_info info[PCORE_PROBES];
        cJSON *arr;
        int n = pcore_probes(a->core, info, PCORE_PROBES);
        int i;

        result = cJSON_CreateObject();
        arr = cJSON_AddArrayToObject(result, "probes");
        for (i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            char text[2 * PC_KEY + 1];

            cJSON_AddNumberToObject(e, "probe_id", info[i].id);
            addr_text(info[i].addr, text, sizeof(text));
            cJSON_AddStringToObject(e, "address", text);
            cJSON_AddNumberToObject(e, "port", info[i].port);
            cJSON_AddStringToObject(e, "state",
                                    info[i].state == PCORE_PROBE_OK
                                        ? "answered"
                                        : info[i].state == PCORE_PROBE_PENDING ? "pending" : "no_answer");
            if (info[i].state == PCORE_PROBE_OK) {
                to_hex(info[i].key, PC_KEY, text);
                cJSON_AddStringToObject(e, "key", text);
            }
            cJSON_AddItemToArray(arr, e);
        }
    } else if (strcmp(method, "peer.keep") == 0 || strcmp(method, "peer.release") == 0) {
        if (only_keys(params, key_keys, err, sizeof(err)) < 0 || key_param(params, "key", key) < 0) {
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS,
                        err[0] ? err : "key must be a whole public key, 64 hex digits");
            return;
        }
        rc = method[5] == 'k' ? pcore_keep(a->core, key) : pcore_release(a->core, key);
        if (rc != PCORE_OK) {
            reply_error(a, c, id, rc_error(rc, &msg), msg);
            return;
        }
        /* The list on disk is whole again: it was just written. */
        a->boot.kept_fault = 0;
        result = cJSON_CreateObject();
    } else if (strcmp(method, "peer.kept") == 0) {
        struct ps_kept recs[PCORE_KEPT];
        cJSON *arr;
        int n = pcore_kept(a->core, recs, PCORE_KEPT);
        int i;

        result = cJSON_CreateObject();
        arr = cJSON_AddArrayToObject(result, "kept");
        for (i = 0; i < n; i++) {
            cJSON *e = cJSON_CreateObject();
            char text[2 * PC_KEY + 1];

            to_hex(recs[i].key, PC_KEY, text);
            cJSON_AddStringToObject(e, "key", text);
            cJSON_AddBoolToObject(e, "floor", recs[i].epoch != 0);
            if (recs[i].addr) {
                addr_text(recs[i].addr, text, sizeof(text));
                cJSON_AddStringToObject(e, "address", text);
            }
            cJSON_AddItemToArray(arr, e);
        }
        cJSON_AddNumberToObject(result, "count", n);
        cJSON_AddBoolToObject(result, "kept_fault", a->boot.kept_fault);
    } else if (strcmp(method, "peer.app_send") == 0) {
        static uint8_t payload[PW_PAYLOAD_MAX];
        const cJSON *hex = param(params, "payload_hex");
        int n;

        if (only_keys(params, send_keys, err, sizeof(err)) < 0 || key_param(params, "to", key) < 0 ||
            int_param(params, "port", PW_PORT_MIN, PW_PORT_MAX, &v) < 0 || !cJSON_IsString(hex) ||
            (n = from_hex(hex->valuestring, payload, 1, PW_PAYLOAD_MAX)) < 0) {
            pc_wipe(payload, sizeof(payload));
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS,
                        err[0] ? err : "to: 64 hex digits; port 1..15; payload_hex: 1..1024 bytes");
            return;
        }
        rc = pcore_send_app(a->core, key, (uint8_t)v, payload, (size_t)n, now);
        pc_wipe(payload, sizeof(payload));      /* a game's packet does not linger here */
        if (rc != PCORE_OK) {
            reply_error(a, c, id, rc_error(rc, &msg), msg);
            return;
        }
        result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "accepted", 1);
        cJSON_AddStringToObject(result, "route", "lan");
        cJSON_AddNumberToObject(result, "est_timeout_ms", PEERD_EST_TIMEOUT_MS);
        cJSON_AddNumberToObject(result, "bytes", n);
    } else if (strcmp(method, "peer.app_inbox") == 0) {
        static struct pcore_dgram out[PCORE_INBOX];
        cJSON *arr;
        long after = 0;
        int n;
        int i;

        if (only_keys(params, inbox_keys, err, sizeof(err)) < 0 ||
            int_param(params, "port", PW_PORT_MIN, PW_PORT_MAX, &v) < 0 ||
            (param(params, "after_id") && int_param(params, "after_id", 0, 2000000000L, &after) < 0)) {
            reply_error(a, c, id, POCKETIPC_ERR_INVALID_PARAMS,
                        err[0] ? err : "port 1..15; after_id a whole number");
            return;
        }
        n = pcore_inbox(a->core, (uint8_t)v, (uint64_t)after, out, PCORE_INBOX);
        result = cJSON_CreateObject();
        arr = cJSON_AddArrayToObject(result, "datagrams");
        for (i = 0; i < n; i++) {
            cJSON_AddItemToArray(arr, dgram_json(&out[i]));
        }
        cJSON_AddNumberToObject(result, "count", n);
    } else {
        reply_error(a, c, id, POCKETIPC_ERR_UNKNOWN_METHOD, "unknown method");
        return;
    }
    pocketipc_server_reply(a->server, c, pocketipc_response(id, result));
}

/* ---- the loop -------------------------------------------------------------------------- */

static void socket_follow(struct app *a, int64_t now)
{
    int want = a->core && pcore_wants_socket(a->core, now);

    if (!want) {
        if (a->fd >= 0) {
            pnet_close(a->fd);
            a->fd = -1;
            LOG_INFO("closed: not visible and no kept peer");
        }
        return;
    }
    if (a->fd < 0 && now >= a->next_open) {
        a->fd = pnet_open(a->port);
        if (a->fd < 0) {
            a->next_open = now + SOCKET_RETRY_MS;
            LOG_WARN("cannot bind UDP port %u; trying again", (unsigned)a->port);
            return;
        }
        a->next_ifaces = now;
        LOG_INFO("listening on UDP port %u", (unsigned)a->port);
    }
    if (a->fd >= 0 && now >= a->next_ifaces) {
        a->next_ifaces = now + IFACE_REFRESH_MS;
        a->n_ifs = pnet_ifaces(a->ifs, PCORE_IFACES);
        pcore_set_ifaces(a->core, a->ifs, a->n_ifs);
    }
}

static void drain(struct app *a, int64_t now)
{
    /* One byte over the largest frame, so a longer datagram is seen to be
     * too long instead of being read as its first PW_FRAME_MAX bytes. */
    static uint8_t buf[PW_FRAME_MAX + 1];
    int i;

    for (i = 0; i < RX_BATCH && a->fd >= 0; i++) {
        uint32_t src = 0;
        uint16_t sport = 0;
        long n = pnet_recv(a->fd, &src, &sport, buf, sizeof(buf));

        if (n <= 0) {
            break;
        }
        pcore_input(a->core, src, sport, buf, (size_t)n > sizeof(buf) ? sizeof(buf) : (size_t)n, now);
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [--port N] [--name NAME] [--socket NAME]\n", argv0);
}

int main(int argc, char **argv)
{
    static struct app a;
    static const struct pcore_ops ops = { cb_send, cb_source_for, cb_kept_save, cb_delivered };
    const char *socket_name = "peerd";
    const char *name_opt = NULL;
    uint8_t rnd[8];
    int i;

    a.fd = -1;
    a.port = PEERD_DEFAULT_PORT;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long p = strtol(argv[++i], NULL, 10);

            if (p < 1 || p > 65535) {
                usage(argv[0]);
                return 2;
            }
            a.port = (uint16_t)p;
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name_opt = argv[++i];
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_name = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    pocketlog_init("peerd");
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (pc_init() != 0) {
        LOG_ERROR("libsodium cannot start");
        return 1;
    }
    pc_random(rnd, sizeof(rnd));
    to_hex(rnd, sizeof(rnd), a.run_id);
    if (pocketos_mkdir_p(pocketos_state_dir(), 0755) != 0 ||
        snprintf(a.dir, sizeof(a.dir), "%s/peerd", pocketos_state_dir()) >= (int)sizeof(a.dir)) {
        LOG_ERROR("no state directory");
        return 1;
    }
    if (ps_boot(a.dir, &a.boot) != 0) {
        /* Stay up and say so: a supervisor restarting this would change
         * nothing, and whoever asks peer.status should be told which fault. */
        LOG_ERROR("closed: %s in %s. Nothing is regenerated; see docs/api/peer.md",
                  ps_fault_name(a.boot.fault), a.dir);
    } else {
        const char *name = name_opt ? name_opt : a.boot.name;

        a.core = pcore_new(&a.boot.id, a.boot.epoch, name, a.port, &ops, &a, a.boot.kept, a.boot.kept_n);
        if (!a.core) {
            LOG_ERROR("out of memory");
            pc_identity_wipe(&a.boot.id);
            return 1;
        }
        if (a.boot.created) {
            LOG_INFO("a new identity was made for this device");
        }
        if (a.boot.kept_fault) {
            LOG_WARN("kept.v1 could not be read: no kept peers until a game asserts them again");
        }
        LOG_INFO("ready: epoch %llu, %d kept peer(s)", (unsigned long long)a.boot.epoch, a.boot.kept_n);
    }
    a.server = pocketipc_server_new(socket_name, on_request, &a);
    if (!a.server) {
        LOG_ERROR("cannot listen on the %s socket", socket_name);
        pcore_free(a.core);
        pc_identity_wipe(&a.boot.id);
        return 1;
    }
    while (!stop_requested) {
        int64_t now = mono_ms();
        int ready = 0;

        socket_follow(&a, now);
        /* Awake four times a second while there is a socket; otherwise only
         * for a request or a signal. */
        if (pocketipc_server_poll_fd(a.server, a.fd >= 0 ? PCORE_TICK_MS : 1000, a.fd, &ready) < 0) {
            break;
        }
        now = mono_ms();
        if (a.core) {
            if (ready) {
                drain(&a, now);
            }
            /* A request may just have made peerd visible or kept a peer:
             * open the socket before the tick that would announce on it. */
            socket_follow(&a, now);
            pcore_tick(a.core, now);
        }
    }
    LOG_INFO("stopping");
    pocketipc_server_free(a.server);
    a.server = NULL;
    pnet_close(a.fd);
    pcore_free(a.core);
    pc_identity_wipe(&a.boot.id);
    return 0;
}
