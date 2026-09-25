/*
 * pocketcam's photo store. See pocketcam_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketcam_store.h"
#include "pocketpaths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

int64_t (*pocketcam_store_free_hook)(const char *dir);
int64_t pocketcam_store_fail_after = -1;

static bool all_digits(const char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return n > 0;
}

/* The sequence number of a valid name, or -1. */
static long name_seq(const char *name)
{
    const char *dot;
    const char *start;
    size_t n = strlen(name);

    if (n < 9 || n >= POCKETCAM_STORE_NAME_MAX || strncmp(name, "IMG_", 4) != 0) {
        return -1;
    }
    dot = strrchr(name, '.');
    if (!dot || (strcmp(dot, ".jpg") != 0 && strcmp(dot, ".ppm") != 0)) {
        return -1;
    }
    start = name + 4;
    if ((size_t)(dot - start) > 16 && start[8] == '_' && start[15] == '_') {
        /* IMG_yyyymmdd_hhmmss_nnnn */
        if (!all_digits(start, 8) || !all_digits(start + 9, 6)) {
            return -1;
        }
        start += 16;
    }
    if (dot - start < 4 || dot - start > 6 || !all_digits(start, (size_t)(dot - start))) {
        return -1;
    }
    return strtol(start, NULL, 10);
}

bool pocketcam_store_valid_name(const char *name)
{
    return name && name_seq(name) >= 0;
}

void pocketcam_store_default_dir(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", pocketos_state_dir(), POCKETCAM_STORE_SUBDIR);
}

static bool is_leftover(const char *name)
{
    size_t n = strlen(name);

    return name[0] == '.' && n > 5 && strcmp(name + n - 4, ".tmp") == 0 &&
           strncmp(name + 1, "IMG_", 4) == 0;
}

int pocketcam_store_scan(struct pocketcam_store *s)
{
    DIR *d = opendir(s->dir);
    struct dirent *e;
    long best = -1;

    if (!d) {
        return -errno;
    }
    s->files = 0;
    s->bytes = 0;
    s->last[0] = '\0';
    while ((e = readdir(d)) != NULL) {
        long seq = name_seq(e->d_name);
        struct stat st;

        if (seq < 0 || fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode)) {
            continue;
        }
        s->files++;
        s->bytes += (uint64_t)st.st_size;
        if (seq > best) {
            best = seq;
            /* A valid name is shorter than POCKETCAM_STORE_NAME_MAX. */
            memcpy(s->last, e->d_name, strlen(e->d_name) + 1);
        }
    }
    closedir(d);
    s->next_seq = best < 0 ? 1 : (uint32_t)best + 1;
    return 0;
}

int pocketcam_store_open(struct pocketcam_store *s, const char *dir)
{
    DIR *d;
    struct dirent *e;

    memset(s, 0, sizeof(*s));
    if (!dir || !*dir || strlen(dir) >= sizeof(s->dir)) {
        return -EINVAL;
    }
    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    s->max_files = POCKETCAM_STORE_MAX_FILES;
    s->max_bytes = POCKETCAM_STORE_MAX_BYTES;
    s->reserve_bytes = POCKETCAM_STORE_RESERVE_BYTES;
    if (pocketos_mkdir_p(s->dir, 0755) != 0) {
        return -errno;
    }
    /* What a write interrupted by a crash or a power cut left behind. */
    d = opendir(s->dir);
    if (!d) {
        return -errno;
    }
    while ((e = readdir(d)) != NULL) {
        if (is_leftover(e->d_name)) {
            unlinkat(dirfd(d), e->d_name, 0);
        }
    }
    closedir(d);
    return pocketcam_store_scan(s);
}

static int64_t free_bytes(const char *dir)
{
    struct statvfs v;

    if (pocketcam_store_free_hook) {
        return pocketcam_store_free_hook(dir);
    }
    if (statvfs(dir, &v) != 0) {
        return -errno;
    }
    return (int64_t)((uint64_t)v.f_bavail * v.f_frsize);
}

int pocketcam_store_room(const struct pocketcam_store *s, uint64_t estimate)
{
    int64_t avail;

    if (s->files >= s->max_files || s->bytes + estimate > s->max_bytes) {
        return -EDQUOT;
    }
    avail = free_bytes(s->dir);
    if (avail < 0) {
        return (int)avail;
    }
    if ((uint64_t)avail < estimate + s->reserve_bytes) {
        return -ENOSPC;
    }
    return 0;
}

