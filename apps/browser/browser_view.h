/*
 * What the Browser shows and allows, decided without LVGL: the page on
 * screen, the back/forward list, what the address field says, the status
 * line and the header hint, the error pages and their words, and what to
 * ask the helper next. browser_app.c draws it; browser_session.c feeds it.
 *
 * NAVIGATION. The back/forward list (web_history) changes only when a page
 * or an error for it actually arrives, so a load that is stopped, or that is
 * replaced by another before it answers, leaves the list as it was. A page
 * that failed is an entry like any other, so BACK leaves it. Redirects
 * replace the address of the entry with where the page really came from.
 *
 * STOP ends the load at once on screen: the page that was there stays (or
 * the start page, when there was none), and anything that arrives for the
 * stopped load later is dropped by its sequence number.
 *
 * PICTURES belong to the page on screen and nothing else: at most
 * BROWSER_IMAGE_BYTES in all, freed with the page.
 *
 * Pure C: tests/browser_view_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef BROWSER_VIEW_H
#define BROWSER_VIEW_H

#include "web/web_doc.h"
#include "web/web_history.h"
#include "web/web_proto.h"
#include "web/web_store.h"
#include "web/web_url.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All the pictures of one page together, in the shell. */
#define BROWSER_IMAGE_BYTES (3u * 1024u * 1024u)
#define BROWSER_STATUS_MAX 160
#define BROWSER_ERROR_TEXT_MAX 400
#define BROWSER_HINT_MAX 24
/* How many helper failures in a minute before Browser stops restarting it. */
#define BROWSER_CRASH_LIMIT 4

#define BROWSER_CHANGED_STATE 0x01   /* what the viewport shows (start, page, error) */
#define BROWSER_CHANGED_DOC 0x02     /* a new page document */
#define BROWSER_CHANGED_IMAGES 0x04  /* a picture arrived or was given up */
#define BROWSER_CHANGED_STATUS 0x08  /* status line, hint, buttons */
#define BROWSER_CHANGED_FIELD 0x10   /* the address field's text */
#define BROWSER_CHANGED_START 0x20   /* the start page's lists */
/* The helper was ended because it did not answer a STOP while a newer page
 * was already asked for: start a new one and send browser_view_resend(). */
#define BROWSER_CHANGED_RESTART 0x40

enum browser_state {
    BROWSER_START = 0,
    BROWSER_PAGE,
    BROWSER_ERROR
};

enum browser_exit {
    BROWSER_EXIT_NORMAL = 0,
    BROWSER_EXIT_START,     /* the helper could not be started */
    BROWSER_EXIT_CRASHED,
    BROWSER_EXIT_HUNG,
    BROWSER_EXIT_PROTOCOL
};

enum browser_image_state {
    BROWSER_IMAGE_NONE = 0, /* not asked for (or no helper directory) */
    BROWSER_IMAGE_WAIT,
    BROWSER_IMAGE_READY,
    BROWSER_IMAGE_FAILED
};

struct browser_image {
    enum browser_image_state state;
    uint16_t *px;           /* w * h RGB565, owned */
    int w;
    int h;
    char why[48];
};

enum browser_nav {
    BROWSER_NAV_NEW = 0,
    BROWSER_NAV_BACK,
    BROWSER_NAV_FORWARD,
    BROWSER_NAV_RELOAD
};

enum browser_cmd_kind {
    BROWSER_CMD_NONE = 0,
    BROWSER_CMD_OPEN,
    BROWSER_CMD_STOP
};

/* What the app must tell the helper. */
struct browser_cmd {
    enum browser_cmd_kind kind;
    int seq;
    int max_width;
    bool images;
    char url[WEB_URL_MAX];
};

struct browser_error {
    enum web_fail fail;
    bool helper;                    /* the helper failed, not the page */
    char title[64];
    char text[BROWSER_ERROR_TEXT_MAX];
    char detail[WEB_FAIL_TEXT_MAX];
    char url[WEB_URL_MAX];
    char alt_url[WEB_URL_MAX];      /* "try http:// (not secure)", only when asked for by the person */
};

struct browser_view {
    struct web_store store;
    char store_path[512];           /* "" : web_store_path() */
    bool store_dirty;
    bool store_aside;               /* the file was not understood: kept as <file>.bad at the next save */
    bool store_unreadable;          /* the file could not be read: never written over this run */
    struct web_history hist;

