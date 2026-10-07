/*
 * The parts of fetching that do not depend on the transport. See web_fetch.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "web/web_fetch.h"
#include "web/web_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void web_fetch_resp_free(struct web_fetch_resp *r)
{
    if (r) {
        free(r->body);
        r->body = NULL;
        r->len = 0;
    }
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

void web_fetch_parse_type(const char *ct, char *type, size_t tlen, char *charset, size_t clen)
{
    size_t n = 0;
    const char *p;

    if (tlen) {
        type[0] = '\0';
    }
    if (clen) {
        charset[0] = '\0';
    }
    if (!ct) {
        return;
    }
    while (*ct == ' ') {
        ct++;
    }
    for (p = ct; *p && *p != ';' && *p != ' ' && n + 1 < tlen; p++) {
        type[n++] = (char)lower((unsigned char)*p);
    }
    if (tlen) {
        type[n] = '\0';
    }
    p = strchr(ct, ';');
    while (p) {
        p++;
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, "charset=", 8) == 0 || strncmp(p, "Charset=", 8) == 0 ||
            strncmp(p, "CHARSET=", 8) == 0) {
            size_t k = 0;

            p += 8;
            if (*p == '"') {
                p++;
            }
            while (*p && *p != ';' && *p != '"' && *p != ' ' && k + 1 < clen) {
                charset[k++] = *p++;
            }
            if (clen) {
                charset[k] = '\0';
            }
            return;
        }
        p = strchr(p, ';');
    }
}

static bool default_in(const char *path, bool v6)
{
    FILE *f = fopen(path, "r");
    char line[256];
    bool found = false;

    if (!f) {
        return false;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        if (v6) {
            /* dest, prefix length, ..., interface: ::/0 not on lo */
            if (strncmp(line, "00000000000000000000000000000000 00 ", 36) == 0 && !strstr(line, " lo\n")) {
                found = true;
            }
        } else {
            char ifname[32];
            char dest[16];

            if (sscanf(line, "%31s %15s", ifname, dest) == 2 && strcmp(dest, "00000000") == 0) {
                found = true;
            }
        }
    }
    fclose(f);
    return found;
}

bool web_net_has_route(void)
{
    const char *root = getenv("POCKETOS_BROWSER_PROC_NET");
    char v4[256];
    char v6[256];

    if (!root || !*root) {
        root = "/proc/net";
    }
    snprintf(v4, sizeof(v4), "%s/route", root);
    snprintf(v6, sizeof(v6), "%s/ipv6_route", root);
    return default_in(v4, false) || default_in(v6, true);
}

static void fail(struct web_fetch_resp *r, enum web_fail f, const char *text)
{
    r->failed = true;
    r->fail = f;
    web_copy(r->text, sizeof(r->text), text ? text : "");
}

int web_fetch(struct web_fetcher *f, const struct web_fetch_req *req, struct web_fetch_resp *resp)
{
    char url[WEB_URL_MAX];
    int hops;

    memset(resp, 0, sizeof(*resp));
    web_copy(url, sizeof(url), req->url);
    for (hops = 0;; hops++) {
        struct web_url cur;
        struct web_url to;
        struct web_hop h;

        web_copy(resp->final_url, sizeof(resp->final_url), url);
        if (web_url_parse(url, &cur) != WEB_URL_OK ||
            (cur.scheme != WEB_SCHEME_HTTP && cur.scheme != WEB_SCHEME_HTTPS)) {
            fail(resp, hops ? WEB_FAIL_BAD_REDIRECT : WEB_FAIL_URL,
                 hops ? "the server sent Browser to an address it cannot open"
                      : "not an address Browser can open");
            return -1;
        }
        memset(&h, 0, sizeof(h));
        f->hop(f, req, url, &h);
        resp->status = h.status;
        if (h.failed) {
            free(h.body);
            fail(resp, h.fail, h.text);
            return -1;
        }
        if (h.status >= 300 && h.status < 400 && h.status != 304 && h.location[0]) {
            free(h.body);
            if (hops + 1 > WEB_FETCH_REDIRECTS) {
                fail(resp, WEB_FAIL_REDIRECTS, "more than 8 redirects");
                return -1;
            }
            if (web_url_parse(h.location, &to) != WEB_URL_OK ||
                (to.scheme != WEB_SCHEME_HTTP && to.scheme != WEB_SCHEME_HTTPS)) {
                fail(resp, WEB_FAIL_BAD_REDIRECT, "the server sent Browser to an address it cannot open");
                return -1;
            }
            if (cur.scheme == WEB_SCHEME_HTTPS && to.scheme == WEB_SCHEME_HTTP) {
                char shown[WEB_URL_MAX];

                /* Never quietly from a secure page to an insecure one. */
                web_url_origin(&to, shown, sizeof(shown));
                fail(resp, WEB_FAIL_INSECURE_REDIRECT, shown);
                return -1;
            }
            if (web_url_format(&to, url, sizeof(url)) < 0) {
                fail(resp, WEB_FAIL_BAD_REDIRECT, "the redirect address is too long");
                return -1;
            }
            continue;
        }
        web_fetch_parse_type(h.content_type, resp->type, sizeof(resp->type), resp->charset,
                             sizeof(resp->charset));
        resp->body = h.body ? h.body : calloc(1, 1);
        resp->len = h.body ? h.len : 0;
        resp->cut = h.cut;
        if (!resp->body) {
            fail(resp, WEB_FAIL_INTERNAL, "out of memory");
            return -1;
        }
        return 0;
    }
}
