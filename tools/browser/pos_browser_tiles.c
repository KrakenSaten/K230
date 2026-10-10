/*
 * pos-browser tiles: RIFT's basemap helper (core/web/web_tiles.h for the
 * protocol and the cache, docs/apps/RIFT.md "MAP" for what is shown).
 *
 *   pos-browser tiles --cache DIR --out DIR --url TEMPLATE [--fake] [--ca-file FILE]
 *
 * stdin and stdout are a socketpair to the shell, as for `session`. It
 * lives as long as RIFT's MAP has its basemap on: it leaves on `quit`, when
 * the shell closes its end, on SIGTERM, and - PR_SET_PDEATHSIG, set by the
 * shell before exec - when the shell dies. DIR for --cache is the kept
 * tiles, --out the session's directory for the decoded pictures; both are
 * the shell's, 0700, and refused otherwise.
 *
 * The network, TLS and the PNG/JPEG decoder run here and never in the
 * shell, exactly as for the Browser's pages (ADR-009 Amendment 1). Only
 * tiles the shell named are fetched, one at a time, under a User-Agent of
 * its own (the OpenStreetMap tile policy asks every app to name itself).
 * After a network failure nothing is asked for BACKOFF_NET_MS, and after a
 * refusal by the server (403, 429, 5xx) or a TLS failure for BACKOFF_SERVER_MS:
 * kept tiles are still shown meanwhile.
 *
 * Logs carry counts and the tile server's host, never a tile's coordinates
 * (they say where the reader is looking).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"
#include "web/web_fetch.h"
#include "web/web_image.h"
#include "web/web_proto.h"
#include "web/web_tiles.h"
#include "web/web_url.h"

#include <errno.h>
#include <fcntl.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef POCKETOS_VERSION
#define POCKETOS_VERSION "dev"
#endif

#define USER_AGENT "DOORS-RIFT/" POCKETOS_VERSION " (Doors mesh client basemap; +https://github.com/KrakenSaten/K230)"
#define BACKOFF_NET_MS 30000
#define BACKOFF_SERVER_MS 60000

int pos_browser_tiles(int argc, char **argv);

static volatile sig_atomic_t term;

static void on_signal(int sig)
{
    (void)sig;
    term = 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct want {
    int z;
    int x;
    int y;
};

struct tiles {
    struct web_fetcher f;
    bool fake;
    bool net;                   /* a fetcher started */
    struct web_tile_cache cache;
    const char *out_dir;
    const char *url;
    const char *ca_file;

    char in[WEB_LINE_MAX];
    size_t in_len;
    bool in_over;
    bool quit;

    int gen;
    struct want want[WEB_TILE_WANT_MAX];
    int nwant;
    int next;
    bool idle_sent;

    bool busy;                  /* a tile is being fetched */
    struct want cur;
    bool stop;                  /* it is no longer wanted */

    int64_t backoff_until;
    char backoff_why[24];
    unsigned slot;              /* picture file names */
    unsigned served;
    unsigned fetched;
};

/* ---- lines ---------------------------------------------------------------------- */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    char line[WEB_LINE_MAX];
    const char *p = line;
    va_list ap;
    int n;
    size_t left;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof(line) - 1) {
        return;
    }
    line[n++] = '\n';
    left = (size_t)n;
    while (left > 0) {
        ssize_t w = write(1, p, left);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            _exit(0); /* the shell has gone: so do we */
        }
        p += w;
        left -= (size_t)w;
    }
}

static bool wanted(const struct tiles *t, const struct want *w)
{
    int i;

    for (i = 0; i < t->nwant; i++) {
        if (t->want[i].z == w->z && t->want[i].x == w->x && t->want[i].y == w->y) {
            return true;
        }
    }
    return false;
}

/* want <gen> <z/x/y> ...: replaces the queue. A malformed entry ends the
 * list there; the shell is the only writer. */
static void command(struct tiles *t, char *line)
{
    char *tab = strchr(line, '\t');
    char *p;

    if (strcmp(line, "quit") == 0) {
        t->quit = true;
        return;
    }
    if (!tab) {
        return;
    }
    *tab = '\0';
    if (strcmp(line, "want") != 0) {
        return;
    }
    p = tab + 1;
    t->gen = atoi(p);
    t->nwant = 0;
    t->next = 0;
    t->idle_sent = false;
    p = strchr(p, '\t');
    while (p && *p && t->nwant < WEB_TILE_WANT_MAX) {
        struct want w;
        int used = 0;

        p++;
        if (sscanf(p, "%d/%d/%d%n", &w.z, &w.x, &w.y, &used) != 3 || !web_tile_valid(w.z, w.x, w.y)) {
            break;
        }
        if (!wanted(t, &w)) {
            t->want[t->nwant++] = w;
        }
        p += used;
        if (*p != ' ') {
            break;
        }
    }
    /* The tile in transfer goes on only if it is still in view. */
    if (t->busy && !wanted(t, &t->cur)) {
        t->stop = true;
    }
}

