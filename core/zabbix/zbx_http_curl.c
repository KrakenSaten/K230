/*
 * The real transport: one libcurl easy handle. See zbx_http.h.
 *
 * Only options that libcurl 7.81 already had are used, so the host tests
 * (Ubuntu 22.04's libcurl) run the same code as the device (8.12.1).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix/zbx_http.h"

#include "zabbix/zbx_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void zbx_http_resp_free(struct zbx_http_resp *r)
{
    if (r) {
        free(r->body);
        r->body = NULL;
        r->len = 0;
    }
}

#ifndef ZBX_HAVE_CURL

bool zbx_http_available(void)
{
    return false;
}

int zbx_transport_curl(struct zbx_transport *t, char *err, size_t errlen)
{
    memset(t, 0, sizeof(*t));
    if (err && errlen) {
        snprintf(err, errlen, "this build has no HTTP client (built without libcurl)");
    }
    return -1;
}

#else

#include <curl/curl.h>

#define USER_AGENT "Doors-Zabbix/" POCKETOS_VERSION

struct curl_ctx {
    CURL *h;
};

struct sink {
    char *buf;
    size_t len;
    size_t cap;
    size_t max;
    bool too_large;
    long long date;
};

static size_t on_body(char *data, size_t size, size_t n, void *user)
{
    struct sink *s = user;
    size_t add = size * n;

    if (s->len + add > s->max) {
        s->too_large = true;
        return 0; /* curl stops with CURLE_WRITE_ERROR */
    }
    if (s->len + add + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 16384;
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
    return add;
}

static size_t on_header(char *data, size_t size, size_t n, void *user)
{
    struct sink *s = user;
    size_t len = size * n;
    char line[128];

    if (len > 5 && len < sizeof(line) && strncasecmp(data, "Date:", 5) == 0) {
        memcpy(line, data + 5, len - 5);
        line[len - 5] = '\0';
        s->date = zbx_http_date(line);
    }
    return len;
}

static enum zbx_err classify(CURLcode rc, bool too_large)
{
    if (too_large) {
        return ZBX_ERR_TOO_LARGE;
    }
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return ZBX_ERR_DNS;
    case CURLE_OPERATION_TIMEDOUT:
        return ZBX_ERR_TIMEOUT;
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
        return ZBX_ERR_TLS;
    case CURLE_UNSUPPORTED_PROTOCOL:
        return ZBX_ERR_UNSUPPORTED;
    case CURLE_FILESIZE_EXCEEDED:
        return ZBX_ERR_TOO_LARGE;
    case CURLE_OUT_OF_MEMORY:
        return ZBX_ERR_INTERNAL;
    default:
        return ZBX_ERR_CONNECT;
    }
}

/* A certificate is valid from a date; a board whose clock still says 1970
 * cannot check that, so its TLS failure is said differently. */
static bool clock_unset(void)
{
    return time(NULL) < 1700000000; /* before 2023-11-14 */
}

static enum zbx_err curl_post(struct zbx_transport *t, const struct zbx_http_req *req,
                              struct zbx_http_resp *resp)
{
    struct curl_ctx *c = t->ctx;
    struct curl_slist *hdr = NULL;
    struct sink s;
    char errbuf[CURL_ERROR_SIZE];
    char auth[ZBX_TEXT_MAX * 4];
    CURLcode rc;
    long status = 0;

    memset(resp, 0, sizeof(*resp));
    resp->date = -1;
    memset(&s, 0, sizeof(s));
    s.max = req->max_bytes ? req->max_bytes : ZBX_RESPONSE_MAX;
    s.date = -1;
    errbuf[0] = '\0';

