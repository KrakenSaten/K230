/*
 * The fake network: deterministic pages answered in-process, for the tests
 * and the simulator's demo (pos-browser --fake). Nothing here opens a socket.
 *
 * Only hosts under doors.test exist:
 *
 *   https://doors.test/            a demo page: headings, links, a list,
 *                                  preformatted text, a quote, an image
 *   https://doors.test/about       a short page, linked from the demo
 *   https://doors.test/long        300 paragraphs
 *   https://doors.test/big         3 MB of HTML (cut at the page limit)
 *   https://doors.test/text        text/plain
 *   https://doors.test/latin1      windows-1252, declared in the header
 *   https://doors.test/missing     404 with a page
 *   https://doors.test/redirect    302 to /about
 *   https://doors.test/loop        redirects to itself
 *   https://doors.test/insecure    redirects to http://
 *   https://doors.test/file        redirects to file:///etc/passwd
 *   https://doors.test/zip         application/zip
 *   https://doors.test/garbage     random bytes as text/html
 *   https://doors.test/slow        answers after 5 s, stoppable
 *   https://doors.test/hang        never answers and cannot be stopped
 *                                  (a helper stuck in the kernel)
 *   https://doors.test/crash       the helper crashes
 *   https://doors.test/img/<w>x<h>.jpg   a picture of that size
 *   http://doors.test/...          redirects to https
 *   https://tls.doors.test/        certificate refused
 *   https://clock.doors.test/      certificate refused, clock unset
 *   https://timeout.doors.test/    times out
 *   https://refused.doors.test/    connection refused
 *   https://offline.doors.test/    no network at all
 *   anything else                  no such host
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "web/web_fetch.h"
#include "web/web_url.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char demo[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Doors Browser demo</title></head>"
    "<body><h1>Doors Browser</h1>"
    "<p>A <b>lightweight</b> reader for simple web pages on the T-Display K230. It shows text, "
    "headings, lists, links and pictures, and leaves out <code>JavaScript</code> and CSS.</p>"
    "<h2>Try a link</h2><ul><li><a href=\"/about\">About this page</a></li>"
    "<li><a href=\"/long\">A long page</a> to scroll</li>"
    "<li><a href=\"/missing\">A page that is not there</a></li>"
    "<li><a href=\"https://tls.doors.test/\">A certificate that is refused</a></li>"
    "<li><a href=\"mailto:someone@example.com\">An e-mail link</a> (not supported)</li></ul>"
    "<p><img src=\"/img/480x270.jpg\" alt=\"A test picture\" width=\"480\" height=\"270\"></p>"
    "<blockquote>Stability first, then usable rendering, then low memory.</blockquote>"
    "<pre>$ doors browser\n  simple pages, fast</pre>"
    "<form action=\"/search\"><input name=\"q\"></form>"
    "<hr><p>Caf&eacute; &amp; cr&egrave;me &mdash; entities work.</p></body></html>";

static const char about[] =
    "<html><head><title>About</title></head><body><h2>About</h2><p>This page came from the fake "
    "network: nothing left the device.</p><p><a href=\"/\">Back to the demo</a></p></body></html>";

static void body(struct web_hop *h, long status, const char *type, const char *text, size_t n)
{
    h->status = status;
    web_copy(h->content_type, sizeof(h->content_type), type);
    h->body = malloc(n + 1);
    if (!h->body) {
        h->failed = true;
        h->fail = WEB_FAIL_INTERNAL;
        return;
    }
    memcpy(h->body, text, n);
    h->body[n] = '\0';
    h->len = n;
}

static void redirect(struct web_hop *h, const char *to)
{
    h->status = 302;
    web_copy(h->location, sizeof(h->location), to);
}

static void failure(struct web_hop *h, enum web_fail f, const char *text)
{
    h->failed = true;
    h->fail = f;
    web_copy(h->text, sizeof(h->text), text);
}

/* Sleep ms in steps, asking abort() between them. Returns true when stopped. */
static bool wait_stoppable(const struct web_fetch_req *req, int ms)
{
    while (ms > 0) {
        struct timespec d = { 0, 20 * 1000000L };

        if (req->abort_cb && req->abort_cb(req->ctx)) {
            return true;
        }
        nanosleep(&d, NULL);
        ms -= 20;
    }
    return false;
}

