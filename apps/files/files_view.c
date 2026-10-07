/*
 * Files: on-screen text for entries (files_view.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "files_view.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ELLIPSIS "\xE2\x80\xA6" /* U+2026 */
#define MIDDOT "\xC2\xB7"       /* U+00B7 */

void files_view_size(char *out, size_t out_len, int64_t bytes)
{
    static const char *const unit[] = { "KB", "MB", "GB", "TB" };
    double v;
    int u = 0;

    if (bytes < 0) {
        snprintf(out, out_len, "%s", "");
        return;
    }
    if (bytes < 1024) {
        snprintf(out, out_len, "%lld B", (long long)bytes);
        return;
    }
    v = (double)bytes / 1024.0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    /* One decimal below ten, where it tells something; none above. The
     * rounding is checked so 9.96 KB reads "10 KB", never "10.0 KB". */
    if (v < 9.95) {
        snprintf(out, out_len, "%.1f %s", v, unit[u]);
    } else {
        snprintf(out, out_len, "%.0f %s", v, unit[u]);
    }
}

void files_view_type(char *out, size_t out_len, const struct files_entry *e)
{
    const char *dot;
    char ext[12];
    size_t i;

    switch (e->kind) {
    case FILES_KIND_DIR:
        snprintf(out, out_len, "Folder");
        return;
    case FILES_KIND_LINK:
        snprintf(out, out_len, "%s",
                 e->link_broken ? "Broken link" : e->link_dir ? "Link to folder" : "Link");
        return;
    case FILES_KIND_OTHER:
        snprintf(out, out_len, "Device or pipe");
        return;
    case FILES_KIND_FILE:
        break;
    }
    dot = strrchr(e->name, '.');
    if (!dot || dot == e->name || !dot[1] || strlen(dot + 1) >= sizeof(ext)) {
        snprintf(out, out_len, "File");
        return;
    }
    for (i = 0; dot[1 + i]; i++) {
        unsigned char c = (unsigned char)dot[1 + i];

        if (!isalnum(c)) {
            /* Not an extension anyone would read as a type. */
            snprintf(out, out_len, "File");
            return;
        }
        ext[i] = (char)toupper(c);
    }
    ext[i] = '\0';
    snprintf(out, out_len, "%s file", ext);
}

void files_view_when(char *out, size_t out_len, int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;

    if (t <= 0 || !localtime_r(&tt, &tm) || strftime(out, out_len, "%Y-%m-%d %H:%M", &tm) == 0) {
        snprintf(out, out_len, "%s", "");
    }
}

void files_view_caption(char *out, size_t out_len, const struct files_entry *e)
{
    char first[32];
    char when[24];

    if (e->kind == FILES_KIND_FILE || (e->kind == FILES_KIND_LINK && !e->link_dir && e->size >= 0)) {
        files_view_size(first, sizeof(first), e->size);
    } else {
        files_view_type(first, sizeof(first), e);
    }
    files_view_when(when, sizeof(when), e->mtime);
    if (when[0]) {
        snprintf(out, out_len, "%s " MIDDOT " %s", first, when);
    } else {
        snprintf(out, out_len, "%s", first);
    }
}

/* Length of the UTF-8 sequence at s, or 0 when it is not one. */
static size_t seq_len(const unsigned char *s)
{
    size_t n;
    size_t i;

    if (s[0] < 0x80) {
        return 1;
    } else if ((s[0] & 0xE0) == 0xC0 && s[0] >= 0xC2) {
        n = 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        n = 3;
    } else if ((s[0] & 0xF8) == 0xF0 && s[0] <= 0xF4) {
        n = 4;
    } else {
        return 0;
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return n;
}

void files_view_name(char *out, size_t out_len, const char *name)
{
    const unsigned char *s = (const unsigned char *)name;
    size_t o = 0;

    if (out_len == 0) {
        return;
    }
    while (*s) {
        size_t n = seq_len(s);

        if (n == 0 || (n == 1 && (*s < 0x20 || *s == 0x7F))) {
            n = 1;
            if (o + 1 >= out_len) {
                break;
            }
            out[o++] = '?';
        } else {
            if (o + n >= out_len) {
                break;
            }
            memcpy(out + o, s, n);
            o += n;
        }
        s += n;
    }
    out[o] = '\0';
}

/* Characters (not bytes) in a UTF-8 string. */
static int chars(const char *s)
{
    int n = 0;

    for (; *s; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

void files_view_path(char *out, size_t out_len, const char *path, int max_chars)
{
    char safe[FILES_PATH_MAX];
    const char *p;
    const char *last;

    files_view_name(safe, sizeof(safe), path);
    if (max_chars < 4 || chars(safe) <= max_chars) {
        snprintf(out, out_len, "%s", safe);
        return;
    }
    /* Drop whole leading components until the rest fits behind "…". */
    last = strrchr(safe, '/');
    p = safe;
    while (p < last) {
        const char *next = strchr(p + 1, '/');

        if (!next) {
            break;
        }
        p = next;
        if (chars(p) + 1 <= max_chars) {
            break;
        }
    }
    snprintf(out, out_len, ELLIPSIS "%s", p);
}

const char *files_view_sort_name(enum files_sort key)
{
    static const char *const names[FILES_SORT_COUNT] = { "Name", "Type", "Size", "Date" };

    return (unsigned)key < FILES_SORT_COUNT ? names[key] : names[0];
}
