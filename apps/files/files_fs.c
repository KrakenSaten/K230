/*
 * Files: listing, the write policy and the operations (files_fs.h).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "files_fs.h"
#include "pocketpaths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

#define COPY_CHUNK (64 * 1024)

const char *files_strerror(int err)
{
    if (err < 0) {
        err = -err;
    }
    switch (err) {
    case 0: return "Done";
    case FILES_EPOLICY: return "This place is protected; nothing here can be changed";
    case FILES_EBINARY: return "This is not a text file";
    case FILES_EINSIDE: return "A folder cannot go inside itself";
    case FILES_EMOUNT: return "It holds another storage device; nothing was changed";
    case FILES_ESPECIAL: return "This is a device or pipe, not a file";
    case FILES_ECANCELED: return "Stopped; nothing was left half done";
    case FILES_EDEPTH: return "The folder is nested too deeply";
    case FILES_ESAME: return "It is already here";
    case FILES_ENAME: return "That is not a valid name";
    case FILES_ESOURCE_KEPT: return "Moved, but the original could not be removed completely";
    case EACCES:
    case EPERM: return "Permission denied";
    case ENOENT: return "It is no longer there";
    case EEXIST:
    case ENOTEMPTY: return "Something with that name is already there";
    case ENOSPC: return "The storage is full";
    case EROFS: return "This storage is read-only";
    case ENAMETOOLONG: return "The name or path is too long";
    case ENOTDIR: return "That is not a folder";
    case EISDIR: return "That is a folder";
    case EBUSY: return "It is in use";
    case EIO: return "The storage could not be read or written";
    case ELOOP: return "Too many links in the path";
    case EAGAIN: return "It is not ready to be read";
    case ENOMEM: return "Out of memory";
    case EMFILE:
    case ENFILE: return "Too many files open";
    default: return "The operation failed";
    }
}

/* ---- paths ---------------------------------------------------------------- */

int files_path_join(char *out, size_t out_len, const char *dir, const char *name)
{
    int n;

    if (strcmp(dir, "/") == 0) {
        n = snprintf(out, out_len, "/%s", name);
    } else {
        n = snprintf(out, out_len, "%s/%s", dir, name);
    }
    return (n < 0 || (size_t)n >= out_len) ? -ENAMETOOLONG : 0;
}

int files_path_parent(char *out, size_t out_len, const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t len;

    if (!slash || strcmp(path, "/") == 0) {
        return -1;
    }
    len = (size_t)(slash - path);
    if (len == 0) {
        len = 1; /* "/a" -> "/" */
    }
    if (len >= out_len) {
        return -1;
    }
    memmove(out, path, len);
    out[len] = '\0';
    return 0;
}

const char *files_path_base(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (!slash || strcmp(path, "/") == 0) {
        return path;
    }
    return slash + 1;
}