void pocketcam_store_next_name(const struct pocketcam_store *s, int64_t now, const char *ext,
                               char *out, size_t out_len)
{
    if (now >= POCKETCAM_WALL_VALID_FROM) {
        time_t t = (time_t)now;
        struct tm tm;

        localtime_r(&t, &tm);
        snprintf(out, out_len, "IMG_%04d%02d%02d_%02d%02d%02d_%04u.%s", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, s->next_seq, ext);
    } else {
        snprintf(out, out_len, "IMG_%04u.%s", s->next_seq, ext);
    }
}

/* ---- the write stream ----------------------------------------------------------
 * A stdio stream over the temporary file's descriptor, so any encoder that
 * writes to a FILE works, and so the byte count and the test seam's
 * simulated full disk sit under every encoder alike. */

struct sink {
    int fd;
    uint64_t written;
    int err;
};

static ssize_t sink_write(void *cookie, const char *buf, size_t n)
{
    struct sink *k = cookie;
    size_t done = 0;

    if (pocketcam_store_fail_after >= 0 &&
        k->written + n > (uint64_t)pocketcam_store_fail_after) {
        k->err = ENOSPC;
        errno = ENOSPC;
        return -1;
    }
    while (done < n) {
        ssize_t w = write(k->fd, buf + done, n - done);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            k->err = errno;
            return done ? (ssize_t)done : -1;
        }
        done += (size_t)w;
    }
    k->written += done;
    return (ssize_t)done;
}

static int sink_close(void *cookie)
{
    (void)cookie;
    return 0; /* the descriptor is fsync'd and closed by the store */
}

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int r = 0;

    if (fd < 0) {
        return -errno;
    }
    if (fsync(fd) != 0) {
        r = -errno;
    }
    close(fd);
    return r;
}

int pocketcam_store_write(struct pocketcam_store *s, const char *name,
                          pocketcam_store_writer writer, void *user, uint64_t *bytes_out)
{
    char tmp[PATH_MAX];
    char final[PATH_MAX];
    cookie_io_functions_t io = { .read = NULL, .write = sink_write, .seek = NULL,
                                 .close = sink_close };
    struct sink k = { .fd = -1, .written = 0, .err = 0 };
    struct stat st;
    FILE *fp;
    int r;

    if (!pocketcam_store_valid_name(name) || !writer) {
        return -EINVAL;
    }
    if (snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", s->dir, name) >= (int)sizeof(tmp) ||
        snprintf(final, sizeof(final), "%s/%s", s->dir, name) >= (int)sizeof(final)) {
        return -ENAMETOOLONG;
    }
    if (lstat(final, &st) == 0) {
        return -EEXIST;
    }
    unlink(tmp);
    k.fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (k.fd < 0) {
        return -errno;
    }
    fp = fopencookie(&k, "w", io);
    if (!fp) {
        r = -errno;
        close(k.fd);
        unlink(tmp);
        return r;
    }
    r = writer(fp, user);
    if (fflush(fp) != 0 && r == 0) {
        r = -(k.err ? k.err : EIO);
    }
    fclose(fp);
    if (r == 0 && k.err) {
        r = -k.err;
    }
    if (r == 0 && fsync(k.fd) != 0) {
        r = -errno;
    }
    if (close(k.fd) != 0 && r == 0) {
        r = -errno;
    }
    if (r == 0 && rename(tmp, final) != 0) {
        r = -errno;
    }
    if (r != 0) {
        unlink(tmp);
        return r == -EDQUOT ? -ENOSPC : r;
    }
    fsync_dir(s->dir);
    if (bytes_out) {
        *bytes_out = k.written;
    }
    return pocketcam_store_scan(s);
}

int pocketcam_store_delete(struct pocketcam_store *s, const char *name)
{
    char path[PATH_MAX];

    if (!pocketcam_store_valid_name(name)) {
        return -EINVAL;
    }
    if (snprintf(path, sizeof(path), "%s/%s", s->dir, name) >= (int)sizeof(path)) {
        return -ENAMETOOLONG;
    }
    if (unlink(path) != 0) {
        return -errno;
    }
    fsync_dir(s->dir);
    return pocketcam_store_scan(s);
}
