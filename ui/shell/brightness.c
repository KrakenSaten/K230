/*
 * Display brightness over the Linux backlight class. See brightness.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "brightness.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A sysfs attribute is one short line. Reads a non-negative decimal integer
 * followed by at most a newline; anything else is -1. */
static int read_int(const char *path)
{
    char buf[32];
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    long v;
    char *end;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    if (buf[0] < '0' || buf[0] > '9') {
        return -1;
    }
    errno = 0;
    v = strtol(buf, &end, 10);
    if (errno != 0 || v < 0 || v > 1000000 || (*end != '\0' && strcmp(end, "\n") != 0)) {
        return -1;
    }
    return (int)v;
}

static int by_name(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

int brightness_probe(struct brightness *b, const char *sysfs_root)
{
    char dir[256];
    struct dirent **list = NULL;
    int n;
    int i;
    int found = 0;

    memset(b, 0, sizeof(*b));
    if (!sysfs_root || snprintf(dir, sizeof(dir), "%s/class/backlight", sysfs_root) >=
                           (int)sizeof(dir)) {
        return 1;
    }
    n = scandir(dir, &list, NULL, by_name);
    if (n < 0) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        const char *name = list[i]->d_name;
        char attr[384];
        int max;

        if (found || name[0] == '.' || strlen(name) >= sizeof(b->name)) {
            continue;
        }
        if (snprintf(attr, sizeof(attr), "%s/%s/max_brightness", dir, name) >= (int)sizeof(attr)) {
            continue;
        }
        max = read_int(attr);
        if (max < BRIGHTNESS_MIN_LEVELS) {
            continue;
        }
        if (snprintf(attr, sizeof(attr), "%s/%s/brightness", dir, name) >= (int)sizeof(attr) ||
            access(attr, F_OK) != 0) {
            continue;
        }
        b->supported = 1;
        b->max_raw = max;
        snprintf(b->name, sizeof(b->name), "%s", name);
        snprintf(b->path, sizeof(b->path), "%s", attr);
        found = 1;
    }
    for (i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
    return found ? 0 : 1;
}

int brightness_clamp_percent(int pct)
{
    if (pct < BRIGHTNESS_MIN_PCT) {
        return BRIGHTNESS_MIN_PCT;
    }
    if (pct > BRIGHTNESS_MAX_PCT) {
        return BRIGHTNESS_MAX_PCT;
    }
    return pct;
}

int brightness_percent_to_raw(int pct, int max_raw)
{
    int raw;

    if (max_raw <= 0) {
        return 0;
    }
    pct = brightness_clamp_percent(pct);
    raw = (pct * max_raw + 50) / 100;
    return raw < 1 ? 1 : raw;
}

int brightness_raw_to_percent(int raw, int max_raw)
{
    if (max_raw <= 0 || raw < 0) {
        return -1;
    }
    if (raw > max_raw) {
        raw = max_raw;
    }
    return (raw * 100 + max_raw / 2) / max_raw;
}

int brightness_get_percent(const struct brightness *b)
{
    int raw;

    if (!b || !b->supported) {
        return -1;
    }
    raw = read_int(b->path);
    return raw < 0 ? -1 : brightness_raw_to_percent(raw, b->max_raw);
}

int brightness_set_percent(const struct brightness *b, int pct)
{
    char buf[16];
    int len;
    int fd;
    ssize_t n;

    if (!b || !b->supported) {
        errno = ENODEV;
        return -1;
    }
    pct = brightness_clamp_percent(pct);
    len = snprintf(buf, sizeof(buf), "%d\n", brightness_percent_to_raw(pct, b->max_raw));
    fd = open(b->path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    n = write(fd, buf, (size_t)len);
    if (close(fd) != 0 && n == len) {
        return -1;
    }
    if (n != len) {
        if (n >= 0) {
            errno = EIO;
        }
        return -1;
    }
    return pct;
}

int brightness_parse_setting(const char *s, int *pct)
{
    int v = 0;
    size_t i;
    size_t n = s ? strlen(s) : 0;

    if (n == 0 || n > 3 || (n > 1 && s[0] == '0')) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    if (v < BRIGHTNESS_MIN_PCT || v > BRIGHTNESS_MAX_PCT) {
        return -1;
    }
    *pct = v;
    return 0;
}