int files_path_clean(char *out, size_t out_len, const char *path)
{
    size_t o = 0;
    const char *p = path;

    if (!path || path[0] != '/' || out_len < 2) {
        return -EINVAL;
    }
    out[0] = '\0';
    while (*p) {
        const char *seg;
        size_t len;

        while (*p == '/') {
            p++;
        }
        seg = p;
        while (*p && *p != '/') {
            p++;
        }
        len = (size_t)(p - seg);
        if (len == 0 || (len == 1 && seg[0] == '.')) {
            continue;
        }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') {
                o--;
            }
            if (o > 0) {
                o--; /* the slash before it */
            }
            out[o] = '\0';
            continue;
        }
        if (o + 1 + len + 1 > out_len) {
            return -ENAMETOOLONG;
        }
        out[o++] = '/';
        memcpy(out + o, seg, len);
        o += len;
        out[o] = '\0';
    }
    if (o == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

bool files_path_within(const char *path, const char *dir)
{
    size_t n = strlen(dir);

    if (strcmp(dir, "/") == 0) {
        return path[0] == '/';
    }
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Length of the UTF-8 sequence starting at s, or 0 when it is not one. */
static size_t utf8_len(const unsigned char *s)
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

const char *files_name_problem(const char *name)
{
    const unsigned char *s = (const unsigned char *)name;
    size_t len;

    if (!name || !name[0]) {
        return "Type a name";
    }
    len = strlen(name);
    if (len > FILES_NAME_MAX) {
        return "That name is too long";
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return "That name is reserved";
    }
    if (name[0] == ' ' || name[len - 1] == ' ') {
        return "A name cannot start or end with a space";
    }
    while (*s) {
        size_t n = utf8_len(s);

        if (n == 0) {
            return "That name has characters that cannot be stored";
        }
        if (n == 1 && (*s < 0x20 || *s == 0x7F)) {
            return "That name has characters that cannot be stored";
        }
        if (*s == '/') {
            return "A name cannot contain /";
        }
        s += n;
    }
    return NULL;
}

/* ---- listing -------------------------------------------------------------- */

static void fill_entry(struct files_entry *e, const struct stat *st)
{
    e->mode = (uint32_t)st->st_mode;
    e->mtime = (int64_t)st->st_mtime;
    e->size = -1;
    e->link_dir = false;
    e->link_broken = false;
    if (S_ISDIR(st->st_mode)) {
        e->kind = FILES_KIND_DIR;
    } else if (S_ISREG(st->st_mode)) {
        e->kind = FILES_KIND_FILE;
        e->size = (int64_t)st->st_size;
    } else if (S_ISLNK(st->st_mode)) {
        e->kind = FILES_KIND_LINK;
    } else {
        e->kind = FILES_KIND_OTHER;
    }
}

/* What a link leads to, for an entry already filled from lstat. */
static void fill_link_target(struct files_entry *e, const struct stat *target, int found)
{
    if (!found) {
        e->link_broken = true;
        return;
    }
    e->link_dir = S_ISDIR(target->st_mode);
    if (S_ISREG(target->st_mode)) {
        e->size = (int64_t)target->st_size;
    }
}

int files_dir_read(struct files_dir *d, const char *path)
{
    DIR *dir;
    struct dirent *de;
    int cap = 0;
    int fd;

    memset(d, 0, sizeof(*d));
    dir = opendir(path);
    if (!dir) {
        return -errno;
    }
    fd = dirfd(dir);
    while ((de = readdir(dir)) != NULL) {
        struct files_entry *e;
        struct stat st;
        struct stat target;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (strlen(de->d_name) > FILES_NAME_MAX) {
            continue;
        }
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) {
                continue; /* gone between readdir and stat */
            }
            memset(&st, 0, sizeof(st));
        }
        d->total++;
        if (d->n >= FILES_LIST_MAX) {
            continue;
        }
        if (d->n == cap) {
            int grow = cap ? cap * 2 : 64;
            struct files_entry *more;

            if (grow > FILES_LIST_MAX) {
                grow = FILES_LIST_MAX;
            }
            more = realloc(d->entries, (size_t)grow * sizeof(*more));
            if (!more) {
                closedir(dir);
                files_dir_free(d);
                return -ENOMEM;
            }
            d->entries = more;
            cap = grow;
        }
        e = &d->entries[d->n++];
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        fill_entry(e, &st);
        if (e->kind == FILES_KIND_LINK) {
            fill_link_target(e, &target, fstatat(fd, de->d_name, &target, 0) == 0);
        }
    }
    closedir(dir);
    return 0;
}

void files_dir_free(struct files_dir *d)
{
    free(d->entries);
    memset(d, 0, sizeof(*d));
}

bool files_entry_is_dir(const struct files_entry *e)
{
    return e->kind == FILES_KIND_DIR || (e->kind == FILES_KIND_LINK && e->link_dir);
}

int files_stat(const char *path, struct files_entry *out)
{
    struct stat st;
    struct stat target;

    memset(out, 0, sizeof(*out));
    if (lstat(path, &st) != 0) {
        return -errno;
    }
    snprintf(out->name, sizeof(out->name), "%s", files_path_base(path));
    fill_entry(out, &st);
    if (out->kind == FILES_KIND_LINK) {
        fill_link_target(out, &target, stat(path, &target) == 0);
    }
    return 0;
}

