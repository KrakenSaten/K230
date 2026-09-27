/*
 * A recording on disk. See rec_file.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "rec_file.h"

#include "pocketwav/pocketwav.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* The hidden name a recording is born under: flocked before it is renamed
 * to <name>.part, so a .part is locked from the instant it exists and the
 * repair can never mistake a writer that has just started for a dead one. */
#define NEW_PREFIX ".new-"
#define CHUNK_FRAMES 1024

int64_t rec_file_fail_after = -1;

static int errno_or(int fallback)
{
    return errno ? errno : fallback;
}

/* rename without replacing anything: RENAME_NOREPLACE, or link and unlink
 * where the kernel or filesystem lacks it (both refuse an existing name). */
static int rename_noreplace(int dirfd, const char *from, const char *to)
{
    if (renameat2(dirfd, from, dirfd, to, RENAME_NOREPLACE) == 0) {
        return 0;
    }
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) {
        return -errno;
    }
    if (linkat(dirfd, from, dirfd, to, 0) != 0) {
        return -errno;
    }
    return unlinkat(dirfd, from, 0) == 0 ? 0 : -errno;
}

static int write_all(int fd, const void *buf, size_t n, size_t *done)
{
    const char *p = buf;

    *done = 0;
    while (*done < n) {
        ssize_t w = write(fd, p + *done, n - *done);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (w == 0) {
            return -EIO;
        }
        *done += (size_t)w;
    }
    return 0;
}

static int write_header(struct rec_file *f)
{
    uint8_t h[POCKETWAV_HEADER_BYTES];
    size_t done = 0;

    pocketwav_header(h, f->rate, 1, (uint32_t)f->data_bytes);
    while (done < sizeof(h)) {
        ssize_t w = pwrite(f->fd, h + done, sizeof(h) - done, (off_t)done);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (w == 0) {
            return -EIO;
        }
        done += (size_t)w;
    }
    return 0;
}

static void reset(struct rec_file *f)
{
    memset(f, 0, sizeof(*f));
    f->fd = -1;
    f->dirfd = -1;
}

