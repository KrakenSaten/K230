/*
 * Web addresses. See web_url.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "web/web_url.h"

#include <stdio.h>
#include <string.h>

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Copy in into buf without the tabs and line breaks browsers drop and
 * without the leading and trailing blanks; refuse any other control byte. */
static enum web_url_err clean(const char *in, char *buf, size_t buflen)
{
    size_t start = 0;
    size_t end;
    size_t n = 0;
    size_t i;

    if (!in) {
        return WEB_URL_EMPTY;
    }
    end = strlen(in);
    while (start < end && (unsigned char)in[start] <= ' ') {
        start++;
    }
    while (end > start && (unsigned char)in[end - 1] <= ' ') {
        end--;
    }
    if (start == end) {
        return WEB_URL_EMPTY;
    }
    for (i = start; i < end; i++) {
        unsigned char c = (unsigned char)in[i];

        if (c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            return WEB_URL_CHAR;
        }
        if (n + 1 >= buflen) {
            return WEB_URL_TOO_LONG;
        }
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return n ? WEB_URL_OK : WEB_URL_EMPTY;
}

/* Length of the scheme at s ("http" in "http://"), or 0 when s has none. */
static size_t scheme_len(const char *s)
{
    size_t i = 0;

    if (!is_alpha((unsigned char)s[0])) {
        return 0;
    }
    while (is_alpha((unsigned char)s[i]) || is_digit((unsigned char)s[i]) || s[i] == '+' ||
           s[i] == '-' || s[i] == '.') {
        i++;
    }
    return s[i] == ':' ? i : 0;
}

static bool scheme_is(const char *s, size_t n, const char *name)
{
    size_t i;

    if (strlen(name) != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (lower((unsigned char)s[i]) != name[i]) {
            return false;
        }
    }
    return true;
}

/* ---- dot segments (RFC 3986 5.2.4) ------------------------------------------ */

/* Remove "." and ".." from the path part of p (up to '?'), in place. p
 * starts with '/'. */
static void remove_dots(char *p)
{
    char out[WEB_URL_MAX];
    size_t seg_off[WEB_URL_MAX / 2];
    size_t seg_len[WEB_URL_MAX / 2];
    size_t nseg = 0;
    bool trailing = false;
    const char *query = strchr(p, '?');
    size_t plen = query ? (size_t)(query - p) : strlen(p);
    size_t i = 1;
    size_t on = 0;
    size_t k;

    if (plen == 0 || p[0] != '/') {
        return;
    }
    for (;;) {
        size_t start = i;
        size_t len;
        bool last;

        while (i < plen && p[i] != '/') {
            i++;
        }
        len = i - start;
        last = i >= plen;
        if (len == 1 && p[start] == '.') {
            trailing = last;
        } else if (len == 2 && p[start] == '.' && p[start + 1] == '.') {
            if (nseg > 0) {
                nseg--;
            }
            trailing = last;
        } else if (nseg < sizeof(seg_off) / sizeof(seg_off[0])) {
            seg_off[nseg] = start;
            seg_len[nseg] = len;
            nseg++;
            trailing = false;
        }
        if (last) {
            break;
        }
        i++; /* past the '/' */
    }
    out[on++] = '/';
    for (k = 0; k < nseg; k++) {
        if (k > 0) {
            out[on++] = '/';
        }
        memcpy(out + on, p + seg_off[k], seg_len[k]);
        on += seg_len[k];
    }
    if (trailing && nseg > 0) {
        out[on++] = '/';
    }
    if (query) {
        size_t ql = strlen(query);

        if (on + ql >= sizeof(out)) {
            return;
        }
        memcpy(out + on, query, ql);
        on += ql;
    }
    out[on] = '\0';
    memcpy(p, out, on + 1);
}

/* ---- parsing ------------------------------------------------------------------ */

static enum web_url_err parse_host(const char *a, size_t n, struct web_url *out)
{
    size_t i;
    size_t hn;
    const char *port = NULL;
    size_t port_n = 0;