static const char *extension(const char *name)
{
    const char *dot = strrchr(name, '.');

    return (dot && dot != name && dot[1]) ? dot + 1 : "";
}

static int by_name(const struct files_entry *a, const struct files_entry *b)
{
    int c = strcasecmp(a->name, b->name);

    return c ? c : strcmp(a->name, b->name);
}

static int dirs_first(const struct files_entry *a, const struct files_entry *b)
{
    return (int)files_entry_is_dir(b) - (int)files_entry_is_dir(a);
}

static int cmp_name(const void *pa, const void *pb)
{
    const struct files_entry *a = pa;
    const struct files_entry *b = pb;
    int c = dirs_first(a, b);

    return c ? c : by_name(a, b);
}

static int cmp_type(const void *pa, const void *pb)
{
    const struct files_entry *a = pa;
    const struct files_entry *b = pb;
    int c = dirs_first(a, b);

    if (!c) {
        c = strcasecmp(extension(a->name), extension(b->name));
    }
    return c ? c : by_name(a, b);
}

static int cmp_size(const void *pa, const void *pb)
{
    const struct files_entry *a = pa;
    const struct files_entry *b = pb;
    int c = dirs_first(a, b);

    if (!c && a->size != b->size) {
        c = a->size > b->size ? -1 : 1;
    }
    return c ? c : by_name(a, b);
}

static int cmp_date(const void *pa, const void *pb)
{
    const struct files_entry *a = pa;
    const struct files_entry *b = pb;
    int c = dirs_first(a, b);

    if (!c && a->mtime != b->mtime) {
        c = a->mtime > b->mtime ? -1 : 1;
    }
    return c ? c : by_name(a, b);
}

void files_sort(struct files_entry *e, int n, enum files_sort key)
{
    static int (*const cmp[FILES_SORT_COUNT])(const void *, const void *) = {
        cmp_name, cmp_type, cmp_size, cmp_date
    };

    if (!e || n < 2) {
        return;
    }
    qsort(e, (size_t)n, sizeof(*e), cmp[(unsigned)key < FILES_SORT_COUNT ? key : 0]);
}

/* ---- the write policy ------------------------------------------------------ */

/* A path as the kernel will resolve it. The entry form resolves the parent
 * and keeps the last component, so a link is judged where it is and not
 * where it points; the directory form resolves all of it. */
static int canon(const char *path, char *out, size_t out_len, bool entry)
{
    char clean[FILES_PATH_MAX];
    char parent[FILES_PATH_MAX];
    char *real;
    int r;

    if (files_path_clean(clean, sizeof(clean), path) != 0) {
        return -1;
    }
    if (!entry || strcmp(clean, "/") == 0) {
        real = realpath(clean, NULL);
        if (!real) {
            return -1;
        }
        r = snprintf(out, out_len, "%s", real);
        free(real);
        return (r < 0 || (size_t)r >= out_len) ? -1 : 0;
    }
    if (files_path_parent(parent, sizeof(parent), clean) != 0) {
        return -1;
    }
    real = realpath(parent, NULL);
    if (!real) {
        return -1;
    }
    r = files_path_join(out, out_len, real, files_path_base(clean));
    free(real);
    return r;
}

/* A policy path resolved when it exists, cleaned when it does not. */
static int policy_path(const char *p, char *out, size_t out_len)
{
    if (canon(p, out, out_len, false) == 0) {
        return 0;
    }
    return files_path_clean(out, out_len, p);
}

