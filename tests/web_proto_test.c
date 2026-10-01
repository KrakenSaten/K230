/*
 * The Browser's app/helper protocol (core/web/web_proto.h): a document
 * written by the helper's side comes back identical on the shell's side, a
 * page is handed over only when complete, and a damaged or lying line -
 * ids out of order, references out of range, oversized numbers, a file name
 * that is a path, a "supported" link that is javascript: - throws the page
 * away instead of half-applying it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_html.h"
#include "web/web_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

struct sink {
    char *buf;
    size_t n;
    size_t cap;
    size_t longest;
};

static int emit(void *ctx, const char *line, size_t n)
{
    struct sink *s = ctx;

    if (s->n + n + 2 > s->cap) {
        s->cap = (s->n + n + 2) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    memcpy(s->buf + s->n, line, n);
    s->n += n;
    s->buf[s->n++] = '\n';
    s->buf[s->n] = '\0';
    if (n > s->longest) {
        s->longest = n;
    }
    return 0;
}

/* Feed text line by line; returns the last non-NONE kind, and the page if one arrived. */
static enum web_rx_kind feed(struct web_rx *rx, const char *text, struct web_doc *page, struct web_rx_msg *last)
{
    char *copy = strdup(text);
    char *save = NULL;
    char *line;
    enum web_rx_kind k = WEB_RX_NONE;

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct web_rx_msg m;
        enum web_rx_kind r = web_rx_line(rx, line, &m);

        if (r != WEB_RX_NONE) {
            k = r;
            *last = m;
        }
        if (r == WEB_RX_PAGE && page) {
            web_rx_take(rx, page);
        }
    }
    free(copy);
    return k;
}

static int same_doc(const struct web_doc *a, const struct web_doc *b)
{
    size_t i;

    if (a->nblocks != b->nblocks || a->nlinks != b->nlinks || a->nimages != b->nimages ||
        strcmp(a->title, b->title) != 0) {
        printf("     differ: %zu/%zu blocks %zu/%zu links %zu/%zu images [%s]/[%s]\n", a->nblocks, b->nblocks,
               a->nlinks, b->nlinks, a->nimages, b->nimages, a->title, b->title);
        return 0;
    }
    for (i = 0; i < a->nblocks; i++) {
        const struct web_block *x = &a->blocks[i];
        const struct web_block *y = &b->blocks[i];
        size_t r;
        char tx[65536];
        char ty[65536];
        size_t nx = 0;
        size_t ny = 0;

        if (x->type != y->type || x->level != y->level || x->image != y->image) {
            return 0;
        }
        /* Runs may be split differently; the text and its links may not. */
        {
            int pl = -2;
            int pf = -1;

            for (r = x->run_first; r < x->run_first + x->run_count; r++) {
                size_t n;
                const char *t = web_doc_run_text(a, r, &n);

                if (a->runs[r].link != pl || a->runs[r].flags != pf) {
                    pl = a->runs[r].link;
                    pf = a->runs[r].flags;
                    nx += (size_t)snprintf(tx + nx, sizeof(tx) - nx, "|%d:%d:", pl, pf);
                }
                nx += (size_t)snprintf(tx + nx, sizeof(tx) - nx, "%.*s", (int)n, t);
            }
            pl = -2;
            pf = -1;
            for (r = y->run_first; r < y->run_first + y->run_count; r++) {
                size_t n;
                const char *t = web_doc_run_text(b, r, &n);

                if (b->runs[r].link != pl || b->runs[r].flags != pf) {
                    pl = b->runs[r].link;
                    pf = b->runs[r].flags;
                    ny += (size_t)snprintf(ty + ny, sizeof(ty) - ny, "|%d:%d:", pl, pf);
                }
                ny += (size_t)snprintf(ty + ny, sizeof(ty) - ny, "%.*s", (int)n, t);
            }
        }
        if (strcmp(tx, ty) != 0) {
            printf("     block %zu differs:\n     %.200s\n     %.200s\n", i, tx, ty);
            return 0;
        }
    }
    for (i = 0; i < a->nlinks; i++) {
        size_t n1;
        size_t n2;
        const char *u1 = web_doc_link_url(a, (int)i, &n1);
        const char *u2 = web_doc_link_url(b, (int)i, &n2);

        if (n1 != n2 || memcmp(u1, u2, n1) != 0 || a->links[i].supported != b->links[i].supported) {
            return 0;
        }
    }
    for (i = 0; i < a->nimages; i++) {
        if (strcmp(a->images[i].alt, b->images[i].alt) != 0 || a->images[i].link != b->images[i].link ||
            a->images[i].width != b->images[i].width) {
            return 0;
        }
    }
    return 1;
}

