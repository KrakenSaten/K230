/*
 * peerd's persistent state (peer_store.h, ADR-017).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "peer_store.h"

#include "peer_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_MAX_LEN 512

static const uint8_t MAGIC_ID[4] = { 'P', 'D', 'I', '1' };
static const uint8_t MAGIC_EPOCH[4] = { 'P', 'D', 'E', '1' };
static const uint8_t MAGIC_KEPT[4] = { 'P', 'D', 'K', '1' };
#define STORE_VERSION 1

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int path_of(char *out, size_t n, const char *dir, const char *file)
{
    int len = snprintf(out, n, "%s/%s", dir, file);

    return len > 0 && (size_t)len < n ? 0 : -1;
}

/* What is at a path: 0 nothing, 1 a file, -1 something that cannot be looked at. */
static int exists(const char *dir, const char *file)
{
    char path[PATH_MAX_LEN];
    struct stat st;

    if (path_of(path, sizeof(path), dir, file) != 0) {
        return -1;
    }
    if (lstat(path, &st) == 0) {
        return 1;
    }
    return errno == ENOENT ? 0 : -1;
}

/* A whole file of at most max bytes. Returns its length, -1 when it is not
 * there, -2 when it cannot be read or is larger than max. */
static long read_all(const char *dir, const char *file, uint8_t *buf, size_t max)
{
    char path[PATH_MAX_LEN];
    ssize_t got;
    size_t total = 0;
    int fd;

    if (path_of(path, sizeof(path), dir, file) != 0) {
        return -2;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT ? -1 : -2;
    }
    for (;;) {
        got = read(fd, buf + total, max - total);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -2;
        }
        if (got == 0) {
            break;
        }
        total += (size_t)got;
        if (total == max) {
            uint8_t extra;

            if (read(fd, &extra, 1) != 0) {
                close(fd);
                return -2;      /* longer than anything this could be */
            }
            break;
        }
    }
    close(fd);
    return (long)total;
}

static int write_full(int fd, const uint8_t *buf, size_t n)
{
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, buf + done, n - done);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

static int sync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int rc;

    if (fd < 0) {
        return -1;
    }
    rc = fsync(fd);
    close(fd);
    return rc == 0 ? 0 : -1;
}

/* Whole or not at all: a temporary file, flushed, renamed over the name, and
 * the directory flushed so the new name survives a power cut too. */
static int write_atomic(const char *dir, const char *file, const uint8_t *buf, size_t n)
{
    char path[PATH_MAX_LEN];
    char tmp[PATH_MAX_LEN + 8];
    int fd;

    if (path_of(path, sizeof(path), dir, file) != 0) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0600) != 0 || write_full(fd, buf, n) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return sync_dir(dir);
}

/* ---- the epoch --------------------------------------------------------------- */

/* One copy: 1 and *epoch when it is valid, 0 when it is absent, -1 when it
 * is there and is not a valid epoch file. */
static int epoch_read(const char *dir, const char *file, uint64_t *epoch)
{
    uint8_t buf[PS_EPOCH_SIZE + 1];
    long n = read_all(dir, file, buf, sizeof(buf));

    if (n == -1) {
        return 0;
    }
    if (n != PS_EPOCH_SIZE || memcmp(buf, MAGIC_EPOCH, 4) != 0 ||
        get32(buf + 12) != fnv1a(buf, 12) || pw_get64(buf + 4) == 0) {
        return -1;
    }
    *epoch = pw_get64(buf + 4);
    return 1;
}

static void epoch_bytes(uint8_t buf[PS_EPOCH_SIZE], uint64_t epoch)
{
    memcpy(buf, MAGIC_EPOCH, 4);
    pw_put64(buf + 4, epoch);
    put32(buf + 12, fnv1a(buf, 12));
}

/* In place, flushed: the other copy is not touched, so a power cut here
 * leaves at least the copy that was valid before. */
static int epoch_write(const char *dir, const char *file, uint64_t epoch)
{
    char path[PATH_MAX_LEN];
    uint8_t buf[PS_EPOCH_SIZE];
    int created;
    int fd;

    if (path_of(path, sizeof(path), dir, file) != 0) {
        return -1;
    }
    epoch_bytes(buf, epoch);
    created = exists(dir, file) == 0;
    fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, 0600) != 0 || lseek(fd, 0, SEEK_SET) != 0 || write_full(fd, buf, sizeof(buf)) != 0 ||
        ftruncate(fd, PS_EPOCH_SIZE) != 0 || fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    if (close(fd) != 0) {
        return -1;
    }
    return created ? sync_dir(dir) : 0;
}

/* At first creation only, when there is no other copy to fall back on:
 * whole or not at all, so a power cut cannot leave half an epoch file and a
 * device that can never be given an identity (S8). */