const struct files_policy *files_policy_default(void)
{
    static struct files_policy pol;
    static char doors[4][POCKETOS_PATH_MAX];
    static bool built;

    /* Once, on the first call - the app makes it from the LVGL thread before
     * its worker can exist - so the worker only ever reads it. */
    if (built) {
        return &pol;
    }
    built = true;
    snprintf(doors[0], sizeof(doors[0]), "%s", pocketos_runtime_dir());
    snprintf(doors[1], sizeof(doors[1]), "%s", pocketos_config_dir());
    snprintf(doors[2], sizeof(doors[2]), "%s", pocketos_state_dir());
    snprintf(doors[3], sizeof(doors[3]), "%s", pocketos_log_dir());
    memset(&pol, 0, sizeof(pol));
    pol.writable[0] = "/root";
    pol.writable[1] = "/home";
    pol.writable[2] = "/tmp";
    pol.writable[3] = "/mnt";
    pol.writable[4] = "/media";
    pol.protect[0] = doors[0];
    pol.protect[1] = doors[1];
    pol.protect[2] = doors[2];
    pol.protect[3] = doors[3];
    pol.protect[4] = "/root/.ssh";
    return &pol;
}

static enum files_access judge(const struct files_policy *pol, const char *c, bool entry)
{
    char p[FILES_PATH_MAX];
    int i;

    if (!pol) {
        pol = files_policy_default();
    }
    for (i = 0; i < FILES_POLICY_MAX && pol->protect[i]; i++) {
        if (policy_path(pol->protect[i], p, sizeof(p)) != 0) {
            continue;
        }
        /* At or inside a protected path; or, for an entry that could be
         * moved or deleted, around one. */
        if (files_path_within(c, p) || (entry && files_path_within(p, c))) {
            return FILES_ACCESS_DOORS;
        }
    }
    for (i = 0; i < FILES_POLICY_MAX && pol->writable[i]; i++) {
        if (canon(pol->writable[i], p, sizeof(p), false) != 0) {
            continue; /* a root that does not exist here opens nothing */
        }
        if (strcmp(c, p) == 0) {
            return entry ? FILES_ACCESS_ROOT : FILES_ACCESS_OK;
        }
        if (files_path_within(c, p)) {
            return FILES_ACCESS_OK;
        }
    }
    return FILES_ACCESS_SYSTEM;
}

enum files_access files_policy_entry(const struct files_policy *pol, const char *path)
{
    char c[FILES_PATH_MAX];

    if (canon(path, c, sizeof(c), true) != 0) {
        return FILES_ACCESS_MISSING;
    }
    return judge(pol, c, true);
}

enum files_access files_policy_dir(const struct files_policy *pol, const char *dir)
{
    char c[FILES_PATH_MAX];

    if (canon(dir, c, sizeof(c), false) != 0) {
        return FILES_ACCESS_MISSING;
    }
    return judge(pol, c, false);
}

const char *files_access_text(enum files_access a)
{
    switch (a) {
    case FILES_ACCESS_OK: return "";
    case FILES_ACCESS_SYSTEM: return "Read-only: system area";
    case FILES_ACCESS_ROOT: return "Read-only: a top-level folder";
    case FILES_ACCESS_DOORS: return "Read-only: protected Doors or private data";
    case FILES_ACCESS_MISSING: return "Not found";
    }
    return "";
}

/* ---- operations ------------------------------------------------------------ */

/* A refusal as an error: a place that is not there is ENOENT, which says what
 * happened, rather than a protection that does not exist. */
static int refusal(enum files_access a)
{
    return a == FILES_ACCESS_MISSING ? -ENOENT : -FILES_EPOLICY;
}

static int cancelled(atomic_int *cancel)
{
    return cancel && atomic_load(cancel);
}

/* rename(2) that never replaces what is at `to`. The kernel does it in one
 * step where it can (renameat2 RENAME_NOREPLACE); on a kernel or filesystem
 * without it, a check first, which leaves only a window no user can hit. */
static int rename_noreplace(const char *from, const char *to)
{
    struct stat st;

#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -errno;
    }
#endif
    if (lstat(to, &st) == 0) {
        return -EEXIST;
    }
    if (errno != ENOENT) {
        return -errno;
    }
    return rename(from, to) == 0 ? 0 : -errno;
}

