/*
 * The line protocol between the Browser app (in the shell) and its helper,
 * pos-browser. One line per message, tab-separated fields, text fields
 * escaped (\\ \t \n), UTF-8. Every line fits WEB_LINE_MAX.
 *
 * Shell to helper:
 *
 *   open  <seq> <max width> <flags> <url>   flags: 'i' fetch images, 'r' reload
 *   stop  <seq>                             abandon seq (and its images)
 *   quit
 *
 * Helper to shell:
 *
 *   hello    <proto> <features>              features: "net", "img", "fake"
 *   progress <seq> <stage> <bytes>           stage: connect, receive, images
 *   page     <seq> <flags> <http> <url> <title>   a document follows, then:
 *     link   <id> <supported> <url>          ids from 0, in order
 *     image  <id> <link> <w> <h> <alt>       ids from 0, in order; no address
 *     block  <type> <level> <image>
 *     run    <flags> <link> <text>
 *   end      <seq>                           the document is complete
 *   fail     <seq> <kind> <http> <url> <text>
 *   pixels   <seq> <image> <w> <h> <file>    an RGB565 file in the image directory
 *   nopixels <seq> <image> <why>
 *   idle     <seq>                           nothing more for seq
 *
 * The shell never gets an image address or a byte of an image file format:
 * the helper decodes, scales and writes w*h RGB565 pixels, and the shell
 * reads exactly that many bytes from a file whose name it checks
 * (web_rx_file_name_ok).
 *
 * The receiver (web_rx) checks every line against the document it is
 * building - ids in order, every reference in range, every number bounded -
 * and throws a page away whole rather than show part of a bad one.
 *
 * Pure C: tests/web_proto_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_PROTO_H
#define POCKETOS_WEB_PROTO_H

#include "web/web_doc.h"
#include "web/web_url.h"

#include <stdbool.h>
#include <stddef.h>

#define WEB_PROTO_VERSION 1
#define WEB_LINE_MAX 8192
/* A run is sent in pieces of at most this many bytes. */
#define WEB_RUN_PIECE 1024
#define WEB_FAIL_TEXT_MAX 256
#define WEB_FILE_NAME_MAX 48
/* The largest image the shell accepts, per side and in pixels. */
#define WEB_IMAGE_SIDE_MAX 1600
#define WEB_IMAGE_PIXELS_MAX (560u * 1024u)

/* page flags */
#define WEB_PAGE_SECURE 0x01
#define WEB_PAGE_TRUNCATED 0x02
#define WEB_PAGE_IMAGES 0x04    /* pixels lines will follow */

enum web_fail {
    WEB_FAIL_URL = 0,           /* the address itself */
    WEB_FAIL_OFFLINE,           /* no network at all (no route) */
    WEB_FAIL_DNS,
    WEB_FAIL_CONNECT,           /* refused, unreachable */
    WEB_FAIL_TIMEOUT,
    WEB_FAIL_TLS,               /* the certificate or the handshake */
    WEB_FAIL_CLOCK,             /* TLS, with the clock unset */
    WEB_FAIL_REDIRECTS,         /* too many */
    WEB_FAIL_INSECURE_REDIRECT, /* https to http, not followed */
    WEB_FAIL_BAD_REDIRECT,      /* to something that is not http(s) */
    WEB_FAIL_TYPE,              /* not a page Browser can show */
    WEB_FAIL_RESPONSE,          /* the server's answer could not be read */
    WEB_FAIL_STOPPED,
    WEB_FAIL_NO_NETWORK_BUILD,  /* this helper was built without an HTTP client */
    WEB_FAIL_INTERNAL,
    WEB_FAIL_COUNT
};

const char *web_fail_name(enum web_fail f);
enum web_fail web_fail_from_name(const char *s);

/* ---- writing ------------------------------------------------------------------- */

/* Escape n bytes of s into out. Returns the length, or -1 when out is too
 * small. */
int web_escape(const char *s, size_t n, char *out, size_t cap);

/* Where lines go: return 0, or -1 to stop. line has no '\n'. */
typedef int (*web_emit_fn)(void *ctx, const char *line, size_t n);

/* The page, link, image, block and run lines of doc, then end. */
int web_proto_send_doc(web_emit_fn emit, void *ctx, int seq, unsigned flags, int http,
                       const char *url, const struct web_doc *doc);

/* ---- reading ------------------------------------------------------------------- */

enum web_rx_kind {
    WEB_RX_NONE = 0,    /* a line of a page still coming, or nothing to report */
    WEB_RX_HELLO,
    WEB_RX_PROGRESS,
    WEB_RX_PAGE,        /* a whole page arrived: web_rx_take() it */
    WEB_RX_FAIL,
    WEB_RX_PIXELS,
    WEB_RX_NOPIXELS,
    WEB_RX_IDLE,
    WEB_RX_BAD          /* not a line of this protocol; any page being received is gone */
};

struct web_rx_msg {
    enum web_rx_kind kind;
    int seq;
    int proto;
    char features[32];
    char stage[16];
    long bytes;
    enum web_fail fail;
    int http;
    unsigned flags;
    char url[WEB_URL_MAX];
    char text[WEB_FAIL_TEXT_MAX];
    int image;
    int w;
    int h;
    char file[WEB_FILE_NAME_MAX];
};

struct web_rx {
    struct web_doc doc;     /* the page being received */
    bool open;
    int seq;
    unsigned flags;
    int http;
    char url[WEB_URL_MAX];
    unsigned bad;           /* lines refused so far */
};

void web_rx_init(struct web_rx *rx);
void web_rx_free(struct web_rx *rx);

/* One line, without its '\n'; line is modified. */
enum web_rx_kind web_rx_line(struct web_rx *rx, char *line, struct web_rx_msg *msg);

/* After WEB_RX_PAGE: move the page into out (which is freed first); rx
 * starts empty again. */
void web_rx_take(struct web_rx *rx, struct web_doc *out);

/* The only names the helper may give an image file: "<seq>-<id>.rgb565",
 * digits only, so no path can be named. */
bool web_rx_file_name_ok(const char *name);

#endif
