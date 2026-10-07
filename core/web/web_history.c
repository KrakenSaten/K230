/*
 * The back/forward list. See web_history.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "web/web_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void web_history_init(struct web_history *h)
{
    memset(h, 0, sizeof(*h));
    h->index = -1;
}

void web_history_free(struct web_history *h)
{
    int i;

    for (i = 0; i < h->count; i++) {
        free(h->e[i].url);
    }
    web_history_init(h);
}

static void set_title(struct web_history_entry *e, const char *title)
{
    web_copy(e->title, sizeof(e->title), title ? title : "");
}

int web_history_push(struct web_history *h, const char *url, const char *title)
{
    char *copy = strdup(url ? url : "");
    int i;

    if (!copy) {
        return -1;
    }
    for (i = h->index + 1; i < h->count; i++) {
        free(h->e[i].url);
        h->e[i].url = NULL;
    }
    h->count = h->index + 1;
    if (h->count == WEB_HISTORY_MAX) {
        free(h->e[0].url);
        memmove(&h->e[0], &h->e[1], sizeof(h->e[0]) * (WEB_HISTORY_MAX - 1));
        h->count--;
    }
    h->e[h->count].url = copy;
    set_title(&h->e[h->count], title);
    h->index = h->count++;
    return 0;
}

int web_history_replace(struct web_history *h, const char *url, const char *title)
{
    struct web_history_entry *e;

    if (h->index < 0) {
        return web_history_push(h, url, title);
    }
    e = &h->e[h->index];
    if (url && strcmp(url, e->url) != 0) {
        char *copy = strdup(url);

        if (!copy) {
            return -1;
        }
        free(e->url);
        e->url = copy;
    }
    if (title) {
        set_title(e, title);
    }
    return 0;
}

bool web_history_can_back(const struct web_history *h)
{
    return h->index > 0;
}

bool web_history_can_forward(const struct web_history *h)
{
    return h->index >= 0 && h->index + 1 < h->count;
}

const struct web_history_entry *web_history_back(struct web_history *h)
{
    if (!web_history_can_back(h)) {
        return NULL;
    }
    return &h->e[--h->index];
}

const struct web_history_entry *web_history_forward(struct web_history *h)
{
    if (!web_history_can_forward(h)) {
        return NULL;
    }
    return &h->e[++h->index];
}

const struct web_history_entry *web_history_current(const struct web_history *h)
{
    return h->index >= 0 ? &h->e[h->index] : NULL;
}
