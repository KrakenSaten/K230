/*
 * Browser's view in Library mode (apps/browser/browser_view.h,
 * docs/apps/LIBRARY.md), with pos-library's side played through the real
 * protocol: the field is a search that becomes an address of the collection,
 * an empty search does nothing, the field shows the query of a results page
 * and nothing on an article, links that leave the collection are refused with
 * the Library's words, the helper's failures get the Library's words, the
 * hint says OFFLINE, Browser's default bookmarks are not carried over, and a
 * bookmark survives the app being closed and opened again. Browser itself is
 * checked to be unchanged by the mode existing.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "browser_view.h"
#include "web/web_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

struct feed {
    struct browser_view *v;
    struct web_rx *rx;
    unsigned changed;
};

static int emit(void *ctx, const char *line, size_t n)
{
    struct feed *f = ctx;
    char buf[WEB_LINE_MAX + 1];
    struct web_rx_msg m;
    enum web_rx_kind k;

    memcpy(buf, line, n);
    buf[n] = '\0';
    k = web_rx_line(f->rx, buf, &m);
    if (k != WEB_RX_NONE && k != WEB_RX_BAD) {
        f->changed |= browser_view_apply(f->v, &m, f->rx, NULL, 0);
    }
    return 0;
}

/* pos-library's answer to seq: an article with one link into the
 * collection and one out of it (as the helper marks it). */
static unsigned article(struct browser_view *v, struct web_rx *rx, int seq, const char *url, const char *title)
{
    struct web_doc d;
    struct feed f = { v, rx, 0 };
    int in;
    int out;

    web_doc_init(&d);
    web_doc_set_title(&d, title, strlen(title));
    in = web_doc_add_link(&d, WEB_LIBRARY_ORIGIN "Greenhouse_gas", true);
    out = web_doc_add_link(&d, "https://en.wikipedia.org/wiki/Water", false);
    web_doc_add_text(&d, "Water and ", 10, 0, -1);
    web_doc_add_text(&d, "greenhouse gas", 14, 0, in);
    web_doc_add_text(&d, " and the ", 9, 0, -1);
    web_doc_add_text(&d, "source", 6, 0, out);
    web_doc_end_block(&d);
    web_proto_send_doc(emit, &f, seq, 0, 200, url, &d);
    web_doc_free(&d);
    return f.changed;
}

static unsigned line(struct browser_view *v, struct web_rx *rx, const char *text)
{
    char buf[WEB_LINE_MAX];
    struct web_rx_msg m;
    enum web_rx_kind k;

    snprintf(buf, sizeof(buf), "%s", text);
    k = web_rx_line(rx, buf, &m);
    if (k == WEB_RX_NONE || k == WEB_RX_BAD) {
        return 0;
    }
    return browser_view_apply(v, &m, rx, NULL, 0);
}

