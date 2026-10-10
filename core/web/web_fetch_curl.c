/*
 * The real fetcher: one libcurl easy handle for the helper's life. See
 * web_fetch.h for the rules it enforces.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_fetch.h"
#include "web/web_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef POCKETOS_VERSION
#define POCKETOS_VERSION "dev"
#endif

#ifndef BROWSER_HAVE_CURL

bool web_fetch_curl_available(void)
{
    return false;
}

int web_fetcher_curl(struct web_fetcher *f, char *err, size_t errlen)
{
    memset(f, 0, sizeof(*f));
    if (err && errlen) {
        snprintf(err, errlen, "this build has no HTTP client (built without libcurl)");
    }
    return -1;
}

#else

#include <curl/curl.h>

#define USER_AGENT "Mozilla/5.0 (Linux; riscv64) DoorsBrowser/" POCKETOS_VERSION " (text; no JavaScript)"

struct curl_ctx {
    CURL *h;
};

struct sink {
    char *buf;
    size_t len;
    size_t cap;
    size_t max;
    bool cut;
    const struct web_fetch_req *req;
};

static size_t on_body(char *data, size_t size, size_t n, void *user)
{
    struct sink *s = user;
    size_t add = size * n;

    if (s->len + add > s->max) {
        size_t room = s->max - s->len;

        s->cut = true;
        if (room == 0) {
            return 0; /* curl stops with CURLE_WRITE_ERROR */
        }
        add = room;
    }
    if (s->len + add + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 32768;
        char *nb;

        while (cap < s->len + add + 1) {
            cap *= 2;
        }
        if (cap > s->max + 1) {
            cap = s->max + 1;
        }
        nb = realloc(s->buf, cap);
        if (!nb) {
            return 0;
        }
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, data, add);
    s->len += add;
    s->buf[s->len] = '\0';
    if (s->req->progress_cb) {
        s->req->progress_cb(s->req->ctx, (long)s->len);
    }
    return s->cut ? 0 : size * n;
}

static size_t on_header(char *data, size_t size, size_t n, void *user)
{
    web_cache_hdr_line(user, data, size * n);
    return size * n;
}

static int on_progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow)
{
    const struct web_fetch_req *req = user;

    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return req->abort_cb && req->abort_cb(req->ctx) ? 1 : 0;
}

/* A certificate is valid from a date; a board whose clock still says 1970
 * cannot check that, so its TLS failure is said differently. */
static bool clock_unset(void)
{
    return time(NULL) < 1700000000; /* before 2023-11-14 */
}

static enum web_fail classify(CURLcode rc)
{
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return WEB_FAIL_DNS;
    case CURLE_OPERATION_TIMEDOUT:
        return WEB_FAIL_TIMEOUT;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_USE_SSL_FAILED:
        return WEB_FAIL_TLS;
    case CURLE_ABORTED_BY_CALLBACK:
        return WEB_FAIL_STOPPED;
    case CURLE_UNSUPPORTED_PROTOCOL:
        return WEB_FAIL_BAD_REDIRECT;
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_BAD_CONTENT_ENCODING:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
        return WEB_FAIL_RESPONSE;
    case CURLE_OUT_OF_MEMORY:
        return WEB_FAIL_INTERNAL;
    default:
        return WEB_FAIL_CONNECT;
    }
}

/* One request, no redirects followed. Returns the curl result. */
static CURLcode one(struct curl_ctx *c, const struct web_fetch_req *req, const char *url, struct sink *s,
                    char *errbuf, struct curl_slist *hdr, struct web_cache_hdrs *cache)
{
    curl_easy_reset(c->h); /* keeps connections, TLS sessions and cookies */
    curl_easy_setopt(c->h, CURLOPT_URL, url);
    curl_easy_setopt(c->h, CURLOPT_HTTPGET, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c->h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c->h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c->h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c->h, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c->h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c->h, CURLOPT_USERAGENT,
                     req->user_agent && *req->user_agent ? req->user_agent : USER_AGENT);
    curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->h, CURLOPT_CONNECTTIMEOUT_MS,
                     (long)(req->connect_timeout_ms ? req->connect_timeout_ms : WEB_CONNECT_TIMEOUT_MS));
    curl_easy_setopt(c->h, CURLOPT_TIMEOUT_MS, (long)(req->timeout_ms ? req->timeout_ms : WEB_TIMEOUT_MS));
    curl_easy_setopt(c->h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c->h, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c->h, CURLOPT_ACCEPT_ENCODING, ""); /* whatever this libcurl decodes */
    curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYHOST, 2L);
    if (req->ca_file && *req->ca_file) {
        curl_easy_setopt(c->h, CURLOPT_CAINFO, req->ca_file);
    }
    curl_easy_setopt(c->h, CURLOPT_COOKIEFILE, ""); /* in memory only */
    curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c->h, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c->h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c->h, CURLOPT_HEADERDATA, cache);
    curl_easy_setopt(c->h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c->h, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c->h, CURLOPT_XFERINFODATA, (void *)req);
    curl_easy_setopt(c->h, CURLOPT_ERRORBUFFER, errbuf);
    errbuf[0] = '\0';
    return curl_easy_perform(c->h);
}

