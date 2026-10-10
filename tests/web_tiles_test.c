/*
 * RIFT's basemap, the helper's half (core/web/web_tiles.h): the cache
 * headers read from an answer, when a tile expires by them, the disk cache
 * and its limits, and the rules for one tile - kept and fresh is not asked
 * for, expired is asked for conditionally and a 304 renews it, a failure
 * shows the kept copy as stale, no clock asks for nothing - on the fake
 * network's tile servers, through a fetcher that counts and records what
 * was asked.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_fetch.h"
#include "web/web_tiles.h"

#include <dirent.h>
#include <fcntl.h>
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

/* ---- a fetcher that counts ---------------------------------------------------- */

struct spy {
    struct web_fetcher fake;
    int hops;
    char ua[160];
    char inm[WEB_HDR_LONG];
    char ims[WEB_HDR_SHORT];
    const char *force_cc;   /* replace the answer's Cache-Control */
};

static struct spy spy;

static int spy_hop(struct web_fetcher *f, const struct web_fetch_req *req, const char *url, struct web_hop *h)
{
    int rc;

    (void)f;
    spy.hops++;
    snprintf(spy.ua, sizeof(spy.ua), "%s", req->user_agent ? req->user_agent : "");
    snprintf(spy.inm, sizeof(spy.inm), "%s", req->if_none_match ? req->if_none_match : "");
    snprintf(spy.ims, sizeof(spy.ims), "%s", req->if_modified_since ? req->if_modified_since : "");
    rc = spy.fake.hop(&spy.fake, req, url, h);
    if (spy.force_cc) {
        snprintf(h->cache.cache_control, sizeof(h->cache.cache_control), "%s", spy.force_cc);
    }
    return rc;
}

static void spy_close(struct web_fetcher *f)
{
    (void)f;
}

static struct web_fetcher spy_fetcher(void)
{
    struct web_fetcher f;

    memset(&spy, 0, sizeof(spy));
    web_fetcher_fake(&spy.fake);
    memset(&f, 0, sizeof(f));
    f.hop = spy_hop;
    f.close = spy_close;
    f.name = "spy";
    return f;
}

static int stop_now;

static int abort_cb(void *ctx)
{
    (void)ctx;
    return stop_now;
}

static struct web_tile_net net_for(struct web_fetcher *f, const char *tmpl)
{
    struct web_tile_net n;

    memset(&n, 0, sizeof(n));
    n.f = f;
    n.url_template = tmpl;
    n.user_agent = "DOORS-RIFT/test";
    n.allow = true;
    n.abort_cb = abort_cb;
    return n;
}

static int files_in(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static void set_used(const char *dir, int z, int x, int y, time_t when)
{
    char path[800];
    struct timespec t[2];

    snprintf(path, sizeof(path), "%s/%d-%d-%d.png", dir, z, x, y);
    t[0].tv_sec = when;
    t[0].tv_nsec = 0;
    t[1] = t[0];
    utimensat(AT_FDCWD, path, t, 0);
}

static void expire(struct web_tile_cache *c, int z, int x, int y, int64_t now)
{
    struct web_tile_meta m;
    char *b;
    size_t n;

    if (web_tile_cache_read(c, z, x, y, &m, &b, &n) == 0) {
        free(b);
        m.expires = now - 1;
        web_tile_cache_renew(c, z, x, y, &m);
    }
}

/* ---- the parts ------------------------------------------------------------------- */

static void test_headers(void)
{
    struct web_cache_hdrs h;
    static const char *const lines[] = {
        "HTTP/2 200\r\n", "ETag: \"abc\"\r\n", "cache-control:  max-age=604800, stale-while-revalidate=1\r\n",
        "LAST-MODIFIED: Wed, 01 Oct 2026 12:00:00 GMT\r\n", "Expires: Thu, 08 Oct 2026 12:00:00 GMT\r\n",
        "Date: Wed, 01 Oct 2026 12:00:00 GMT\r\n", "X-Other: ignored\r\n",
    };
    char longtag[400];
    size_t i;

    memset(&h, 0, sizeof(h));
    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        web_cache_hdr_line(&h, lines[i], strlen(lines[i]));
    }
    check("the cache headers are read in any case, trimmed",
          strcmp(h.etag, "\"abc\"") == 0 && strcmp(h.cache_control, "max-age=604800, stale-while-revalidate=1") == 0 &&
              strcmp(h.last_modified, "Wed, 01 Oct 2026 12:00:00 GMT") == 0 &&
              strcmp(h.expires, "Thu, 08 Oct 2026 12:00:00 GMT") == 0 && h.date[0]);
    memset(longtag, 'a', sizeof(longtag));
    memcpy(longtag, "ETag: ", 6);
    web_cache_hdr_line(&h, longtag, sizeof(longtag));
    check("a validator too long to keep whole is not kept at all", h.etag[0] == '\0');
    web_cache_hdr_line(&h, "ETag: \"a\x01b\"\r\n", 12);
    check("nor one with a control byte in it", h.etag[0] == '\0');
    web_cache_hdr_line(&h, "HTTP/1.1 304 Not Modified\r\n", 27);
    check("a new answer forgets the last one's headers", h.cache_control[0] == '\0' && h.date[0] == '\0');
}