static void read_commands(struct tiles *t, int timeout_ms)
{
    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    char buf[4096];

    for (;;) {
        ssize_t n;
        ssize_t i;
        int r = poll(&pfd, 1, timeout_ms);

        if (r < 0 && errno == EINTR) {
            if (term) {
                return;
            }
            continue;
        }
        if (r <= 0) {
            return;
        }
        n = read(0, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            t->quit = true;
            return;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!t->in_over) {
                    t->in[t->in_len] = '\0';
                    command(t, t->in);
                }
                t->in_len = 0;
                t->in_over = false;
            } else if (t->in_len + 1 >= sizeof(t->in)) {
                t->in_over = true;
            } else if (!t->in_over) {
                t->in[t->in_len++] = buf[i];
            }
        }
        timeout_ms = 0;
    }
}

static int abort_cb(void *ctx)
{
    struct tiles *t = ctx;

    read_commands(t, 0);
    return term || t->quit || t->stop;
}

/* ---- one tile ---------------------------------------------------------------------- */

static int write_pixels(const struct tiles *t, const char *name, const struct web_pixels *px)
{
    char path[1024];
    size_t bytes = (size_t)px->w * (size_t)px->h * sizeof(uint16_t);
    const char *p = (const char *)px->px;
    int fd;

    snprintf(path, sizeof(path), "%s/%s", t->out_dir, name);
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    while (bytes > 0) {
        ssize_t w = write(fd, p, bytes);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            close(fd);
            unlink(path);
            return -1;
        }
        p += w;
        bytes -= (size_t)w;
    }
    return close(fd);
}

static const char *source_name(enum web_tile_source s)
{
    switch (s) {
    case WEB_TILE_CACHE:
        return "cache";
    case WEB_TILE_VALID:
        return "valid";
    case WEB_TILE_NET:
        return "net";
    case WEB_TILE_STALE:
        return "stale";
    case WEB_TILE_NONE:
        break;
    }
    return "none";
}

/* After asking: hold off asking again when the network or the server said no. */
static void learn(struct tiles *t, struct web_tile_result *r)
{
    int64_t hold = 0;

    if (!r->asked || r->stopped) {
        return;
    }
    if (r->source == WEB_TILE_NONE || r->source == WEB_TILE_STALE) {
        if (strcmp(r->why, "dns") == 0 || strcmp(r->why, "connect") == 0 || strcmp(r->why, "timeout") == 0 ||
            strcmp(r->why, "offline") == 0) {
            if (!t->fake && !web_net_has_route()) {
                web_copy(r->why, sizeof(r->why), "offline");
            }
            hold = BACKOFF_NET_MS;
        } else if (strcmp(r->why, "tls") == 0 || strcmp(r->why, "clock") == 0 || r->http == 403 ||
                   r->http == 429 || r->http >= 500) {
            hold = BACKOFF_SERVER_MS;
        }
    }
    if (hold) {
        t->backoff_until = mono_ms() + hold;
        web_copy(t->backoff_why, sizeof(t->backoff_why), r->why);
        LOG_INFO("rift tiles: %s, asking nothing for %lld s", r->why, (long long)(hold / 1000));
    }
}

static void answer(struct tiles *t, const struct want *w, struct web_tile_result *r)
{
    struct web_pixels px;
    enum web_image_err e;
    char name[WEB_FILE_NAME_MAX];
    int i;

    /* Answered now, under the gen in force; a later entry for the same
     * tile in a want that replaced the one it was asked under is done. */
    for (i = t->next; i < t->nwant; i++) {
        if (t->want[i].z == w->z && t->want[i].x == w->x && t->want[i].y == w->y) {
            memmove(&t->want[i], &t->want[i + 1], (size_t)(t->nwant - i - 1) * sizeof(t->want[0]));
            t->nwant--;
            break;
        }
    }
    if (r->source == WEB_TILE_NONE) {
        say("notile\t%d\t%d\t%d\t%d\t%s", t->gen, w->z, w->x, w->y, r->why);
        return;
    }
    e = web_image_decode((const unsigned char *)r->body, r->len, 0, WEB_TILE_SIZE, WEB_TILE_SIZE,
                         (size_t)WEB_TILE_SIZE * WEB_TILE_SIZE, &px);
    if (e != WEB_IMAGE_OK) {
        /* Not a picture this helper can show: not kept either. */
        web_tile_cache_remove(&t->cache, w->z, w->x, w->y);
        say("notile\t%d\t%d\t%d\t%d\t%s", t->gen, w->z, w->x, w->y,
            e == WEB_IMAGE_FORMAT ? "format" : e == WEB_IMAGE_MEMORY ? "memory" : "broken");
        return;
    }
    snprintf(name, sizeof(name), "%d-%u.rgb565", t->gen, t->slot++);
    if (write_pixels(t, name, &px) != 0) {
        say("notile\t%d\t%d\t%d\t%d\tstore", t->gen, w->z, w->x, w->y);
    } else {
        say("tile\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s", t->gen, w->z, w->x, w->y, px.w, px.h, name,
            source_name(r->source), r->source == WEB_TILE_STALE ? r->why : "-");
        t->served++;
    }
    free(px.px);
}

