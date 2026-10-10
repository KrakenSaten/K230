/*
 * How the Browser's helper gets a page or an image: one GET, redirects
 * followed under the Browser's rules, every failure named. Two fetchers:
 *
 *   curl   libcurl with OpenSSL, both already in the K230 image
 *          (docs/apps/BROWSER.md). Built only with BROWSER_HAVE_CURL.
 *   fake   deterministic pages answered in-process (web_fake.c): the
 *          tests and the simulator's demo. No socket at all.
 *
 * THE RULES, all enforced here and not left to libcurl's defaults:
 *
 *   - http and https only, for the request and for every redirect
 *     (CURLOPT_PROTOCOLS_STR); anything else fails as BAD_REDIRECT;
 *   - certificates and host names are always verified, against the system
 *     store or a CA file; there is no switch to turn that off;
 *   - an https page never redirects to http (INSECURE_REDIRECT); an http page
 *     may go to https;
 *   - at most WEB_FETCH_REDIRECTS redirects;
 *   - a connect timeout, a whole-request timeout, a low-speed cut-off, and a
 *     body cap (max_bytes): a page past it is kept up to the cap and marked
 *     cut; an image past it fails;
 *   - cookies live in memory for as long as the helper runs (the app is open)
 *     and are never written anywhere;
 *   - abort() is asked between blocks of the transfer, so STOP ends a load
 *     within a fraction of a second, name resolution aside.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_FETCH_H
#define POCKETOS_WEB_FETCH_H

#include "web/web_proto.h"
#include "web/web_url.h"

#include <stdbool.h>
#include <stddef.h>

#define WEB_FETCH_REDIRECTS 8
#define WEB_PAGE_BYTES_MAX (2u * 1024u * 1024u)
#define WEB_IMAGE_BYTES_MAX (1536u * 1024u)
#define WEB_CONNECT_TIMEOUT_MS 15000
#define WEB_TIMEOUT_MS 45000

/* The cache headers of an answer, as the server sent them (trimmed), for a
 * client that keeps what it fetched: RIFT's map tiles (web_tiles.h). The
 * Browser keeps nothing and never looks at them. */
#define WEB_HDR_SHORT 64
#define WEB_HDR_LONG 192

struct web_cache_hdrs {
    char etag[WEB_HDR_LONG];
    char last_modified[WEB_HDR_SHORT];
    char cache_control[WEB_HDR_LONG];
    char expires[WEB_HDR_SHORT];
    char date[WEB_HDR_SHORT];
};

struct web_fetch_req {
    const char *url;                /* absolute, already checked by web_url_parse */
    size_t max_bytes;
    bool keep_cut;                  /* past max_bytes: keep what came (a page) or fail (an image) */
    const char *accept;
    const char *ca_file;            /* NULL: the system store */
    int connect_timeout_ms;
    int timeout_ms;
    /* NULL: the Browser's own. A client that is not the Browser names
     * itself (a tile server's policy asks for exactly that). */
    const char *user_agent;
    /* A conditional request: the validators of the copy already held. NULL
     * or "" sends none. A 304 answer then comes back as status 304 with an
     * empty body, never as a failure. */
    const char *if_none_match;
    const char *if_modified_since;
    /* Asked while the transfer runs: nonzero stops it (WEB_FAIL_STOPPED). */
    int (*abort_cb)(void *ctx);
    /* Told how many body bytes have arrived. */
    void (*progress_cb)(void *ctx, long bytes);
    void *ctx;
};

struct web_fetch_resp {
    char *body;                     /* malloc'd, NUL-terminated after len; NULL on failure */
    size_t len;
    bool cut;                       /* the body stopped at max_bytes */
    long status;                    /* the final HTTP status */
    char type[64];                  /* media type, lower case, no parameters */
    char charset[32];               /* its charset parameter, or "" */
    char final_url[WEB_URL_MAX];    /* after redirects */
    enum web_fail fail;
    bool failed;
    char text[WEB_FAIL_TEXT_MAX];   /* what went wrong, for the screen; never holds the query */
    struct web_cache_hdrs cache;    /* of the final answer */
};

/* One request, no redirect followed: what a fetcher does. */
struct web_hop {
    long status;
    char *body;                     /* malloc'd, NUL-terminated after len, or NULL */
    size_t len;
    bool cut;
    char content_type[128];         /* the header as sent */
    char location[WEB_URL_MAX];     /* for a 3xx: the absolute address it names, or "" */
    bool failed;
    enum web_fail fail;
    char text[WEB_FAIL_TEXT_MAX];
    struct web_cache_hdrs cache;
};

struct web_fetcher {
    int (*hop)(struct web_fetcher *f, const struct web_fetch_req *req, const char *url, struct web_hop *out);
    void (*close)(struct web_fetcher *f);
    void *ctx;
    const char *name;               /* "curl", "fake" */
};

/* The whole fetch: hops, with the redirect rules above applied here, the
 * same for every fetcher. 0, or -1 with resp->failed. */
int web_fetch(struct web_fetcher *f, const struct web_fetch_req *req, struct web_fetch_resp *resp);

void web_fetch_resp_free(struct web_fetch_resp *r);

/* 0, or -1 with err (this build has no libcurl, or it did not start). */
int web_fetcher_curl(struct web_fetcher *f, char *err, size_t errlen);
bool web_fetch_curl_available(void);

/* The fake network (web_fake.c). */
int web_fetcher_fake(struct web_fetcher *f);

/* Split "text/html; charset=UTF-8" into type and charset. */
void web_fetch_parse_type(const char *ct, char *type, size_t tlen, char *charset, size_t clen);

/* One raw header line ("Name: value\r\n", any case) into h when it is one of
 * the cache headers; anything else is ignored. For the fetchers. */
void web_cache_hdr_line(struct web_cache_hdrs *h, const char *line, size_t n);

/* Whether this unit has a default route at all (/proc/net/route and
 * ipv6_route), to tell "no network" from "that server". */
bool web_net_has_route(void);

#endif