/* Remove name in dirfd and everything under it, never following a link and
 * never leaving dev. With dry_run nothing is removed: the tree is only
 * walked, so a delete can refuse a tree it could not finish before it has
 * started. */
static int remove_at(int dirfd, const char *name, dev_t dev, int depth, bool dry_run,
                     atomic_int *cancel)
{
    struct stat st;
    struct dirent *de;
    DIR *d;
    int fd;
    int r = 0;

    if (cancelled(cancel)) {
        return -FILES_ECANCELED;
    }
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (dry_run) {
            return 0;
        }
        return unlinkat(dirfd, name, 0) == 0 ? 0 : -errno;
    }
    if (st.st_dev != dev) {
        return -FILES_EMOUNT;
    }
    if (depth >= FILES_DEPTH_MAX) {
        return -FILES_EDEPTH;
    }
    fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    d = fdopendir(fd);
    if (!d) {
        r = -errno;
        close(fd);
        return r;
    }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        r = remove_at(fd, de->d_name, dev, depth + 1, dry_run, cancel);
        if (r != 0) {
            break;
        }
    }
    closedir(d);
    if (r != 0 || dry_run) {
        return r;
    }
    return unlinkat(dirfd, name, AT_REMOVEDIR) == 0 ? 0 : -errno;
}

/* The tree at path, removed; dev is the device it must stay on. */
static int remove_tree(const char *path, dev_t dev, bool dry_run, atomic_int *cancel)
{
    char parent[FILES_PATH_MAX];
    int fd;
    int r;

    if (files_path_parent(parent, sizeof(parent), path) != 0) {
        return -EINVAL;
    }
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    r = remove_at(fd, files_path_base(path), dev, 0, dry_run, cancel);
    close(fd);
    return r;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst, const struct stat *st, atomic_int *cancel)
{
    struct timespec times[2];
    char *buf;
    int in;
    int out;
    int r = 0;

    in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) {
        return -errno;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        r = -errno;
        close(in);
        return r;
    }
    buf = malloc(COPY_CHUNK);
    if (!buf) {
        r = -ENOMEM;
    }
    while (r == 0) {
        ssize_t got = read(in, buf, COPY_CHUNK);

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            r = -errno;
        } else if (got == 0) {
            break;
        } else if (cancelled(cancel)) {
            r = -FILES_ECANCELED;
        } else {
            r = write_all(out, buf, (size_t)got);
        }
    }
    free(buf);
    close(in);
    /* A move removes the source after this returns, so the copy has to be
     * on the storage, not in the page cache, before it does. */
    if (r == 0 && fsync(out) != 0) {
        r = -errno;
    }
    if (r == 0) {
        times[0] = st->st_atim;
        times[1] = st->st_mtim;
        (void)fchmod(out, st->st_mode & 07777);
        (void)futimens(out, times);
    }
    if (close(out) != 0 && r == 0) {
        r = -errno;
    }
    return r;
}

static int copy_tree(const char *src, const char *dst, dev_t dev, int depth, atomic_int *cancel)
{
    struct stat st;
    struct dirent *de;
    DIR *d;
    int r = 0;

    if (cancelled(cancel)) {
        return -FILES_ECANCELED;
    }
    if (lstat(src, &st) != 0) {
        return -errno;
    }
    if (S_ISREG(st.st_mode)) {
        return copy_file(src, dst, &st, cancel);
    }
    if (S_ISLNK(st.st_mode)) {
        char target[FILES_PATH_MAX];
        ssize_t n = readlink(src, target, sizeof(target) - 1);

        if (n < 0) {
            return -errno;
        }
        target[n] = '\0';
        return symlink(target, dst) == 0 ? 0 : -errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return -FILES_ESPECIAL;
    }
    if (st.st_dev != dev) {
        return -FILES_EMOUNT;
    }
    if (depth >= FILES_DEPTH_MAX) {
        return -FILES_EDEPTH;
    }
    if (mkdir(dst, 0700) != 0) {
        return -errno;
    }
    d = opendir(src);
    if (!d) {
        return -errno;
    }
    while ((de = readdir(d)) != NULL) {
        char s[FILES_PATH_MAX];
        char t[FILES_PATH_MAX];

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (files_path_join(s, sizeof(s), src, de->d_name) != 0 ||
            files_path_join(t, sizeof(t), dst, de->d_name) != 0) {
            r = -ENAMETOOLONG;
            break;
        }
        r = copy_tree(s, t, dev, depth + 1, cancel);
        if (r != 0) {
            break;
        }
    }
    closedir(d);
    if (r == 0) {
        struct timespec times[2] = { st.st_atim, st.st_mtim };

        (void)chmod(dst, st.st_mode & 07777);
        (void)utimensat(AT_FDCWD, dst, times, 0);
    }
    return r;
}

