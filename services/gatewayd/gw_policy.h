/*
 * gatewayd: what a client may call (ADR-019 rules 6, 24 and 25;
 * docs/api/gateway.md). The gateway is an allowlist and not a proxy: a
 * method a client can reach is a row of the table in gw_policy.c and
 * nothing else is. A row says who may call it, which scope it needs, which
 * service answers it, and which fields of the answer are passed on.
 *
 * Pure: cJSON only, no I/O.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_POLICY_H
#define GATEWAYD_GW_POLICY_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The five scopes of ADR-019 rule 24. */
enum gw_scope {
    GW_S_STATUS = 1u << 0,
    GW_S_MESH_READ = 1u << 1,
    GW_S_MESH_SEND = 1u << 2,
    GW_S_CAPTURE_VIEW = 1u << 3,
    GW_S_CAPTURE_CONTROL = 1u << 4,
    GW_S_ALL = (1u << 5) - 1u,
};
/* What a newly approved client gets when the person chose nothing else. */
#define GW_S_DEFAULT (GW_S_STATUS | GW_S_MESH_READ | GW_S_MESH_SEND)

enum gw_who {
    GW_W_PAIRED = 1,
    GW_W_CANDIDATE = 2, /* an unknown key inside a pairing window */
};

enum gw_kind {
    GW_K_HELLO,
    GW_K_PING,
    GW_K_PAIR,
    GW_K_UNPAIR,
    GW_K_UPSTREAM,      /* passed to a service, the result filtered */
};

struct gw_method {
    const char *name;           /* as the client sends it and as upstream knows it */
    enum gw_kind kind;
    unsigned who;               /* GW_W_* bits */
    unsigned scope;             /* one GW_S_* bit, or 0: none needed */
    const char *service;        /* GW_K_UPSTREAM: the pocketipc service */
    cJSON *(*filter)(const cJSON *result);
};

enum gw_verdict {
    GW_V_OK = 0,
    GW_V_UNKNOWN,   /* code 1: no such method, or none for this caller */
    GW_V_FORBIDDEN, /* code 7: the client's scopes do not include it */
};

/* The table, for the tests and the lint. */
const struct gw_method *gw_methods(size_t *count);
const struct gw_method *gw_method_find(const char *name);
/* May this caller, with these scopes, call m? m may be NULL (unknown). */
enum gw_verdict gw_policy_check(const struct gw_method *m, enum gw_who who, unsigned scopes);

const char *gw_scope_name(unsigned bit);
/* 0 for a name that is not a scope. */
unsigned gw_scope_parse(const char *name);
/* A set a client may hold: only known bits, mesh_send with mesh_read,
 * capture_control with capture_view. */
bool gw_scopes_valid(unsigned mask);
cJSON *gw_scopes_json(unsigned mask);
/* An array of scope names, each known and none twice, forming a valid set.
 * 0, or -1. */
int gw_scopes_from_json(const cJSON *arr, unsigned *mask);

/* One request. 0 and *root (to delete) with id, method and params inside
 * it; -1 when the bytes are not well-formed UTF-8 holding one JSON object
 * with an integer id, a method name and params that are absent or an
 * object. *params is NULL when absent. */
int gw_request_parse(const uint8_t *p, size_t n, cJSON **root, const cJSON **id, const char **method,
                     const cJSON **params);
/* True when params is absent or an empty object. */
bool gw_params_empty(const cJSON *params);

/* The upstream result with only what m passes on: a new object, never
 * NULL-safe on m. NULL when there is no memory or the result is not an
 * object. Whatever the row says, no member whose name suggests a secret
 * survives at any depth (gw_scrub). */
cJSON *gw_filter_result(const struct gw_method *m, const cJSON *result);
/* Removes, at any depth, members named for a key, a secret, a passphrase,
 * a password, a token or a PSK. "public_key" stays. */
void gw_scrub(cJSON *node);

#endif