    if (strncmp(req->url, "https://", 8) != 0 && strncmp(req->url, "http://", 7) != 0) {
        resp->err = ZBX_ERR_CONFIG;
        snprintf(resp->text, sizeof(resp->text), "the URL is not http:// or https://");
        return resp->err;
    }
    hdr = curl_slist_append(hdr, "Content-Type: application/json-rpc");
    hdr = curl_slist_append(hdr, "Expect:");
    if (req->bearer) {
        if (strlen(req->bearer) + 32 > sizeof(auth)) {
            curl_slist_free_all(hdr);
            resp->err = ZBX_ERR_CONFIG;
            snprintf(resp->text, sizeof(resp->text), "the token is too long");
            return resp->err;
        }
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", req->bearer);
        hdr = curl_slist_append(hdr, auth);
        explicit_bzero(auth, sizeof(auth));
    }
    if (!hdr) {
        resp->err = ZBX_ERR_INTERNAL;
        snprintf(resp->text, sizeof(resp->text), "out of memory");
        return resp->err;
    }

    curl_easy_reset(c->h); /* keeps the connection cache, clears the options */
    curl_easy_setopt(c->h, CURLOPT_URL, req->url);
    curl_easy_setopt(c->h, CURLOPT_POST, 1L);
    curl_easy_setopt(c->h, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(c->h, CURLOPT_POSTFIELDSIZE, (long)strlen(req->body));
    curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c->h, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c->h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->h, CURLOPT_CONNECTTIMEOUT_MS, (long)req->connect_timeout_ms);
    curl_easy_setopt(c->h, CURLOPT_TIMEOUT_MS, (long)req->timeout_ms);
    curl_easy_setopt(c->h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c->h, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)s.max);
    curl_easy_setopt(c->h, CURLOPT_ACCEPT_ENCODING, ""); /* whatever this libcurl decodes */
    curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYPEER, req->verify ? 1L : 0L);
    curl_easy_setopt(c->h, CURLOPT_SSL_VERIFYHOST, req->verify ? 2L : 0L);
    if (req->ca_file && *req->ca_file) {
        curl_easy_setopt(c->h, CURLOPT_CAINFO, req->ca_file);
    }
    curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c->h, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(c->h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c->h, CURLOPT_HEADERDATA, &s);
    curl_easy_setopt(c->h, CURLOPT_ERRORBUFFER, errbuf);

    rc = curl_easy_perform(c->h);
    /* The header list held the token: it goes now, and nothing that follows
     * may point at it. */
    curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(hdr);
    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &status);
    resp->status = status;
    resp->date = s.date;

    if (rc != CURLE_OK) {
        resp->err = classify(rc, s.too_large);
        if (resp->err == ZBX_ERR_TOO_LARGE) {
            snprintf(resp->text, sizeof(resp->text), "answer over %zu KB", s.max / 1024);
        } else if (resp->err == ZBX_ERR_TLS && clock_unset()) {
            snprintf(resp->text, sizeof(resp->text),
                     "device clock not set: the certificate cannot be checked");
        } else {
            zbx_copy_text(resp->text, sizeof(resp->text), errbuf[0] ? errbuf : curl_easy_strerror(rc));
        }
        free(s.buf);
        return resp->err;
    }
    if (status != 200) {
        resp->err = ZBX_ERR_HTTP;
        if (status >= 300 && status < 400) {
            snprintf(resp->text, sizeof(resp->text), "HTTP %ld redirect (not followed): check the URL",
                     status);
        } else {
            snprintf(resp->text, sizeof(resp->text), "HTTP %ld", status);
        }
        free(s.buf);
        return resp->err;
    }
    resp->body = s.buf ? s.buf : strdup("");
    resp->len = s.len;
    resp->err = resp->body ? ZBX_ERR_NONE : ZBX_ERR_INTERNAL;
    return resp->err;
}

static void curl_close(struct zbx_transport *t)
{
    struct curl_ctx *c = t ? t->ctx : NULL;

    if (c) {
        curl_easy_cleanup(c->h);
        free(c);
        t->ctx = NULL;
    }
    curl_global_cleanup();
}

bool zbx_http_available(void)
{
    return true;
}

int zbx_transport_curl(struct zbx_transport *t, char *err, size_t errlen)
{
    struct curl_ctx *c;

    memset(t, 0, sizeof(*t));
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
    t->ctx = c;
    t->post = curl_post;
    t->close = curl_close;
    t->name = "curl";
    return 0;
}

#endif
