/*
 * pos-browser: the only Doors program that fetches a web page.
 *
 *   pos-browser session [--fake] [--images DIR] [--ca-file FILE]
 *       The Browser app's helper (docs/apps/BROWSER.md). stdin and stdout
 *       are a socketpair to the shell; the protocol is core/web/web_proto.h.
 *       It lives exactly as long as the Browser screen: it leaves on `quit`,
 *       when the shell closes its end, on SIGTERM, and - through
 *       PR_SET_PDEATHSIG, set by the session before exec - when the shell
 *       dies. Pictures are written as RGB565 files into DIR, which the shell
 *       made for this session; without DIR none are fetched.
 *   pos-browser dump [--fake] [--ca-file FILE] URL
 *       Fetch one page and print it as text, links numbered, for the bench.
 *       Exit 0 when a page came, 1 when it did not, 2 for usage.
 *   pos-browser features
 *       What this build can do: "net" (libcurl), "img" (decoders), "fake".
 *
 * EVERYTHING UNTRUSTED HAPPENS HERE: the network, TLS, HTTP, the HTML
 * reader and the image decoders. The shell gets a bounded document and
 * pixels, never the page's bytes (ADR-009, like ADR-007 for Zabbix).
 *
 * One page at a time. While a page or its pictures load, the commands the
 * shell sends are read between blocks of the transfer: `stop` abandons the
 * page, a new `open` replaces it, `quit` leaves. Name resolution is the one
 * wait that cannot be interrupted; the session's watchdog is for that.
 *
 * Logs carry the scheme and host of a page and never its path or query,
 * which can hold tokens.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"
#include "web/web_fetch.h"
#include "web/web_url.h"
#include "web/web_html.h"
#include "web/web_image.h"
#include "web/web_proto.h"

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

#define IMAGE_MAX_H 1200
/* Pixels one page's pictures may take in the shell, all together. */
#define PAGE_PIXELS_MAX (1536u * 1024u)
#define PROGRESS_MS 300
#define ACCEPT_PAGE "text/html,application/xhtml+xml,text/plain;q=0.9,image/png;q=0.5,image/jpeg;q=0.5,*/*;q=0.1"
#define ACCEPT_IMAGE "image/png,image/jpeg;q=0.9,*/*;q=0.1"

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

struct helper {
    struct web_fetcher f;
    bool fake;
    bool session;
    const char *img_dir;
    const char *ca_file;

    char in[WEB_LINE_MAX];
    size_t in_len;
    bool in_over;
    bool quit;

    bool have_open;             /* an open is waiting */
    int open_seq;
    int open_maxw;
    char open_flags[8];
    char open_url[WEB_URL_MAX];

    int seq;                    /* the page loading now */
    bool active;
    bool stop;
    int64_t progress_at;
    const char *stage;
};

/* ---- output ---------------------------------------------------------------------- */

static int emit(void *ctx, const char *line, size_t n)
{
    static char buf[WEB_LINE_MAX + 2];
    const char *p = buf;
    size_t left;

    (void)ctx;
    if (n > WEB_LINE_MAX) {
        return -1;
    }
    memcpy(buf, line, n);
    buf[n] = '\n';
    left = n + 1;
    while (left > 0) {
        ssize_t w = write(1, p, left);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* The shell has gone: so do we. */
            _exit(0);
        }
        p += w;
        left -= (size_t)w;
    }
    return 0;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    char line[WEB_LINE_MAX];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(line)) {
        emit(NULL, line, (size_t)n);
    }
}

static void say_fail(int seq, enum web_fail f, long http, const char *url, const char *text)
{
    char eu[WEB_URL_MAX * 2];
    char et[WEB_FAIL_TEXT_MAX * 2];

    if (web_escape(url ? url : "", strlen(url ? url : ""), eu, sizeof(eu)) < 0) {
        eu[0] = '\0';
    }
    if (web_escape(text ? text : "", strlen(text ? text : ""), et, sizeof(et)) < 0) {
        et[0] = '\0';
    }
    say("fail\t%d\t%s\t%ld\t%s\t%s", seq, web_fail_name(f), http > 0 && http < 1000 ? http : 0, eu, et);
}