static int epoch_create(const char *dir, const char *file)
{
    uint8_t buf[PS_EPOCH_SIZE];

    epoch_bytes(buf, PS_EPOCH_INITIAL);
    return write_atomic(dir, file, buf, sizeof(buf));
}

/* ---- kept peers and the name ------------------------------------------------- */

int ps_kept_save(const char *dir, const struct ps_kept *recs, int n)
{
    uint8_t buf[PS_KEPT_HEADER + PS_KEPT_MAX * PS_KEPT_RECORD + 4];
    size_t at = 0;
    int i;

    if (!dir || n < 0 || n > PS_KEPT_MAX || (n > 0 && !recs)) {
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    memcpy(buf, MAGIC_KEPT, 4);
    buf[4] = STORE_VERSION;
    buf[5] = 0;
    buf[6] = (uint8_t)n;
    buf[7] = 0;
    at = PS_KEPT_HEADER;
    for (i = 0; i < n; i++) {
        memcpy(buf + at, recs[i].key, PC_KEY);
        pw_put64(buf + at + 32, recs[i].epoch);
        pw_put64(buf + at + 40, recs[i].mark);
        put32(buf + at + 48, recs[i].addr);
        buf[at + 52] = (uint8_t)(recs[i].port >> 8);
        buf[at + 53] = (uint8_t)recs[i].port;
        at += PS_KEPT_RECORD;
    }
    put32(buf + at, fnv1a(buf, at));
    return write_atomic(dir, "kept.v1", buf, at + 4);
}

/* 0 with the list (possibly empty: no file), -1 for a file that is not a
 * kept.v1. */
static int kept_load(const char *dir, struct ps_kept *recs, int *count)
{
    uint8_t buf[PS_KEPT_HEADER + PS_KEPT_MAX * PS_KEPT_RECORD + 4 + 1];
    long n = read_all(dir, "kept.v1", buf, sizeof(buf));
    size_t at;
    int k;
    int i;
    int j;

    *count = 0;
    if (n == -1) {
        return 0;
    }
    if (n < PS_KEPT_HEADER + 4 || memcmp(buf, MAGIC_KEPT, 4) != 0 || buf[4] != STORE_VERSION ||
        buf[5] != 0 || buf[7] != 0 || buf[6] > PS_KEPT_MAX) {
        return -1;
    }
    k = buf[6];
    at = PS_KEPT_HEADER + (size_t)k * PS_KEPT_RECORD;
    if ((size_t)n != at + 4 || get32(buf + at) != fnv1a(buf, at)) {
        return -1;
    }
    for (i = 0; i < k; i++) {
        const uint8_t *r = buf + PS_KEPT_HEADER + (size_t)i * PS_KEPT_RECORD;

        memcpy(recs[i].key, r, PC_KEY);
        recs[i].epoch = pw_get64(r + 32);
        recs[i].mark = pw_get64(r + 40);
        recs[i].addr = get32(r + 48);
        recs[i].port = (uint16_t)((r[52] << 8) | r[53]);
        if (r[54] != 0 || r[55] != 0 || (recs[i].epoch == 0 && recs[i].mark != 0)) {
            return -1;
        }
        for (j = 0; j < i; j++) {
            if (memcmp(recs[j].key, recs[i].key, PC_KEY) == 0) {
                return -1;      /* one record per peer */
            }
        }
    }
    *count = k;
    return 0;
}

int ps_name_save(const char *dir, const char *name)
{
    size_t len = name ? strlen(name) : 0;

    if (!dir || len > PS_NAME_MAX || !pw_name_ok((const uint8_t *)name, len)) {
        return -1;
    }
    return write_atomic(dir, "name.v1", (const uint8_t *)name, len);
}

static void name_load(const char *dir, char *out)
{
    uint8_t buf[PS_NAME_MAX + 1];
    long n = read_all(dir, "name.v1", buf, sizeof(buf));

    out[0] = '\0';
    if (n > 0 && n <= PS_NAME_MAX && pw_name_ok(buf, (size_t)n)) {
        memcpy(out, buf, (size_t)n);
        out[n] = '\0';
    }
}

/* ---- the identity -------------------------------------------------------------- */

static int identity_write(const char *dir, const struct pc_identity *id)
{
    uint8_t buf[PS_IDENTITY_SIZE];
    int rc;

    memcpy(buf, MAGIC_ID, 4);
    buf[4] = STORE_VERSION;
    buf[5] = 0;
    pc_identity_secret(id, buf + 6);
    put32(buf + 6 + PC_SECRET, fnv1a(buf, 6 + PC_SECRET));
    rc = write_atomic(dir, "identity.v1", buf, sizeof(buf));
    pc_wipe(buf, sizeof(buf));
    return rc;
}

/* 0 with the identity loaded, -1 when the file is not a usable identity. */
static int identity_read(const char *dir, struct pc_identity *id)
{
    uint8_t buf[PS_IDENTITY_SIZE + 1];
    long n = read_all(dir, "identity.v1", buf, sizeof(buf));
    int rc = -1;

    if (n == PS_IDENTITY_SIZE && memcmp(buf, MAGIC_ID, 4) == 0 && buf[4] == STORE_VERSION &&
        buf[5] == 0 && get32(buf + 6 + PC_SECRET) == fnv1a(buf, 6 + PC_SECRET)) {
        rc = pc_identity_load(id, buf + 6);
    }
    pc_wipe(buf, sizeof(buf));
    return rc;
}

const char *ps_fault_name(enum ps_fault f)
{
    switch (f) {
    case PS_FAULT_NONE:
        return "none";
    case PS_FAULT_IDENTITY:
        return "identity_fault";
    case PS_FAULT_EPOCH:
        return "epoch_fault";
    default:
        return "store_fault";
    }
}

static int fail(struct ps_boot *out, enum ps_fault f)
{
    pc_identity_wipe(&out->id);
    out->fault = f;
    return -1;
}

int ps_boot(const char *dir, struct ps_boot *out)
{
    uint64_t ea = 0;
    uint64_t eb = 0;
    uint64_t have;
    uint64_t next;
    int ra;
    int rb;
    int has_id;
    int has_kept;
    const char *target;

    if (!dir || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        return fail(out, PS_FAULT_IO);
    }
    has_id = exists(dir, "identity.v1");
    has_kept = exists(dir, "kept.v1");
    ra = epoch_read(dir, "epoch.a", &ea);
    rb = epoch_read(dir, "epoch.b", &eb);
    if (has_id < 0 || has_kept < 0) {
        return fail(out, PS_FAULT_IO);
    }
    have = ra == 1 && (rb != 1 || ea >= eb) ? ea : (rb == 1 ? eb : 0);

    if (!has_id) {
        /* No identity. That is a new device - or the tail of a first
         * creation a power cut interrupted, which wrote the epoch files
         * first - only when nothing says this device has ever run: no epoch
         * file that is damaged or has moved on, and no kept list. */
        int untouched = ra >= 0 && rb >= 0 && (have == 0 || have == PS_EPOCH_INITIAL) &&
                        (ra != 1 || ea == PS_EPOCH_INITIAL) && (rb != 1 || eb == PS_EPOCH_INITIAL);

        if (!untouched || has_kept) {
            return fail(out, PS_FAULT_IDENTITY);
        }
        /* A copy an interrupted creation already wrote is left as it is. */
        if ((ra != 1 && epoch_create(dir, "epoch.a") != 0) ||
            (rb != 1 && epoch_create(dir, "epoch.b") != 0)) {
            return fail(out, PS_FAULT_IO);
        }
        if (pc_identity_generate(&out->id) != 0) {
            return fail(out, PS_FAULT_IDENTITY);
        }
        if (identity_write(dir, &out->id) != 0) {
            return fail(out, PS_FAULT_IO);
        }
        out->created = 1;
        ea = eb = have = PS_EPOCH_INITIAL;
        ra = rb = 1;
    } else if (identity_read(dir, &out->id) != 0) {
        return fail(out, PS_FAULT_IDENTITY);
    }

    if (have == 0) {
        return fail(out, PS_FAULT_EPOCH);   /* an identity, and no valid epoch: never from 1 again */
    }
    if (have >= PS_EPOCH_LIMIT - 2) {
        return fail(out, PS_FAULT_EPOCH);
    }
    /* One above the largest valid copy - or two, when the other copy is
     * there and unreadable: it was the one being written, so it may have
     * held have + 1, an epoch already used on the air (S16). */
    next = have + 1 + (ra < 0 || rb < 0 ? 1 : 0);
    /* Over the copy that is not the one just read as the largest. */
    target = (ra == 1 && ea == have) ? "epoch.b" : "epoch.a";
    if (epoch_write(dir, target, next) != 0) {
        return fail(out, PS_FAULT_IO);
    }
    out->epoch = next;
    /* After a repair the copies are two apart. Bring the older up to one
     * below, so that losing the newer again could not hand out this run's
     * number a second time. Best effort: this run's epoch is already
     * durable, and a failure here leaves a copy the next start repairs. */
    if (next - have > 1) {
        (void)epoch_write(dir, target[6] == 'a' ? "epoch.b" : "epoch.a", next - 1);
    }

    if (kept_load(dir, out->kept, &out->kept_n) != 0) {
        out->kept_fault = 1;
        out->kept_n = 0;
    } else {
        /* A record for our own key, or for one that cannot be used, is not
         * a kept peer; a list holding one was not written by this code. */
        int i;

        for (i = 0; i < out->kept_n; i++) {
            if (!pc_peer_ok(&out->id, out->kept[i].key)) {
                out->kept_fault = 1;
                out->kept_n = 0;
                break;
            }
        }
    }
    name_load(dir, out->name);
    out->fault = PS_FAULT_NONE;
    return 0;
}
