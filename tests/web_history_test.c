/*
 * The Browser's back/forward list (core/web/web_history.h): back and
 * forward, a new page dropping what was forward, and the bound.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "web/web_history.h"

#include <stdio.h>
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

int main(void)
{
    struct web_history h;
    const struct web_history_entry *e;
    char url[64];
    int i;

    web_history_init(&h);
    check("empty: nothing back, nothing forward, no current",
          !web_history_can_back(&h) && !web_history_can_forward(&h) && !web_history_current(&h) &&
              !web_history_back(&h) && !web_history_forward(&h));

    web_history_push(&h, "about:home", "Start");
    web_history_push(&h, "https://a.example/", "A");
    web_history_push(&h, "https://b.example/", "B");
    check("three pages: at the last, back possible", h.count == 3 && web_history_can_back(&h) &&
                                                         !web_history_can_forward(&h));
    e = web_history_back(&h);
    check("back goes to A", e && strcmp(e->url, "https://a.example/") == 0 && strcmp(e->title, "A") == 0);
    e = web_history_back(&h);
    check("back again to the start page", e && strcmp(e->url, "about:home") == 0);
    check("and no further", !web_history_back(&h) && web_history_can_forward(&h));
    e = web_history_forward(&h);
    check("forward to A", e && strcmp(e->url, "https://a.example/") == 0);
    web_history_push(&h, "https://c.example/", "C");
    check("a new page from A drops B", h.count == 3 && !web_history_can_forward(&h) &&
                                           strcmp(web_history_current(&h)->url, "https://c.example/") == 0);
    web_history_replace(&h, "https://c.example/final", "C final");
    check("replace changes the current entry only",
          h.count == 3 && strcmp(web_history_current(&h)->url, "https://c.example/final") == 0 &&
              strcmp(web_history_current(&h)->title, "C final") == 0);
    web_history_replace(&h, NULL, "Title only");
    check("a title alone can arrive later", strcmp(web_history_current(&h)->title, "Title only") == 0 &&
                                               strcmp(web_history_current(&h)->url, "https://c.example/final") == 0);

    for (i = 0; i < 100; i++) {
        snprintf(url, sizeof(url), "https://n%d.example/", i);
        web_history_push(&h, url, "");
    }
    check("bounded: 100 more pages keep only the newest 32", h.count == WEB_HISTORY_MAX &&
                                                                h.index == WEB_HISTORY_MAX - 1);
    check("the newest is current", strcmp(web_history_current(&h)->url, "https://n99.example/") == 0);
    check("the oldest kept is the 32nd newest", strcmp(h.e[0].url, "https://n68.example/") == 0);
    for (i = 0; i < WEB_HISTORY_MAX - 1; i++) {
        web_history_back(&h);
    }
    check("31 steps back reach the oldest, and no further",
          h.index == 0 && !web_history_back(&h));
    web_history_push(&h, "https://fresh.example/", "");
    check("a page pushed from the oldest drops all 31 forward", h.count == 2);
    {
        char title[400];

        memset(title, 'T', sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        web_history_push(&h, "https://t.example/", title);
        check("a long title is cut to the limit", strlen(web_history_current(&h)->title) == WEB_TITLE_MAX - 1);
    }
    web_history_free(&h);
    check("free leaves an empty list", h.count == 0 && h.index == -1);

    printf("web_history_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
