/*
 * The Browser's app/helper line protocol. See web_proto.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "web/web_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const fail_names[WEB_FAIL_COUNT] = {
    "url",     "offline",  "dns",      "connect", "timeout",  "tls",    "clock",    "redirects",
    "insecure-redirect", "bad-redirect", "type", "response", "stopped", "no-network-build",
    "internal",
};

const char *web_fail_name(enum web_fail f)
{
    return (f >= 0 && f < WEB_FAIL_COUNT) ? fail_names[f] : "internal";
}

enum web_fail web_fail_from_name(const char *s)
{
    int i;

    for (i = 0; i < WEB_FAIL_COUNT; i++) {
        if (strcmp(s, fail_names[i]) == 0) {
            return (enum web_fail)i;
        }
    }
    return WEB_FAIL_INTERNAL;
}

/* ---- writing ------------------------------------------------------------------- */

int web_escape(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = s[i];
        const char *e = NULL;

        if (c == '\\') {
            e = "\\\\";
        } else if (c == '\t') {
            e = "\\t";
        } else if (c == '\n') {
            e = "\\n";
        } else if (c == '\r') {
            continue;
        }
        if (e) {
            if (o + 2 >= cap) {
                return -1;
            }
            out[o++] = e[0];
            out[o++] = e[1];
        } else {
            if (o + 1 >= cap) {
                return -1;
            }
            out[o++] = c;
        }
    }
    if (o >= cap) {
        return -1;
    }
    out[o] = '\0';
    return (int)o;
}

struct line {
    char buf[WEB_LINE_MAX];
    size_t n;
    bool over;
};

static void line_start(struct line *l, const char *verb)
{
    l->n = 0;
    l->over = false;
    l->n = (size_t)snprintf(l->buf, sizeof(l->buf), "%s", verb);
}

static void line_int(struct line *l, long v)
{
    int w;

    if (l->over) {
        return;
    }
    w = snprintf(l->buf + l->n, sizeof(l->buf) - l->n, "\t%ld", v);
    if (w < 0 || (size_t)w >= sizeof(l->buf) - l->n) {
        l->over = true;
        return;
    }
    l->n += (size_t)w;
}

static void line_text(struct line *l, const char *s, size_t n)
{
    int w;

    if (l->over || l->n + 2 >= sizeof(l->buf)) {
        l->over = true;
        return;
    }
    l->buf[l->n++] = '\t';
    w = web_escape(s, n, l->buf + l->n, sizeof(l->buf) - l->n);
    if (w < 0) {
        l->over = true;
        return;
    }
    l->n += (size_t)w;
}

static int line_send(struct line *l, web_emit_fn emit, void *ctx)
{
    return l->over ? -1 : emit(ctx, l->buf, l->n);
}

int web_proto_send_doc(web_emit_fn emit, void *ctx, int seq, unsigned flags, int http,
                       const char *url, const struct web_doc *d)
{
    struct line *l = malloc(sizeof(*l));
    size_t i;
    int rc = -1;

    if (!l) {
        return -1;
    }
    line_start(l, "page");
    line_int(l, seq);
    line_int(l, (long)(flags | (d->truncated ? WEB_PAGE_TRUNCATED : 0)));
    line_int(l, http);
    line_text(l, url, strlen(url));
    line_text(l, d->title, strlen(d->title));
    if (line_send(l, emit, ctx) < 0) {
        goto out;
    }
    for (i = 0; i < d->nlinks; i++) {
        size_t n;
        const char *u = web_doc_link_url(d, (int)i, &n);

        line_start(l, "link");
        line_int(l, (long)i);
        line_int(l, d->links[i].supported ? 1 : 0);
        line_text(l, u, n);
        if (line_send(l, emit, ctx) < 0) {
            goto out;
        }
    }
    for (i = 0; i < d->nimages; i++) {
        const struct web_image *im = &d->images[i];

        line_start(l, "image");
        line_int(l, (long)i);
        line_int(l, im->link);
        line_int(l, im->width);
        line_int(l, im->height);
        line_text(l, im->alt, strlen(im->alt));
        if (line_send(l, emit, ctx) < 0) {
            goto out;
        }
    }
    for (i = 0; i < d->nblocks; i++) {
        const struct web_block *b = &d->blocks[i];
        size_t r;

        line_start(l, "block");
        line_int(l, b->type);
        line_int(l, b->level);
        line_int(l, b->image);
        if (line_send(l, emit, ctx) < 0) {
            goto out;
        }
        for (r = b->run_first; r < b->run_first + b->run_count; r++) {
            size_t n;
            const char *t = web_doc_run_text(d, r, &n);

            /* In pieces, each cut at a character boundary. */
            while (n > 0) {
                size_t take = n > WEB_RUN_PIECE ? WEB_RUN_PIECE : n;

                while (take < n && take > 1 && ((unsigned char)t[take] & 0xc0) == 0x80) {
                    take--;
                }
                line_start(l, "run");
                line_int(l, d->runs[r].flags);
                line_int(l, d->runs[r].link);
                line_text(l, t, take);
                if (line_send(l, emit, ctx) < 0) {
                    goto out;
                }
                t += take;
                n -= take;
            }
        }
    }
    line_start(l, "end");
    line_int(l, seq);
    rc = line_send(l, emit, ctx);
out:
    free(l);
    return rc;
}

