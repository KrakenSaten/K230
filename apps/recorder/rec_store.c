/*
 * The Recorder's side of the filesystem. See rec_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "rec_store.h"

#include "pocketwav/pocketwav.h"
#include "pocketpaths.h"
#include "rec_protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

int rec_store_dir(char *out, size_t n)
{
    const char *env = getenv("POCKETOS_RECORDINGS_DIR");
    const char *home = getenv("HOME");
    int w;

    if (env && env[0] == '/') {
        w = snprintf(out, n, "%s", env);
    } else {
        w = snprintf(out, n, "%s/" REC_STORE_SUBDIR,
                     home && home[0] == '/' && strcmp(home, "/") != 0 ? home : "/root");
    }
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int rec_store_ensure_dir(const char *dir)
{
    struct stat st;

    if (pocketos_mkdir_p(dir, 0700) != 0) {
        return -errno;
    }
    if (stat(dir, &st) != 0) {
        return -errno;
    }
    if ((st.st_mode & 022) && chmod(dir, st.st_mode & 07755) != 0) {
        return -errno;
    }
    return 0;
}

/* ---- the list ----------------------------------------------------------- */

static void describe(int dirfd, const char *name, const struct stat *st, struct rec_entry *e)
{
    struct pocketwav_info info;
    int fd;
    int rc;

    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->mtime = (int64_t)st->st_mtime;
    e->bytes = (uint64_t)st->st_size;
    if (rec_name_is_part(name)) {
        e->status = REC_ENTRY_PART;
        return;
    }
    {
        struct rec_name parts;

        e->recovered = rec_name_parse(name, &parts) && parts.recovered;
    }
    fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        e->status = REC_ENTRY_UNREADABLE;
        return;
    }
    rc = pocketwav_probe_fd(fd, &info);
    close(fd);
    if (rc == POCKETWAV_E_UNSUPPORTED) {
        e->status = REC_ENTRY_OTHER;
        return;
    }
    if (rc != POCKETWAV_OK) {
        e->status = REC_ENTRY_UNREADABLE;
        return;
    }
    e->rate = info.rate;
    e->channels = info.channels;
    e->ms = pocketwav_frames_ms(info.frames, info.rate);
    e->truncated = info.truncated != 0;
    e->status = info.rate == REC_RATE_VOICE || info.rate == REC_RATE_STANDARD ? REC_ENTRY_OK
                                                                              : REC_ENTRY_OTHER;
}

static int newest_first(const void *a, const void *b)
{
    const struct rec_entry *x = a;
    const struct rec_entry *y = b;

    if (x->mtime != y->mtime) {
        return x->mtime > y->mtime ? -1 : 1;
    }
    return -strcmp(x->name, y->name);
}

int rec_store_list(const char *dir, struct rec_list *l)
{
    int dirfd;
    DIR *d;
    struct dirent *de;

    memset(l, 0, sizeof(*l));
    dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    d = fdopendir(dirfd);
    if (!d) {
        int e = -errno;

        close(dirfd);
        return e;
    }
    l->e = calloc(REC_LIST_MAX, sizeof(*l->e));
    if (!l->e) {
        closedir(d);
        return -ENOMEM;
    }
    while ((de = readdir(d)) != NULL) {
        struct stat st;

        if (!rec_name_listable(de->d_name) ||
            fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        l->total++;
        if (l->n < REC_LIST_MAX) {
            describe(dirfd, de->d_name, &st, &l->e[l->n++]);
        } else {
            /* Keep the newest: replace the oldest kept if this one is newer. */
            struct rec_entry cand;
            int oldest = 0;
            int i;

            for (i = 1; i < l->n; i++) {
                if (newest_first(&l->e[i], &l->e[oldest]) > 0) {
                    oldest = i;
                }
            }
            memset(&cand, 0, sizeof(cand));
            cand.mtime = (int64_t)st.st_mtime;
            snprintf(cand.name, sizeof(cand.name), "%s", de->d_name);
            if (newest_first(&cand, &l->e[oldest]) < 0) {
                describe(dirfd, de->d_name, &st, &l->e[oldest]);
            }
        }
    }
    closedir(d);
    qsort(l->e, (size_t)l->n, sizeof(*l->e), newest_first);
    return 0;
}

void rec_store_list_free(struct rec_list *l)
{
    free(l->e);
    memset(l, 0, sizeof(*l));
}

/* ---- names -------------------------------------------------------------- */

static bool taken(const char *dir, const char *name)
{
    char path[REC_STORE_PATH_MAX + REC_NAME_MAX + 8];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (lstat(path, &st) == 0) {
        return true;
    }
    snprintf(path, sizeof(path), "%s/%s" REC_PART_SUFFIX, dir, name);
    return lstat(path, &st) == 0;
}

static unsigned highest_seq(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    unsigned hi = 0;

    if (!d) {
        return 0;
    }
    while ((de = readdir(d)) != NULL) {
        char base[REC_NAME_MAX];
        size_t n = strlen(de->d_name);
        unsigned v;

        /* A .part counts as well: its number is spoken for. */
        if (rec_name_is_part(de->d_name)) {
            n -= strlen(REC_PART_SUFFIX);
        }
        if (n >= sizeof(base)) {
            continue;
        }
        memcpy(base, de->d_name, n);
        base[n] = '\0';
        v = rec_name_seq(base);
        if (v > hi) {
            hi = v;
        }
    }
    closedir(d);
    return hi;
}

int rec_store_next_name(const char *dir, int64_t wall_now, bool wall_valid, char *out, size_t n)
{
    struct rec_name parts;
    unsigned dup;

    if (wall_valid) {
        time_t t = (time_t)wall_now;
        struct tm tm;

        if (!localtime_r(&t, &tm)) {
            return -1;
        }
        rec_name_stamp(&parts, &tm, 0);
    } else {
        unsigned hi = highest_seq(dir);

        if (hi >= REC_SEQ_MAX) {
            return -1;
        }
        rec_name_stamp(&parts, NULL, hi + 1);
    }
    for (dup = 0; dup <= REC_DUP_MAX; dup = dup ? dup + 1 : 2) {
        parts.dup = dup;
        if (rec_name_build(out, n, &parts) != 0) {
            return -1;
        }
        if (!taken(dir, out)) {
            return 0;
        }
    }
    return -1;
}

/* ---- deleting ----------------------------------------------------------- */

int rec_store_delete(const char *dir, const char *name)
{
    int dirfd;
    struct stat st;
    int e = 0;

    if (!rec_name_listable(name)) {
        return -EINVAL;
    }
    dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        return -errno;
    }
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        e = -errno;
    } else if (!S_ISREG(st.st_mode)) {
        e = -EINVAL;
    } else if (rec_name_is_part(name)) {
        /* A .part being written is somebody's recording in progress. */
        int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

        if (fd < 0) {
            e = -errno;
        } else {
            if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
                e = -EBUSY;
            } else if (unlinkat(dirfd, name, 0) != 0) {
                e = -errno;
            }
            close(fd);
        }
        goto out;
    }
    if (e == 0 && unlinkat(dirfd, name, 0) != 0) {
        e = -errno;
    }