static void log_origin(const char *what, const char *url)
{
    struct web_url u;
    char origin[WEB_HOST_MAX + 32];

    if (web_url_parse(url, &u) == WEB_URL_OK && web_url_origin(&u, origin, sizeof(origin)) > 0) {
        LOG_INFO("browser: %s %s", what, origin);
    }
}

/* ---- input ------------------------------------------------------------------------ */

static void command(struct helper *h, char *line)
{
    char *f[5];
    int n = 0;
    char *p = line;

    while (n < 5) {
        char *t = strchr(p, '\t');

        f[n++] = p;
        if (!t) {
            break;
        }
        *t = '\0';
        p = t + 1;
    }
    if (strcmp(f[0], "quit") == 0) {
        h->quit = true;
    } else if (strcmp(f[0], "stop") == 0 && n == 2) {
        int seq = atoi(f[1]);

        if (h->active && seq == h->seq) {
            h->stop = true;
        }
        if (h->have_open && seq == h->open_seq) {
            h->have_open = false;
        }
    } else if (strcmp(f[0], "open") == 0 && n == 5) {
        h->have_open = true;
        h->open_seq = atoi(f[1]);
        h->open_maxw = atoi(f[2]);
        web_copy(h->open_flags, sizeof(h->open_flags), f[3]);
        web_copy(h->open_url, sizeof(h->open_url), f[4]);
        if (h->active) {
            h->stop = true; /* a new page replaces the one loading */
        }
    }
    /* Anything else is ignored: the shell is the only writer. */
}

/* Read what the shell sent; wait up to timeout_ms (-1: until something comes). */
static void read_commands(struct helper *h, int timeout_ms)
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
            h->quit = true; /* the shell closed its end */
            return;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!h->in_over) {
                    h->in[h->in_len] = '\0';
                    command(h, h->in);
                }
                h->in_len = 0;
                h->in_over = false;
            } else if (h->in_len + 1 >= sizeof(h->in)) {
                h->in_over = true;
            } else if (!h->in_over) {
                h->in[h->in_len++] = buf[i];
            }
        }
        timeout_ms = 0; /* take what else is there, then go back to work */
    }
}

static int abort_cb(void *ctx)
{
    struct helper *h = ctx;

    if (h->session) {
        read_commands(h, 0);
    }
    return term || h->quit || h->stop;
}

static void progress_cb(void *ctx, long bytes)
{
    struct helper *h = ctx;
    int64_t now = mono_ms();

    if (h->session && now - h->progress_at >= PROGRESS_MS) {
        h->progress_at = now;
        say("progress\t%d\t%s\t%ld", h->seq, h->stage, bytes);
    }
}

/* ---- a page ----------------------------------------------------------------------- */

static bool is_html(const char *type, const char *body, size_t len)
{
    size_t i;

    if (strcmp(type, "text/html") == 0 || strcmp(type, "application/xhtml+xml") == 0) {
        return true;
    }
    if (type[0]) {
        return false;
    }
    /* No type at all: markup if it starts like markup. */
    for (i = 0; i < len && i < 256 && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' ||
                                       body[i] == '\t'); i++) {
    }
    return i < len && body[i] == '<';
}

static const char *base_name(const char *url)
{
    const char *q = strchr(url, '?');
    const char *end = q ? q : url + strlen(url);
    const char *s = end;

    while (s > url && s[-1] != '/') {
        s--;
    }
    return s;
}

/* Build the document for a fetched body. Returns false with *fail set when
 * the body is not something Browser shows. */
