/*
 * The Browser's remembered state. See web_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "web/web_store.h"

#include "pocketpaths.h"
#include "web/web_url.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEADER "doors-browser-state"

static void set(char *dst, size_t cap, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    size_t i;
    size_t o = 0;

    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) {
            n--;
        }
    }
    /* Tabs and line breaks are the file's separators. */
    for (i = 0; i < n; i++) {
        char c = s[i];

        dst[o++] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[o] = '\0';
}

void web_store_defaults(struct web_store *s)
{
    memset(s, 0, sizeof(*s));
    set(s->home, sizeof(s->home), "about:home");
    /* Two neutral starting points, neither a search engine nor anything
     * commercial; both can be removed like any other bookmark. */
    set(s->bookmark[0].url, sizeof(s->bookmark[0].url), "https://example.com/");
    set(s->bookmark[0].title, sizeof(s->bookmark[0].title), "Example Domain");
    set(s->bookmark[1].url, sizeof(s->bookmark[1].url), "https://en.m.wikipedia.org/");
    set(s->bookmark[1].title, sizeof(s->bookmark[1].title), "Wikipedia");
    s->nbookmark = 2;
}

const char *web_store_path(char *buf, size_t len)
{
    snprintf(buf, len, "%s/browser/state", pocketos_state_dir());
    return buf;
}

static bool url_ok(const char *u)
{
    struct web_url parsed;

    return strlen(u) < WEB_STORE_URL_MAX && web_url_parse(u, &parsed) == WEB_URL_OK;
}

static void say(char *why, size_t whylen, const char *fmt, int line)
{
    if (why && whylen) {
        snprintf(why, whylen, fmt, line);
    }
}

/* The file is there and may hold the person's bookmarks, but it could not
 * be read: the defaults are used and the file must be left alone. */
static enum web_store_load unreadable(struct web_store *s, char *why, size_t whylen, int err)
{
    web_store_defaults(s);
    if (why && whylen) {
        snprintf(why, whylen, "cannot be read (%s)", strerror(err));
    }
    return WEB_STORE_UNREADABLE;
}

/* The first bad line is the one named; the others are skipped quietly. */
static void skip(char *why, size_t whylen, bool *skipped, const char *fmt, int line)
{
    if (!*skipped) {
        say(why, whylen, fmt, line);
    }
    *skipped = true;
}

enum web_store_load web_store_load(struct web_store *s, const char *path, char *why, size_t whylen)
{
    char pbuf[POCKETOS_PATH_MAX];
    struct web_store t;
    char *buf;
    char *line;
    char *save = NULL;
    struct stat st;
    ssize_t got;
    size_t have = 0;
    bool skipped = false;
    int fd;
    int ln = 0;
    int err;