/* A name in dir that nothing has, for building a copy under before it is
 * renamed into place. Hidden, so a listing taken meanwhile does not offer it. */
static int temp_path(char *out, size_t out_len, const char *dir)
{
    static atomic_uint serial;
    struct stat st;
    int i;

    for (i = 0; i < 100; i++) {
        char name[64];

        snprintf(name, sizeof(name), ".files-partial-%ld-%u", (long)getpid(),
                 atomic_fetch_add(&serial, 1));
        if (files_path_join(out, out_len, dir, name) != 0) {
            return -ENAMETOOLONG;
        }
        if (lstat(out, &st) != 0 && errno == ENOENT) {
            return 0;
        }
    }
    return -EEXIST;
}

/* "name (k)" for k >= 2: before the extension of a file, at the end of a
 * folder's name, shortened (on a character boundary) to stay a valid name. */
static void numbered_name(char *out, size_t out_len, const char *name, bool is_dir, int k)
{
    const char *ext = is_dir ? "" : extension(name);
    char suffix[16];
    size_t stem;
    size_t room;

    snprintf(suffix, sizeof(suffix), " (%d)", k);
    stem = ext[0] ? (size_t)(ext - 1 - name) : strlen(name);
    room = FILES_NAME_MAX - strlen(suffix) - (ext[0] ? strlen(ext) + 1 : 0);
    if (stem > room) {
        stem = room;
        while (stem > 0 && ((unsigned char)name[stem] & 0xC0) == 0x80) {
            stem--;
        }
    }
    snprintf(out, out_len, "%.*s%s%s%s", (int)stem, name, suffix, ext[0] ? "." : "", ext);
}

/* Refuse a folder going into itself: dst_dir at or below src. */
static int check_inside(const char *src, const char *dst_dir)
{
    char rs[FILES_PATH_MAX];
    char rd[FILES_PATH_MAX];

    if (canon(src, rs, sizeof(rs), false) != 0 || canon(dst_dir, rd, sizeof(rd), false) != 0) {
        return -ENOENT;
    }
    return files_path_within(rd, rs) ? -FILES_EINSIDE : 0;
}

int files_mkdir(const struct files_policy *pol, const char *dir, const char *name)
{
    char path[FILES_PATH_MAX];
    enum files_access acc;

    if (files_name_problem(name)) {
        return -FILES_ENAME;
    }
    if ((acc = files_policy_dir(pol, dir)) != FILES_ACCESS_OK) {
        return refusal(acc);
    }
    if (files_path_join(path, sizeof(path), dir, name) != 0) {
        return -ENAMETOOLONG;
    }
    return mkdir(path, 0755) == 0 ? 0 : -errno;
}

int files_rename(const struct files_policy *pol, const char *path, const char *name)
{
    char parent[FILES_PATH_MAX];
    char to[FILES_PATH_MAX];
    struct stat st;

    if (files_name_problem(name)) {
        return -FILES_ENAME;
    }
    if (lstat(path, &st) != 0) {
        return -errno;
    }
    if (files_policy_entry(pol, path) != FILES_ACCESS_OK) {
        return -FILES_EPOLICY;
    }
    if (strcmp(files_path_base(path), name) == 0) {
        return 0;
    }
    if (files_path_parent(parent, sizeof(parent), path) != 0 ||
        files_path_join(to, sizeof(to), parent, name) != 0) {
        return -ENAMETOOLONG;
    }
    return rename_noreplace(path, to);
}