static void refused(const char *name, const char *text)
{
    struct web_rx rx;
    struct web_doc page;
    struct web_rx_msg m;
    char full[1024];
    enum web_rx_kind k;

    web_rx_init(&rx);
    web_doc_init(&page);
    snprintf(full, sizeof(full), "page\t7\t1\t200\thttps://example.com/\tT\n%send\t7\n", text);
    k = feed(&rx, full, &page, &m);
    check(name, k == WEB_RX_BAD && page.nblocks == 0 && rx.bad > 0);
    web_rx_free(&rx);
    web_doc_free(&page);
}

int main(void)
{
    static const char html[] =
        "<title>A\ttab\\and\nline</title><h1>Head</h1><p>Text with a <a href=/x>link</a>, "
        "<code>code</code> and a\ttab\\back.</p><ul><li>item</li></ul><pre>pre\n  kept\tt\\b</pre>"
        "<img src=a.jpg alt=\"cat\" width=320 height=200><hr><p><a href=\"mailto:x@y\">mail</a></p>";
    struct web_url base;
    struct web_doc d;
    struct web_doc got;
    struct web_rx rx;
    struct web_rx_msg m;
    struct sink s = { 0 };
    char esc[64];
    char *big;
    size_t i;

    web_url_parse("https://example.com/page", &base);
    web_doc_init(&d);
    web_html_parse(html, strlen(html), &base, NULL, &d, NULL);

    /* ---- round trip ------------------------------------------------------------ */
    check("a document is written", web_proto_send_doc(emit, &s, 5, WEB_PAGE_SECURE | WEB_PAGE_IMAGES, 200,
                                                      "https://example.com/page", &d) == 0);
    web_rx_init(&rx);
    web_doc_init(&got);
    check("and read back as a whole page", feed(&rx, s.buf, &got, &m) == WEB_RX_PAGE && m.seq == 5 &&
                                              m.http == 200 && (m.flags & WEB_PAGE_IMAGES) &&
                                              strcmp(m.url, "https://example.com/page") == 0);
    check("identical: blocks, runs, links, images, title", same_doc(&d, &got));
    check("backslashes and line breaks survive the escaping", strstr(got.text, "pre\n  kept t\\b") != NULL && strstr(got.text, "a tab\\back") != NULL);
    check("no line of it is longer than the limit", s.longest < WEB_LINE_MAX);
    check("the image address never crosses", strstr(s.buf, "a.jpg") == NULL);
    web_doc_free(&got);

    /* A long run crosses in pieces, cut at character boundaries. */
    big = malloc(200000);
    for (i = 0; i + 3 < 150000; i += 3) {
        memcpy(big + i, "\xc3\xa6x", 3);
    }
    memcpy(big, "<p>", 3);
    big[i] = '\0';
    web_doc_free(&d);
    web_html_parse(big, strlen(big), &base, NULL, &d, NULL);
    s.n = 0;
    s.longest = 0;
    web_proto_send_doc(emit, &s, 6, 0, 200, "https://example.com/", &d);
    check("a 150 KB paragraph crosses in lines under the limit", s.longest < WEB_LINE_MAX);
    check("and arrives whole", feed(&rx, s.buf, &got, &m) == WEB_RX_PAGE && same_doc(&d, &got));
    web_doc_free(&got);
    free(big);

    /* ---- one message at a time ---------------------------------------------------- */
    check("hello", feed(&rx, "hello\t1\tnet,img", NULL, &m) == WEB_RX_HELLO && m.proto == 1 &&
                       strcmp(m.features, "net,img") == 0);
    check("progress", feed(&rx, "progress\t3\treceive\t12345", NULL, &m) == WEB_RX_PROGRESS && m.bytes == 12345);
    check("fail", feed(&rx, "fail\t3\ttls\t0\thttps://x.example/\tcertificate has expired", NULL, &m) == WEB_RX_FAIL &&
                      m.fail == WEB_FAIL_TLS && strcmp(m.text, "certificate has expired") == 0);
    check("an unknown fail kind is internal, not trusted",
          feed(&rx, "fail\t3\tpwned\t0\t\t", NULL, &m) == WEB_RX_FAIL && m.fail == WEB_FAIL_INTERNAL);
    check("pixels", feed(&rx, "pixels\t3\t2\t320\t200\t3-2.rgb565", NULL, &m) == WEB_RX_PIXELS && m.w == 320 &&
                        strcmp(m.file, "3-2.rgb565") == 0);
    check("idle", feed(&rx, "idle\t3", NULL, &m) == WEB_RX_IDLE && m.seq == 3);
    check("a page is not handed over before its end",
          feed(&rx, "page\t9\t0\t200\thttps://example.com/\tT\nblock\t0\t0\t-1\nrun\t0\t-1\thi", &got, &m) ==
                  WEB_RX_NONE &&
              got.nblocks == 0);
    check("and a page for another seq does not end it", feed(&rx, "end\t8", &got, &m) == WEB_RX_BAD);
    web_rx_free(&rx);

    /* ---- damage ---------------------------------------------------------------------- */
    refused("a link out of order", "link\t1\t1\thttps://example.com/a\n");
    refused("a supported link that is javascript:", "link\t0\t1\tjavascript:alert(1)\n");
    refused("a supported link that is file:", "link\t0\t1\tfile:///etc/passwd\n");
    refused("a run pointing at a link that does not exist", "block\t0\t0\t-1\nrun\t0\t3\tx\n");
    refused("a run before any block", "run\t0\t-1\tx\n");
    refused("an image block without an image", "block\t6\t0\t0\n");
    refused("an image with a link out of range", "image\t0\t4\t10\t10\talt\n");
    refused("a block type out of range", "block\t42\t0\t-1\n");
    refused("a heading level out of range", "block\t1\t9\t-1\n");
    refused("run flags out of range", "block\t0\t0\t-1\nrun\t64\t-1\tx\n");
    refused("a field missing", "block\t0\t0\n");
    refused("a field too many", "block\t0\t0\t-1\t9\n");
    refused("a number with junk", "block\t0x1\t0\t-1\n");
    refused("an unknown line", "exec\t/bin/sh\n");
    web_rx_init(&rx);
    check("a page line with a bad address", feed(&rx, "page\t1\t0\t200\tfile:///etc\tT", NULL, &m) == WEB_RX_BAD);
    check("pixels naming a path", feed(&rx, "pixels\t1\t0\t10\t10\t../../etc/passwd", NULL, &m) == WEB_RX_BAD);
    check("pixels naming another file", feed(&rx, "pixels\t1\t0\t10\t10\t1-0.png", NULL, &m) == WEB_RX_BAD);
    check("pixels too large", feed(&rx, "pixels\t1\t0\t1600\t1600\t1-0.rgb565", NULL, &m) == WEB_RX_BAD);
    check("pixels of no size", feed(&rx, "pixels\t1\t0\t0\t10\t1-0.rgb565", NULL, &m) == WEB_RX_BAD);
    check("an image id past the limit", feed(&rx, "pixels\t1\t99\t10\t10\t1-99.rgb565", NULL, &m) == WEB_RX_BAD);
    web_rx_free(&rx);
    check("file names", web_rx_file_name_ok("12-3.rgb565") && !web_rx_file_name_ok("-3.rgb565") &&
                            !web_rx_file_name_ok("12-.rgb565") && !web_rx_file_name_ok("1/2-3.rgb565") &&
                            !web_rx_file_name_ok("12-3.rgb565.x") && !web_rx_file_name_ok("a2-3.rgb565"));
    check("escaping refuses what does not fit", web_escape("\t\t\t\t", 4, esc, 8) < 0 &&
                                                    web_escape("ab", 2, esc, 3) == 2);

    web_doc_free(&d);
    free(s.buf);
    printf("web_proto_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
