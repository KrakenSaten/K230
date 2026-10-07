/*
 * The Browser's back/forward list: at most WEB_HISTORY_MAX entries. Going
 * somewhere new drops everything forward of the current entry; past the
 * limit the oldest entry goes. Nothing here is written to disk.
 *
 * Pure C: tests/web_history_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_HISTORY_H
#define POCKETOS_WEB_HISTORY_H

#include "web/web_doc.h"
#include "web/web_url.h"

#include <stdbool.h>

#define WEB_HISTORY_MAX 32

struct web_history_entry {
    char *url;                  /* heap; never NULL in a live entry */
    char title[WEB_TITLE_MAX];
};

struct web_history {
    struct web_history_entry e[WEB_HISTORY_MAX];
    int count;
    int index;                  /* the current entry, -1 when empty */
};

void web_history_init(struct web_history *h);
void web_history_free(struct web_history *h);

/* A new entry after the current one. Returns 0, or -1 (out of memory: the
 * list is unchanged). */
int web_history_push(struct web_history *h, const char *url, const char *title);
/* Change the current entry (a redirect's final address, a title that arrived). */
int web_history_replace(struct web_history *h, const char *url, const char *title);

bool web_history_can_back(const struct web_history *h);
bool web_history_can_forward(const struct web_history *h);
/* Move and return the entry moved to, or NULL when there is none. */
const struct web_history_entry *web_history_back(struct web_history *h);
const struct web_history_entry *web_history_forward(struct web_history *h);
const struct web_history_entry *web_history_current(const struct web_history *h);

#endif
