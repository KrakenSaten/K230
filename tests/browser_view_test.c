/*
 * The Browser's presentation logic (apps/browser/browser_view.h), with the
 * helper's side played through the real protocol: what each button asks
 * for, the back/forward list changing only when an answer arrives, STOP,
 * answers for stopped loads dropped, the error pages and their offers, the
 * picture budget, bookmarks and recent pages saved, and the helper dying
 * mid-load.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "browser_view.h"
#include "web/web_html.h"
#include "web/web_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* The helper's answer to seq: a page made from html. */
static unsigned page(struct browser_view *v, struct web_rx *rx, int seq, const char *url, int http,
                     unsigned flags, const char *html)
{
    struct web_doc d;
    struct web_url u;
    struct feed f = { v, rx, 0 };

    web_url_parse(url, &u);
    web_doc_init(&d);
    web_html_parse(html, strlen(html), &u, NULL, &d, NULL);
    web_proto_send_doc(emit, &f, seq, flags | (u.scheme == WEB_SCHEME_HTTPS ? WEB_PAGE_SECURE : 0), http, url, &d);
    web_doc_free(&d);
    return f.changed;
}

static unsigned line(struct browser_view *v, struct web_rx *rx, const char *text, uint16_t *px)
{
    char buf[WEB_LINE_MAX];
    struct web_rx_msg m;
    enum web_rx_kind k;

    snprintf(buf, sizeof(buf), "%s", text);
    k = web_rx_line(rx, buf, &m);
    if (k == WEB_RX_NONE || k == WEB_RX_BAD) {
        free(px);
        return 0;
    }
    return browser_view_apply(v, &m, rx, px, 0);
}

static uint16_t *pixels(int w, int h)
{
    return calloc((size_t)w * (size_t)h, sizeof(uint16_t));
}