static bool make_doc(const struct web_fetch_resp *r, const struct web_url *final, struct web_doc *d,
                     enum web_fail *fail, char *text, size_t textlen)
{
    if (is_html(r->type, r->body, r->len)) {
        web_html_parse(r->body, r->len, final, r->charset[0] ? r->charset : NULL, d, NULL);
    } else if (strncmp(r->type, "text/", 5) == 0) {
        web_text_parse(r->body, r->len, r->charset[0] ? r->charset : NULL, d);
    } else if (strcmp(r->type, "image/jpeg") == 0 || strcmp(r->type, "image/png") == 0) {
        char name[WEB_ALT_MAX];

        snprintf(name, sizeof(name), "%.*s", (int)strcspn(base_name(r->final_url), "?"),
                 base_name(r->final_url));
        web_doc_set_title(d, name, strlen(name));
        web_doc_add_image(d, r->final_url, name, -1, 0, 0);
    } else {
        *fail = WEB_FAIL_TYPE;
        snprintf(text, textlen, "%s, %zu KB: Browser shows web pages and pictures; downloads are not supported",
                 r->type[0] ? r->type : "unknown type", (r->len + 1023) / 1024);
        return false;
    }
    if (r->cut) {
        d->truncated = true;
    }
    return true;
}

static int write_pixels(const char *dir, int seq, int id, const struct web_pixels *px, char *name, size_t nlen)
{
    char path[1024];
    size_t bytes = (size_t)px->w * (size_t)px->h * sizeof(uint16_t);
    const char *p = (const char *)px->px;
    int fd;

    snprintf(name, nlen, "%d-%d.rgb565", seq, id);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    while (bytes > 0) {
        ssize_t w = write(fd, p, bytes);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            unlink(path);
            return -1;
        }
        p += w;
        bytes -= (size_t)w;
    }
    return close(fd);
}

static void images(struct helper *h, const struct web_doc *d, int maxw)
{
    size_t budget = PAGE_PIXELS_MAX;
    size_t i;

    h->stage = "images";
    for (i = 0; i < d->nimages && !abort_cb(h); i++) {
        const struct web_image *im = &d->images[i];
        char url[WEB_URL_MAX];
        struct web_fetch_req req;
        struct web_fetch_resp r;
        struct web_pixels px;
        enum web_image_err e;
        char name[WEB_FILE_NAME_MAX];
        size_t n = im->src_len < sizeof(url) - 1 ? im->src_len : sizeof(url) - 1;

        if (budget < 64 * 64) {
            say("nopixels\t%d\t%zu\tthe page's pictures used their memory", h->seq, i);
            continue;
        }
        memcpy(url, d->text + im->src_off, n);
        url[n] = '\0';
        memset(&req, 0, sizeof(req));
        req.url = url;
        req.max_bytes = WEB_IMAGE_BYTES_MAX;
        req.keep_cut = false;
        req.accept = ACCEPT_IMAGE;
        req.ca_file = h->ca_file;
        req.abort_cb = abort_cb;
        req.progress_cb = progress_cb;
        req.ctx = h;
        if (web_fetch(&h->f, &req, &r) != 0) {
            if (r.fail == WEB_FAIL_STOPPED) {
                web_fetch_resp_free(&r);
                break;
            }
            say("nopixels\t%d\t%zu\t%s", h->seq, i,
                r.fail == WEB_FAIL_TYPE ? "picture too large" : "could not be loaded");
            web_fetch_resp_free(&r);
            continue;
        }
        if (r.status >= 400) {
            say("nopixels\t%d\t%zu\tHTTP %ld", h->seq, i, r.status);
            web_fetch_resp_free(&r);
            continue;
        }
        e = web_image_decode((const unsigned char *)r.body, r.len, im->width, maxw, IMAGE_MAX_H,
                             budget < WEB_IMAGE_PIXELS_MAX ? budget : WEB_IMAGE_PIXELS_MAX, &px);
        web_fetch_resp_free(&r);
        if (e != WEB_IMAGE_OK) {
            say("nopixels\t%d\t%zu\t%s", h->seq, i, web_image_err_text(e));
            continue;
        }
        if (write_pixels(h->img_dir, h->seq, (int)i, &px, name, sizeof(name)) != 0) {
            say("nopixels\t%d\t%zu\tcould not be stored", h->seq, i);
        } else {
            budget -= (size_t)px.w * (size_t)px.h;
            say("pixels\t%d\t%zu\t%d\t%d\t%s", h->seq, i, px.w, px.h, name);
        }
        free(px.px);
    }
}