    web_store_defaults(s);
    if (why && whylen) {
        why[0] = '\0';
    }
    if (!path) {
        path = web_store_path(pbuf, sizeof(pbuf));
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return WEB_STORE_MISSING;
        }
        if (errno == ELOOP) {
            say(why, whylen, "a symbolic link, not the store", 0);
            return WEB_STORE_CORRUPT;
        }
        return unreadable(s, why, whylen, errno);
    }
    if (fstat(fd, &st) != 0) {
        err = errno;
        close(fd);
        return unreadable(s, why, whylen, err);
    }
    if (!S_ISREG(st.st_mode) || st.st_size > (off_t)WEB_STORE_FILE_MAX) {
        close(fd);
        say(why, whylen, "not a regular file of at most 64 KB", 0);
        return WEB_STORE_CORRUPT;
    }
    buf = malloc(WEB_STORE_FILE_MAX + 1);
    if (!buf) {
        close(fd);
        return unreadable(s, why, whylen, ENOMEM);
    }
    while (have < WEB_STORE_FILE_MAX &&
           (got = read(fd, buf + have, WEB_STORE_FILE_MAX - have)) != 0) {
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* Half a file is not the file: nothing of it is used. */
            err = errno;
            close(fd);
            free(buf);
            return unreadable(s, why, whylen, err);
        }
        have += (size_t)got;
    }
    close(fd);
    buf[have] = '\0';
    if (memchr(buf, '\0', have)) {
        free(buf);
        say(why, whylen, "contains a NUL byte", 0);
        return WEB_STORE_CORRUPT;
    }
    memset(&t, 0, sizeof(t));
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f1;
        char *f2;

        ln++;
        if (ln == 1) {
            char want[32];

            snprintf(want, sizeof(want), "%s %d", HEADER, WEB_STORE_VERSION);
            if (strcmp(line, want) != 0) {
                free(buf);
                say(why, whylen, "unknown format or version on line %d", ln);
                return WEB_STORE_CORRUPT;
            }
            continue;
        }
        /* One bad line costs that line, not the person's other bookmarks. */
        f1 = strchr(line, '\t');
        if (!f1) {
            skip(why, whylen, &skipped, "line %d has no value", ln);
            continue;
        }
        *f1++ = '\0';
        f2 = strchr(f1, '\t');
        if (f2) {
            *f2++ = '\0';
        }
        if (!url_ok(f1)) {
            skip(why, whylen, &skipped, "line %d holds an address that is not allowed", ln);
            continue;
        }
        if (strcmp(line, "home") == 0 && !f2) {
            set(t.home, sizeof(t.home), f1);
        } else if (strcmp(line, "last") == 0 && !f2) {
            set(t.last, sizeof(t.last), f1);
        } else if (strcmp(line, "recent") == 0 && t.nrecent < WEB_STORE_RECENT_MAX) {
            set(t.recent[t.nrecent].url, WEB_STORE_URL_MAX, f1);
            set(t.recent[t.nrecent].title, WEB_TITLE_MAX, f2 ? f2 : "");
            t.nrecent++;
        } else if (strcmp(line, "bookmark") == 0 && t.nbookmark < WEB_STORE_BOOKMARK_MAX) {
            set(t.bookmark[t.nbookmark].url, WEB_STORE_URL_MAX, f1);
            set(t.bookmark[t.nbookmark].title, WEB_TITLE_MAX, f2 ? f2 : "");
            t.nbookmark++;
        } else if (strcmp(line, "recent") != 0 && strcmp(line, "bookmark") != 0) {
            skip(why, whylen, &skipped, "line %d is not a known entry", ln);
        }
        /* Past a list's limit the rest of that list is dropped. */
    }
    free(buf);
    if (ln == 0) {
        say(why, whylen, "empty file", 0);
        return WEB_STORE_CORRUPT;
    }
    if (!t.home[0]) {
        set(t.home, sizeof(t.home), "about:home");
    }
    *s = t;
    return skipped ? WEB_STORE_PARTIAL : WEB_STORE_LOADED;
}

int web_store_set_aside(const char *path)
{
    char pbuf[POCKETOS_PATH_MAX];
    char bad[POCKETOS_PATH_MAX + 8];

    if (!path) {
        path = web_store_path(pbuf, sizeof(pbuf));
    }
    snprintf(bad, sizeof(bad), "%s.bad", path);
    if (rename(path, bad) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        s += w;
        n -= (size_t)w;
    }
    return 0;
}

int web_store_save(const struct web_store *s, const char *path)
{
    char pbuf[POCKETOS_PATH_MAX];
    char dir[POCKETOS_PATH_MAX];
    char tmp[POCKETOS_PATH_MAX + 8];
    char *slash;
    char *out;
    size_t n = 0;
    size_t cap = WEB_STORE_FILE_MAX;
    int fd;
    int i;
    int saved;

    if (!path) {
        path = web_store_path(pbuf, sizeof(pbuf));
    }
    web_copy(dir, sizeof(dir), path);
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (pocketos_mkdir_p(dir, 0700) != 0) {
            return -1;
        }
        /* Ours alone, whatever made it before. */
        chmod(dir, 0700);
    }
    out = malloc(cap);
    if (!out) {
        errno = ENOMEM;
        return -1;
    }