static int curl_hop(struct web_fetcher *f, const struct web_fetch_req *req, const char *url,
                    struct web_hop *h)
{
    struct curl_ctx *c = f->ctx;
    struct curl_slist *hdr = NULL;
    char accept[160];
    char errbuf[CURL_ERROR_SIZE];
    struct sink s;
    CURLcode rc;
    long status = 0;
    char *ct = NULL;
    char *next = NULL;

    snprintf(accept, sizeof(accept), "Accept: %s", req->accept ? req->accept : "*/*");
    hdr = curl_slist_append(hdr, accept);
    if (hdr) {
        struct curl_slist *more = curl_slist_append(hdr, "Accept-Language: en;q=0.9, *;q=0.5");

        hdr = more ? more : hdr;
    }
    /* The validators of a copy already held. Both are values a server sent
     * and web_cache_hdr_line kept: printable, no line break. */
    if (hdr && req->if_none_match && *req->if_none_match) {
        char line[WEB_HDR_LONG + 32];
        struct curl_slist *more;

        snprintf(line, sizeof(line), "If-None-Match: %s", req->if_none_match);
        more = curl_slist_append(hdr, line);
        if (!more) {
            curl_slist_free_all(hdr);
        }
        hdr = more;
    }
    if (hdr && req->if_modified_since && *req->if_modified_since) {
        char line[WEB_HDR_SHORT + 32];
        struct curl_slist *more;

        snprintf(line, sizeof(line), "If-Modified-Since: %s", req->if_modified_since);
        more = curl_slist_append(hdr, line);
        if (!more) {
            curl_slist_free_all(hdr);
        }
        hdr = more;
    }
    if (!hdr) {
        h->failed = true;
        h->fail = WEB_FAIL_INTERNAL;
        snprintf(h->text, sizeof(h->text), "out of memory");
        return -1;
    }
    memset(&s, 0, sizeof(s));
    s.max = req->max_bytes ? req->max_bytes : WEB_PAGE_BYTES_MAX;
    s.req = req;
    rc = one(c, req, url, &s, errbuf, hdr, &h->cache);
    curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(hdr);
    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &status);
    h->status = status;
    if (rc == CURLE_WRITE_ERROR && s.cut && req->keep_cut) {
        rc = CURLE_OK; /* a page cut at the cap is shown as far as it came */
    }
    if (rc != CURLE_OK) {
        free(s.buf);
        h->failed = true;
        if (s.cut) {
            h->fail = WEB_FAIL_TYPE;
            snprintf(h->text, sizeof(h->text), "larger than %zu KB", s.max / 1024);
        } else if ((h->fail = classify(rc)) == WEB_FAIL_TLS && clock_unset()) {
            h->fail = WEB_FAIL_CLOCK;
            snprintf(h->text, sizeof(h->text), "the device clock is not set, so no certificate can be checked");
        } else {
            web_copy(h->text, sizeof(h->text), errbuf[0] ? errbuf : curl_easy_strerror(rc));
        }
        return -1;
    }
    if (curl_easy_getinfo(c->h, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) {
        web_copy(h->content_type, sizeof(h->content_type), ct);
    }
    if (status >= 300 && status < 400 && curl_easy_getinfo(c->h, CURLINFO_REDIRECT_URL, &next) == CURLE_OK &&
        next && strlen(next) < sizeof(h->location)) {
        web_copy(h->location, sizeof(h->location), next);
    }
    h->body = s.buf;
    h->len = s.len;
    h->cut = s.cut;
    return 0;
}

static void curl_close(struct web_fetcher *f)
{
    struct curl_ctx *c = f ? f->ctx : NULL;

    if (c) {
        curl_easy_cleanup(c->h);
        free(c);
        f->ctx = NULL;
    }
    curl_global_cleanup();
}

bool web_fetch_curl_available(void)
{
    return true;
}

int web_fetcher_curl(struct web_fetcher *f, char *err, size_t errlen)
{
    struct curl_ctx *c;

    memset(f, 0, sizeof(*f));
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        snprintf(err, errlen, "libcurl did not start");
        return -1;
    }
    c = calloc(1, sizeof(*c));
    if (!c || !(c->h = curl_easy_init())) {
        free(c);
        curl_global_cleanup();
        snprintf(err, errlen, "libcurl did not start");
        return -1;
    }
    f->ctx = c;
    f->hop = curl_hop;
    f->close = curl_close;
    f->name = "curl";
    return 0;
}

#endif
