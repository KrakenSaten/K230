/*
 * A deterministic, pretend Zabbix server, for everything that must work
 * without a real one: the simulator, the tests, the demo on the unit, and
 * the mock HTTP server (tools/zabbix/pos_zabbix_mock.c) that puts the same
 * answers behind a real socket.
 *
 * It answers the JSON-RPC requests of zbx_api.c the way the documented API
 * does (docs/apps/ZABBIX.md): the same envelopes, numbers as strings, the
 * same error codes and messages, the version rules (the "auth" member before
 * 6.4, the header after, apiinfo.version refused with a header on 7.4). Its
 * data is generated from fixed tables and the request counter, never from a
 * random source, so a screenshot or a test sees the same server every run.
 *
 * SCENARIOS (zbx_fake_scenarios() lists them):
 *   demo       a realistic small estate: 36 hosts, 9 problems of every
 *              severity, two hosts down, one in maintenance (the default)
 *   healthy    24 hosts, all up, no problems
 *   empty      no hosts, no problems
 *   large      1 500 hosts and 1 200 problems: more than the viewer asks
 *              for, with long names
 *   slow       demo, every answer 4 s late
 *   timeout    nothing ever answers
 *   refused    the connection is refused
 *   dns        the name does not resolve
 *   tls        the TLS handshake fails
 *   auth       every authenticated call: "Not authorized."
 *   expired    every authenticated call: "API token expired."
 *   apierror   problem.get: "No permissions to call "problem.get"."
 *   malformed  problem.get answers half a JSON document
 *   http500    the web server answers 500 with an HTML page
 *   huge       problem.get answers more than ZBX_RESPONSE_MAX
 *   flap       twelve answers, then four refusals, round and round
 *   drop       twelve answers (a first round and more), then the server is
 *              gone for good (stale data)
 *   old        a 6.0 LTS server: the token only in the "auth" member
 *   v74        a 7.4 server: header only, strict about apiinfo.version
 *
 * Pure C with cJSON; no network, no clock of its own (the caller passes the
 * time). Built into the helper and the tests, never into the shell.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_ZBX_FAKE_H
#define POCKETOS_ZBX_FAKE_H

#include "zabbix/zbx_http.h"

#include <stdbool.h>
#include <stdint.h>

#define ZBX_FAKE_DEFAULT "demo"
#define ZBX_FAKE_NAME_MAX 16
/* What the fake accepts as credentials: any non-empty token, and the user
 * "demo" with any password. */
#define ZBX_FAKE_USER "demo"

/* Something other than an HTTP answer. */
enum zbx_fake_fault {
    ZBX_FAKE_OK = 0,
    ZBX_FAKE_HANG,              /* never answers: the client's timeout ends it */
    ZBX_FAKE_REFUSED,
    ZBX_FAKE_DNS,
    ZBX_FAKE_TLS,
};

struct zbx_fake_answer {
    enum zbx_fake_fault fault;
    int status;                 /* HTTP status */
    long delay_ms;              /* how late the answer is */
    char *body;                 /* malloc'd; NULL for a fault */
    size_t len;
};

struct zbx_fake {
    char scenario[ZBX_FAKE_NAME_MAX];
    int64_t epoch;              /* the server's "now" at the first request */
    unsigned requests;          /* answered so far (faults included) */
    bool realtime;              /* the transport sleeps the delays */
};

/* The scenario names, NULL-terminated. */
const char *const *zbx_fake_scenarios(void);
bool zbx_fake_known(const char *name);

/* 0, or -1 for an unknown scenario (the fake is left as it was). */
int zbx_fake_init(struct zbx_fake *f, const char *scenario);
int zbx_fake_set_scenario(struct zbx_fake *f, const char *scenario);

/* Answer one POST as the server would. bearer is the Authorization header's
 * token or NULL; now is Unix seconds. The answer's body belongs to the
 * caller (free()). */
void zbx_fake_answer(struct zbx_fake *f, const char *bearer, const char *body, int64_t now,
                     struct zbx_fake_answer *a);

/* The fake as a transport: zbx_fake_answer() plus, when f->realtime, the
 * delays slept for real (a hang sleeps the request's timeout). */
void zbx_transport_fake(struct zbx_transport *t, struct zbx_fake *f);

#endif