static void page(struct web_hop *h, const struct web_fetch_req *req, const char *path)
{
    size_t max = req->max_bytes ? req->max_bytes : WEB_PAGE_BYTES_MAX;

    if (strcmp(path, "/") == 0) {
        body(h, 200, "text/html; charset=utf-8", demo, sizeof(demo) - 1);
    } else if (strcmp(path, "/about") == 0) {
        body(h, 200, "text/html", about, sizeof(about) - 1);
    } else if (strcmp(path, "/long") == 0 || strcmp(path, "/big") == 0) {
        bool big = strcmp(path, "/big") == 0;
        size_t cap = big ? 3u * 1024u * 1024u : 64u * 1024u;
        char *b = malloc(cap + 256);
        size_t n = 0;
        int i;

        if (!b) {
            failure(h, WEB_FAIL_INTERNAL, "out of memory");
            return;
        }
        n += (size_t)sprintf(b, "<title>%s</title><h1>%s</h1>", big ? "Big" : "Long", big ? "Big" : "Long");
        for (i = 1; n + 200 < cap && (big || i <= 300); i++) {
            n += (size_t)sprintf(b + n, "<p>Paragraph %d. The quick brown fox jumps over the lazy dog, "
                                        "again and again, so there is something to scroll.</p>", i);
        }
        if (n > max && req->keep_cut) {
            n = max;
            h->cut = true;
        }
        body(h, 200, "text/html", b, n);
        free(b);
    } else if (strcmp(path, "/text") == 0) {
        static const char t[] = "Plain text.\n\nIt keeps  its spacing\nand its lines.\n";

        body(h, 200, "text/plain; charset=utf-8", t, sizeof(t) - 1);
    } else if (strcmp(path, "/latin1") == 0) {
        static const char t[] = "<title>Latin-1</title><p>Caf\xe9 \x93quoted\x94</p>";

        body(h, 200, "text/html; charset=windows-1252", t, sizeof(t) - 1);
    } else if (strcmp(path, "/missing") == 0) {
        static const char t[] = "<title>Not Found</title><h1>404</h1><p>No such page.</p>";

        body(h, 404, "text/html", t, sizeof(t) - 1);
    } else if (strcmp(path, "/redirect") == 0) {
        redirect(h, "https://doors.test/about");
    } else if (strcmp(path, "/loop") == 0) {
        redirect(h, "https://doors.test/loop");
    } else if (strcmp(path, "/insecure") == 0) {
        redirect(h, "http://doors.test/about");
    } else if (strcmp(path, "/file") == 0) {
        redirect(h, "file:///etc/passwd");
    } else if (strcmp(path, "/zip") == 0) {
        body(h, 200, "application/zip", "PK\x03\x04", 4);
    } else if (strcmp(path, "/garbage") == 0) {
        char b[8192];
        unsigned x = 7;
        size_t i;

        for (i = 0; i < sizeof(b); i++) {
            x = x * 1103515245u + 12345u;
            b[i] = (char)(x >> 16);
        }
        body(h, 200, "text/html", b, sizeof(b));
    } else if (strcmp(path, "/slow") == 0) {
        if (wait_stoppable(req, 5000)) {
            failure(h, WEB_FAIL_STOPPED, "stopped");
            return;
        }
        body(h, 200, "text/html", "<title>Slow</title><p>Finally.</p>", 34);
    } else if (strcmp(path, "/hang") == 0) {
        sleep(3600);
        failure(h, WEB_FAIL_TIMEOUT, "hung");
    } else if (strcmp(path, "/crash") == 0) {
        raise(SIGSEGV);
    } else if (strncmp(path, "/img/", 5) == 0) {
        int w;
        int hh;
        char b[64];

        if (sscanf(path + 5, "%dx%d.jpg", &w, &hh) == 2 && w > 0 && hh > 0) {
            int n = snprintf(b, sizeof(b), "DOORS-FAKE-IMAGE %d %d\n", w, hh);

            body(h, 200, "image/jpeg", b, (size_t)n);
        } else {
            body(h, 404, "text/plain", "no", 2);
        }
    } else {
        static const char t[] = "<title>Not Found</title><p>The fake network has no such page.</p>";

        body(h, 404, "text/html", t, sizeof(t) - 1);
    }
}

static int fake_hop(struct web_fetcher *f, const struct web_fetch_req *req, const char *url, struct web_hop *h)
{
    struct web_url u;

    (void)f;
    memset(h, 0, sizeof(*h));
    if (web_url_parse(url, &u) != WEB_URL_OK) {
        failure(h, WEB_FAIL_URL, "not an address");
        return -1;
    }
    if (strcmp(u.host, "tls.doors.test") == 0) {
        failure(h, WEB_FAIL_TLS, "SSL certificate problem: self-signed certificate");
    } else if (strcmp(u.host, "clock.doors.test") == 0) {
        failure(h, WEB_FAIL_CLOCK, "the device clock is not set, so no certificate can be checked");
    } else if (strcmp(u.host, "timeout.doors.test") == 0) {
        failure(h, WEB_FAIL_TIMEOUT, "Connection timed out after 15000 milliseconds");
    } else if (strcmp(u.host, "refused.doors.test") == 0) {
        failure(h, WEB_FAIL_CONNECT, "Failed to connect to refused.doors.test port 443: Connection refused");
    } else if (strcmp(u.host, "offline.doors.test") == 0) {
        failure(h, WEB_FAIL_OFFLINE, "no network");
    } else if (strcmp(u.host, "doors.test") != 0) {
        char t[WEB_HOST_MAX + 40];

        snprintf(t, sizeof(t), "Could not resolve host: %s", u.host);
        failure(h, WEB_FAIL_DNS, t);
    } else if (u.scheme == WEB_SCHEME_HTTP) {
        char to[WEB_URL_MAX + 32];

        snprintf(to, sizeof(to), "https://doors.test%s", u.path);
        redirect(h, to);
    } else {
        if (req->progress_cb) {
            req->progress_cb(req->ctx, 0);
        }
        page(h, req, u.path);
        if (!h->failed && h->body && h->len > (req->max_bytes ? req->max_bytes : WEB_PAGE_BYTES_MAX)) {
            if (req->keep_cut) {
                h->len = req->max_bytes;
                h->body[h->len] = '\0';
                h->cut = true;
            } else {
                free(h->body);
                h->body = NULL;
                failure(h, WEB_FAIL_TYPE, "too large");
            }
        }
    }
    return h->failed ? -1 : 0;
}

static void fake_close(struct web_fetcher *f)
{
    (void)f;
}

int web_fetcher_fake(struct web_fetcher *f)
{
    memset(f, 0, sizeof(*f));
    f->hop = fake_hop;
    f->close = fake_close;
    f->name = "fake";
    return 0;
}
