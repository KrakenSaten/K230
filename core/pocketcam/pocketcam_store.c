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
    /* The photo is gone whatever a count taken now says. */
    pocketcam_store_scan(s);
    return 0;
}

/* ---- the library ------------------------------------------------------------ */

struct listed {
    long seq;
    char name[POCKETCAM_STORE_NAME_MAX];
};

static int newest_first(const void *a, const void *b)
{
    const struct listed *x = a;
    const struct listed *y = b;

    if (x->seq != y->seq) {
        return x->seq > y->seq ? -1 : 1;
    }
    return -strcmp(x->name, y->name);
}

int pocketcam_store_list(const struct pocketcam_store *s, char (*names)[POCKETCAM_STORE_NAME_MAX],
                         uint32_t max, uint32_t *total)
{
    struct listed *all = NULL;
    size_t n = 0;
    size_t cap = 0;
    uint32_t i;
    DIR *d = opendir(s->dir);
    struct dirent *e;

    if (total) {
        *total = 0;
    }
    if (!d) {
        return -errno;
    }
    while ((e = readdir(d)) != NULL) {
        long seq = name_seq(e->d_name);
        struct stat st;

        if (seq < 0 || fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode)) {
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            struct listed *grown = realloc(all, ncap * sizeof(*all));

            if (!grown) {
                free(all);
                closedir(d);
                return -ENOMEM;
            }
            all = grown;
            cap = ncap;
        }
        all[n].seq = seq;
        memcpy(all[n].name, e->d_name, strlen(e->d_name) + 1);
        n++;
    }
    closedir(d);
    if (n > 1) {
        qsort(all, n, sizeof(*all), newest_first);
    }
    for (i = 0; i < max && i < n; i++) {
        memcpy(names[i], all[i].name, sizeof(all[i].name));
    }
    if (total) {
        *total = (uint32_t)n;
    }
    free(all);
    return (int)i;
}

void pocketcam_export_default_dir(char *out, size_t out_len)
{
    const char *home = getenv("HOME");

    if (!home || home[0] != '/' || strlen(home) + sizeof(POCKETCAM_EXPORT_SUBDIR) + 2 > out_len) {
        home = "/root";
    }
    snprintf(out, out_len, "%s%s%s", home, home[strlen(home) - 1] == '/' ? "" : "/",
             POCKETCAM_EXPORT_SUBDIR);
}

/* Copy the open descriptor in to out, whole, or fail with a negative errno. */
static int copy_fd(int in, int out)
{
    char buf[64 * 1024];

    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        ssize_t off = 0;

        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));

            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -errno;
            }
            off += w;
        }
    }
}

/* 1 when the file at path is a regular file with exactly the bytes of the open
 * descriptor in (st: its fstat), 0 when it is anything else, or a negative
 * errno when either cannot be read. A name and a size prove nothing: a photo
 * number is used again once the newest photo has been deleted. */
static int same_content(int in, const struct stat *st, const char *path)
{
    char a[16 * 1024];
    char b[16 * 1024];
    struct stat there;
    off_t off = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int r = 1;

    if (fd < 0) {
        return errno == ELOOP ? 0 : -errno;
    }
    if (fstat(fd, &there) != 0) {
        r = -errno;
    } else if (!S_ISREG(there.st_mode) || there.st_size != st->st_size) {
        r = 0;
    }
    while (r == 1 && off < st->st_size) {
        size_t want = (uint64_t)(st->st_size - off) < sizeof(a) ? (size_t)(st->st_size - off)
                                                                : sizeof(a);
        ssize_t na = pread(in, a, want, off);
        ssize_t nb = pread(fd, b, want, off);

        if (na < 0 || nb < 0) {
            if (errno != EINTR) {
                r = -errno;
            }
            continue;
        }
        if (na != nb || na == 0 || memcmp(a, b, (size_t)na) != 0) {
            r = 0; /* different, or one of them changed size meanwhile */
            break;
        }
        off += na;
    }
    close(fd);
    return r;
}

/* The k-th choice of a path for name in dest_dir: the name itself, then
 * IMG_..._nnnn-2.jpg, -3 and on. */