    enum browser_state state;
    struct web_doc doc;             /* valid in BROWSER_PAGE */
    char doc_url[WEB_URL_MAX];
    unsigned doc_flags;
    int doc_http;
    int doc_seq;
    struct browser_image img[WEB_DOC_IMAGE_MAX];
    size_t img_bytes;
    bool images_loading;
    struct browser_error err;       /* valid in BROWSER_ERROR */

    bool loading;
    int seq;                        /* the load in flight, or the last one */
    int next_seq;
    enum browser_nav nav;
    int nav_target;                 /* the history index BACK/FORWARD will land on */
    bool nav_auto_https;            /* the address was typed without a scheme */
    char loading_url[WEB_URL_MAX];
    long bytes;
    char stage[16];
    int64_t load_started;

    int max_width;                  /* what pictures are scaled to, from the app */
    bool images_wanted;
    char features[32];              /* what the helper said it can do */
    bool helper_up;
    int crashes;                    /* helper failures in the current minute */
    int64_t crash_window;

    char field[WEB_URL_MAX];        /* what the address field shows */
    char status[BROWSER_STATUS_MAX];
    const char *hint;               /* the header hint: a literal */
    char note[BROWSER_STATUS_MAX];  /* a one-off word in the status line ("mailto: links ...") */
};

/* Load the remembered state (store_path NULL: the default file) and show the
 * start page. why gets a reason when the file was not usable. */
void browser_view_init(struct browser_view *v, const char *store_path, char *why, size_t whylen);
void browser_view_free(struct browser_view *v);

/* The address field's text, from the person. On success cmd says what to
 * do; on a malformed address the view shows an error page and cmd is NONE. */
unsigned browser_view_go(struct browser_view *v, const char *typed, int64_t now, struct browser_cmd *cmd);
/* A link of the page on screen, by index. */
unsigned browser_view_follow(struct browser_view *v, int link, int64_t now, struct browser_cmd *cmd);
/* An address from the start page (a bookmark or a recent page). */
unsigned browser_view_open(struct browser_view *v, const char *url, int64_t now, struct browser_cmd *cmd);
unsigned browser_view_back(struct browser_view *v, int64_t now, struct browser_cmd *cmd);
unsigned browser_view_forward(struct browser_view *v, int64_t now, struct browser_cmd *cmd);
unsigned browser_view_reload(struct browser_view *v, int64_t now, struct browser_cmd *cmd);
unsigned browser_view_stop(struct browser_view *v, struct browser_cmd *cmd);
unsigned browser_view_home(struct browser_view *v, int64_t now, struct browser_cmd *cmd);
/* The error page's second button: the http:// address, asked for by the person. */
unsigned browser_view_try_insecure(struct browser_view *v, int64_t now, struct browser_cmd *cmd);
unsigned browser_view_toggle_bookmark(struct browser_view *v);
unsigned browser_view_clear_recent(struct browser_view *v);

bool browser_view_can_back(const struct browser_view *v);
bool browser_view_can_forward(const struct browser_view *v);
bool browser_view_is_bookmark(const struct browser_view *v);
/* The address of what is on screen ("about:home" on the start page). */
const char *browser_view_current_url(const struct browser_view *v);

/* From the session. A page arriving is taken from rx. px (owned, may be
 * NULL) comes with WEB_RX_PIXELS. */
unsigned browser_view_apply(struct browser_view *v, const struct web_rx_msg *m, struct web_rx *rx, uint16_t *px,
                            int64_t now);
/* The helper has gone; why and when. */
unsigned browser_view_helper_stopped(struct browser_view *v, enum browser_exit why, int64_t now);
/* The load in flight, again, for a fresh helper. */
void browser_view_resend(const struct browser_view *v, struct browser_cmd *cmd);
/* Whether the helper may be (re)started for the next page. */
bool browser_view_may_start_helper(const struct browser_view *v, int64_t now);

/* Save the remembered state if it changed. 0, or -1 (logged by the caller).
 * A file that could not be read at init is never written over: the changes
 * stay in memory (store_dirty stays set) and 0 is returned. A file that was
 * not understood is renamed to <file>.bad before the first save. */
int browser_view_save(struct browser_view *v);

/* The words for an exit reason and for a failure. */
const char *browser_exit_text(enum browser_exit e);

#endif
