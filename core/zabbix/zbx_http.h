/*
 * How the helper reaches a Zabbix server: one POST of a JSON-RPC body,
 * one answer, every failure named. Two transports sit behind it:
 *
 *   curl   the real one: libcurl with OpenSSL, both already in the K230
 *          image (docs/apps/ZABBIX.md). HTTPS with the peer and the host
 *          name verified against the system store, or a CA file of the
 *          configuration's own. One handle for the helper's life, so the
 *          connection and its TLS session are reused between refreshes.
 *          Built only when ZBX_HAVE_CURL is defined.
 *   fake   zbx_fake.c's deterministic server, answered in-process. No
 *          socket at all; used by the simulator, the tests and the demo.
 *
 * Every request is bounded: a connect timeout, a whole-request timeout, and
 * a response size cap (ZBX_RESPONSE_MAX) past which the transfer is cut and
 * the answer is TOO_LARGE. No redirect is ever followed: a redirect is an
 * HTTP error, so a token is never sent anywhere but the configured URL.
 *
 * The token is only ever put into the Authorization header of a request
 * that asks for it (bearer != NULL) and is never logged, copied into an
 * error text, or kept by the transport after the call.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_ZBX_HTTP_H
#define POCKETOS_ZBX_HTTP_H

#include "zabbix/zbx_proto.h"

#include <stdbool.h>
#include <stddef.h>

/* The largest answer read. The biggest request the helper makes (500
 * problems or 500 hosts with interfaces) answers in well under 300 KB. */
#define ZBX_RESPONSE_MAX (2u * 1024u * 1024u)

struct zbx_http_req {
    const char *url;            /* http:// or https://, ending in api_jsonrpc.php */
    const char *body;
    const char *bearer;         /* NULL: no Authorization header */
    int connect_timeout_ms;
    int timeout_ms;
    bool verify;                /* check the certificate and the host name */
    const char *ca_file;        /* NULL or "": the system store */
    size_t max_bytes;           /* 0: ZBX_RESPONSE_MAX */
};

struct zbx_http_resp {
    char *body;                 /* malloc'd, NUL-terminated; NULL on failure */
    size_t len;
    long status;                /* HTTP status, 0 when none arrived */
    long long date;             /* the Date header in Unix seconds, or -1 */
    enum zbx_err err;
    char text[ZBX_TEXT_MAX];    /* what went wrong, for the screen and the log */
};

struct zbx_transport {
    enum zbx_err (*post)(struct zbx_transport *t, const struct zbx_http_req *req,
                         struct zbx_http_resp *resp);
    void (*close)(struct zbx_transport *t);
    void *ctx;
    const char *name;           /* "curl", "fake" */
};

void zbx_http_resp_free(struct zbx_http_resp *r);

/* Whether this build has the real transport. */
bool zbx_http_available(void);

/* The libcurl transport. 0, or -1 with a reason when this build has no
 * libcurl (err says so) or it could not start. */
int zbx_transport_curl(struct zbx_transport *t, char *err, size_t errlen);

#endif