static void open_page(struct helper *h, int seq, int maxw, const char *flags, const char *url)
{
    struct web_fetch_req req;
    struct web_fetch_resp r;
    struct web_url u;
    struct web_url final;
    struct web_doc d;
    enum web_fail fail = WEB_FAIL_INTERNAL;
    char text[WEB_FAIL_TEXT_MAX];
    bool want_images = strchr(flags, 'i') != NULL && h->img_dir != NULL;

    h->seq = seq;
    h->active = true;
    h->stop = false;
    h->stage = "receive";
    h->progress_at = 0;
    if (maxw < 32 || maxw > WEB_IMAGE_SIDE_MAX) {
        maxw = 560;
    }
    if (web_url_parse(url, &u) != WEB_URL_OK || (u.scheme != WEB_SCHEME_HTTP && u.scheme != WEB_SCHEME_HTTPS)) {
        say_fail(seq, WEB_FAIL_URL, 0, url, "not an address Browser can open");
        goto done;
    }
    log_origin("open", url);
    say("progress\t%d\tconnect\t0", seq);
    memset(&req, 0, sizeof(req));
    req.url = url;
    req.max_bytes = WEB_PAGE_BYTES_MAX;
    req.keep_cut = true;
    req.accept = ACCEPT_PAGE;
    req.ca_file = h->ca_file;
    req.abort_cb = abort_cb;
    req.progress_cb = progress_cb;
    req.ctx = h;
    if (web_fetch(&h->f, &req, &r) != 0) {
        if ((r.fail == WEB_FAIL_DNS || r.fail == WEB_FAIL_CONNECT || r.fail == WEB_FAIL_TIMEOUT) && !h->fake &&
            !web_net_has_route()) {
            r.fail = WEB_FAIL_OFFLINE;
        }
        LOG_INFO("browser: failed: %s", web_fail_name(r.fail));
        say_fail(seq, r.fail, r.status, r.final_url, r.text);
        web_fetch_resp_free(&r);
        goto done;
    }
    if (web_url_parse(r.final_url, &final) != WEB_URL_OK) {
        final = u;
    }
    web_doc_init(&d);
    if (!make_doc(&r, &final, &d, &fail, text, sizeof(text))) {
        say_fail(seq, fail, r.status, r.final_url, text);
    } else {
        unsigned pflags = (final.scheme == WEB_SCHEME_HTTPS ? WEB_PAGE_SECURE : 0) |
                          (want_images && d.nimages > 0 ? WEB_PAGE_IMAGES : 0);

        web_proto_send_doc(emit, NULL, seq, pflags, (int)(r.status > 0 && r.status < 1000 ? r.status : 0),
                           r.final_url, &d);
        web_fetch_resp_free(&r);
        if (pflags & WEB_PAGE_IMAGES) {
            images(h, &d, maxw);
        }
    }
    web_fetch_resp_free(&r);
    web_doc_free(&d);
done:
    h->active = false;
#ifdef __GLIBC__
    /* A page and its pictures can take a few MB for a moment; give them back
     * while the helper waits, rather than keep the high-water mark. */
    malloc_trim(0);
#endif
    say("idle\t%d", seq);
}

/* ---- modes ------------------------------------------------------------------------- */