/* ---- reading ------------------------------------------------------------------- */

void web_rx_init(struct web_rx *rx)
{
    memset(rx, 0, sizeof(*rx));
    web_doc_init(&rx->doc);
}

void web_rx_free(struct web_rx *rx)
{
    web_doc_free(&rx->doc);
    web_rx_init(rx);
}

static void unescape(char *s)
{
    char *o = s;

    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* Split line at tabs into exactly want fields, each unescaped. */
static bool split(char *line, char **f, int want)
{
    int n = 0;
    char *p = line;

    for (;;) {
        char *t = strchr(p, '\t');

        if (n >= want) {
            return false;
        }
        f[n++] = p;
        if (!t) {
            break;
        }
        *t = '\0';
        p = t + 1;
    }
    if (n != want) {
        return false;
    }
    for (n = 0; n < want; n++) {
        unescape(f[n]);
    }
    return true;
}

static bool num(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v;

    if (!*s || (*s != '-' && (*s < '0' || *s > '9')) || strlen(s) > 12) {
        return false;
    }
    v = strtol(s, &end, 10);
    if (*end || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static void copy(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);

    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static enum web_rx_kind bad(struct web_rx *rx)
{
    rx->bad++;
    if (rx->open) {
        web_doc_free(&rx->doc);
        rx->open = false;
    }
    return WEB_RX_BAD;
}

bool web_rx_file_name_ok(const char *name)
{
    const char *dot = strchr(name, '.');
    const char *dash = strchr(name, '-');
    const char *p;

    if (!dot || !dash || dash > dot || dash == name || dot == dash + 1 ||
        strcmp(dot, ".rgb565") != 0 || strlen(name) >= WEB_FILE_NAME_MAX) {
        return false;
    }
    for (p = name; p < dot; p++) {
        if (p != dash && (*p < '0' || *p > '9')) {
            return false;
        }
    }
    return true;
}

static enum web_rx_kind page_line(struct web_rx *rx, const char *verb, char *rest)
{
    struct web_doc *d = &rx->doc;
    char *f[5];
    long a;
    long b;
    long c;
    long e;

    if (!rx->open) {
        return bad(rx);
    }
    if (strcmp(verb, "link") == 0) {
        struct web_url u;

        if (!split(rest, f, 3) || !num(f[0], 0, WEB_DOC_LINK_MAX - 1, &a) || a != (long)d->nlinks ||
            !num(f[1], 0, 1, &b) || strlen(f[2]) >= WEB_URL_MAX) {
            return bad(rx);
        }
        /* A supported link must be one the shell would open anyway. */
        if (b && web_url_parse(f[2], &u) != WEB_URL_OK) {
            return bad(rx);
        }
        if (web_doc_add_link(d, f[2], b != 0) != a) {
            return bad(rx);
        }
        return WEB_RX_NONE;
    }
    if (strcmp(verb, "image") == 0) {
        if (!split(rest, f, 5) || !num(f[0], 0, WEB_DOC_IMAGE_MAX - 1, &a) ||
            a != (long)d->nimages || !num(f[1], -1, (long)d->nlinks - 1, &b) ||
            !num(f[2], 0, 65535, &c) || !num(f[3], 0, 65535, &e)) {
            return bad(rx);
        }
        {
            struct web_image *im = &d->images[d->nimages++];

            memset(im, 0, sizeof(*im));
            copy(im->alt, sizeof(im->alt), f[4]);
            im->link = (int16_t)b;
            im->width = (uint16_t)c;
            im->height = (uint16_t)e;
        }
        return WEB_RX_NONE;
    }
    if (strcmp(verb, "block") == 0) {
        int idx;

        if (!split(rest, f, 3) || !num(f[0], 0, WEB_BLOCK_TYPE_COUNT - 1, &a) ||
            !num(f[1], 0, a == WEB_BLOCK_HEADING ? 6 : WEB_INDENT_MAX, &b) ||
            !num(f[2], -1, (long)d->nimages - 1, &c) || ((a == WEB_BLOCK_IMAGE) != (c >= 0))) {
            return bad(rx);
        }
        idx = web_doc_begin_block(d, (enum web_block_type)a, (int)b);
        if (idx < 0) {
            return bad(rx);
        }
        d->blocks[idx].image = (int16_t)c;
        return WEB_RX_NONE;
    }
    if (strcmp(verb, "run") == 0) {
        if (!split(rest, f, 3) || !num(f[0], 0, WEB_RUN_CODE | WEB_RUN_STRONG | WEB_RUN_EM, &a) ||
            !num(f[1], -1, (long)d->nlinks - 1, &b) || !d->open_block ||
            strlen(f[2]) > WEB_RUN_PIECE) {
            return bad(rx);
        }
        /* Past the document's limits the page is simply cut here. */
        web_doc_add_text(d, f[2], strlen(f[2]), (unsigned)a, (int)b);
        return WEB_RX_NONE;
    }
    return bad(rx);
}

enum web_rx_kind web_rx_line(struct web_rx *rx, char *line, struct web_rx_msg *m)
{
    char *tab = strchr(line, '\t');
    char *rest = tab ? tab + 1 : line + strlen(line);
    char *f[5];
    long a;
    long b;
    long c;

    memset(m, 0, sizeof(*m));
    m->kind = WEB_RX_NONE;
    if (tab) {
        *tab = '\0';
    }
    if (strcmp(line, "link") == 0 || strcmp(line, "image") == 0 || strcmp(line, "block") == 0 ||
        strcmp(line, "run") == 0) {
        return page_line(rx, line, rest);
    }
    if (strcmp(line, "hello") == 0) {
        if (!split(rest, f, 2) || !num(f[0], 0, 1000, &a)) {
            return bad(rx);
        }
        m->proto = (int)a;
        copy(m->features, sizeof(m->features), f[1]);
        return m->kind = WEB_RX_HELLO;
    }
    if (strcmp(line, "progress") == 0) {
        if (!split(rest, f, 3) || !num(f[0], 0, 0x7fffffff, &a) || !num(f[2], 0, 1L << 40, &b)) {
            return bad(rx);
        }
        m->seq = (int)a;
        copy(m->stage, sizeof(m->stage), f[1]);
        m->bytes = b;
        return m->kind = WEB_RX_PROGRESS;
    }
    if (strcmp(line, "page") == 0) {
        struct web_url u;

        if (!split(rest, f, 5) || !num(f[0], 0, 0x7fffffff, &a) || !num(f[1], 0, 7, &b) ||
            !num(f[2], 0, 999, &c) || strlen(f[3]) >= WEB_URL_MAX ||
            web_url_parse(f[3], &u) != WEB_URL_OK) {
            return bad(rx);
        }
        web_doc_free(&rx->doc);
        rx->open = true;
        rx->seq = (int)a;
        rx->flags = (unsigned)b;
        rx->http = (int)c;
        copy(rx->url, sizeof(rx->url), f[3]);
        web_doc_set_title(&rx->doc, f[4], strlen(f[4]));
        return WEB_RX_NONE;
    }
    if (strcmp(line, "end") == 0) {
        if (!rx->open || !split(rest, f, 1) || !num(f[0], 0, 0x7fffffff, &a) || a != rx->seq) {
            return bad(rx);
        }
        web_doc_end_block(&rx->doc);
        if (rx->flags & WEB_PAGE_TRUNCATED) {
            rx->doc.truncated = true; /* only now: a cut document takes no more text */
        }
        rx->open = false;
        m->seq = rx->seq;
        m->flags = rx->flags;
        m->http = rx->http;
        copy(m->url, sizeof(m->url), rx->url);
        return m->kind = WEB_RX_PAGE;
    }
    if (strcmp(line, "fail") == 0) {
        if (!split(rest, f, 5) || !num(f[0], 0, 0x7fffffff, &a) || !num(f[2], 0, 999, &c)) {
            return bad(rx);
        }
        m->seq = (int)a;
        m->fail = web_fail_from_name(f[1]);
        m->http = (int)c;
        copy(m->url, sizeof(m->url), f[3]);
        copy(m->text, sizeof(m->text), f[4]);
        return m->kind = WEB_RX_FAIL;
    }
    if (strcmp(line, "pixels") == 0) {
        long w;
        long h;

        if (!split(rest, f, 5) || !num(f[0], 0, 0x7fffffff, &a) ||
            !num(f[1], 0, WEB_DOC_IMAGE_MAX - 1, &b) || !num(f[2], 1, WEB_IMAGE_SIDE_MAX, &w) ||
            !num(f[3], 1, WEB_IMAGE_SIDE_MAX, &h) || (unsigned long)(w * h) > WEB_IMAGE_PIXELS_MAX ||
            !web_rx_file_name_ok(f[4])) {
            return bad(rx);
        }
        m->seq = (int)a;
        m->image = (int)b;
        m->w = (int)w;
        m->h = (int)h;
        copy(m->file, sizeof(m->file), f[4]);
        return m->kind = WEB_RX_PIXELS;
    }
    if (strcmp(line, "nopixels") == 0) {
        if (!split(rest, f, 3) || !num(f[0], 0, 0x7fffffff, &a) ||
            !num(f[1], 0, WEB_DOC_IMAGE_MAX - 1, &b)) {
            return bad(rx);
        }
        m->seq = (int)a;
        m->image = (int)b;
        copy(m->text, sizeof(m->text), f[2]);
        return m->kind = WEB_RX_NOPIXELS;
    }
    if (strcmp(line, "idle") == 0) {
        if (!split(rest, f, 1) || !num(f[0], 0, 0x7fffffff, &a)) {
            return bad(rx);
        }
        m->seq = (int)a;
        return m->kind = WEB_RX_IDLE;
    }
    return bad(rx);
}

void web_rx_take(struct web_rx *rx, struct web_doc *out)
{
    web_doc_free(out);
    *out = rx->doc;
    web_doc_init(&rx->doc);
    rx->open = false;
}
