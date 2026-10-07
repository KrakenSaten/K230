/*
 * Recording file names. See rec_names.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_names.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define PREFIX "REC-"
#define RECOVERED "-recovered"

static size_t digits(const char *s)
{
    size_t n = 0;

    while (s[n] >= '0' && s[n] <= '9') {
        n++;
    }
    return n;
}

static bool stamp_valid(const char *s, size_t len)
{
    size_t d = digits(s);

    if (len == 15) {
        return d == 8 && s[8] == '-' && digits(s + 9) >= 6;
    }
    return len >= 4 && len <= 6 && d >= len;
}

bool rec_name_parse(const char *name, struct rec_name *out)
{
    struct rec_name r;
    size_t n = name ? strlen(name) : 0;
    const char *p;
    const char *end;
    size_t len;

    memset(&r, 0, sizeof(r));
    if (n < strlen(PREFIX) + 4 + strlen(REC_WAV_SUFFIX) || n >= REC_NAME_MAX ||
        strncmp(name, PREFIX, strlen(PREFIX)) != 0 ||
        strcmp(name + n - strlen(REC_WAV_SUFFIX), REC_WAV_SUFFIX) != 0) {
        return false;
    }
    p = name + strlen(PREFIX);
    end = name + n - strlen(REC_WAV_SUFFIX);
    /* The stamp: 15 characters with a dash, or 4 to 6 digits. */
    len = digits(p);
    if (len == 8 && p[8] == '-') {
        len = 15;
    }
    if (p + len > end || !stamp_valid(p, len) || len >= sizeof(r.stamp)) {
        return false;
    }
    if (len == 15 && digits(p + 9) != 6) {
        return false;
    }
    memcpy(r.stamp, p, len);
    p += len;
    /* -<n>, then -recovered, each optional, in that order. */
    if (p < end && *p == '-' && p[1] >= '1' && p[1] <= '9') {
        size_t d = digits(p + 1);
        unsigned v = 0;
        size_t i;

        if (d > 3) {
            return false;
        }
        for (i = 0; i < d; i++) {
            v = v * 10 + (unsigned)(p[1 + i] - '0');
        }
        if (v < 2) {
            return false;
        }
        r.dup = v;
        p += 1 + d;
    }
    if ((size_t)(end - p) == strlen(RECOVERED) && strncmp(p, RECOVERED, strlen(RECOVERED)) == 0) {
        r.recovered = true;
        p = end;
    }
    if (p != end) {
        return false;
    }
    if (out) {
        *out = r;
    }
    return true;
}

int rec_name_build(char *out, size_t n, const struct rec_name *parts)
{
    char dup[8] = "";
    int w;

    if (!out || !parts || !stamp_valid(parts->stamp, strlen(parts->stamp)) ||
        parts->dup == 1 || parts->dup > REC_DUP_MAX) {
        return -1;
    }
    if (parts->dup) {
        snprintf(dup, sizeof(dup), "-%u", parts->dup);
    }
    w = snprintf(out, n, PREFIX "%s%s%s" REC_WAV_SUFFIX, parts->stamp, dup,
                 parts->recovered ? RECOVERED : "");
    return w < 0 || (size_t)w >= n || (size_t)w >= REC_NAME_MAX ? -1 : 0;
}

void rec_name_stamp(struct rec_name *out, const struct tm *tm, unsigned seq)
{
    memset(out, 0, sizeof(*out));
    if (tm) {
        snprintf(out->stamp, sizeof(out->stamp), "%04u%02u%02u-%02u%02u%02u",
                 (unsigned)(tm->tm_year + 1900) % 10000u, (unsigned)(tm->tm_mon + 1) % 100u,
                 (unsigned)tm->tm_mday % 100u, (unsigned)tm->tm_hour % 100u,
                 (unsigned)tm->tm_min % 100u, (unsigned)tm->tm_sec % 100u);
    } else {
        if (seq < 1) {
            seq = 1;
        }
        if (seq > REC_SEQ_MAX) {
            seq = REC_SEQ_MAX;
        }
        snprintf(out->stamp, sizeof(out->stamp), "%04u", seq);
    }
}

unsigned rec_name_seq(const char *name)
{
    struct rec_name r;
    unsigned v = 0;
    size_t i;

    if (!rec_name_parse(name, &r) || strlen(r.stamp) > 6) {
        return 0;
    }
    for (i = 0; r.stamp[i]; i++) {
        v = v * 10 + (unsigned)(r.stamp[i] - '0');
    }
    return v;
}

bool rec_name_is_part(const char *name)
{
    char base[REC_NAME_MAX];
    size_t n = name ? strlen(name) : 0;
    size_t s = strlen(REC_PART_SUFFIX);

    if (n <= s || n - s >= sizeof(base) || strcmp(name + n - s, REC_PART_SUFFIX) != 0) {
        return false;
    }
    memcpy(base, name, n - s);
    base[n - s] = '\0';
    return rec_name_parse(base, NULL);
}

bool rec_name_listable(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    size_t i;

    if (n == 0 || n >= REC_FILE_NAME_MAX || name[0] == '.') {
        return false;
    }
    for (i = 0; i < n; i++) {
        if ((unsigned char)name[i] < 0x20 || name[i] == '/' || name[i] == 0x7f) {
            return false;
        }
    }
    if (rec_name_is_part(name)) {
        return true;
    }
    return n > strlen(REC_WAV_SUFFIX) && strcasecmp(name + n - strlen(REC_WAV_SUFFIX), REC_WAV_SUFFIX) == 0;
}