int rec_file_create(struct rec_file *f, const char *dir, const char *name, unsigned rate)
{
    char born[REC_NAME_MAX + sizeof(NEW_PREFIX) + sizeof(REC_PART_SUFFIX)];
    uint8_t h[POCKETWAV_HEADER_BYTES];
    struct stat st;
    size_t done;
    int e = 0;

    reset(f);
    if (!dir || !name || !rec_name_parse(name, NULL) || rate < POCKETWAV_MIN_RATE ||
        rate > POCKETWAV_MAX_RATE || strlen(dir) >= sizeof(f->dir)) {
        return -EINVAL;
    }
    snprintf(f->dir, sizeof(f->dir), "%s", dir);
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->part, sizeof(f->part), "%s" REC_PART_SUFFIX, name);
    snprintf(born, sizeof(born), NEW_PREFIX "%s", f->part);
    f->rate = rate;

    f->dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (f->dirfd < 0) {
        e = -errno;
        reset(f);
        return e;
    }
    if (fstatat(f->dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
        fstatat(f->dirfd, f->part, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        close(f->dirfd);
        reset(f);
        return -EEXIST;
    }
    f->fd = openat(f->dirfd, born, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (f->fd < 0) {
        e = -errno;
        close(f->dirfd);
        reset(f);
        return e;
    }
    pocketwav_header(h, rate, 1, 0);
    errno = 0;
    if (flock(f->fd, LOCK_EX | LOCK_NB) != 0 || (e = write_all(f->fd, h, sizeof(h), &done)) != 0 ||
        fdatasync(f->fd) != 0) {
        e = e ? e : -errno_or(EIO);
        goto fail;
    }
    e = rename_noreplace(f->dirfd, born, f->part);
    if (e != 0) {
        goto fail;
    }
    if (fsync(f->dirfd) != 0) {
        e = -errno_or(EIO);
        unlinkat(f->dirfd, f->part, 0);
        close(f->fd);
        close(f->dirfd);
        reset(f);
        return e;
    }
    return 0;

fail:
    unlinkat(f->dirfd, born, 0);
    close(f->fd);
    close(f->dirfd);
    reset(f);
    return e;
}

uint64_t rec_file_room(const struct rec_file *f)
{
    return f->data_bytes < POCKETWAV_MAX_DATA_BYTES ? POCKETWAV_MAX_DATA_BYTES - f->data_bytes : 0;
}

int rec_file_append(struct rec_file *f, const int16_t *samples, size_t frames)
{
    uint8_t buf[CHUNK_FRAMES * 2];
    size_t off = 0;

    if (f->fd < 0 || (!samples && frames)) {
        return -EBADF;
    }
    if ((uint64_t)frames * 2u > rec_file_room(f)) {
        return -EFBIG;
    }
    while (off < frames) {
        size_t k = frames - off < CHUNK_FRAMES ? frames - off : CHUNK_FRAMES;
        size_t bytes = k * 2;
        size_t done = 0;
        size_t i;
        int e = 0;

        for (i = 0; i < k; i++) {
            uint16_t v = (uint16_t)samples[off + i];

            buf[2 * i] = (uint8_t)v;
            buf[2 * i + 1] = (uint8_t)(v >> 8);
        }
        if (rec_file_fail_after >= 0 && f->data_bytes + bytes > (uint64_t)rec_file_fail_after) {
            /* The test seam: the disk is full from here on. */
            bytes = f->data_bytes < (uint64_t)rec_file_fail_after
                        ? (size_t)((uint64_t)rec_file_fail_after - f->data_bytes)
                        : 0;
            if (bytes) {
                e = write_all(f->fd, buf, bytes, &done);
            }
            e = e ? e : -ENOSPC;
        } else {
            e = write_all(f->fd, buf, bytes, &done);
        }
        /* Whole frames that reached the file count; a stray half frame is
         * cut when the file is finished or repaired. */
        f->data_bytes += done - done % 2;
        if (e != 0) {
            return e;
        }
        off += k;
    }
    return 0;
}

int rec_file_checkpoint(struct rec_file *f)
{
    int e;

    if (f->fd < 0) {
        return -EBADF;
    }
    e = write_header(f);
    if (e == 0 && f->data_bytes > f->kicked_to) {
        /* Start writeback of what is new, without waiting for it: the
         * capture must not stall behind the card. */
        sync_file_range(f->fd, (off64_t)(POCKETWAV_HEADER_BYTES + f->kicked_to),
                        (off64_t)(f->data_bytes - f->kicked_to), SYNC_FILE_RANGE_WRITE);
        f->kicked_to = f->data_bytes;
    }
    return e;
}

static void close_all(struct rec_file *f)
{
    if (f->fd >= 0) {
        close(f->fd); /* releases the flock */
    }
    if (f->dirfd >= 0) {
        close(f->dirfd);
    }
    reset(f);
}

int rec_file_finish(struct rec_file *f, char *final, size_t final_len)
{
    struct rec_name parts;
    char name[REC_NAME_MAX];
    int e;

    if (final && final_len) {
        final[0] = '\0';
    }
    if (f->fd < 0) {
        return -EBADF;
    }
    if (f->data_bytes == 0) {
        unlinkat(f->dirfd, f->part, 0);
        fsync(f->dirfd);
        close_all(f);
        return -ENODATA;
    }
    errno = 0;
    if (ftruncate(f->fd, (off_t)(POCKETWAV_HEADER_BYTES + f->data_bytes)) != 0 ||
        (e = write_header(f)) != 0 || fsync(f->fd) != 0) {
        e = -errno_or(EIO);
        close_all(f);
        return e;
    }
    snprintf(name, sizeof(name), "%s", f->name);
    rec_name_parse(f->name, &parts);
    for (;;) {
        e = rename_noreplace(f->dirfd, f->part, name);
        if (e != -EEXIST) {
            break;
        }
        /* Somebody made a file under our name while we recorded: take the
         * next free one rather than replacing theirs. */
        parts.dup = parts.dup < 2 ? 2 : parts.dup + 1;
        if (parts.dup > REC_DUP_MAX || rec_name_build(name, sizeof(name), &parts) != 0) {
            break;
        }
    }
    if (e == 0 && fsync(f->dirfd) != 0) {
        e = -errno_or(EIO);
    }
    if (e == 0 && final && final_len) {
        snprintf(final, final_len, "%s", name);
    }
    close_all(f);
    return e;
}

void rec_file_abandon(struct rec_file *f)
{
    if (f->fd >= 0 && f->data_bytes == 0) {
        unlinkat(f->dirfd, f->part, 0);
    } else if (f->fd >= 0) {
        write_header(f);
    }
    close_all(f);
}

/* ---- repair ------------------------------------------------------------ */

static int repair_one(int dirfd, const char *part, rec_recover_fn fn, void *user)
{
    struct pocketwav_info info;
    struct rec_name parts;
    char base[REC_NAME_MAX];
    char name[REC_NAME_MAX];
    size_t n = strlen(part) - strlen(REC_PART_SUFFIX);
    int fd = openat(dirfd, part, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    int rc;
    int e;

    if (fd < 0) {
        return 0;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return 0;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        if (fn) {
            fn(REC_RECOVER_LIVE, part, user);
        }
        return 0;
    }
    rc = pocketwav_fix_fd(fd, &info);
    if (rc != POCKETWAV_OK && (uint64_t)st.st_size > POCKETWAV_HEADER_BYTES) {
        /* Not something we can truthfully call a recording. */
        close(fd);
        if (fn) {
            fn(REC_RECOVER_DAMAGED, part, user);
        }
        return 1;
    }
    if (rc != POCKETWAV_OK || info.frames == 0) {
        unlinkat(dirfd, part, 0);
        fsync(dirfd);
        close(fd);
        if (fn) {
            fn(REC_RECOVER_EMPTY, part, user);
        }
        return 1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        if (fn) {
            fn(REC_RECOVER_DAMAGED, part, user);
        }
        return 1;
    }
    memcpy(base, part, n);
    base[n] = '\0';
    rec_name_parse(base, &parts);
    parts.recovered = true;
    e = -EEXIST;
    while (e == -EEXIST && rec_name_build(name, sizeof(name), &parts) == 0) {
        e = rename_noreplace(dirfd, part, name);
        if (e == -EEXIST) {
            parts.dup = parts.dup < 2 ? 2 : parts.dup + 1;
        }
    }
    if (e == 0) {
        fsync(dirfd);
    }
    close(fd);
    if (fn) {
        fn(e == 0 ? REC_RECOVER_REPAIRED : REC_RECOVER_DAMAGED, e == 0 ? name : part, user);
    }
    return 1;
}

/* A writer that died in the instant between creating its hidden file and
 * renaming it: nothing recorded, nothing to show. Removed when unlocked. */
static void sweep_new(int dirfd, const char *name)
{
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        return;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        unlinkat(dirfd, name, 0);
    }
    close(fd);
}

int rec_file_recover_dir(const char *dir, rec_recover_fn fn, void *user)
{
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d;
    struct dirent *e;
    int count = 0;

    if (dirfd < 0) {
        return -errno;
    }
    d = fdopendir(dup(dirfd));
    if (!d) {
        int err = -errno;

        close(dirfd);
        return err;
    }
    while ((e = readdir(d)) != NULL) {
        if (rec_name_is_part(e->d_name)) {
            count += repair_one(dirfd, e->d_name, fn, user);
        } else if (strncmp(e->d_name, NEW_PREFIX, strlen(NEW_PREFIX)) == 0 &&
                   rec_name_is_part(e->d_name + strlen(NEW_PREFIX))) {
            sweep_new(dirfd, e->d_name);
        }
    }
    closedir(d);
    close(dirfd);
    return count;
}