out:
    if (e == 0) {
        fsync(dirfd);
    }
    close(dirfd);
    return e;
}

int64_t rec_store_free(const char *dir)
{
    struct statvfs v;

    if (statvfs(dir, &v) != 0) {
        return -errno;
    }
    return (int64_t)v.f_bavail * (int64_t)v.f_frsize;
}

/* ---- preferences -------------------------------------------------------- */

static int prefs_path(char *out, size_t n, bool make_dir)
{
    char dir[POCKETOS_PATH_MAX];
    int w = snprintf(dir, sizeof(dir), "%s/" REC_PREFS_SUBDIR, pocketos_state_dir());

    if (w < 0 || (size_t)w >= sizeof(dir)) {
        return -1;
    }
    if (make_dir && pocketos_mkdir_p(dir, 0700) != 0) {
        return -1;
    }
    w = snprintf(out, n, "%s/" REC_PREFS_FILE, dir);
    return w < 0 || (size_t)w >= n ? -1 : 0;
}

int rec_store_load_preset(enum rec_preset *p)
{
    char path[POCKETOS_PATH_MAX];
    char buf[512];
    char *line;
    char *save = NULL;
    size_t got;
    FILE *f;

    *p = REC_PRESET_VOICE;
    if (prefs_path(path, sizeof(path), false) != 0) {
        return -1;
    }
    f = fopen(path, "re");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    line = strtok_r(buf, "\n", &save);
    if (!line || strcmp(line, REC_PREFS_MAGIC) != 0) {
        return -1;
    }
    while ((line = strtok_r(NULL, "\n", &save)) != NULL) {
        if (strcmp(line, "preset=voice") == 0) {
            *p = REC_PRESET_VOICE;
        } else if (strcmp(line, "preset=standard") == 0) {
            *p = REC_PRESET_STANDARD;
        }
        /* Unknown keys are a later version's. */
    }
    return 0;
}

int rec_store_save_preset(enum rec_preset p)
{
    char path[POCKETOS_PATH_MAX];
    char tmp[POCKETOS_PATH_MAX + 8];
    char text[64];
    int n;
    int fd;
    ssize_t w;

    if (prefs_path(path, sizeof(path), true) != 0) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    n = snprintf(text, sizeof(text), REC_PREFS_MAGIC "\npreset=%s\n",
                 p == REC_PRESET_STANDARD ? "standard" : "voice");
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    w = write(fd, text, (size_t)n);
    if (w != n || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int rec_preset_rate(enum rec_preset p)
{
    return p == REC_PRESET_STANDARD ? REC_RATE_STANDARD : REC_RATE_VOICE;
}

const char *rec_preset_label(enum rec_preset p)
{
    return p == REC_PRESET_STANDARD ? "STANDARD 48 kHz" : "VOICE 16 kHz";
}