static int start_fetcher(struct helper *h)
{
    char err[160];

    if (h->fake || web_fetcher_curl(&h->f, err, sizeof(err)) != 0) {
        if (!h->fake) {
            return -1;
        }
        web_fetcher_fake(&h->f);
    }
    return 0;
}

static const char *features(bool fake)
{
    static char buf[32];
    const char *img = web_image_formats();

    snprintf(buf, sizeof(buf), "%s%s%s", fake ? "fake" : web_fetch_curl_available() ? "net" : "none",
             img[0] ? ",img:" : "", img);
    return buf;
}

static int session(struct helper *h)
{
    bool no_net = false;

    h->session = true;
    if (start_fetcher(h) != 0) {
        no_net = true; /* every open is answered with no-network-build */
    }
    LOG_INFO("browser: session start, %s", features(h->fake));
    say("hello\t%d\t%s", WEB_PROTO_VERSION, no_net ? "none" : features(h->fake));
    while (!term && !h->quit) {
        if (!h->have_open) {
            read_commands(h, -1);
            continue;
        }
        h->have_open = false;
        if (no_net) {
            say_fail(h->open_seq, WEB_FAIL_NO_NETWORK_BUILD, 0, h->open_url,
                     "this build of Browser has no HTTP client");
            say("idle\t%d", h->open_seq);
            continue;
        }
        {
            char url[WEB_URL_MAX];
            char flags[8];
            int seq = h->open_seq;
            int maxw = h->open_maxw;

            web_copy(url, sizeof(url), h->open_url);
            web_copy(flags, sizeof(flags), h->open_flags);
            open_page(h, seq, maxw, flags, url);
        }
    }
    if (!no_net && h->f.close) {
        h->f.close(&h->f);
    }
    LOG_INFO("browser: session end");
    return 0;
}

static void print_doc(const struct web_doc *d, const char *url)
{
    size_t i;

    printf("%s\n%s\n\n", d->title[0] ? d->title : "(no title)", url);
    for (i = 0; i < d->nblocks; i++) {
        const struct web_block *b = &d->blocks[i];
        size_t r;

        if (b->type == WEB_BLOCK_RULE) {
            printf("----\n\n");
            continue;
        }
        if (b->type == WEB_BLOCK_IMAGE) {
            printf("[picture: %s]\n\n", d->images[b->image].alt[0] ? d->images[b->image].alt : "no description");
            continue;
        }
        printf("%*s%s", b->type == WEB_BLOCK_HEADING ? 0 : b->level * 2, "",
               b->type == WEB_BLOCK_HEADING ? "# " : b->type == WEB_BLOCK_QUOTE ? "> " : "");
        for (r = b->run_first; r < b->run_first + b->run_count; r++) {
            size_t n;
            const char *t = web_doc_run_text(d, r, &n);

            printf("%.*s", (int)n, t);
            if (d->runs[r].link >= 0 && (r + 1 == b->run_first + b->run_count ||
                                         d->runs[r + 1].link != d->runs[r].link)) {
                printf("[%d]", d->runs[r].link + 1);
            }
        }
        printf("\n\n");
    }
    if (d->nlinks) {
        printf("Links:\n");
        for (i = 0; i < d->nlinks; i++) {
            size_t n;
            const char *u = web_doc_link_url(d, (int)i, &n);

            printf("%4zu. %.*s%s\n", i + 1, (int)n, u, d->links[i].supported ? "" : " (not supported)");
        }
    }
    if (d->truncated) {
        printf("\n(the page was cut at Browser's limits)\n");
    }
}

