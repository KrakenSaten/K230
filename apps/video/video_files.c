/*
 * The Video app's file list. See video_files.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "video_files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

int video_files_dir(char *out, size_t n)
{
    const char *env = getenv("POCKETOS_VIDEOS_DIR");
    const char *home = getenv("HOME");
    int w;

    if (env && env[0] == '/') {
        w = snprintf(out, n, "%s", env);
    } else {
        w = snprintf(out, n, "%s/" VIDEO_FILES_SUBDIR,
                     home && home[0] == '/' && strcmp(home, "/") != 0 ? home : "/root");
    }
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int video_files_playable_name(const char *name)
{
    size_t len;
    size_t i;

    if (!name || name[0] == '\0' || name[0] == '.') {
        return 0;
    }
    len = strlen(name);
    if (len >= VIDEO_FILES_NAME_MAX || len < 5 || strcasecmp(name + len - 4, ".mp4") != 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct video_file *)a)->name, ((const struct video_file *)b)->name);
}

int video_files_scan(struct video_files *list, const char *dir)
{
    DIR *d;
    struct dirent *de;
    int looked = 0;

    memset(list, 0, sizeof(*list));
    d = opendir(dir);
    if (!d) {
        list->error = errno ? errno : EIO;
        return -1;
    }
    while (looked < VIDEO_FILES_SCAN_MAX && (de = readdir(d)) != NULL) {
        struct stat st;
        struct video_file *f;

        looked++;
        if (!video_files_playable_name(de->d_name)) {
            continue;
        }
        if (fstatat(dirfd(d), de->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (list->count == VIDEO_FILES_MAX) {
            list->more = 1;
            continue;
        }
        f = &list->items[list->count++];
        /* video_files_playable_name() bounded it below VIDEO_FILES_NAME_MAX. */
        memcpy(f->name, de->d_name, strlen(de->d_name) + 1);
        f->bytes = (uint64_t)st.st_size;
    }
    if (looked == VIDEO_FILES_SCAN_MAX && readdir(d) != NULL) {
        list->more = 1;
    }
    closedir(d);
    qsort(list->items, (size_t)list->count, sizeof(list->items[0]), by_name);
    return list->count;
}

void video_files_size_text(uint64_t bytes, char *out, size_t n)
{
    if (bytes >= 1000ull * 1000) {
        uint64_t tenths = (bytes + 50000) / 100000;

        snprintf(out, n, "%llu.%llu MB", (unsigned long long)(tenths / 10),
                 (unsigned long long)(tenths % 10));
    } else {
        snprintf(out, n, "%llu KB", (unsigned long long)((bytes + 500) / 1000));
    }
}

void video_files_time_text(int64_t ms, char *out, size_t n)
{
    int64_t s = ms > 0 ? ms / 1000 : 0;

    if (s >= 3600) {
        snprintf(out, n, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60),
                 (long long)(s % 60));
    } else {
        snprintf(out, n, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    }
}