#define PUT(...)                                                                         \
    do {                                                                                 \
        int w_ = snprintf(out + n, cap - n, __VA_ARGS__);                                \
        if (w_ > 0 && (size_t)w_ < cap - n) {                                            \
            n += (size_t)w_;                                                             \
        }                                                                                \
    } while (0)
    PUT("%s %d\n", HEADER, WEB_STORE_VERSION);
    if (url_ok(s->home)) {
        PUT("home\t%s\n", s->home);
    }
    if (s->last[0] && url_ok(s->last)) {
        PUT("last\t%s\n", s->last);
    }
    for (i = 0; i < s->nrecent && i < WEB_STORE_RECENT_MAX; i++) {
        if (url_ok(s->recent[i].url)) {
            PUT("recent\t%s\t%s\n", s->recent[i].url, s->recent[i].title);
        }
    }
    for (i = 0; i < s->nbookmark && i < WEB_STORE_BOOKMARK_MAX; i++) {
        if (url_ok(s->bookmark[i].url)) {
            PUT("bookmark\t%s\t%s\n", s->bookmark[i].url, s->bookmark[i].title);
        }
    }
#undef PUT
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        saved = errno;
        free(out);
        errno = saved;
        return -1;
    }
    if (fchmod(fd, 0600) != 0 || write_all(fd, out, n) != 0 || fsync(fd) != 0) {
        saved = errno;
        close(fd);
        unlink(tmp);
        free(out);
        errno = saved;
        return -1;
    }
    free(out);
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}

void web_store_visit(struct web_store *s, const char *url, const char *title)
{
    struct web_place p;
    int i;

    if (!url || strlen(url) >= WEB_STORE_URL_MAX || !url_ok(url) || strncmp(url, "about:", 6) == 0) {
        return;
    }
    set(s->last, sizeof(s->last), url);
    memset(&p, 0, sizeof(p));
    set(p.url, sizeof(p.url), url);
    set(p.title, sizeof(p.title), title);
    for (i = 0; i < s->nrecent; i++) {
        if (strcmp(s->recent[i].url, url) == 0) {
            memmove(&s->recent[i], &s->recent[i + 1], sizeof(p) * (size_t)(s->nrecent - i - 1));
            s->nrecent--;
            break;
        }
    }
    if (s->nrecent == WEB_STORE_RECENT_MAX) {
        s->nrecent--;
    }
    memmove(&s->recent[1], &s->recent[0], sizeof(p) * (size_t)s->nrecent);
    s->recent[0] = p;
    s->nrecent++;
}

void web_store_clear_recent(struct web_store *s)
{
    memset(s->recent, 0, sizeof(s->recent));
    s->nrecent = 0;
    s->last[0] = '\0';
}

bool web_store_is_bookmark(const struct web_store *s, const char *url)
{
    int i;

    for (i = 0; url && i < s->nbookmark; i++) {
        if (strcmp(s->bookmark[i].url, url) == 0) {
            return true;
        }
    }
    return false;
}

bool web_store_toggle_bookmark(struct web_store *s, const char *url, const char *title)
{
    int i;

    if (!url || strlen(url) >= WEB_STORE_URL_MAX || !url_ok(url) || strncmp(url, "about:", 6) == 0) {
        return false;
    }
    for (i = 0; i < s->nbookmark; i++) {
        if (strcmp(s->bookmark[i].url, url) == 0) {
            memmove(&s->bookmark[i], &s->bookmark[i + 1],
                    sizeof(s->bookmark[0]) * (size_t)(s->nbookmark - i - 1));
            s->nbookmark--;
            return false;
        }
    }
    if (s->nbookmark >= WEB_STORE_BOOKMARK_MAX) {
        return false;
    }
    set(s->bookmark[s->nbookmark].url, WEB_STORE_URL_MAX, url);
    set(s->bookmark[s->nbookmark].title, WEB_TITLE_MAX, title && *title ? title : url);
    s->nbookmark++;
    return true;
}
