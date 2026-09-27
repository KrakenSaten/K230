/*
 * The Browser's presentation logic. See browser_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "browser_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOME_URL "about:home"
#define CRASH_WINDOW_MS 60000

/* ---- small things ------------------------------------------------------------------ */

/* snprintf into dst, cut at a character boundary rather than inside one. */
static void put(char *dst, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void put(char *dst, size_t cap, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    web_copy(dst, cap, tmp);
}

static void set_field(struct browser_view *v, const char *url)
{
    web_copy(v->field, sizeof(v->field), strcmp(url, HOME_URL) == 0 ? "" : url);
}

static const char *host_of(const char *url, char *buf, size_t len)
{
    struct web_url u;

    if (web_url_parse(url, &u) == WEB_URL_OK && u.host[0]) {
        web_copy(buf, len, u.host);
    } else {
        web_copy(buf, len, url);
    }
    return buf;
}

static void free_images(struct browser_view *v)
{
    size_t i;

    for (i = 0; i < WEB_DOC_IMAGE_MAX; i++) {
        free(v->img[i].px);
    }
    memset(v->img, 0, sizeof(v->img));
    v->img_bytes = 0;
    v->images_loading = false;
}

static void drop_page(struct browser_view *v)
{
    web_doc_free(&v->doc);
    free_images(v);
    v->doc_url[0] = '\0';
    v->doc_flags = 0;
    v->doc_http = 0;
}

static unsigned refresh_status(struct browser_view *v)
{
    char host[WEB_HOST_MAX];

    if (v->loading) {
        if (v->bytes > 0) {
            put(v->status, sizeof(v->status), "Loading %s \xe2\x80\x94 %ld KB",
                     host_of(v->loading_url, host, sizeof(host)), (v->bytes + 1023) / 1024);
        } else {
            put(v->status, sizeof(v->status), "Connecting to %s\xe2\x80\xa6",
                     host_of(v->loading_url, host, sizeof(host)));
        }
        v->hint = "LOADING";
    } else if (v->note[0]) {
        web_copy(v->status, sizeof(v->status), v->note);
        v->hint = v->state == BROWSER_ERROR ? "ERROR" : v->state == BROWSER_START ? "" :
                  (v->doc_flags & WEB_PAGE_SECURE) ? "SECURE" : "NOT SECURE";
    } else if (v->state == BROWSER_START) {
        put(v->status, sizeof(v->status), "Start page");
        v->hint = "";
    } else if (v->state == BROWSER_ERROR) {
        web_copy(v->status, sizeof(v->status), v->err.title);
        v->hint = "ERROR";
    } else {
        const char *title = v->doc.title[0] ? v->doc.title : host_of(v->doc_url, host, sizeof(host));

        if (v->doc_http >= 400) {
            put(v->status, sizeof(v->status), "HTTP %d \xe2\x80\x94 %s", v->doc_http, title);
        } else if (v->doc.truncated) {
            put(v->status, sizeof(v->status), "%s (page cut: too long)", title);
        } else if (v->images_loading) {
            put(v->status, sizeof(v->status), "%s \xe2\x80\x94 loading pictures", title);
        } else {
            web_copy(v->status, sizeof(v->status), title);
        }
        v->hint = (v->doc_flags & WEB_PAGE_SECURE) ? "SECURE" : "NOT SECURE";
    }
    return BROWSER_CHANGED_STATUS;
}

static void note(struct browser_view *v, const char *text)
{
    web_copy(v->note, sizeof(v->note), text ? text : "");
}

/* ---- life ----------------------------------------------------------------------------- */

void browser_view_init(struct browser_view *v, const char *store_path, char *why, size_t whylen)
{
    memset(v, 0, sizeof(*v));
    web_doc_init(&v->doc);
    web_history_init(&v->hist);
    if (store_path) {
        web_copy(v->store_path, sizeof(v->store_path), store_path);
    }
    if (web_store_load(&v->store, v->store_path[0] ? v->store_path : NULL, why, whylen) == WEB_STORE_CORRUPT) {
        v->store_dirty = true; /* replaced at the next save */
    }
    v->max_width = 528;
    v->images_wanted = true;
    v->next_seq = 1;
    web_history_push(&v->hist, HOME_URL, "Start");
    v->state = BROWSER_START;
    set_field(v, HOME_URL);
    refresh_status(v);
}

void browser_view_free(struct browser_view *v)
{
    drop_page(v);
    web_history_free(&v->hist);
}

int browser_view_save(struct browser_view *v)
{
    if (!v->store_dirty) {
        return 0;
    }
    if (web_store_save(&v->store, v->store_path[0] ? v->store_path : NULL) != 0) {
        return -1;
    }
    v->store_dirty = false;
    return 0;
}

/* ---- going somewhere -------------------------------------------------------------------- */

static unsigned show_start(struct browser_view *v)
{
    drop_page(v);
    v->state = BROWSER_START;
    set_field(v, HOME_URL);
    return BROWSER_CHANGED_STATE | BROWSER_CHANGED_FIELD | BROWSER_CHANGED_START | refresh_status(v);
}

/* Start loading url (already checked) as nav. */
static unsigned start_load(struct browser_view *v, const char *url, enum browser_nav nav, int target,
                           bool auto_https, int64_t now, struct browser_cmd *cmd)
{
    unsigned changed = 0;

    memset(cmd, 0, sizeof(*cmd));
    note(v, NULL);
    if (strncmp(url, "about:", 6) == 0) {
        /* The Browser's own pages never reach the helper. about:blank is the
         * start page too: there is nothing else to show for it. */
        if (v->loading) {
            cmd->kind = BROWSER_CMD_STOP;
            cmd->seq = v->seq;
            v->loading = false;
        }
        if (nav == BROWSER_NAV_NEW) {
            const struct web_history_entry *cur = web_history_current(&v->hist);

            if (!cur || strcmp(cur->url, HOME_URL) != 0) {
                web_history_push(&v->hist, HOME_URL, "Start");
            }
        } else if (nav == BROWSER_NAV_BACK || nav == BROWSER_NAV_FORWARD) {
            v->hist.index = target;
        }
        return show_start(v);
    }
    v->loading = true;
    v->seq = v->next_seq++;
    v->nav = nav;
    v->nav_target = target;
    v->nav_auto_https = auto_https;
    v->bytes = 0;
    v->stage[0] = '\0';
    v->load_started = now;
    web_copy(v->loading_url, sizeof(v->loading_url), url);
    set_field(v, url);
    cmd->kind = BROWSER_CMD_OPEN;
    cmd->seq = v->seq;
    cmd->max_width = v->max_width;
    cmd->images = v->images_wanted;
    web_copy(cmd->url, sizeof(cmd->url), url);
    changed |= BROWSER_CHANGED_FIELD | refresh_status(v);
    return changed;
}

static unsigned url_error(struct browser_view *v, const char *typed, enum web_url_err e)
{
    drop_page(v);
    memset(&v->err, 0, sizeof(v->err));
    v->err.fail = WEB_FAIL_URL;
    web_copy(v->err.title, sizeof(v->err.title), "Not an address");
    web_copy(v->err.text, sizeof(v->err.text), web_url_err_text(e));
    web_copy(v->err.url, sizeof(v->err.url), typed);
    v->state = BROWSER_ERROR;
    note(v, NULL);
    return BROWSER_CHANGED_STATE | refresh_status(v);
}

unsigned browser_view_go(struct browser_view *v, const char *typed, int64_t now, struct browser_cmd *cmd)
{
    struct web_url u;
    enum web_url_err e = web_url_from_input(typed, &u);
    char url[WEB_URL_MAX];
    size_t sl;

    memset(cmd, 0, sizeof(*cmd));
    if (e != WEB_URL_OK || web_url_format(&u, url, sizeof(url)) < 0) {
        if (v->loading) {
            cmd->kind = BROWSER_CMD_STOP;
            cmd->seq = v->seq;
            v->loading = false;
        }
        return url_error(v, typed, e != WEB_URL_OK ? e : WEB_URL_TOO_LONG);
    }
    /* Did the person write the scheme? Only then is https not our choice. */
    sl = strspn(typed, " \t");
    return start_load(v, url, BROWSER_NAV_NEW, -1,
                      strncmp(typed + sl, "https://", 8) != 0 && strncmp(typed + sl, "HTTPS://", 8) != 0 &&
                          u.scheme == WEB_SCHEME_HTTPS,
                      now, cmd);
}

unsigned browser_view_open(struct browser_view *v, const char *url, int64_t now, struct browser_cmd *cmd)
{
    struct web_url u;
    char abs[WEB_URL_MAX];

    memset(cmd, 0, sizeof(*cmd));
    if (!url || web_url_parse(url, &u) != WEB_URL_OK || web_url_format(&u, abs, sizeof(abs)) < 0) {
        return url_error(v, url ? url : "", WEB_URL_HOST);
    }
    return start_load(v, abs, BROWSER_NAV_NEW, -1, false, now, cmd);
}

unsigned browser_view_follow(struct browser_view *v, int link, int64_t now, struct browser_cmd *cmd)
{
    size_t n;
    const char *u;
    char url[WEB_URL_MAX];

    memset(cmd, 0, sizeof(*cmd));
    if (v->state != BROWSER_PAGE || !(u = web_doc_link_url(&v->doc, link, &n)) || n >= sizeof(url)) {
        return 0;
    }
    memcpy(url, u, n);
    url[n] = '\0';
    if (!v->doc.links[link].supported) {
        char words[BROWSER_STATUS_MAX];

        put(words, sizeof(words), "%s links cannot be opened in Browser", url[0] ? url : "These");
        note(v, words);
        return refresh_status(v);
    }
    return browser_view_open(v, url, now, cmd);
}

/* The index BACK and FORWARD start from: where a BACK or FORWARD in flight
 * is going, or where the list is. */
static int base_index(const struct browser_view *v)
{
    if (v->loading && (v->nav == BROWSER_NAV_BACK || v->nav == BROWSER_NAV_FORWARD)) {
        return v->nav_target;
    }
    return v->hist.index;
}

bool browser_view_can_back(const struct browser_view *v)
{
    return base_index(v) > 0;
}

bool browser_view_can_forward(const struct browser_view *v)
{
    int b = base_index(v);

    return b >= 0 && b + 1 < v->hist.count;
}

unsigned browser_view_back(struct browser_view *v, int64_t now, struct browser_cmd *cmd)
{
    int t = base_index(v) - 1;

    memset(cmd, 0, sizeof(*cmd));
    if (t < 0 || t >= v->hist.count) {
        return 0;
    }
    return start_load(v, v->hist.e[t].url, BROWSER_NAV_BACK, t, false, now, cmd);
}

unsigned browser_view_forward(struct browser_view *v, int64_t now, struct browser_cmd *cmd)
{
    int t = base_index(v) + 1;

    memset(cmd, 0, sizeof(*cmd));
    if (t < 1 || t >= v->hist.count) {
        return 0;
    }
    return start_load(v, v->hist.e[t].url, BROWSER_NAV_FORWARD, t, false, now, cmd);
}

const char *browser_view_current_url(const struct browser_view *v)
{
    const struct web_history_entry *e = web_history_current(&v->hist);

    return e ? e->url : HOME_URL;
}

unsigned browser_view_reload(struct browser_view *v, int64_t now, struct browser_cmd *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    if (v->state == BROWSER_ERROR && v->err.fail == WEB_FAIL_URL) {
        return 0; /* nothing to load again */
    }
    if (v->state == BROWSER_START) {
        return show_start(v);
    }
    return start_load(v, v->state == BROWSER_ERROR && v->err.url[0] ? v->err.url : browser_view_current_url(v),
                      BROWSER_NAV_RELOAD, v->hist.index, v->state == BROWSER_ERROR && v->nav_auto_https, now,
                      cmd);
}

unsigned browser_view_stop(struct browser_view *v, struct browser_cmd *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    if (v->loading) {
        cmd->kind = BROWSER_CMD_STOP;
        cmd->seq = v->seq;
        v->loading = false;
        set_field(v, browser_view_current_url(v));
        note(v, "Stopped");
        return BROWSER_CHANGED_FIELD | refresh_status(v);
    }
    if (v->images_loading) {
        cmd->kind = BROWSER_CMD_STOP;
        cmd->seq = v->doc_seq;
        v->images_loading = false;
        return BROWSER_CHANGED_IMAGES | refresh_status(v);
    }
    return 0;
}

unsigned browser_view_home(struct browser_view *v, int64_t now, struct browser_cmd *cmd)
{
    return browser_view_open(v, v->store.home[0] ? v->store.home : HOME_URL, now, cmd);
}

unsigned browser_view_try_insecure(struct browser_view *v, int64_t now, struct browser_cmd *cmd)
{
    char url[WEB_URL_MAX];

    memset(cmd, 0, sizeof(*cmd));
    if (v->state != BROWSER_ERROR || !v->err.alt_url[0]) {
        return 0;
    }
    web_copy(url, sizeof(url), v->err.alt_url);
    return browser_view_open(v, url, now, cmd);
}

bool browser_view_is_bookmark(const struct browser_view *v)
{
    return v->state == BROWSER_PAGE && web_store_is_bookmark(&v->store, v->doc_url);
}

unsigned browser_view_toggle_bookmark(struct browser_view *v)
{
    bool on;

    if (v->state != BROWSER_PAGE) {
        return 0;
    }
    if (!web_store_is_bookmark(&v->store, v->doc_url) && v->store.nbookmark >= WEB_STORE_BOOKMARK_MAX) {
        note(v, "Bookmarks are full: remove one first");
        return refresh_status(v);
    }
    on = web_store_toggle_bookmark(&v->store, v->doc_url, v->doc.title);
    v->store_dirty = true;
    note(v, on ? "Bookmarked" : "Bookmark removed");
    return BROWSER_CHANGED_START | refresh_status(v);
}

unsigned browser_view_clear_recent(struct browser_view *v)
{
    web_store_clear_recent(&v->store);
    v->store_dirty = true;
    return BROWSER_CHANGED_START;
}

/* ---- what arrives ------------------------------------------------------------------------- */

/* The load in flight has an answer: record it in the list. */
static void commit(struct browser_view *v, const char *url, const char *title)
{
    switch (v->nav) {
    case BROWSER_NAV_NEW:
        web_history_push(&v->hist, url, title);
        break;
    case BROWSER_NAV_BACK:
    case BROWSER_NAV_FORWARD:
        if (v->nav_target >= 0 && v->nav_target < v->hist.count) {
            v->hist.index = v->nav_target;
        }
        web_history_replace(&v->hist, url, title);
        break;
    case BROWSER_NAV_RELOAD:
        web_history_replace(&v->hist, url, title);
        break;
    }
    v->loading = false;
}

static void error_words(struct browser_error *e, const char *host)
{
    static const struct {
        const char *title;
        const char *text;
    } words[WEB_FAIL_COUNT] = {
        [WEB_FAIL_URL] = { "Not an address", "Browser cannot open that address." },
        [WEB_FAIL_OFFLINE] = { "No network",
                               "This unit has no network connection. Connect to Wi-Fi in Settings, or plug in "
                               "Ethernet, then try again." },
        [WEB_FAIL_DNS] = { "Server not found",
                           "Browser could not find %s. Check the address. If every address fails, the network "
                           "may be down or have no name server." },
        [WEB_FAIL_CONNECT] = { "Could not connect",
                               "%s did not accept the connection. The server may be down, or not reachable from "
                               "this network." },
        [WEB_FAIL_TIMEOUT] = { "No answer in time",
                               "%s did not answer in time. The network may be slow or the connection lost." },
        [WEB_FAIL_TLS] = { "Not a trusted connection",
                           "The certificate of %s could not be verified, so Browser did not open the page. "
                           "Browser has no way to skip this check." },
        [WEB_FAIL_CLOCK] = { "Clock not set",
                             "The device clock is not set yet, so no certificate can be checked. Once the "
                             "network has set the clock, try again." },
        [WEB_FAIL_REDIRECTS] = { "Too many redirects", "%s kept sending Browser somewhere else." },
        [WEB_FAIL_INSECURE_REDIRECT] = { "Insecure redirect not followed",
                                         "This secure page sent Browser to an insecure http:// address. Browser "
                                         "does not leave https for http on its own." },
        [WEB_FAIL_BAD_REDIRECT] = { "Redirect not followed",
                                    "%s sent Browser to an address it does not open." },
        [WEB_FAIL_TYPE] = { "Cannot show this", "This is not a web page or a picture Browser can show." },
        [WEB_FAIL_RESPONSE] = { "Unreadable answer", "%s sent an answer Browser could not read." },
        [WEB_FAIL_STOPPED] = { "Stopped", "Loading was stopped." },
        [WEB_FAIL_NO_NETWORK_BUILD] = { "No web access in this build",
                                        "This build of Browser was made without its HTTP client." },
        [WEB_FAIL_INTERNAL] = { "Something went wrong", "Browser could not load this page." },
    };
    int f = e->fail >= 0 && e->fail < WEB_FAIL_COUNT ? e->fail : WEB_FAIL_INTERNAL;

    web_copy(e->title, sizeof(e->title), words[f].title);
    put(e->text, sizeof(e->text), words[f].text, host && *host ? host : "The server");
}

static unsigned show_error(struct browser_view *v, enum web_fail f, const char *url, const char *detail,
                           bool helper)
{
    char host[WEB_HOST_MAX];

    drop_page(v);
    memset(&v->err, 0, sizeof(v->err));
    v->err.fail = f;
    v->err.helper = helper;
    web_copy(v->err.url, sizeof(v->err.url), url);
    web_copy(v->err.detail, sizeof(v->err.detail), detail ? detail : "");
    error_words(&v->err, host_of(url, host, sizeof(host)));
    /* An address typed without a scheme was made https by Browser, not by the
     * person; when the server is not there on https, offer - never take -
     * the http:// one. */
    /* Never after a certificate failure: that would invite going round the
     * check, not past a server that has no https. */
    if (v->nav_auto_https && (f == WEB_FAIL_CONNECT || f == WEB_FAIL_TIMEOUT || f == WEB_FAIL_RESPONSE)) {
        struct web_url u;

        if (web_url_parse(url, &u) == WEB_URL_OK && u.scheme == WEB_SCHEME_HTTPS) {
            u.scheme = WEB_SCHEME_HTTP;
            if (web_url_format(&u, v->err.alt_url, sizeof(v->err.alt_url)) < 0) {
                v->err.alt_url[0] = '\0';
            }
        }
    }
    v->state = BROWSER_ERROR;
    return BROWSER_CHANGED_STATE | BROWSER_CHANGED_DOC;
}

static unsigned page_arrived(struct browser_view *v, const struct web_rx_msg *m, struct web_rx *rx)
{
    size_t i;

    drop_page(v);
    web_rx_take(rx, &v->doc);
    web_copy(v->doc_url, sizeof(v->doc_url), m->url);
    v->doc_flags = m->flags;
    v->doc_http = m->http;
    v->doc_seq = m->seq;
    v->images_loading = (m->flags & WEB_PAGE_IMAGES) != 0;
    for (i = 0; i < v->doc.nimages; i++) {
        v->img[i].state = v->images_loading ? BROWSER_IMAGE_WAIT : BROWSER_IMAGE_NONE;
    }
    commit(v, v->doc_url, v->doc.title[0] ? v->doc.title : v->doc_url);
    if (m->http < 400) {
        web_store_visit(&v->store, v->doc_url, v->doc.title);
        v->store_dirty = true;
    }
    v->state = BROWSER_PAGE;
    set_field(v, v->doc_url);
    note(v, NULL);
    return BROWSER_CHANGED_STATE | BROWSER_CHANGED_DOC | BROWSER_CHANGED_FIELD | BROWSER_CHANGED_START |
           refresh_status(v);
}

unsigned browser_view_apply(struct browser_view *v, const struct web_rx_msg *m, struct web_rx *rx, uint16_t *px,
                            int64_t now)
{
    unsigned changed = 0;

    (void)now;
    switch (m->kind) {
    case WEB_RX_HELLO:
        web_copy(v->features, sizeof(v->features), m->features);
        v->helper_up = true;
        break;
    case WEB_RX_PROGRESS:
        if (v->loading && m->seq == v->seq && strcmp(m->stage, "images") != 0) {
            v->bytes = m->bytes;
            web_copy(v->stage, sizeof(v->stage), m->stage);
            changed |= refresh_status(v);
        }
        break;
    case WEB_RX_PAGE:
        if (v->loading && m->seq == v->seq) {
            changed |= page_arrived(v, m, rx);
        } else {
            struct web_doc stale;

            web_doc_init(&stale);
            web_rx_take(rx, &stale); /* a page for a load that was stopped: dropped */
            web_doc_free(&stale);
        }
        break;
    case WEB_RX_FAIL:
        if (v->loading && m->seq == v->seq && m->fail != WEB_FAIL_STOPPED) {
            const char *url = m->url[0] ? m->url : v->loading_url;
            char keep[WEB_URL_MAX];

            web_copy(keep, sizeof(keep), url);
            commit(v, keep, "Error");
            changed |= show_error(v, m->fail, keep, m->text, false);
            set_field(v, keep);
            note(v, NULL);
            changed |= BROWSER_CHANGED_FIELD | refresh_status(v);
        }
        break;
    case WEB_RX_PIXELS:
        if (v->state == BROWSER_PAGE && m->seq == v->doc_seq && m->image >= 0 &&
            (size_t)m->image < v->doc.nimages && v->img[m->image].state == BROWSER_IMAGE_WAIT && px) {
            size_t bytes = (size_t)m->w * (size_t)m->h * sizeof(uint16_t);
            struct browser_image *im = &v->img[m->image];

            if (v->img_bytes + bytes > BROWSER_IMAGE_BYTES) {
                im->state = BROWSER_IMAGE_FAILED;
                web_copy(im->why, sizeof(im->why), "not shown: the page's pictures are too large");
            } else {
                im->px = px;
                px = NULL;
                im->w = m->w;
                im->h = m->h;
                im->state = BROWSER_IMAGE_READY;
                v->img_bytes += bytes;
            }
            changed |= BROWSER_CHANGED_IMAGES;
        }
        break;
    case WEB_RX_NOPIXELS:
        if (v->state == BROWSER_PAGE && m->seq == v->doc_seq && m->image >= 0 &&
            (size_t)m->image < v->doc.nimages && v->img[m->image].state == BROWSER_IMAGE_WAIT) {
            v->img[m->image].state = BROWSER_IMAGE_FAILED;
            web_copy(v->img[m->image].why, sizeof(v->img[m->image].why), m->text);
            changed |= BROWSER_CHANGED_IMAGES;
        }
        break;
    case WEB_RX_IDLE:
        if (v->images_loading && m->seq == v->doc_seq) {
            size_t i;

            v->images_loading = false;
            for (i = 0; i < v->doc.nimages; i++) {
                if (v->img[i].state == BROWSER_IMAGE_WAIT) {
                    v->img[i].state = BROWSER_IMAGE_FAILED;
                    web_copy(v->img[i].why, sizeof(v->img[i].why), "not loaded");
                }
            }
            changed |= BROWSER_CHANGED_IMAGES | refresh_status(v);
        }
        break;
    default:
        break;
    }
    free(px);
    return changed;
}

void browser_view_resend(const struct browser_view *v, struct browser_cmd *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    if (!v->loading) {
        return;
    }
    cmd->kind = BROWSER_CMD_OPEN;
    cmd->seq = v->seq;
    cmd->max_width = v->max_width;
    cmd->images = v->images_wanted;
    web_copy(cmd->url, sizeof(cmd->url), v->loading_url);
}

bool browser_view_may_start_helper(const struct browser_view *v, int64_t now)
{
    return v->crashes < BROWSER_CRASH_LIMIT || now - v->crash_window >= CRASH_WINDOW_MS;
}

const char *browser_exit_text(enum browser_exit e)
{
    switch (e) {
    case BROWSER_EXIT_NORMAL:
        return "the page loader stopped";
    case BROWSER_EXIT_START:
        return "the page loader (pos-browser) could not be started";
    case BROWSER_EXIT_CRASHED:
        return "the page loader crashed";
    case BROWSER_EXIT_HUNG:
        return "the page loader stopped answering and was ended";
    case BROWSER_EXIT_PROTOCOL:
        return "the page loader sent something Browser does not understand";
    }
    return "the page loader stopped";
}

unsigned browser_view_helper_stopped(struct browser_view *v, enum browser_exit why, int64_t now)
{
    unsigned changed = 0;

    v->helper_up = false;
    if (why != BROWSER_EXIT_NORMAL) {
        if (now - v->crash_window >= CRASH_WINDOW_MS) {
            v->crash_window = now;
            v->crashes = 0;
        }
        v->crashes++;
    }
    if (v->loading) {
        char keep[WEB_URL_MAX];

        web_copy(keep, sizeof(keep), v->loading_url);
        commit(v, keep, "Error");
        changed |= show_error(v, WEB_FAIL_INTERNAL, keep, browser_exit_text(why), true);
        web_copy(v->err.title, sizeof(v->err.title), "The page could not be loaded");
        put(v->err.text, sizeof(v->err.text), "Browser's page loader stopped while loading this page (%s). "
                 "Try again; it starts afresh.", browser_exit_text(why));
        set_field(v, keep);
        changed |= BROWSER_CHANGED_FIELD;
    }
    if (v->images_loading) {
        size_t i;

        v->images_loading = false;
        for (i = 0; i < v->doc.nimages; i++) {
            if (v->img[i].state == BROWSER_IMAGE_WAIT) {
                v->img[i].state = BROWSER_IMAGE_FAILED;
                web_copy(v->img[i].why, sizeof(v->img[i].why), "not loaded");
            }
        }
        changed |= BROWSER_CHANGED_IMAGES;
    }
    return changed | refresh_status(v);
}