int main(void)
{
    char dir[] = "/tmp/library_view_test.XXXXXX";
    char store[256];
    char buf[512];
    struct browser_view *v = malloc(sizeof(*v));
    struct web_rx rx;
    struct browser_cmd cmd;
    unsigned ch;
    char why[128];

    if (!v || !mkdtemp(dir)) {
        return 2;
    }
    snprintf(store, sizeof(store), "%s/library/state", dir);
    web_rx_init(&rx);

    /* ---- a first start ---- */
    browser_view_init(v, store, why, sizeof(why));
    browser_view_set_library(v);
    check("a first start: the start page, no bookmarks (Browser's two are not carried over)",
          v->library && v->state == BROWSER_START && v->store.nbookmark == 0 && v->field[0] == '\0');
    check("and the start page has no hint", v->hint && v->hint[0] == '\0');

    /* ---- searching ---- */
    ch = browser_view_go(v, "   ", 0, &cmd);
    check("an empty search does nothing", ch == 0 && cmd.kind == BROWSER_CMD_NONE && !v->loading);
    ch = browser_view_go(v, "  climate change & CO\xe2\x82\x82 ", 0, &cmd);
    check("words become an address of the collection, encoded",
          cmd.kind == BROWSER_CMD_OPEN && v->loading &&
              strcmp(cmd.url, WEB_LIBRARY_SEARCH "climate%20change%20%26%20CO%E2%82%82") == 0);
    check("searching says so, and LOADING", strcmp(v->status, "Searching\xe2\x80\xa6") == 0 &&
                                                strcmp(v->hint, "LOADING") == 0);
    check("the field keeps the words while searching",
          strcmp(v->field, "climate change & CO\xe2\x82\x82") == 0);
    {
        struct browser_cmd c2;
        char typed[400];

        memset(typed, 'a', sizeof(typed) - 1);
        typed[sizeof(typed) - 1] = '\0';
        browser_view_go(v, typed, 0, &c2);
        check("a very long search is cut to WEB_LIBRARY_QUERY_MAX bytes",
              strlen(c2.url) == strlen(WEB_LIBRARY_SEARCH) + WEB_LIBRARY_QUERY_MAX);
        browser_view_go(v, "climate change & CO\xe2\x82\x82", 0, &cmd);
    }
    ch = article(v, &rx, cmd.seq, cmd.url, "Search: climate change");
    check("the results page is shown, OFFLINE", v->state == BROWSER_PAGE && strcmp(v->hint, "OFFLINE") == 0 &&
                                                    strcmp(v->status, "Search: climate change") == 0);
    check("and its field shows the query again", strcmp(v->field, "climate change & CO\xe2\x82\x82") == 0);

    /* ---- reading and links ---- */
    ch = browser_view_follow(v, 0, 0, &cmd);
    check("a link into the collection is opened", cmd.kind == BROWSER_CMD_OPEN &&
                                                     strcmp(cmd.url, WEB_LIBRARY_ORIGIN "Greenhouse_gas") == 0);
    check("opening says so", strcmp(v->status, "Opening\xe2\x80\xa6") == 0);
    article(v, &rx, cmd.seq, cmd.url, "Greenhouse gas");
    check("an article: its title, OFFLINE, an empty field",
          strcmp(v->status, "Greenhouse gas") == 0 && strcmp(v->hint, "OFFLINE") == 0 && v->field[0] == '\0');
    ch = browser_view_follow(v, 1, 0, &cmd);
    check("a link out of the collection is not followed", cmd.kind == BROWSER_CMD_NONE && v->state == BROWSER_PAGE);
    check("and the status line says why, in the Library's words",
          strstr(v->status, "outside this collection") != NULL && strstr(v->status, "offline") != NULL);
    ch = browser_view_back(v, 0, &cmd);
    check("BACK asks for the results page again", cmd.kind == BROWSER_CMD_OPEN &&
                                                     strncmp(cmd.url, WEB_LIBRARY_SEARCH, strlen(WEB_LIBRARY_SEARCH)) == 0);
    article(v, &rx, cmd.seq, cmd.url, "Search: climate change");
    check("and lands there", v->hist.index == 1 && browser_view_can_forward(v));
    browser_view_forward(v, 0, &cmd);
    article(v, &rx, cmd.seq, cmd.url, "Greenhouse gas");

    /* ---- bookmarks survive reopening ---- */
    ch = browser_view_toggle_bookmark(v);
    check("an article is bookmarked", browser_view_is_bookmark(v) && strcmp(v->status, "Bookmarked") == 0);
    check("and saved", browser_view_save(v) == 0 && access(store, F_OK) == 0);
    browser_view_free(v);
    browser_view_init(v, store, why, sizeof(why));
    browser_view_set_library(v);
    check("reopened: the bookmark is there, with its title",
          v->store.nbookmark == 1 && strcmp(v->store.bookmark[0].url, WEB_LIBRARY_ORIGIN "Greenhouse_gas") == 0 &&
              strcmp(v->store.bookmark[0].title, "Greenhouse gas") == 0);
    ch = browser_view_open(v, v->store.bookmark[0].url, 0, &cmd);
    check("and opens the article", cmd.kind == BROWSER_CMD_OPEN && strcmp(cmd.url, v->store.bookmark[0].url) == 0);

    /* ---- failures ---- */
    snprintf(buf, sizeof(buf), "fail\t%d\tnotfound\t0\t%sNo_such\tno entry", cmd.seq, WEB_LIBRARY_ORIGIN);
    line(v, &rx, buf);
    check("an entry the collection lacks: Not in this collection",
          v->state == BROWSER_ERROR && strcmp(v->err.title, "Not in this collection") == 0 &&
              strcmp(v->hint, "ERROR") == 0);
    browser_view_open(v, WEB_LIBRARY_MAIN, 0, &cmd);
    snprintf(buf, sizeof(buf), "fail\t%d\tarchive\t0\t%s\tthere is no .zim archive in /root/Library", cmd.seq,
             WEB_LIBRARY_MAIN);
    line(v, &rx, buf);
    check("no collection: Collection not available, and the helper's reason",
          strcmp(v->err.title, "Collection not available") == 0 &&
              strcmp(v->err.detail, "there is no .zim archive in /root/Library") == 0);
    check("an error page in Library mode never offers http://", v->err.alt_url[0] == '\0');
    browser_view_open(v, WEB_LIBRARY_ABOUT, 0, &cmd);
    ch = browser_view_helper_stopped(v, BROWSER_EXIT_START, 0);
    check("a helper that cannot start is named: pos-library", strstr(v->err.text, "pos-library") != NULL);
    browser_view_free(v);

    /* ---- Browser is unchanged ---- */
    snprintf(store, sizeof(store), "%s/browser/state", dir);
    browser_view_init(v, store, why, sizeof(why));
    check("Browser: a first start keeps its two bookmarks", !v->library && v->store.nbookmark == 2);
    browser_view_go(v, "example.com", 0, &cmd);
    check("Browser: typed words are still an address", cmd.kind == BROWSER_CMD_OPEN &&
                                                          strcmp(cmd.url, "https://example.com/") == 0 &&
                                                          strcmp(v->field, "https://example.com/") == 0);
    browser_view_free(v);

    (void)ch;
    web_rx_free(&rx);
    free(v);
    snprintf(buf, sizeof(buf), "rm -rf '%s'", dir);
    if (system(buf) != 0) {
        printf("note: could not remove %s\n", dir);
    }
    printf("library_view_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