/* Build a copy of src in dst_dir under a temporary name. */
static int copy_to_temp(const char *src, const char *dst_dir, char *tmp, size_t tmp_len,
                        atomic_int *cancel)
{
    struct stat st;
    int r;

    if (lstat(src, &st) != 0) {
        return -errno;
    }
    r = temp_path(tmp, tmp_len, dst_dir);
    if (r != 0) {
        return r;
    }
    r = copy_tree(src, tmp, st.st_dev, 0, cancel);
    if (r != 0) {
        struct stat t;

        /* Whatever of the copy exists goes again; the source was only read. */
        if (lstat(tmp, &t) == 0) {
            (void)remove_tree(tmp, t.st_dev, false, NULL);
        }
    }
    return r;
}

int files_copy(const struct files_policy *pol, const char *src, const char *dst_dir,
               char *out_name, size_t out_name_len, atomic_int *cancel)
{
    char tmp[FILES_PATH_MAX];
    char name[FILES_NAME_MAX + 1];
    char to[FILES_PATH_MAX];
    struct stat st;
    struct stat t;
    const char *base = files_path_base(src);
    enum files_access acc;
    int k;
    int r;

    if (out_name && out_name_len) {
        out_name[0] = '\0';
    }
    if (lstat(src, &st) != 0) {
        return -errno;
    }
    if ((acc = files_policy_dir(pol, dst_dir)) != FILES_ACCESS_OK) {
        return refusal(acc);
    }
    if (S_ISDIR(st.st_mode) && (r = check_inside(src, dst_dir)) != 0) {
        return r;
    }
    r = copy_to_temp(src, dst_dir, tmp, sizeof(tmp), cancel);
    if (r != 0) {
        return r;
    }
    /* Into place under the first free name. A name taken meanwhile is
     * simply the next one tried: nothing is ever replaced. */
    r = -EEXIST;
    for (k = 1; k < 1000 && r == -EEXIST; k++) {
        if (k == 1) {
            snprintf(name, sizeof(name), "%s", base);
        } else {
            numbered_name(name, sizeof(name), base, S_ISDIR(st.st_mode), k);
        }
        if (files_path_join(to, sizeof(to), dst_dir, name) != 0) {
            r = -ENAMETOOLONG;
            break;
        }
        r = rename_noreplace(tmp, to);
    }
    if (r != 0) {
        if (lstat(tmp, &t) == 0) {
            (void)remove_tree(tmp, t.st_dev, false, NULL);
        }
        return r;
    }
    if (out_name && out_name_len) {
        snprintf(out_name, out_name_len, "%s", name);
    }
    return 0;
}

int files_move(const struct files_policy *pol, const char *src, const char *dst_dir,
               atomic_int *cancel)
{
    char parent[FILES_PATH_MAX];
    char rp[FILES_PATH_MAX];
    char rd[FILES_PATH_MAX];
    char to[FILES_PATH_MAX];
    char tmp[FILES_PATH_MAX];
    struct stat st;
    struct stat t;
    enum files_access acc;
    int r;

    if (lstat(src, &st) != 0) {
        return -errno;
    }
    if ((acc = files_policy_entry(pol, src)) != FILES_ACCESS_OK ||
        (acc = files_policy_dir(pol, dst_dir)) != FILES_ACCESS_OK) {
        return refusal(acc);
    }
    if (files_path_parent(parent, sizeof(parent), src) != 0 ||
        canon(parent, rp, sizeof(rp), false) != 0 || canon(dst_dir, rd, sizeof(rd), false) != 0) {
        return -ENOENT;
    }
    if (strcmp(rp, rd) == 0) {
        return -FILES_ESAME;
    }
    if (S_ISDIR(st.st_mode) && (r = check_inside(src, dst_dir)) != 0) {
        return r;
    }
    if (files_path_join(to, sizeof(to), dst_dir, files_path_base(src)) != 0) {
        return -ENAMETOOLONG;
    }
    r = rename_noreplace(src, to);
    if (r != -EXDEV) {
        return r;
    }
    /* Another filesystem: copy, put the copy in place, and only then remove
     * the original. Anything that fails before the last step leaves the
     * original exactly as it was. */
    if (lstat(to, &t) == 0) {
        return -EEXIST;
    }
    if (S_ISDIR(st.st_mode) && (r = remove_tree(src, st.st_dev, true, NULL)) != 0) {
        return r; /* the original could not be removed afterwards */
    }
    r = copy_to_temp(src, dst_dir, tmp, sizeof(tmp), cancel);
    if (r != 0) {
        return r;
    }
    r = rename_noreplace(tmp, to);
    if (r != 0) {
        if (lstat(tmp, &t) == 0) {
            (void)remove_tree(tmp, t.st_dev, false, NULL);
        }
        return r;
    }
    return remove_tree(src, st.st_dev, false, NULL) == 0 ? 0 : -FILES_ESOURCE_KEPT;
}