static int export_candidate(const char *dest_dir, const char *name, int k, char *out,
                            size_t out_len)
{
    const char *dot = strrchr(name, '.'); /* a valid name has one */
    int n = k == 1 ? snprintf(out, out_len, "%s/%s", dest_dir, name)
                   : snprintf(out, out_len, "%s/%.*s-%d%s", dest_dir, (int)(dot - name), name, k,
                              dot);

    return n < 0 || (size_t)n >= out_len ? -ENAMETOOLONG : 0;
}

void pocketcam_export_sweep(const char *dest_dir)
{
    DIR *d = dest_dir ? opendir(dest_dir) : NULL;
    struct dirent *e;

    if (!d) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        struct stat st;

        if (n > 12 && strncmp(e->d_name, ".IMG_", 5) == 0 &&
            strcmp(e->d_name + n - 7, ".export") == 0 &&
            fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)) {
            unlinkat(dirfd(d), e->d_name, 0);
        }
    }
    closedir(d);
}

int pocketcam_store_export(const struct pocketcam_store *s, const char *name, const char *dest_dir,
                           char *out, size_t out_len, bool *already)
{
    char src[PATH_MAX];
    char final[PATH_MAX];
    char tmp[PATH_MAX];
    struct stat st;
    struct stat there;
    struct timespec times[2];
    int64_t avail;
    int in;
    int fd;
    int k;
    int r;

    if (already) {
        *already = false;
    }
    if (!pocketcam_store_valid_name(name) || !dest_dir || dest_dir[0] != '/') {
        return -EINVAL;
    }
    /* The temporary carries the helper's pid, so it is its own; one that a
     * killed helper left is swept when the next library helper starts. */
    if (snprintf(src, sizeof(src), "%s/%s", s->dir, name) >= (int)sizeof(src) ||
        snprintf(tmp, sizeof(tmp), "%s/.%s.%ld.export", dest_dir, name, (long)getpid()) >=
            (int)sizeof(tmp)) {
        return -ENAMETOOLONG;
    }
    in = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) {
        return -errno;
    }
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(in);
        return -EINVAL;
    }
    if (pocketos_mkdir_p(dest_dir, 0755) != 0) {
        r = -errno;
        close(in);
        return r;
    }
    /* The first free name, unless a name already taken holds exactly this
     * photo: then it is there, and nothing is written. */
    for (k = 1; k <= POCKETCAM_EXPORT_TRIES; k++) {
        r = export_candidate(dest_dir, name, k, final, sizeof(final));
        if (r == 0 && strlen(final) >= out_len) {
            r = -ENAMETOOLONG;
        }
        if (r == 0 && lstat(final, &there) != 0) {
            if (errno == ENOENT) {
                break;
            }
            r = -errno;
        }
        if (r == 0) {
            r = same_content(in, &st, final);
            if (r == 1) {
                close(in);
                snprintf(out, out_len, "%s", final);
                if (already) {
                    *already = true;
                }
                return 0;
            }
        }
        if (r < 0) {
            close(in);
            return r;
        }
    }
    if (k > POCKETCAM_EXPORT_TRIES) {
        close(in);
        return -EEXIST;
    }
    avail = free_bytes(dest_dir);
    if (avail >= 0 && (uint64_t)avail < (uint64_t)st.st_size + POCKETCAM_STORE_RESERVE_BYTES) {
        close(in);
        return -ENOSPC;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        r = -errno;
        close(in);
        return r;
    }
    r = copy_fd(in, fd);
    close(in);
    /* The photo's own time, so Files sorts the copy by when it was taken. */
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    if (r == 0) {
        futimens(fd, times);
        if (fsync(fd) != 0) {
            r = -errno;
        }
    }
    if (close(fd) != 0 && r == 0) {
        r = -errno;
    }
    /* link() never replaces: a name that appeared meanwhile is kept. */
    if (r == 0 && link(tmp, final) != 0) {
        r = -errno;
        if (r == -EPERM || r == -EOPNOTSUPP) {
            /* A filesystem without hard links (vfat): checked, then renamed. */
            r = lstat(final, &there) == 0 ? -EEXIST : rename(tmp, final) == 0 ? 0 : -errno;
        }
    }
    unlink(tmp);
    if (r != 0) {
        return r == -EDQUOT ? -ENOSPC : r;
    }
    fsync_dir(dest_dir);
    snprintf(out, out_len, "%s", final);
    return 0;
}
