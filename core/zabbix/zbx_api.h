/*
 * The Zabbix JSON-RPC API, as far as a read-only viewer needs it: the
 * requests the helper sends, and the parsing of what comes back into the
 * bounded model of zbx_model.h.
 *
 * WHAT IS ASKED (docs/apps/ZABBIX.md has the full table and the versions):
 *   apiinfo.version          without authentication, always first
 *   user.login               only for username/password configurations
 *   problem.get              open, unsuppressed problems, newest first,
 *                            ZBX_PROBLEM_FETCH at most; countOutput for totals
 *   trigger.get              the hosts of the problems kept (problem.get does
 *                            not return hosts)
 *   hostinterface.get        which hosts have an unavailable interface
 *   host.get                 monitored hosts with their interfaces'
 *                            availability, the ones with problems or down
 *                            first; countOutput for the total
 *   item.get                 one host's latest values, when a host is open
 * Nothing that writes: no acknowledge, no maintenance, no configuration.
 *
 * AUTHENTICATION. From Zabbix 6.4 the token (or a session id from
 * user.login) travels in an "Authorization: Bearer" header; before 6.4 it
 * is the "auth" member of the request, which 7.2 no longer accepts. The
 * builders put it in the body only when asked to (auth_in_body), and the
 * transport sends the header otherwise. apiinfo.version and user.login are
 * always built without it.
 *
 * PARSING never trusts the answer: every member is looked up by name,
 * checked for type (Zabbix sends numbers as strings, and both are accepted),
 * and a missing or mistyped one makes the whole answer MALFORMED rather than
 * a record with zeroes in it. Strings are cut to the model's sizes.
 *
 * Pure C with cJSON; no network. Used by the helper and its tests only - the
 * shell never links this file (tests/zabbix_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_ZBX_API_H
#define POCKETOS_ZBX_API_H

#include "zabbix/zbx_model.h"
#include "zabbix/zbx_proto.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

/* The oldest server this viewer speaks to: 6.0 LTS (interfaces carry the
 * availability, user.login takes "username"). */
#define ZBX_API_MIN_VERSION 60000
/* Where the token moved from the body to the Authorization header. */
#define ZBX_API_BEARER_VERSION 60400
/* Hosts gained active_available (the agent's active checks) in 6.2. */
#define ZBX_API_ACTIVE_AVAIL_VERSION 60200
/* Where the "auth" member stopped being accepted at all (7.2). Recorded, not
 * used: the header is sent from 6.4, before this matters. */
#define ZBX_API_NO_AUTH_MEMBER_VERSION 70200

/* "7.0.5" -> 70005, "6.4.12" -> 60412; -1 when it is not a version. */
long zbx_version_num(const char *v);

/* ---- requests (malloc'd JSON text, or NULL when out of memory) ------------------ */

struct zbx_req_ctx {
    int id;                     /* the JSON-RPC id; the caller increments it */
    const char *auth_in_body;   /* token or session for servers before 6.4, else NULL */
    bool active_available;      /* the server knows host.active_available */
};

char *zbx_req_version(int id);
char *zbx_req_login(int id, const char *user, const char *password);
/* user.logout: params [], with the session like any authenticated call. */
char *zbx_req_logout(const struct zbx_req_ctx *c);
/* hostid NULL: every host; else only that host's. */
char *zbx_req_problems(const struct zbx_req_ctx *c, const char *hostid, int limit);
/* severity -1: all severities; unacknowledged: only those not acknowledged. */
char *zbx_req_problem_count(const struct zbx_req_ctx *c, int severity, bool unacknowledged);
char *zbx_req_trigger_hosts(const struct zbx_req_ctx *c, const char *const *triggerids, int n);
char *zbx_req_host_count(const struct zbx_req_ctx *c);
/* hostid NULL: the list; else that one host. */
char *zbx_req_hosts(const struct zbx_req_ctx *c, const char *hostid, int limit);
/* Those hosts (the ones that matter first: with problems, or down), if they
 * are monitored. */
char *zbx_req_hosts_by_id(const struct zbx_req_ctx *c, const char *const *hostids, int n);
/* The hosts that have an unavailable interface: hostinterface.get filtered
 * on available = 2, hostids only, at most limit interfaces. */
char *zbx_req_down_interfaces(const struct zbx_req_ctx *c, int limit);
char *zbx_req_items(const struct zbx_req_ctx *c, const char *hostid, int limit);

/* ---- replies ------------------------------------------------------------------------ */

struct zbx_reply {
    cJSON *root;
    const cJSON *result;        /* inside root; valid until zbx_reply_free() */
    enum zbx_err err;           /* NONE, MALFORMED, API or AUTH */
    long code;                  /* the JSON-RPC error code, when there was one */
    char text[ZBX_TEXT_MAX];    /* the server's message and data, or why it is malformed */
};

/* Parse one answer to the request with this id. Returns r->err. */
enum zbx_err zbx_reply_parse(struct zbx_reply *r, const char *body, size_t len, int id);
void zbx_reply_free(struct zbx_reply *r);

/* Whether a JSON-RPC error means "these credentials are not accepted"
 * rather than "this request is wrong". Exposed for the test. */
bool zbx_error_is_auth(long code, const char *message, const char *data);

/* ---- results (each returns 0, or -1 for a result of the wrong shape) ---------------- */

int zbx_parse_version(const cJSON *result, char *out, size_t len);
int zbx_parse_login(const cJSON *result, char *session, size_t len);
/* countOutput answers: a number in a string. */
int zbx_parse_count(const cJSON *result, int *out);
/* Up to max problems into p; *n is how many. */
int zbx_parse_problems(const cJSON *result, struct zbx_problem *p, int max, int *n);
/* Fill hostid and host of every problem whose objectid is a trigger in the
 * answer (the first of its hosts). */
int zbx_parse_trigger_hosts(const cJSON *result, struct zbx_problem *p, int n);
/* Up to max hosts into h; *n is how many there were in the answer. */
int zbx_parse_hosts(const cJSON *result, struct zbx_host *h, int max, int *n);
/* The distinct hostids of an answer of objects with a "hostid", appended to
 * ids[*n] up to max (ids already there are not repeated); *seen is how many
 * objects the answer held. */
int zbx_parse_hostids(const cJSON *result, char (*ids)[ZBX_ID_MAX], int max, int *n, int *seen);
/* The items worth showing for a host, at most max, in the order of
 * ZBX_KEY_PRIORITY and then by name (see zbx_api.c). */
int zbx_parse_items(const cJSON *result, struct zbx_item *it, int max, int *n);

/* A latest value made readable: numbers trimmed, bytes and bits scaled
 * ("B" -> "KB/MB/GB", "bps" -> "Kbps/Mbps"), uptime in days and hours,
 * agent.ping's 1 as "Up". key may be NULL. */
void zbx_format_value(char *out, size_t len, const char *key, const char *value,
                      const char *units, int value_type);

/* Parse an HTTP Date header value ("Sun, 06 Nov 1994 08:49:37 GMT") into
 * Unix seconds; -1 when it is not one. */
long long zbx_http_date(const char *s);

#endif
