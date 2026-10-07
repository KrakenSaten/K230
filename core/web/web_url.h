/*
 * Web addresses for the Browser (docs/apps/BROWSER.md): what the address
 * field accepts, how a link on a page is resolved against the page, and the
 * one place that decides which schemes are ever fetched.
 *
 * Only http:// and https:// are fetched. about:home and about:blank are the
 * Browser's own pages and never leave the unit. Everything else - file:,
 * data:, javascript:, ftp:, mailto: and whatever else a page may link to - is
 * refused here, before anything reaches libcurl, so a page can never make the
 * helper open a local file or run anything.
 *
 * What was typed becomes an address by a fixed rule (web_url_from_input):
 * with no scheme, https:// is put in front. Nothing here ever turns https
 * into http: a plain-http address is only fetched when it says http://.
 *
 * Not supported, and refused with a reason rather than guessed at:
 * international (non-ASCII) host names, user names or passwords in the
 * address, and anything with a space in it (there is no built-in search).
 *
 * Pure C, no allocation, bounded by WEB_URL_MAX: tests/web_url_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_URL_H
#define POCKETOS_WEB_URL_H

#include <stdbool.h>
#include <stddef.h>

/* The longest address, terminator included. Longer ones are refused. */
#define WEB_URL_MAX 2048
#define WEB_HOST_MAX 256

enum web_scheme {
    WEB_SCHEME_NONE = 0,
    WEB_SCHEME_HTTP,
    WEB_SCHEME_HTTPS,
    WEB_SCHEME_ABOUT
};

enum web_url_err {
    WEB_URL_OK = 0,
    WEB_URL_EMPTY,
    WEB_URL_TOO_LONG,
    WEB_URL_SCHEME,        /* a scheme the Browser never fetches */
    WEB_URL_HOST,          /* missing or malformed host */
    WEB_URL_IDN,           /* a non-ASCII host name */
    WEB_URL_PORT,
    WEB_URL_USERINFO,      /* user:password@ */
    WEB_URL_SPACE,         /* typed words, not an address */
    WEB_URL_CHAR,          /* a control character */
    WEB_URL_ABOUT          /* an about: page that does not exist */
};

struct web_url {
    enum web_scheme scheme;
    char host[WEB_HOST_MAX];   /* lower case; IPv6 keeps its brackets */
    int port;                  /* 0: the scheme's default */
    char path[WEB_URL_MAX];    /* path and query, always starting with '/';
                                * for about: the page name */
};

/* Parse an absolute address. The fragment is dropped (the Browser does not
 * scroll to anchors); spaces and bytes above 0x7e in the path are
 * percent-encoded; tabs and line breaks are removed, as browsers do. */
enum web_url_err web_url_parse(const char *in, struct web_url *out);

/* What the address field turns into: trimmed, https:// added when there is
 * no scheme. */
enum web_url_err web_url_from_input(const char *typed, struct web_url *out);

/* A reference found on a page (href, src), resolved against the page's own
 * address. Relative paths, "..", "//host", "?query" all work. */
enum web_url_err web_url_resolve(const struct web_url *base, const char *ref, struct web_url *out);

/* Write the address back out. Returns the length, or -1 when it does not fit. */
int web_url_format(const struct web_url *u, char *out, size_t outlen);

/* scheme://host[:port], never the path or the query: what logs may carry,
 * since a query can hold a token. */
int web_url_origin(const struct web_url *u, char *out, size_t outlen);

bool web_url_is_secure(const struct web_url *u);

/* Copy src into dst (cap bytes), cut at a UTF-8 character boundary when it
 * does not fit. Returns the length copied. */
size_t web_copy(char *dst, size_t cap, const char *src);

/* A short sentence for the screen. */
const char *web_url_err_text(enum web_url_err e);

#endif