static int dump(struct helper *h, const char *url)
{
    struct web_url u;
    struct web_url final;
    struct web_fetch_req req;
    struct web_fetch_resp r;
    struct web_doc d;
    enum web_fail fail = WEB_FAIL_INTERNAL;
    char text[WEB_FAIL_TEXT_MAX];
    char err[160];
    int rc = 1;

    if (web_url_from_input(url, &u) != WEB_URL_OK || (u.scheme != WEB_SCHEME_HTTP && u.scheme != WEB_SCHEME_HTTPS)) {
        fprintf(stderr, "pos-browser: %s\n", web_url_err_text(web_url_from_input(url, &u)));
        return 2;
    }
    if (h->fake) {
        web_fetcher_fake(&h->f);
    } else if (web_fetcher_curl(&h->f, err, sizeof(err)) != 0) {
        fprintf(stderr, "pos-browser: %s\n", err);
        return 1;
    }
    {
        char abs[WEB_URL_MAX];

        web_url_format(&u, abs, sizeof(abs));
        memset(&req, 0, sizeof(req));
        req.url = abs;
        req.max_bytes = WEB_PAGE_BYTES_MAX;
        req.keep_cut = true;
        req.accept = ACCEPT_PAGE;
        req.ca_file = h->ca_file;
        req.abort_cb = abort_cb;
        req.ctx = h;
        if (web_fetch(&h->f, &req, &r) != 0) {
            if ((r.fail == WEB_FAIL_DNS || r.fail == WEB_FAIL_CONNECT) && !h->fake && !web_net_has_route()) {
                r.fail = WEB_FAIL_OFFLINE;
            }
            fprintf(stderr, "pos-browser: %s: %s\n", web_fail_name(r.fail), r.text);
            web_fetch_resp_free(&r);
            h->f.close(&h->f);
            return 1;
        }
    }
    if (web_url_parse(r.final_url, &final) != WEB_URL_OK) {
        final = u;
    }
    web_doc_init(&d);
    if (make_doc(&r, &final, &d, &fail, text, sizeof(text))) {
        printf("HTTP %ld, %s, %zu bytes%s\n", r.status, r.type[0] ? r.type : "no type", r.len,
               r.cut ? " (cut)" : "");
        print_doc(&d, r.final_url);
        fprintf(stderr, "document: %zu blocks, %zu runs, %zu links, %zu images, %zu KB in memory\n", d.nblocks,
                d.nruns, d.nlinks, d.nimages, web_doc_heap_bytes(&d) / 1024);
        rc = 0;
    } else {
        fprintf(stderr, "pos-browser: %s: %s\n", web_fail_name(fail), text);
    }
    web_doc_free(&d);
    web_fetch_resp_free(&r);
    h->f.close(&h->f);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: pos-browser session [--fake] [--images DIR] [--ca-file FILE]\n"
                    "       pos-browser dump [--fake] [--ca-file FILE] URL\n"
                    "       pos-browser features\n");
    return 2;
}

int main(int argc, char **argv)
{
    struct helper h;
    struct sigaction sa;
    const char *mode;
    const char *url = NULL;
    int i;

    memset(&h, 0, sizeof(h));
    if (argc < 2) {
        return usage();
    }
    mode = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fake") == 0) {
            h.fake = true;
        } else if (strcmp(argv[i], "--images") == 0 && i + 1 < argc) {
            h.img_dir = argv[++i];
        } else if (strcmp(argv[i], "--ca-file") == 0 && i + 1 < argc) {
            h.ca_file = argv[++i];
        } else if (argv[i][0] != '-' && !url) {
            url = argv[i];
        } else {
            return usage();
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (strcmp(mode, "features") == 0) {
        printf("%s\n", features(false));
        return 0;
    }
    pocketlog_init("pos-browser");
    if (strcmp(mode, "session") == 0 && !url) {
        if (h.img_dir) {
            struct stat st;

            /* The shell made it for this session; anything else is not used. */
            if (lstat(h.img_dir, &st) != 0 || !S_ISDIR(st.st_mode) || (st.st_mode & 077) != 0) {
                LOG_WARN("browser: image directory refused, no pictures this session");
                h.img_dir = NULL;
            }
        }
        return session(&h);
    }
    if (strcmp(mode, "dump") == 0 && url) {
        return dump(&h, url);
    }
    return usage();
}