static void test_dates(void)
{
    int64_t t = 1759320000; /* 2025-10-01 12:00:00 UTC */

    check("an IMF-fixdate", web_http_date("Wed, 01 Oct 2025 12:00:00 GMT") == t);
    check("an RFC 850 date", web_http_date("Wednesday, 01-Oct-25 12:00:00 GMT") == t);
    check("an asctime date", web_http_date("Wed Oct  1 12:00:00 2025") == t);
    check("and nothing else", web_http_date("0") == -1 && web_http_date("") == -1 &&
                                  web_http_date("Wed, 32 Oct 2025 12:00:00 GMT") == -1 &&
                                  web_http_date("Wed, 01 Foo 2025 12:00:00 GMT") == -1);
}

static void test_expiry(void)
{
    struct web_cache_hdrs h;
    int64_t now = 1800000000;
    bool store;

    memset(&h, 0, sizeof(h));
    check("no header at all: the policy's seven days",
          web_tile_expiry(&h, now, &store) == now + 7 * 86400 && store);
    snprintf(h.cache_control, sizeof(h.cache_control), "public, max-age=3600");
    check("max-age is believed", web_tile_expiry(&h, now, &store) == now + 3600 && store);
    snprintf(h.cache_control, sizeof(h.cache_control), "max-age=999999999999");
    check("up to a year", web_tile_expiry(&h, now, &store) == now + WEB_TILE_MAX_AGE_S);
    snprintf(h.cache_control, sizeof(h.cache_control), "no-cache");
    check("no-cache: kept, asked again every time", web_tile_expiry(&h, now, &store) == now && store);
    snprintf(h.cache_control, sizeof(h.cache_control), "max-age=60, no-store");
    check("no-store: not kept", web_tile_expiry(&h, now, &store) == now && !store);
    memset(&h, 0, sizeof(h));
    snprintf(h.expires, sizeof(h.expires), "Thu, 08 Oct 2026 12:00:00 GMT");
    snprintf(h.date, sizeof(h.date), "Wed, 07 Oct 2026 12:00:00 GMT");
    check("Expires is read against the server's own Date", web_tile_expiry(&h, now, &store) == now + 86400);
    snprintf(h.cache_control, sizeof(h.cache_control), "max-age=10");
    check("and max-age wins over it", web_tile_expiry(&h, now, &store) == now + 10);
    memset(&h, 0, sizeof(h));
    snprintf(h.expires, sizeof(h.expires), "0");
    check("an Expires that is not a date has already expired", web_tile_expiry(&h, now, &store) == now);
}