int files_delete(const struct files_policy *pol, const char *path, atomic_int *cancel)
{
    struct stat st;
    int r;

    if (lstat(path, &st) != 0) {
        return -errno;
    }
    if (files_policy_entry(pol, path) != FILES_ACCESS_OK) {
        return -FILES_EPOLICY;
    }
    if (S_ISDIR(st.st_mode)) {
        /* The whole tree is walked first: a mount point or a depth it could
         * not finish is found before anything is gone. */
        r = remove_tree(path, st.st_dev, true, cancel);
        if (r != 0) {
            return r;
        }
    }
    return remove_tree(path, st.st_dev, false, cancel);
}

int files_read_text(const char *path, char *buf, size_t buf_len, bool *truncated)
{
    struct stat st;
    size_t got = 0;
    size_t i;
    size_t o;
    int fd;

    *truncated = false;
    if (buf_len < 2) {
        return -EINVAL;
    }
    buf[0] = '\0';
    /* Non-blocking, so a FIFO or a /proc file that waits for data cannot
     * hang the app; anything but a regular file is refused anyway. */
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    if (fstat(fd, &st) != 0) {
        int r = -errno;

        close(fd);
        return r;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return -FILES_ESPECIAL;
    }
    while (got < buf_len - 1) {
        ssize_t n = read(fd, buf + got, buf_len - 1 - got);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            {
                int r = -errno;

                close(fd);
                buf[0] = '\0';
                return r;
            }
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    if (got == buf_len - 1) {
        char more;

        *truncated = read(fd, &more, 1) == 1 || (st.st_size > 0 && (size_t)st.st_size > got);
    }
    close(fd);
    if (memchr(buf, '\0', got)) {
        buf[0] = '\0';
        return -FILES_EBINARY;
    }
    /* Displayable: CR dropped, tabs as spaces, other controls and bytes that
     * are not UTF-8 as '?', and a character cut off at the end left out. */
    buf[got] = '\0';
    for (i = 0, o = 0; i < got;) {
        unsigned char c = (unsigned char)buf[i];
        size_t n;

        if (c >= 0x80) {
            if (i + 4 > got) {
                /* Near the end: too short to hold what it starts is a cut. */
                size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;

                if (i + need > got) {
                    if (*truncated) {
                        break;
                    }
                    buf[o++] = '?';
                    i++;
                    continue;
                }
            }
            n = utf8_len((const unsigned char *)buf + i);
            if (n == 0) {
                buf[o++] = '?';
                i++;
                continue;
            }
            memmove(buf + o, buf + i, n);
            o += n;
            i += n;
            continue;
        }
        i++;
        if (c == '\r') {
            continue;
        }
        if (c == '\t') {
            c = ' ';
        } else if ((c < 0x20 && c != '\n') || c == 0x7F) {
            c = '?';
        }
        buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return (int)o;
}