int main(void)
{
    char dir[] = "/tmp/browser_view_test.XXXXXX";
    char path[256];
    char why[128];
    char text[512];
    struct browser_view *v = malloc(sizeof(*v));
    struct browser_view *w = malloc(sizeof(*w));
    struct web_rx rx;
    struct browser_cmd cmd;
    unsigned ch;
    int seq;
    int i;

    if (!mkdtemp(dir) || !v || !w) {
        return 1;
    }
    snprintf(path, sizeof(path), "%s/state", dir);
    web_rx_init(&rx);
    browser_view_init(v, path, why, sizeof(why));

    /* ---- start ----------------------------------------------------------------------- */
    check("it opens on the start page, nothing loading, nothing to go back to",
          v->state == BROWSER_START && !v->loading && !browser_view_can_back(v) && !browser_view_can_forward(v) &&
              strcmp(browser_view_current_url(v), "about:home") == 0 && v->field[0] == '\0');
    check("the header hint is quiet on the start page", strcmp(v->hint, "") == 0);

    /* ---- malformed addresses never reach the helper --------------------------------------- */
    ch = browser_view_go(v, "javascript:alert(1)", 0, &cmd);
    check("javascript: becomes an error page and no command", cmd.kind == BROWSER_CMD_NONE &&
                                                                  v->state == BROWSER_ERROR &&
                                                                  v->err.fail == WEB_FAIL_URL &&
                                                                  (ch & BROWSER_CHANGED_STATE));
    browser_view_go(v, "two words", 0, &cmd);
    check("words are not an address, and say there is no search",
          cmd.kind == BROWSER_CMD_NONE && strstr(v->err.text, "no built-in search") != NULL);
    browser_view_reload(v, 0, &cmd);
    check("reload on a malformed address does nothing", cmd.kind == BROWSER_CMD_NONE);

    /* ---- a page ------------------------------------------------------------------------ */
    ch = browser_view_go(v, "doors.test", 1000, &cmd);
    seq = cmd.seq;
    check("an address typed without a scheme opens as https", cmd.kind == BROWSER_CMD_OPEN &&
                                                                 strcmp(cmd.url, "https://doors.test/") == 0 &&
                                                                 cmd.images && v->loading);
    check("while loading: the field shows it, the hint says LOADING", strcmp(v->field, "https://doors.test/") == 0 &&
                                                                          strcmp(v->hint, "LOADING") == 0);
    line(v, &rx, "progress\t1\treceive\t40960", NULL);
    check("progress is counted in the status line", strstr(v->status, "40 KB") != NULL);
    line(v, &rx, "progress\t999\treceive\t99999", NULL);
    check("progress of another load is not", strstr(v->status, "40 KB") != NULL);
    ch = page(v, &rx, seq, "https://doors.test/", 200, WEB_PAGE_IMAGES,
              "<title>Demo</title><p><a href=/a>A</a> <a href=mailto:x@y>mail</a></p>"
              "<img src=/i.jpg alt=pic width=200><img src=/j.jpg alt=pic2 width=200>");
    check("the page arrives and is shown", v->state == BROWSER_PAGE && !v->loading && (ch & BROWSER_CHANGED_DOC) &&
                                               strcmp(v->doc.title, "Demo") == 0);
    check("it is in the list, and back leads to where it came from",
          v->hist.count == 2 && browser_view_can_back(v) && !browser_view_can_forward(v));
    check("the hint says SECURE for https", strcmp(v->hint, "SECURE") == 0);
    check("it is remembered as a recent page", v->store.nrecent == 1 &&
                                                   strcmp(v->store.recent[0].url, "https://doors.test/") == 0);
    check("pictures are waited for", v->images_loading && v->img[0].state == BROWSER_IMAGE_WAIT &&
                                         strstr(v->status, "pictures") != NULL);
    line(v, &rx, "pixels\t1\t0\t200\t100\t1-0.rgb565", pixels(200, 100));
    check("a picture arrives", v->img[0].state == BROWSER_IMAGE_READY && v->img[0].w == 200 &&
                                   v->img_bytes == 200 * 100 * 2);
    line(v, &rx, "pixels\t1\t0\t200\t100\t1-0.rgb565", pixels(200, 100));
    check("the same picture twice is taken once", v->img_bytes == 200 * 100 * 2);
    line(v, &rx, "pixels\t77\t1\t200\t100\t77-1.rgb565", pixels(200, 100));
    check("a picture for another page is not taken", v->img[1].state == BROWSER_IMAGE_WAIT);
    line(v, &rx, "idle\t1", NULL);
    check("idle: the pictures not sent are given up", !v->images_loading &&
                                                          v->img[1].state == BROWSER_IMAGE_FAILED);

    /* ---- links -------------------------------------------------------------------------- */
    ch = browser_view_follow(v, 1, 0, &cmd);
    check("a mailto: link is not opened, and it says so", cmd.kind == BROWSER_CMD_NONE &&
                                                               strstr(v->status, "mailto:") != NULL);
    browser_view_follow(v, 99, 0, &cmd);
    check("a link that does not exist does nothing", cmd.kind == BROWSER_CMD_NONE);
    browser_view_follow(v, 0, 0, &cmd);
    seq = cmd.seq;
    check("a link opens its address", cmd.kind == BROWSER_CMD_OPEN && strcmp(cmd.url, "https://doors.test/a") == 0);
    check("the old page stays on screen while the new one loads", v->state == BROWSER_PAGE &&
                                                                      strcmp(v->doc.title, "Demo") == 0);
    browser_view_stop(v, &cmd);
    check("STOP: a stop command, the old page stays, the list is unchanged",
          cmd.kind == BROWSER_CMD_STOP && cmd.seq == seq && !v->loading && v->hist.count == 2 &&
              strcmp(v->field, "https://doors.test/") == 0 && strcmp(v->status, "Stopped") == 0);
    page(v, &rx, seq, "https://doors.test/a", 200, 0, "<title>Late</title><p>x</p>");
    check("the page for the stopped load, arriving late, is dropped", strcmp(v->doc.title, "Demo") == 0);
    line(v, &rx, "fail\t2\tdns\t0\thttps://doors.test/a\tlate", NULL);
    check("and so is a late failure", v->state == BROWSER_PAGE);

    /* ---- redirects, back, forward ---------------------------------------------------------- */
    browser_view_open(v, "https://doors.test/redirect", 0, &cmd);
    page(v, &rx, cmd.seq, "https://doors.test/about", 200, 0, "<title>About</title><p>about</p>");
    check("a redirect's final address is what the list and the field hold",
          strcmp(browser_view_current_url(v), "https://doors.test/about") == 0 &&
              strcmp(v->field, "https://doors.test/about") == 0 && v->hist.count == 3);
    browser_view_back(v, 0, &cmd);
    check("BACK asks for the previous page", cmd.kind == BROWSER_CMD_OPEN &&
                                                 strcmp(cmd.url, "https://doors.test/") == 0);
    check("the list moves only when it arrives", v->hist.index == 2 && browser_view_can_forward(v));
    seq = cmd.seq;
    browser_view_back(v, 0, &cmd);
    check("a second BACK while the first loads goes one further: the start page",
          cmd.kind == BROWSER_CMD_STOP && v->state == BROWSER_START && v->hist.index == 0 &&
              browser_view_can_forward(v));
    browser_view_forward(v, 0, &cmd);
    page(v, &rx, cmd.seq, "https://doors.test/", 200, 0, "<title>Demo</title><p>d</p>");
    check("FORWARD from the start page returns to the first page", v->hist.index == 1 &&
                                                                     strcmp(v->doc.title, "Demo") == 0);
    browser_view_forward(v, 0, &cmd);
    page(v, &rx, cmd.seq, "https://doors.test/about", 200, 0, "<title>About</title><p>a</p>");
    check("and FORWARD again to the last", v->hist.index == 2 && !browser_view_can_forward(v));
    browser_view_reload(v, 0, &cmd);
    check("RELOAD asks for the same address", cmd.kind == BROWSER_CMD_OPEN &&
                                                  strcmp(cmd.url, "https://doors.test/about") == 0);
    page(v, &rx, cmd.seq, "https://doors.test/about", 200, 0, "<title>About again</title><p>a</p>");
    check("and replaces the entry, not adds one", v->hist.count == 3 &&
                                                   strcmp(v->hist.e[2].title, "About again") == 0);
    browser_view_home(v, 0, &cmd);
    check("HOME shows the start page locally", cmd.kind == BROWSER_CMD_NONE && v->state == BROWSER_START &&
                                                   v->hist.count == 4);
    browser_view_back(v, 0, &cmd);
    check("and BACK from it loads the last page", cmd.kind == BROWSER_CMD_OPEN &&
                                                   strcmp(cmd.url, "https://doors.test/about") == 0);
    browser_view_stop(v, &cmd);
    check("STOP from the start page leaves the start page", v->state == BROWSER_START && v->hist.index == 3);

    /* ---- errors ------------------------------------------------------------------------ */
    browser_view_go(v, "refused.example", 0, &cmd);
    line(v, &rx, "progress\t0\tconnect\t0", NULL);
    snprintf(text, sizeof(text), "fail\t%d\tconnect\t0\thttps://refused.example/\tConnection refused", cmd.seq);
    line(v, &rx, text, NULL);
    check("connection refused: an error page with words, the detail and the host",
          v->state == BROWSER_ERROR && v->err.fail == WEB_FAIL_CONNECT && strstr(v->err.text, "refused.example") &&
              strcmp(v->err.detail, "Connection refused") == 0 && strcmp(v->hint, "ERROR") == 0);
    check("an address Browser made https offers http:// - as a choice, never on its own",
          strcmp(v->err.alt_url, "http://refused.example/") == 0);
    check("the error page is an entry: BACK leaves it", v->hist.count == 5 && browser_view_can_back(v));
    browser_view_try_insecure(v, 0, &cmd);
    check("the offer taken opens exactly the http:// address", cmd.kind == BROWSER_CMD_OPEN &&
                                                                  strcmp(cmd.url, "http://refused.example/") == 0);
    snprintf(text, sizeof(text), "fail\t%d\ttls\t0\thttps://x.example/\tcertificate", cmd.seq);
    browser_view_go(v, "https://secure.example", 0, &cmd);
    snprintf(text, sizeof(text), "fail\t%d\tconnect\t0\thttps://secure.example/\tConnection refused", cmd.seq);
    line(v, &rx, text, NULL);
    check("an address typed with https:// gets no http:// offer", v->err.alt_url[0] == '\0');
    browser_view_go(v, "doors.test/insecure", 0, &cmd);
    snprintf(text, sizeof(text), "fail\t%d\tinsecure-redirect\t302\thttps://doors.test/insecure\thttp://doors.test",
             cmd.seq);
    line(v, &rx, text, NULL);
    check("an https page redirecting to http is an error, with no offer",
          v->err.fail == WEB_FAIL_INSECURE_REDIRECT && v->err.alt_url[0] == '\0' &&
              strstr(v->err.text, "does not leave https") != NULL);
    browser_view_go(v, "tls.example", 0, &cmd);
    snprintf(text, sizeof(text), "fail\t%d\ttls\t0\thttps://tls.example/\tself-signed certificate", cmd.seq);
    line(v, &rx, text, NULL);
    check("a certificate refused says there is no way to skip the check, and offers no http://",
          v->err.fail == WEB_FAIL_TLS && strstr(v->err.text, "no way to skip") != NULL && v->err.alt_url[0] == '\0');
    browser_view_go(v, "clock.example", 0, &cmd);
    snprintf(text, sizeof(text), "fail\t%d\tclock\t0\thttps://clock.example/\t", cmd.seq);
    line(v, &rx, text, NULL);
    check("an unset clock is said as such", v->err.fail == WEB_FAIL_CLOCK && strstr(v->err.text, "clock") != NULL);
    browser_view_go(v, "offline.example", 0, &cmd);
    snprintf(text, sizeof(text), "fail\t%d\toffline\t0\thttps://offline.example/\t", cmd.seq);
    line(v, &rx, text, NULL);
    check("no network points at Wi-Fi in Settings", v->err.fail == WEB_FAIL_OFFLINE &&
                                                        strstr(v->err.text, "Wi-Fi in Settings") != NULL);
    browser_view_reload(v, 0, &cmd);
    check("reload on an error page tries the same address", cmd.kind == BROWSER_CMD_OPEN &&
                                                                strcmp(cmd.url, "https://offline.example/") == 0);
    browser_view_stop(v, &cmd);

    /* ---- the helper dies ------------------------------------------------------------------ */
    browser_view_go(v, "doors.test/crash", 5000, &cmd);
    ch = browser_view_helper_stopped(v, BROWSER_EXIT_CRASHED, 5000);
    check("a helper crash mid-load is an error page that says what happened",
          v->state == BROWSER_ERROR && v->err.helper && !v->loading && strstr(v->err.text, "crashed") != NULL);
    check("and the helper may be started again", browser_view_may_start_helper(v, 5000));
    for (i = 0; i < 3; i++) {
        browser_view_helper_stopped(v, BROWSER_EXIT_HUNG, 6000 + i);
    }
    check("four failures in a minute: no more restarts", !browser_view_may_start_helper(v, 7000));
    check("until the minute has passed", browser_view_may_start_helper(v, 5000 + 60000));

    /* ---- pictures within the budget ------------------------------------------------------ */
    browser_view_go(v, "doors.test", 0, &cmd);
    {
        char html[2048];
        size_t n = 0;

        for (i = 0; i < 6; i++) {
            n += (size_t)snprintf(html + n, sizeof(html) - n, "<img src=/%d.jpg alt=p%d width=500>", i, i);
        }
        page(v, &rx, cmd.seq, "https://doors.test/", 200, WEB_PAGE_IMAGES, html);
    }
    for (i = 0; i < 6; i++) {
        snprintf(text, sizeof(text), "pixels\t%d\t%d\t560\t560\t%d-%d.rgb565", cmd.seq, i, cmd.seq, i);
        line(v, &rx, text, pixels(560, 560));
    }
    check("pictures past the page's budget are given up, not kept",
          v->img_bytes <= BROWSER_IMAGE_BYTES && v->img[5].state == BROWSER_IMAGE_FAILED &&
              v->img[0].state == BROWSER_IMAGE_READY);
    browser_view_home(v, 0, &cmd);
    check("leaving the page frees its pictures", v->img_bytes == 0 && v->img[0].px == NULL);

    /* ---- http pages and bookmarks, saved ------------------------------------------------- */
    browser_view_go(v, "http://plain.example/", 0, &cmd);
    page(v, &rx, cmd.seq, "http://plain.example/", 200, 0, "<title>Plain</title><p>p</p>");
    check("an http page says NOT SECURE", strcmp(v->hint, "NOT SECURE") == 0);
    browser_view_toggle_bookmark(v);
    check("bookmarked", browser_view_is_bookmark(v) && strcmp(v->status, "Bookmarked") == 0);
    browser_view_go(v, "http://plain.example/404", 0, &cmd);
    page(v, &rx, cmd.seq, "http://plain.example/404", 404, 0, "<title>Not Found</title><p>p</p>");
    check("a 404 page is shown, says HTTP 404, and is not a recent page",
          v->state == BROWSER_PAGE && strstr(v->status, "HTTP 404") != NULL &&
              strcmp(v->store.recent[0].url, "http://plain.example/") == 0);
    check("saved", browser_view_save(v) == 0 && !v->store_dirty);
    {
        struct stat st;

        check("the state file is 0600", stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    }
    browser_view_init(w, path, why, sizeof(why));
    check("a new view reads the bookmark and the recent pages back",
          web_store_is_bookmark(&w->store, "http://plain.example/") && w->store.nrecent >= 2);
    browser_view_free(w);
    {
        FILE *f = fopen(path, "w");

        fputs("garbage\n", f);
        fclose(f);
    }
    browser_view_init(w, path, why, sizeof(why));
    check("a corrupt state file: defaults, a reason, and replaced at the next save",
          w->store.nrecent == 0 && why[0] && w->store_dirty && browser_view_save(w) == 0);
    browser_view_free(w);

    browser_view_free(v);
    web_rx_free(&rx);
    unlink(path);
    rmdir(dir);
    free(v);
    free(w);
    printf("browser_view_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