static void test_url(void)
{
    char out[256];

    check("the template is filled in",
          web_tile_url("https://tile.openstreetmap.org/{z}/{x}/{y}.png", 12, 2170, 1190, out, sizeof(out)) > 0 &&
              strcmp(out, "https://tile.openstreetmap.org/12/2170/1190.png") == 0);
    check("only https", web_tile_url("http://tile.openstreetmap.org/{z}/{x}/{y}.png", 1, 0, 0, out,
                                     sizeof(out)) < 0);
    check("all three coordinates", web_tile_url("https://t.example/{z}/{x}.png", 1, 0, 0, out, sizeof(out)) < 0);
    check("a tile in range only", web_tile_url("https://t.example/{z}/{x}/{y}.png", 2, 4, 0, out, sizeof(out)) < 0 &&
                                      !web_tile_valid(20, 0, 0) && web_tile_valid(19, 524287, 0));
    check("nothing that would break a line or a request",
          web_tile_url("https://t.example/{z}/{x}/{y}.png\tx", 1, 0, 0, out, sizeof(out)) < 0);
}

static void test_cache(const char *root)
{
    struct web_tile_cache c;
    struct web_tile_meta m;
    char dir[600];
    char path[700];
    char *body;
    size_t len;
    int i;

    snprintf(dir, sizeof(dir), "%s/open", root);
    mkdir(dir, 0755);
    check("a directory others can reach is refused", web_tile_cache_open(&c, dir, 0, 0) != 0 && !c.ok);
    chmod(dir, 0700);
    snprintf(path, sizeof(path), "%s/link", root);
    if (symlink(dir, path) != 0) {
        printf("note: no symbolic link could be made\n");
    }
    check("and so is a link to one", web_tile_cache_open(&c, path, 0, 0) != 0);
    snprintf(path, sizeof(path), "%s/.tmp-99", dir);
    close(open(path, O_WRONLY | O_CREAT, 0600));
    snprintf(path, sizeof(path), "%s/3-1-1.meta", dir);
    close(open(path, O_WRONLY | O_CREAT, 0600));
    check("a private one is used", web_tile_cache_open(&c, dir, 0, 0) == 0 && c.ok && c.files == 0);
    check("and what a cut-off write left in it is gone", files_in(dir) == 0);

    memset(&m, 0, sizeof(m));
    m.fetched = 1800000000;
    m.expires = 1800003600;
    snprintf(m.etag, sizeof(m.etag), "\"x\"");
    check("a tile is kept", web_tile_cache_write(&c, 5, 3, 4, &m, "PNGDATA", 7) == 0 && c.files == 1);
    check("and read back as it was",
          web_tile_cache_read(&c, 5, 3, 4, &m, &body, &len) == 0 && len == 7 && memcmp(body, "PNGDATA", 7) == 0 &&
              m.expires == 1800003600 && strcmp(m.etag, "\"x\"") == 0);
    free(body);
    snprintf(path, sizeof(path), "%s/5-3-4.png", dir);
    {
        FILE *f = fopen(path, "w");

        fputs("SHORT", f);
        fclose(f);
    }
    check("a picture that does not match its record is not a tile, and goes",
          web_tile_cache_read(&c, 5, 3, 4, &m, &body, &len) != 0 && files_in(dir) == 0);
    check("nothing out of range is ever a file", web_tile_cache_write(&c, 2, 9, 0, &m, "x", 1) != 0);
    {
        static char big[WEB_TILE_BYTES_MAX + 1];

        check("nor anything larger than a tile may be",
              web_tile_cache_write(&c, 2, 1, 0, &m, big, sizeof(big)) != 0);
    }

    /* The limits: ten tiles in a cache of eight, used in order. */
    web_tile_cache_open(&c, dir, 0, 8);
    for (i = 0; i < 10; i++) {
        web_tile_cache_write(&c, 4, i, 0, &m, "PNGDATA", 7);
        set_used(dir, 4, i, 0, 1000000 + i);
    }
    check("past its file limit the cache makes room", c.files <= 8 && c.pruned > 0);
    check("from the least recently shown",
          web_tile_cache_read(&c, 4, 0, 0, &m, &body, &len) != 0 &&
              web_tile_cache_read(&c, 4, 9, 0, &m, &body, &len) == 0);
    free(body);
    web_tile_cache_touch(&c, 4, 3, 0);
    web_tile_cache_prune(&c, 0, 2);
    check("a tile shown just now outlives older ones",
          c.files == 2 && web_tile_cache_read(&c, 4, 3, 0, &m, &body, &len) == 0);
    free(body);
    web_tile_cache_open(&c, dir, 4000, 0);
    for (i = 0; i < 6; i++) {
        char pic[600];

        memset(pic, 'p', sizeof(pic));
        web_tile_cache_write(&c, 6, i, 1, &m, pic, sizeof(pic));
    }
    check("and past its byte limit too", c.bytes <= 4000);
    c.reserve = (uint64_t)1 << 62;
    check("nothing is written once the disk is short of its reserve",
          web_tile_cache_write(&c, 6, 9, 9, &m, "PNGDATA", 7) != 0);
}

