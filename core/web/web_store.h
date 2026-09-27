/*
 * What the Browser remembers between runs: the home page, the last page
 * opened, the recent pages and the bookmarks. Nothing else - no cookies, no
 * page content, no form data, no passwords.
 *
 * FILE. $POCKETOS_STATE_DIR/browser/state (/var/lib/pocketos/browser/state),
 * a versioned text file:
 *
 *   doors-browser-state 1
 *   home<TAB><url>
 *   last<TAB><url>
 *   recent<TAB><url><TAB><title>       newest first, at most WEB_STORE_RECENT_MAX
 *   bookmark<TAB><url><TAB><title>     in the order added, at most WEB_STORE_BOOKMARK_MAX
 *
 * The directory is 0700 and the file 0600: a list of pages someone visited
 * is private. It is written whole to a temporary file, synced and renamed
 * over the old one, and never through a symbolic link.
 *
 * READING. A missing file gives the defaults (about:home, two neutral
 * bookmarks). A file with another first line, one that is too large, or a
 * line that is not one of the above is not trusted: the defaults are used,
 * the reason is given, and the next save replaces the file. Every address is
 * checked with web_url_parse, so a planted file:// or javascript: address
 * cannot come back as a bookmark.
 *
 * Pure C: tests/web_store_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_WEB_STORE_H
#define POCKETOS_WEB_STORE_H

#include "web/web_doc.h"

#include <stdbool.h>
#include <stddef.h>

#define WEB_STORE_VERSION 1
#define WEB_STORE_URL_MAX 1024      /* longer addresses are not remembered */
#define WEB_STORE_RECENT_MAX 12
#define WEB_STORE_BOOKMARK_MAX 24
#define WEB_STORE_FILE_MAX (64u * 1024u)

struct web_place {
    char url[WEB_STORE_URL_MAX];
    char title[WEB_TITLE_MAX];
};

struct web_store {
    char home[WEB_STORE_URL_MAX];
    char last[WEB_STORE_URL_MAX];
    struct web_place recent[WEB_STORE_RECENT_MAX];
    int nrecent;
    struct web_place bookmark[WEB_STORE_BOOKMARK_MAX];
    int nbookmark;
};

enum web_store_load {
    WEB_STORE_LOADED = 0,
    WEB_STORE_MISSING,          /* defaults */
    WEB_STORE_CORRUPT           /* defaults; why says what was wrong */
};

void web_store_defaults(struct web_store *s);

/* path NULL: web_store_path(). */
enum web_store_load web_store_load(struct web_store *s, const char *path, char *why, size_t whylen);
/* 0, or -1 with errno. */
int web_store_save(const struct web_store *s, const char *path);

/* $POCKETOS_STATE_DIR/browser/state into buf. */
const char *web_store_path(char *buf, size_t len);

/* Remember a page opened: to the front of recent (once), and as last. */
void web_store_visit(struct web_store *s, const char *url, const char *title);
void web_store_clear_recent(struct web_store *s);
bool web_store_is_bookmark(const struct web_store *s, const char *url);
/* Add or remove; returns true when url is a bookmark afterwards. */
bool web_store_toggle_bookmark(struct web_store *s, const char *url, const char *title);

#endif
