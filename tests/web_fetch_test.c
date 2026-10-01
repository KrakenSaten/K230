/*
 * The Browser's fetch rules (core/web/web_fetch.h), applied the same for
 * every fetcher and checked here on the fake network: redirects followed,
 * never from https to http, never to another scheme, at most eight; a page
 * past the size cap kept cut, a picture past it refused; STOP; the
 * Content-Type split; and telling "no network" from "that server".
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int stop_now;

static int abort_cb(void *ctx)
{
    (void)ctx;
    return stop_now;
}

static int get(struct web_fetcher *f, const char *url, size_t max, bool keep, struct web_fetch_resp *r)
{
    struct web_fetch_req req;

    memset(&req, 0, sizeof(req));
    req.url = url;
    req.max_bytes = max;
    req.keep_cut = keep;
    req.abort_cb = abort_cb;
    return web_fetch(f, &req, r);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    fputs(text, f);
    fclose(f);
}

int main(void)
{
    struct web_fetcher f;
    struct web_fetch_resp r;
    char type[64];
    char cs[32];
    char dir[] = "/tmp/web_fetch_test.XXXXXX";
    char path[128];

    web_fetcher_fake(&f);

    check("a page", get(&f, "https://doors.test/", 0, true, &r) == 0 && r.status == 200 &&
                        strcmp(r.type, "text/html") == 0 && strcmp(r.charset, "utf-8") == 0 &&
                        strstr(r.body, "Doors Browser") != NULL);
    web_fetch_resp_free(&r);
    check("a redirect is followed, and the final address kept",
          get(&f, "https://doors.test/redirect", 0, true, &r) == 0 &&
              strcmp(r.final_url, "https://doors.test/about") == 0);
    web_fetch_resp_free(&r);
    check("http to https is followed", get(&f, "http://doors.test/about", 0, true, &r) == 0 &&
                                           strcmp(r.final_url, "https://doors.test/about") == 0);
    web_fetch_resp_free(&r);
    check("https to http is not", get(&f, "https://doors.test/insecure", 0, true, &r) != 0 &&
                                      r.fail == WEB_FAIL_INSECURE_REDIRECT &&
                                      strcmp(r.text, "http://doors.test") == 0 && r.body == NULL);
    web_fetch_resp_free(&r);
    check("a redirect to file:// is not", get(&f, "https://doors.test/file", 0, true, &r) != 0 &&
                                              r.fail == WEB_FAIL_BAD_REDIRECT);
    web_fetch_resp_free(&r);
    check("a redirect loop ends after eight", get(&f, "https://doors.test/loop", 0, true, &r) != 0 &&
                                                  r.fail == WEB_FAIL_REDIRECTS);
    web_fetch_resp_free(&r);
    check("a request for file:// is refused before anything", get(&f, "file:///etc/passwd", 0, true, &r) != 0 &&
                                                                  r.fail == WEB_FAIL_URL);
    web_fetch_resp_free(&r);
    check("a page past the cap is kept, cut", get(&f, "https://doors.test/big", 100000, true, &r) == 0 && r.cut &&
                                                  r.len == 100000);
    web_fetch_resp_free(&r);
    check("a picture past the cap is refused", get(&f, "https://doors.test/big", 100000, false, &r) != 0 &&
                                                   r.fail == WEB_FAIL_TYPE && r.body == NULL);
    web_fetch_resp_free(&r);
    check("a 404 is an answer, not a failure", get(&f, "https://doors.test/missing", 0, true, &r) == 0 &&
                                                   r.status == 404);
    web_fetch_resp_free(&r);
    {
        struct timespec a;
        struct timespec b;

        stop_now = 1;
        clock_gettime(CLOCK_MONOTONIC, &a);
        check("STOP ends a slow load", get(&f, "https://doors.test/slow", 0, true, &r) != 0 &&
                                           r.fail == WEB_FAIL_STOPPED);
        clock_gettime(CLOCK_MONOTONIC, &b);
        check("at once", (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000 < 200);
        web_fetch_resp_free(&r);
        stop_now = 0;
    }
    check("the failures of the fake network are named",
          get(&f, "https://tls.doors.test/", 0, true, &r) != 0 && r.fail == WEB_FAIL_TLS);
    web_fetch_resp_free(&r);
    check("an unknown host", get(&f, "https://nowhere.example/", 0, true, &r) != 0 && r.fail == WEB_FAIL_DNS &&
                                 strstr(r.text, "nowhere.example") != NULL);
    web_fetch_resp_free(&r);

    web_fetch_parse_type("Text/HTML; Charset=\"ISO-8859-1\"; x=y", type, sizeof(type), cs, sizeof(cs));
    check("Content-Type: type lower case, charset without quotes", strcmp(type, "text/html") == 0 &&
                                                                   strcmp(cs, "ISO-8859-1") == 0);
    web_fetch_parse_type(NULL, type, sizeof(type), cs, sizeof(cs));
    check("no Content-Type: empty", type[0] == '\0' && cs[0] == '\0');
    web_fetch_parse_type("text/plain;charset=utf-8", type, sizeof(type), cs, sizeof(cs));
    check("no space after the semicolon", strcmp(type, "text/plain") == 0 && strcmp(cs, "utf-8") == 0);

    /* ---- no network, or that server -------------------------------------------------------- */
    if (!mkdtemp(dir)) {
        return 1;
    }
    setenv("POCKETOS_BROWSER_PROC_NET", dir, 1);
    snprintf(path, sizeof(path), "%s/route", dir);
    write_file(path, "Iface\tDestination\tGateway\tFlags\n"
                     "wlan0\t0000A8C0\t00000000\t0001\n");
    snprintf(path, sizeof(path), "%s/ipv6_route", dir);
    write_file(path, "00000000000000000000000000000001 80 00000000000000000000000000000000 00 "
                     "00000000000000000000000000000000 00000000 00000001 00000000 00200001 lo\n"
                     "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
                     "00000000000000000000000000000000 ffffffff 00000001 00000000 00200200 lo\n");
    check("a local network with no default route is no network", !web_net_has_route());
    snprintf(path, sizeof(path), "%s/route", dir);
    write_file(path, "Iface\tDestination\tGateway\tFlags\n"
                     "wlan0\t00000000\t0100A8C0\t0003\n");
    check("a default route is a network", web_net_has_route());
    unlink(path);
    snprintf(path, sizeof(path), "%s/ipv6_route", dir);
    write_file(path, "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
                     "fe800000000000000000000000000001 00000400 00000001 00000000 00000003 wlan0\n");
    check("an IPv6 default route is a network", web_net_has_route());
    unlink(path);
    rmdir(dir);
    check("no route files at all: no network", !web_net_has_route());
    unsetenv("POCKETOS_BROWSER_PROC_NET");

    f.close(&f);
    printf("web_fetch_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