static void test_one_tile(const char *root)
{
    struct web_fetcher f = spy_fetcher();
    struct web_tile_cache c;
    struct web_tile_net net;
    struct web_tile_result r;
    struct web_tile_meta m;
    char dir[600];
    char *body;
    size_t len;
    int64_t now = 1800000000;
    int hops;

    snprintf(dir, sizeof(dir), "%s/tiles", root);
    mkdir(dir, 0700);
    web_tile_cache_open(&c, dir, 0, 0);
    net = net_for(&f, "https://tiles.doors.test/{z}/{x}/{y}.png");

    web_tile_get(&c, &net, 10, 543, 297, now, &r);
    check("a tile not held is fetched", r.source == WEB_TILE_NET && r.asked && r.len > 0 && spy.hops == 1);
    check("under the client's own User-Agent", strcmp(spy.ua, "DOORS-RIFT/test") == 0);
    check("unconditionally", spy.inm[0] == '\0' && spy.ims[0] == '\0');
    web_tile_result_free(&r);
    check("and kept, with its validators and the server's max-age",
          web_tile_cache_read(&c, 10, 543, 297, &m, &body, &len) == 0 && m.expires == now + 86400 &&
              strcmp(m.etag, "\"t-10-543-297\"") == 0 && m.last_modified[0]);
    free(body);

    web_tile_get(&c, &net, 10, 543, 297, now + 60, &r);
    check("held and fresh: shown, and nothing is asked", r.source == WEB_TILE_CACHE && !r.asked && spy.hops == 1);
    web_tile_result_free(&r);

    expire(&c, 10, 543, 297, now);
    web_tile_get(&c, &net, 10, 543, 297, now + 120, &r);
    check("held and expired: asked again, conditionally",
          r.asked && spy.hops == 2 && strcmp(spy.inm, "\"t-10-543-297\"") == 0 && spy.ims[0]);
    check("and a 304 renews it without a body", r.source == WEB_TILE_VALID && r.http == 304 && r.len > 0);
    web_tile_result_free(&r);
    check("for as long as the 304 says",
          web_tile_cache_read(&c, 10, 543, 297, &m, &body, &len) == 0 && m.expires == now + 120 + 86400);
    free(body);

    net = net_for(&f, "https://tiles-plain.doors.test/{z}/{x}/{y}.png");
    web_tile_get(&c, &net, 10, 544, 297, now, &r);
    web_tile_result_free(&r);
    check("an answer with no cache header is kept the policy's seven days",
          web_tile_cache_read(&c, 10, 544, 297, &m, &body, &len) == 0 && m.expires == now + WEB_TILE_FALLBACK_S);
    free(body);

    net = net_for(&f, "https://tiles-stale.doors.test/{z}/{x}/{y}.png");
    web_tile_get(&c, &net, 11, 1, 1, now, &r);
    web_tile_result_free(&r);
    hops = spy.hops;
    web_tile_get(&c, &net, 11, 1, 1, now + 1, &r);
    check("max-age=0: asked every time, and 304 each time", r.source == WEB_TILE_VALID && spy.hops == hops + 1);
    web_tile_result_free(&r);

    net = net_for(&f, "https://offline.doors.test/{z}/{x}/{y}.png");
    web_tile_get(&c, &net, 11, 1, 1, now + 2, &r);
    check("no network: the kept copy is shown, stale, and says why",
          r.source == WEB_TILE_STALE && r.len > 0 && strcmp(r.why, "offline") == 0);
    web_tile_result_free(&r);
    web_tile_get(&c, &net, 11, 2, 2, now + 2, &r);
    check("and a tile never seen is not there", r.source == WEB_TILE_NONE && !r.body && strcmp(r.why, "offline") == 0);
    web_tile_result_free(&r);

    hops = spy.hops;
    web_tile_get(&c, &net, 11, 1, 1, 1000, &r);
    check("no clock: nothing is asked, the kept copy is shown as it is",
          r.source == WEB_TILE_STALE && !r.asked && spy.hops == hops && strcmp(r.why, "clock") == 0);
    web_tile_result_free(&r);
    web_tile_get(&c, &net, 11, 3, 3, 1000, &r);
    check("and one not kept is not had", r.source == WEB_TILE_NONE && !r.asked && strcmp(r.why, "clock") == 0);
    web_tile_result_free(&r);

    net = net_for(&f, "https://tiles.doors.test/{z}/{x}/{y}.png");
    net.allow = false;
    snprintf(net.why_not, sizeof(net.why_not), "http-429");
    web_tile_get(&c, &net, 11, 1, 1, now + 3, &r);
    check("while backing off nothing is asked, and the reason is kept",
          r.source == WEB_TILE_STALE && !r.asked && spy.hops == hops && strcmp(r.why, "http-429") == 0);
    web_tile_result_free(&r);

    net = net_for(&f, "https://tiles-busy.doors.test/{z}/{x}/{y}.png");
    web_tile_get(&c, &net, 12, 5, 5, now, &r);
    check("a server error is a tile not had", r.source == WEB_TILE_NONE && strcmp(r.why, "http-503") == 0 &&
                                                  r.http == 503);
    web_tile_result_free(&r);
    web_tile_get(&c, &net, 11, 1, 1, now + 4, &r);
    check("or the kept copy, stale", r.source == WEB_TILE_STALE && strcmp(r.why, "http-503") == 0);
    web_tile_result_free(&r);

    net = net_for(&f, "https://tiles.doors.test/{z}/{x}/{y}.png");
    spy.force_cc = "no-store";
    web_tile_get(&c, &net, 13, 7, 7, now, &r);
    spy.force_cc = NULL;
    check("no-store: shown, never kept",
          r.source == WEB_TILE_NET && web_tile_cache_read(&c, 13, 7, 7, &m, &body, &len) != 0);
    web_tile_result_free(&r);

    net = net_for(&f, "https://tiles-slow.doors.test/{z}/{x}/{y}.png");
    stop_now = 1;
    web_tile_get(&c, &net, 14, 1, 1, now, &r);
    stop_now = 0;
    check("a tile asked to stop is not answered at all", r.stopped && r.source == WEB_TILE_NONE && !r.body);
    web_tile_result_free(&r);
}

int main(void)
{
    char root[] = "/tmp/web_tiles_test.XXXXXX";

    if (!mkdtemp(root)) {
        printf("FAIL a temporary directory\n");
        return 1;
    }
    test_headers();
    test_dates();
    test_expiry();
    test_url();
    test_cache(root);
    test_one_tile(root);
    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0) {
            printf("note: %s was not removed\n", root);
        }
    }
    printf("web_tiles_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