    if (n == 0) {
        return WEB_URL_HOST;
    }
    for (i = 0; i < n; i++) {
        if (a[i] == '@') {
            return WEB_URL_USERINFO;
        }
    }
    if (a[0] == '[') {
        const char *close = memchr(a, ']', n);

        if (!close) {
            return WEB_URL_HOST;
        }
        hn = (size_t)(close - a) + 1;
        if (hn < 4) {
            return WEB_URL_HOST;
        }
        for (i = 1; i + 1 < hn; i++) {
            if (!is_hex((unsigned char)a[i]) && a[i] != ':' && a[i] != '.') {
                return WEB_URL_HOST;
            }
        }
        if (hn < n) {
            if (a[hn] != ':') {
                return WEB_URL_HOST;
            }
            port = a + hn + 1;
            port_n = n - hn - 1;
        }
    } else {
        const char *colon = memchr(a, ':', n);

        hn = colon ? (size_t)(colon - a) : n;
        if (colon) {
            port = colon + 1;
            port_n = n - hn - 1;
        }
        if (hn == 0) {
            return WEB_URL_HOST;
        }
        for (i = 0; i < hn; i++) {
            unsigned char c = (unsigned char)a[i];

            if (c >= 0x80) {
                return WEB_URL_IDN;
            }
            if (!is_alpha(c) && !is_digit(c) && c != '-' && c != '.' && c != '_') {
                return WEB_URL_HOST;
            }
            if (c == '.' && (i == 0 || a[i - 1] == '.')) {
                return WEB_URL_HOST;
            }
        }
        if (a[hn - 1] == '.') {
            hn--; /* "example.com." is example.com */
        }
        if (hn == 0 || a[0] == '-') {
            return WEB_URL_HOST;
        }
    }
    if (hn >= sizeof(out->host)) {
        return WEB_URL_HOST;
    }
    for (i = 0; i < hn; i++) {
        out->host[i] = (char)lower((unsigned char)a[i]);
    }
    out->host[hn] = '\0';
    out->port = 0;
    if (port && port_n > 0) {
        long v = 0;

        if (port_n > 5) {
            return WEB_URL_PORT;
        }
        for (i = 0; i < port_n; i++) {
            if (!is_digit((unsigned char)port[i])) {
                return WEB_URL_PORT;
            }
            v = v * 10 + (port[i] - '0');
        }
        if (v < 1 || v > 65535) {
            return WEB_URL_PORT;
        }
        out->port = (int)v;
    }
    return WEB_URL_OK;
}

/* The path and query from p (which starts at '/', '?' or is empty), up to the
 * fragment, percent-encoding what must not travel raw. */
static enum web_url_err parse_path(const char *p, struct web_url *out)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    size_t i;

    if (*p != '/') {
        out->path[n++] = '/';
    }
    for (i = 0; p[i] && p[i] != '#'; i++) {
        unsigned char c = (unsigned char)p[i];

        if (n + 4 >= sizeof(out->path)) {
            return WEB_URL_TOO_LONG;
        }
        if (c == '\\') {
            out->path[n++] = '/';
        } else if (c <= ' ' || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '`' ||
                   c == '{' || c == '}' || c == '|' || c == '^') {
            out->path[n++] = '%';
            out->path[n++] = hex[c >> 4];
            out->path[n++] = hex[c & 15];
        } else {
            out->path[n++] = (char)c;
        }
    }
    out->path[n] = '\0';
    remove_dots(out->path);
    return WEB_URL_OK;
}

static enum web_url_err parse_clean(const char *s, struct web_url *out)
{
    size_t sl = scheme_len(s);
    const char *a;
    size_t an;