static void one(struct tiles *t, const struct want *w)
{
    struct web_tile_net net;
    struct web_tile_result r;

    memset(&net, 0, sizeof(net));
    net.f = t->net ? &t->f : NULL;
    net.url_template = t->url;
    net.user_agent = USER_AGENT;
    net.ca_file = t->ca_file;
    net.allow = t->net && mono_ms() >= t->backoff_until;
    web_copy(net.why_not, sizeof(net.why_not),
             !t->net ? "no-network-build" : t->backoff_why[0] ? t->backoff_why : "offline");
    net.abort_cb = abort_cb;
    net.ctx = t;
    t->busy = true;
    t->cur = *w;
    t->stop = false;
    web_tile_get(&t->cache, &net, w->z, w->x, w->y, (int64_t)time(NULL), &r);
    t->busy = false;
    if (r.asked && !r.stopped) {
        t->fetched++;
    }
    learn(t, &r);
    /* A want that came while it was being got may not hold it any more:
     * a tile from the cache is got without a pause to be stopped in. */
    if (!r.stopped && !term && !t->quit && wanted(t, w)) {
        answer(t, w, &r);
    }
    web_tile_result_free(&r);
}

/* ---- the session ---------------------------------------------------------------------- */

static bool private_dir(const char *dir)
{
    struct stat st;

    return dir && lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 077) == 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: pos-browser tiles --cache DIR --out DIR --url TEMPLATE [--fake] [--ca-file FILE]\n");
    return 2;
}

int pos_browser_tiles(int argc, char **argv)
{
    static struct tiles t; /* the want list and the line buffer: not on the stack */
    const char *cache_dir = NULL;
    struct sigaction sa;
    char err[160];
    char probe[WEB_URL_MAX];
    char host[WEB_HOST_MAX + 32];
    struct web_url u;
    const char *img;
    int i;

    memset(&t, 0, sizeof(t));
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fake") == 0) {
            t.fake = true;
        } else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            t.out_dir = argv[++i];
        } else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            t.url = argv[++i];
        } else if (strcmp(argv[i], "--ca-file") == 0 && i + 1 < argc) {
            t.ca_file = argv[++i];
        } else {
            return usage();
        }
    }
    if (!t.url || web_tile_url(t.url, 0, 0, 0, probe, sizeof(probe)) < 0 || !t.out_dir) {
        return usage();
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    pocketlog_init("pos-browser");
    if (!private_dir(t.out_dir)) {
        LOG_WARN("rift tiles: picture directory refused");
        return 2;
    }
    if (!cache_dir || web_tile_cache_open(&t.cache, cache_dir, 0, 0) != 0) {
        /* Shown, never kept: every tile is asked for each time it is
         * needed, and nothing is written. */
        LOG_WARN("rift tiles: cache directory refused, nothing is kept this session");
    }
    if (t.fake) {
        web_fetcher_fake(&t.f);
        t.net = true;
    } else if (web_fetcher_curl(&t.f, err, sizeof(err)) == 0) {
        t.net = true;
    }
    if (web_url_parse(probe, &u) != WEB_URL_OK || web_url_origin(&u, host, sizeof(host)) <= 0) {
        web_copy(host, sizeof(host), "?");
    }
    img = web_image_formats();
    LOG_INFO("rift tiles: session start, %s, %s, %u tiles kept", t.fake ? "fake" : t.net ? "net" : "none",
             host, t.cache.files);
    say("hello\t%d\t%s%s%s", WEB_TILE_PROTO_VERSION, t.fake ? "fake" : t.net ? "net" : "none",
        img[0] ? ",img:" : "", img);
    t.idle_sent = true;
    while (!term && !t.quit) {
        struct want w;

        if (t.next >= t.nwant) {
            if (!t.idle_sent) {
                say("idle\t%d", t.gen);
                t.idle_sent = true;
#ifdef __GLIBC__
                malloc_trim(0);
#endif
            }
            read_commands(&t, -1);
            continue;
        }
        w = t.want[t.next++];
        one(&t, &w);
        read_commands(&t, 0);
    }
    if (t.net && t.f.close) {
        t.f.close(&t.f);
    }
    LOG_INFO("rift tiles: session end, %u shown, %u asked of the server, %u kept", t.served, t.fetched,
             t.cache.files);
    return 0;
}
