/*
 * What the Browser remembers (core/web/web_store.h): defaults, a save read
 * back unchanged, the file's and directory's permissions, the bounds, and
 * every kind of bad file - wrong version, junk, a symbolic link, one too
 * large, NUL bytes - giving the defaults and never a crash; a bad line (a
 * planted file:// address, an unknown key) costing only that line; and a
 * file that cannot be read reported as such, never as a store to replace.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failed;
static char dir[256];

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

static void write_file(const char *path, const char *text, size_t n)
{
    FILE *f = fopen(path, "wb");

    fwrite(text, 1, n, f);
    fclose(f);
}

/* Every field, as strings: bytes after a terminator are nobody's business. */
static int same(const struct web_store *a, const struct web_store *b)
{
    int i;

    if (strcmp(a->home, b->home) || strcmp(a->last, b->last) || a->nrecent != b->nrecent ||
        a->nbookmark != b->nbookmark) {
        return 0;
    }
    for (i = 0; i < a->nrecent; i++) {
        if (strcmp(a->recent[i].url, b->recent[i].url) || strcmp(a->recent[i].title, b->recent[i].title)) {
            return 0;
        }
    }
    for (i = 0; i < a->nbookmark; i++) {
        if (strcmp(a->bookmark[i].url, b->bookmark[i].url) || strcmp(a->bookmark[i].title, b->bookmark[i].title)) {
            return 0;
        }
    }
    return 1;
}

static void corrupt(const char *name, const char *text)
{
    char path[512];
    char why[128];
    struct web_store s;
    enum web_store_load r;

    snprintf(path, sizeof(path), "%s/bad", dir);
    write_file(path, text, strlen(text));
    r = web_store_load(&s, path, why, sizeof(why));
    check(name, r == WEB_STORE_CORRUPT && why[0] && strcmp(s.home, "about:home") == 0 && s.nrecent == 0 &&
                    s.nbookmark == 2);
}

/* One bad line is skipped; the store around it is kept, and the bad
 * address never comes back. */
static void partial(const char *name, const char *text)
{
    char path[512];
    char why[128];
    char full[1024];
    struct web_store s;
    enum web_store_load r;
    int i;
    int planted = 0;

    snprintf(full, sizeof(full), "doors-browser-state 1\nbookmark\thttps://kept.example/\tKept\n%s"
                                 "recent\thttps://seen.example/\tSeen\n", text);
    snprintf(path, sizeof(path), "%s/partial", dir);
    write_file(path, full, strlen(full));
    r = web_store_load(&s, path, why, sizeof(why));
    for (i = 0; i < s.nbookmark; i++) {
        planted |= strncmp(s.bookmark[i].url, "https:", 6) != 0;
    }
    planted |= strncmp(s.home, "file:", 5) == 0;
    check(name, r == WEB_STORE_PARTIAL && why[0] && strstr(why, "line 3") != NULL && s.nbookmark == 1 &&
                    strcmp(s.bookmark[0].url, "https://kept.example/") == 0 && s.nrecent == 1 && !planted);
}