    memset(out, 0, sizeof(*out));
    if (sl == 0) {
        return WEB_URL_SCHEME;
    }
    if (scheme_is(s, sl, "about")) {
        const char *name = s + sl + 1;
        size_t nn = strcspn(name, "?#");

        out->scheme = WEB_SCHEME_ABOUT;
        if ((nn == 4 && scheme_is(name, 4, "home")) || (nn == 5 && scheme_is(name, 5, "blank"))) {
            size_t k;

            for (k = 0; k < nn; k++) {
                out->path[k] = (char)lower((unsigned char)name[k]);
            }
            out->path[nn] = '\0';
            return WEB_URL_OK;
        }
        return WEB_URL_ABOUT;
    }
    if (scheme_is(s, sl, "https")) {
        out->scheme = WEB_SCHEME_HTTPS;
    } else if (scheme_is(s, sl, "http")) {
        out->scheme = WEB_SCHEME_HTTP;
    } else {
        return WEB_URL_SCHEME;
    }
    s += sl + 1;
    if (s[0] != '/' || s[1] != '/') {
        return WEB_URL_HOST;
    }
    a = s + 2;
    an = strcspn(a, "/?#\\");
    {
        enum web_url_err e = parse_host(a, an, out);

        if (e != WEB_URL_OK) {
            return e;
        }
    }
    if ((out->scheme == WEB_SCHEME_HTTPS && out->port == 443) ||
        (out->scheme == WEB_SCHEME_HTTP && out->port == 80)) {
        out->port = 0;
    }
    return parse_path(a + an, out);
}

enum web_url_err web_url_parse(const char *in, struct web_url *out)
{
    char buf[WEB_URL_MAX];
    enum web_url_err e = clean(in, buf, sizeof(buf));
    struct web_url tmp;

    if (e != WEB_URL_OK) {
        return e;
    }
    e = parse_clean(buf, &tmp);
    if (e == WEB_URL_OK) {
        *out = tmp;
    }
    return e;
}

/* Schemes someone might type or paste that must never be taken for a host
 * name ("javascript:alert(1)" is not host "javascript", port "alert(1)"). */
static bool known_foreign_scheme(const char *s, size_t n)
{
    static const char *const names[] = { "javascript", "data", "file", "ftp", "ftps", "mailto",
                                         "blob", "view-source", "tel", "sms", "ws", "wss",
                                         "chrome", "gopher", "ssh", "telnet", "vbscript" };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (scheme_is(s, n, names[i])) {
            return true;
        }
    }
    return false;
}

enum web_url_err web_url_from_input(const char *typed, struct web_url *out)
{
    char buf[WEB_URL_MAX];
    char full[WEB_URL_MAX];
    enum web_url_err e = clean(typed, buf, sizeof(buf));
    struct web_url tmp;
    size_t sl;

    if (e != WEB_URL_OK) {
        return e;
    }
    if (strchr(buf, ' ')) {
        return WEB_URL_SPACE;
    }
    sl = scheme_len(buf);
    if (sl && (strncmp(buf + sl, "://", 3) == 0 || scheme_is(buf, sl, "about") ||
               scheme_is(buf, sl, "http") || scheme_is(buf, sl, "https") ||
               known_foreign_scheme(buf, sl))) {
        e = parse_clean(buf, &tmp);
    } else {
        if (strlen(buf) + 8 >= sizeof(full)) {
            return WEB_URL_TOO_LONG;
        }
        memcpy(full, "https://", 8);
        memcpy(full + 8, buf, strlen(buf) + 1);
        e = parse_clean(full, &tmp);
    }
    if (e == WEB_URL_OK) {
        *out = tmp;
    }
    return e;
}

enum web_url_err web_url_resolve(const struct web_url *base, const char *ref, struct web_url *out)
{
    char r[WEB_URL_MAX];
    char full[WEB_URL_MAX * 2];
    char origin[WEB_HOST_MAX + 32];
    enum web_url_err e = clean(ref, r, sizeof(r));
    struct web_url tmp;
    int n;

