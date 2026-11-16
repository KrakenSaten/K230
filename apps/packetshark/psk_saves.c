/*
 * PacketShark's saved captures: listing and deleting. See psk_saves.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "psk_saves.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLOCK_SET_FROM 1735689600 /* 2025-01-01: the clock counts as set from here */

static DIR *open_folder(void)
{
    char buf[512];
    const char *dir = ppc_captures_dir(buf, sizeof(buf));

    return dir ? opendir(dir) : NULL;
}

int psk_saves_list(struct psk_saved *out, int max, uint32_t *count, uint64_t *bytes)
{
    DIR *d = open_folder();
    struct dirent *e;
    uint32_t total = 0;
    uint64_t sum = 0;
    int n = 0;
    int scanned = 0;

    if (count) {
        *count = 0;
    }
    if (bytes) {
        *bytes = 0;
    }
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL && scanned < PSK_SAVES_SCAN) {
        struct stat sb;
        struct psk_saved s;
        int i;

        scanned++;
        if (!ppc_save_name_ok(e->d_name) || fstatat(dirfd(d), e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(sb.st_mode)) {
            continue;
        }
        total++;
        sum += (uint64_t)sb.st_size;
        memset(&s, 0, sizeof(s));
        snprintf(s.name, sizeof(s.name), "%.*s", (int)sizeof(s.name) - 1, e->d_name);
        s.bytes = (uint64_t)sb.st_size;
        s.mtime = (int64_t)sb.st_mtime;
        /* Insert by date, the newest first, then by name so the order is
         * the same every time. */
        for (i = 0; i < n; i++) {
            if (s.mtime > out[i].mtime || (s.mtime == out[i].mtime && strcmp(s.name, out[i].name) > 0)) {
                break;
            }
        }
        if (i >= max) {
            continue;
        }
        if (n < max) {
            n++;
        }
        memmove(&out[i + 1], &out[i], (size_t)(n - 1 - i) * sizeof(out[0]));
        out[i] = s;
    }
    closedir(d);
    if (count) {
        *count = total;
    }
    if (bytes) {
        *bytes = sum;
    }
    return n;
}

int psk_saves_delete(const char *name)
{
    DIR *d;
    struct stat sb;
    int rc = -1;

    if (!ppc_save_name_ok(name)) {
        return -1;
    }
    d = open_folder();
    if (!d) {
        return -1;
    }
    if (fstatat(dirfd(d), name, &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sb.st_mode)) {
        rc = unlinkat(dirfd(d), name, 0) == 0 ? 0 : -1;
    }
    closedir(d);
    return rc;
}

const char *psk_saves_age(int64_t mtime, int64_t now, char *buf, size_t buflen)
{
    int64_t s = now - mtime;

    if (mtime < CLOCK_SET_FROM || now < CLOCK_SET_FROM || s < 0) {
        snprintf(buf, buflen, "%s", "");
    } else if (s < 60) {
        snprintf(buf, buflen, "JUST NOW");
    } else if (s < 3600) {
        snprintf(buf, buflen, "%lld MIN AGO", (long long)(s / 60));
    } else if (s < 86400) {
        snprintf(buf, buflen, "%lld H AGO", (long long)(s / 3600));
    } else {
        snprintf(buf, buflen, "%lld D AGO", (long long)(s / 86400));
    }
    return buf;
}