int main(void)
{
    char path[512];
    char why[128];
    struct web_store s;
    struct web_store t;
    struct stat st;
    char url[512];
    int i;

    snprintf(dir, sizeof(dir), "/tmp/web_store_test.XXXXXX");
    if (!mkdtemp(dir)) {
        return 1;
    }
    setenv("POCKETOS_STATE_DIR", dir, 1);
    web_store_path(path, sizeof(path));
    check("the file lives under the state directory, in browser/", strstr(path, "/browser/state") != NULL &&
                                                                     strncmp(path, dir, strlen(dir)) == 0);

    check("a missing file gives the defaults", web_store_load(&s, NULL, why, sizeof(why)) == WEB_STORE_MISSING &&
                                                   strcmp(s.home, "about:home") == 0 && s.nbookmark == 2);
    check("the default bookmarks are no search engine", strstr(s.bookmark[0].url, "search") == NULL &&
                                                            strstr(s.bookmark[1].url, "google") == NULL);

    web_store_visit(&s, "https://a.example/", "A");
    web_store_visit(&s, "https://b.example/", "B\tTab");
    web_store_visit(&s, "https://a.example/", "A again");
    web_store_visit(&s, "about:home", "never");
    web_store_visit(&s, "javascript:alert(1)", "never");
    check("recent: newest first, once each, only real addresses",
          s.nrecent == 2 && strcmp(s.recent[0].url, "https://a.example/") == 0 &&
              strcmp(s.recent[0].title, "A again") == 0 && strcmp(s.last, "https://a.example/") == 0);
    check("a tab in a title cannot break the file", strchr(s.recent[1].title, '\t') == NULL);
    check("bookmark added", web_store_toggle_bookmark(&s, "https://b.example/", "B") &&
                                web_store_is_bookmark(&s, "https://b.example/"));
    check("a file:// bookmark is refused", !web_store_toggle_bookmark(&s, "file:///etc/passwd", "x") &&
                                               s.nbookmark == 3);
    check("saved", web_store_save(&s, NULL) == 0);
    stat(path, &st);
    check("the file is 0600", (st.st_mode & 0777) == 0600);
    snprintf(url, sizeof(url), "%s/browser", dir);
    stat(url, &st);
    check("its directory is 0700", (st.st_mode & 0777) == 0700);
    check("read back unchanged", web_store_load(&t, NULL, why, sizeof(why)) == WEB_STORE_LOADED && same(&s, &t));
    check("bookmark removed", !web_store_toggle_bookmark(&t, "https://b.example/", NULL) && t.nbookmark == 2);

    for (i = 0; i < 40; i++) {
        snprintf(url, sizeof(url), "https://r%d.example/", i);
        web_store_visit(&t, url, "");
        web_store_toggle_bookmark(&t, url, "");
    }
    check("recent is bounded", t.nrecent == WEB_STORE_RECENT_MAX &&
                                   strcmp(t.recent[0].url, "https://r39.example/") == 0);
    check("bookmarks are bounded", t.nbookmark == WEB_STORE_BOOKMARK_MAX);
    web_store_clear_recent(&t);
    check("clearing recent also forgets the last page", t.nrecent == 0 && t.last[0] == '\0');
    check("saved again", web_store_save(&t, NULL) == 0 && web_store_load(&s, NULL, why, sizeof(why)) == WEB_STORE_LOADED &&
                             s.nrecent == 0 && s.nbookmark == WEB_STORE_BOOKMARK_MAX);
    snprintf(url, sizeof(url), "%s/browser/state.tmp", dir);
    check("no temporary file is left behind", access(url, F_OK) != 0);

    /* ---- files that are not to be trusted ------------------------------------------------ */
    corrupt("another version", "doors-browser-state 2\nhome\thttps://x.example/\n");
    corrupt("another format", "PK\x03\x04 zip data");
    corrupt("an empty file", "");
    partial("a planted javascript: bookmark is skipped, the rest kept", "bookmark\tjavascript:alert(1)\tx\n");
    partial("a planted file:// home is skipped, the rest kept", "home\tfile:///etc/shadow\n");
    partial("an unknown key is skipped, the rest kept", "exec\thttps://x.example/\n");
    partial("a key without a value is skipped, the rest kept", "home\n");
    {
        /* The finding's case: one damaged line among the person's bookmarks.
         * They load, and saving what was loaded keeps every one of them. */
        char text[2048];
        size_t n = (size_t)snprintf(text, sizeof(text), "doors-browser-state 1\nhome\thttps://h.example/\n");

        for (i = 0; i < 6; i++) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "bookmark\thttps://b%d.example/\tB%d\n", i, i);
            if (i == 2) {
                n += (size_t)snprintf(text + n, sizeof(text) - n, "bookmark https://broken.example/ no tabs\n");
            }
        }
        snprintf(path, sizeof(path), "%s/mine", dir);
        write_file(path, text, n);
        check("one malformed line among six bookmarks: PARTIAL, all six kept",
              web_store_load(&s, path, why, sizeof(why)) == WEB_STORE_PARTIAL && s.nbookmark == 6 &&
                  strcmp(s.bookmark[5].url, "https://b5.example/") == 0 && strcmp(s.home, "https://h.example/") == 0 &&
                  strstr(why, "line 6") != NULL);
        check("saved and read back: still all six, the bad line gone",
              web_store_save(&s, path) == 0 && web_store_load(&t, path, why, sizeof(why)) == WEB_STORE_LOADED &&
                  same(&s, &t) && t.nbookmark == 6);
    }
    /* ---- files that are there but cannot be read ------------------------------------------ */
    if (geteuid() != 0) {
        const char *mine = "doors-browser-state 1\nbookmark\thttps://mine.example/\tMine\n";

        snprintf(path, sizeof(path), "%s/locked", dir);
        write_file(path, mine, strlen(mine));
        chmod(path, 0);
        check("no permission: UNREADABLE, defaults, a reason",
              web_store_load(&s, path, why, sizeof(why)) == WEB_STORE_UNREADABLE && s.nbookmark == 2 &&
                  strstr(why, "cannot be read") != NULL);
        chmod(path, 0600);
    } else {
        printf("skip the permission check (running as root)\n");
    }
    /* A regular file whose read() fails with EIO: /proc/self/mem at offset 0. */
    check("a read error part-way: UNREADABLE, none of it used",
          web_store_load(&s, "/proc/self/mem", why, sizeof(why)) == WEB_STORE_UNREADABLE && s.nbookmark == 2 &&
              strcmp(s.home, "about:home") == 0 && strstr(why, "cannot be read") != NULL);
    {
        char bad[600];
        char back[64];
        FILE *f;

        snprintf(path, sizeof(path), "%s/aside", dir);
        write_file(path, "junk\n", 5);
        snprintf(bad, sizeof(bad), "%s.bad", path);
        check("set aside: the file becomes <file>.bad", web_store_set_aside(path) == 0 && access(path, F_OK) != 0 &&
                                                           access(bad, F_OK) == 0);
        f = fopen(bad, "r");
        check("set aside: its bytes are unchanged", f && fgets(back, sizeof(back), f) && strcmp(back, "junk\n") == 0);
        if (f) {
            fclose(f);
        }
        check("set aside with nothing there is fine", web_store_set_aside(path) == 0);
    }
    {
        char text[256] = "doors-browser-state 1\nhome\thttps://x.example/\0junk\n";

        snprintf(path, sizeof(path), "%s/nul", dir);
        write_file(path, text, 50);
        check("a NUL byte", web_store_load(&s, path, why, sizeof(why)) == WEB_STORE_CORRUPT);
    }
    {
        size_t n = WEB_STORE_FILE_MAX + 100;
        char *big = malloc(n);

        memset(big, 'x', n);
        memcpy(big, "doors-browser-state 1\n", 22);
        snprintf(path, sizeof(path), "%s/big", dir);
        write_file(path, big, n);
        free(big);
        check("a file over 64 KB", web_store_load(&s, path, why, sizeof(why)) == WEB_STORE_CORRUPT);
    }
    {
        char target[512];

        snprintf(target, sizeof(target), "%s/browser/state", dir);
        snprintf(path, sizeof(path), "%s/link", dir);
        check("a symbolic link is not followed", symlink(target, path) == 0 &&
                                                     web_store_load(&s, path, why, sizeof(why)) == WEB_STORE_CORRUPT);
        check("and not written through", web_store_save(&s, path) == 0 && lstat(path, &st) == 0 &&
                                             S_ISREG(st.st_mode));
    }
    snprintf(path, sizeof(path), "%s/lists", dir);
    {
        char text[8192];
        size_t n = (size_t)snprintf(text, sizeof(text), "doors-browser-state 1\n");

        for (i = 0; i < 30; i++) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "recent\thttps://z%d.example/\tZ\n", i);
        }
        write_file(path, text, n);
        check("a list longer than the limit is cut, not refused",
              web_store_load(&s, path, why, sizeof(why)) == WEB_STORE_LOADED && s.nrecent == WEB_STORE_RECENT_MAX);
    }
    {
        char cmd[300];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) {
            printf("warning: %s not removed\n", dir);
        }
    }
    printf("web_store_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