    if (e == WEB_URL_EMPTY || (e == WEB_URL_OK && r[0] == '#')) {
        *out = *base;
        return WEB_URL_OK;
    }
    if (e != WEB_URL_OK) {
        return e;
    }
    if (scheme_len(r)) {
        e = parse_clean(r, &tmp);
        if (e == WEB_URL_OK) {
            *out = tmp;
        }
        return e;
    }
    if (base->scheme != WEB_SCHEME_HTTP && base->scheme != WEB_SCHEME_HTTPS) {
        return WEB_URL_SCHEME;
    }
    if ((r[0] == '/' || r[0] == '\\') && (r[1] == '/' || r[1] == '\\')) {
        n = snprintf(full, sizeof(full), "%s:%s", base->scheme == WEB_SCHEME_HTTPS ? "https" : "http", r);
    } else {
        if (web_url_origin(base, origin, sizeof(origin)) < 0) {
            return WEB_URL_HOST;
        }
        if (r[0] == '/' || r[0] == '\\') {
            n = snprintf(full, sizeof(full), "%s%s", origin, r);
        } else if (r[0] == '?') {
            size_t pl = strcspn(base->path, "?");

            n = snprintf(full, sizeof(full), "%s%.*s%s", origin, (int)pl, base->path, r);
        } else {
            size_t pl = strcspn(base->path, "?");
            size_t dir = pl;

            while (dir > 0 && base->path[dir - 1] != '/') {
                dir--;
            }
            n = snprintf(full, sizeof(full), "%s%.*s%s", origin, (int)dir, base->path, r);
        }
    }
    if (n < 0 || (size_t)n >= WEB_URL_MAX) {
        return WEB_URL_TOO_LONG;
    }
    e = parse_clean(full, &tmp);
    if (e == WEB_URL_OK) {
        *out = tmp;
    }
    return e;
}

int web_url_origin(const struct web_url *u, char *out, size_t outlen)
{
    int n;

    if (u->scheme == WEB_SCHEME_ABOUT) {
        n = snprintf(out, outlen, "about:");
    } else if (u->scheme == WEB_SCHEME_HTTP || u->scheme == WEB_SCHEME_HTTPS) {
        const char *s = u->scheme == WEB_SCHEME_HTTPS ? "https" : "http";

        n = u->port ? snprintf(out, outlen, "%s://%s:%d", s, u->host, u->port)
                    : snprintf(out, outlen, "%s://%s", s, u->host);
    } else {
        n = -1;
    }
    return (n < 0 || (size_t)n >= outlen) ? -1 : n;
}

int web_url_format(const struct web_url *u, char *out, size_t outlen)
{
    int n;

    if (u->scheme == WEB_SCHEME_ABOUT) {
        n = snprintf(out, outlen, "about:%s", u->path);
    } else {
        char origin[WEB_HOST_MAX + 32];

        if (web_url_origin(u, origin, sizeof(origin)) < 0) {
            return -1;
        }
        n = snprintf(out, outlen, "%s%s", origin, u->path);
    }
    return (n < 0 || (size_t)n >= outlen) ? -1 : n;
}

size_t web_copy(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;

    if (cap == 0) {
        return 0;
    }
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
    return n;
}

bool web_url_is_secure(const struct web_url *u)
{
    return u->scheme == WEB_SCHEME_HTTPS || u->scheme == WEB_SCHEME_ABOUT;
}

const char *web_url_err_text(enum web_url_err e)
{
    switch (e) {
    case WEB_URL_OK:
        return "OK";
    case WEB_URL_EMPTY:
        return "Type an address, for example example.com";
    case WEB_URL_TOO_LONG:
        return "That address is too long";
    case WEB_URL_SCHEME:
        return "Only http:// and https:// addresses can be opened";
    case WEB_URL_HOST:
        return "That is not a valid address";
    case WEB_URL_IDN:
        return "International domain names are not supported";
    case WEB_URL_PORT:
        return "The port in that address is not valid";
    case WEB_URL_USERINFO:
        return "Addresses with a user name or password are not supported";
    case WEB_URL_SPACE:
        return "Not an address. There is no built-in search: type an address like example.com";
    case WEB_URL_CHAR:
        return "That address contains a control character";
    case WEB_URL_ABOUT:
        return "Unknown about: page";
    }
    return "That is not a valid address";
}
