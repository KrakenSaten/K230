/*
 * PocketRadar record file. See radar_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "radar_store.h"

#include "engine/radar_rules.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PATH_MAX 512

static char dir_buf[STORE_PATH_MAX];
static char path_buf[STORE_PATH_MAX + 32];

const char *radar_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : RADAR_STORE_DEFAULT_DIR, RADAR_STORE_SUBDIR);
    return dir_buf;
}

const char *radar_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", radar_store_dir(), RADAR_STORE_FILE);
    return path_buf;
}

/* ---- codec ------------------------------------------------------------ */

struct cursor {
    uint8_t *buf;
    const uint8_t *src;
    size_t n;
    size_t at;
    int bad;
};

static uint32_t fnv1a(const uint8_t *data, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static void put_u8(struct cursor *c, uint8_t v)
{
    if (c->bad || c->at + 1 > c->n) {
        c->bad = 1;
        return;
    }
    c->buf[c->at++] = v;
}

static void put_u16(struct cursor *c, uint16_t v)
{
    put_u8(c, (uint8_t)(v & 0xFFu));
    put_u8(c, (uint8_t)((v >> 8) & 0xFFu));
}

static void put_u32(struct cursor *c, uint32_t v)
{
    put_u16(c, (uint16_t)(v & 0xFFFFu));
    put_u16(c, (uint16_t)((v >> 16) & 0xFFFFu));
}

static uint8_t get_u8(struct cursor *c)
{
    if (c->bad || c->at + 1 > c->n) {
        c->bad = 1;
        return 0;
    }
    return c->src[c->at++];
}

static uint16_t get_u16(struct cursor *c)
{
    uint16_t lo = get_u8(c);

    return (uint16_t)(lo | ((uint16_t)get_u8(c) << 8));
}

static uint32_t get_u32(struct cursor *c)
{
    uint32_t lo = get_u16(c);

    return lo | ((uint32_t)get_u16(c) << 16);
}

int radar_record_encode(const struct radar_record *record, uint8_t *buf, size_t n)
{
    struct cursor c;

    if (!record || !buf || n < RADAR_RECORD_SIZE) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.buf = buf;
    c.n = n;
    put_u8(&c, RADAR_RECORD_MAGIC0);
    put_u8(&c, RADAR_RECORD_MAGIC1);
    put_u8(&c, RADAR_RECORD_MAGIC2);
    put_u8(&c, RADAR_RECORD_MAGIC3);
    put_u16(&c, RADAR_RECORD_VERSION);
    put_u32(&c, record->best_score);
    put_u16(&c, record->best_streak);
    put_u16(&c, record->best_level);
    put_u32(&c, record->runs);
    put_u32(&c, record->engaged);
    put_u32(&c, record->mistakes);
    put_u32(&c, fnv1a(buf, c.at));
    if (c.bad) {
        return -1;
    }
    return (int)c.at;
}

/* Could the game have produced this lifetime? A record that decodes but
 * describes something the rules cannot reach is refused, the same way
 * PocketFleet refuses an impossible board: the checksum only proves the
 * bytes survived the disk, not that they ever meant anything. */
static int record_valid(const struct radar_record *r)
{
    if (r->best_level > RADAR_LEVEL_MAX) {
        return 0;
    }
    /* A streak is made of engagements, so it cannot exceed their total. */
    if (r->best_streak > r->engaged) {
        return 0;
    }
    /* Points only ever come from an engagement, and every counter belongs
     * to some run. */
    if (r->best_score > 0 && r->engaged == 0) {
        return 0;
    }
    if (r->runs == 0 &&
        (r->best_score || r->best_streak || r->best_level || r->engaged ||
         r->mistakes)) {
        return 0;
    }
    if (r->engaged + r->mistakes < r->engaged) {
        return 0; /* the counters wrapped, so they were never ours */
    }
    return 1;
}

int radar_record_decode(struct radar_record *record, const uint8_t *buf, size_t n)
{
    struct radar_record tmp;
    struct cursor c;
    uint32_t stored;

    if (!record || !buf || n != RADAR_RECORD_SIZE) {
        return -1;
    }
    if (buf[0] != RADAR_RECORD_MAGIC0 || buf[1] != RADAR_RECORD_MAGIC1 ||
        buf[2] != RADAR_RECORD_MAGIC2 || buf[3] != RADAR_RECORD_MAGIC3) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.src = buf;
    c.n = n;
    c.at = n - 4;
    stored = get_u32(&c);
    if (c.bad || stored != fnv1a(buf, n - 4)) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.src = buf;
    c.n = n - 4;
    c.at = 4;
    if (get_u16(&c) != RADAR_RECORD_VERSION) {
        return -1;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.best_score = get_u32(&c);
    tmp.best_streak = get_u16(&c);
    tmp.best_level = get_u16(&c);
    tmp.runs = get_u32(&c);
    tmp.engaged = get_u32(&c);
    tmp.mistakes = get_u32(&c);
    if (c.bad || c.at != c.n || !record_valid(&tmp)) {
        return -1;
    }
    *record = tmp;
    return 0;
}

/* ---- file ------------------------------------------------------------- */

/* Create every missing component of the record directory. Best effort: a
 * failure here surfaces as a failed write, which the caller already
 * tolerates. */
static void make_dirs(const char *dir)
{
    char work[STORE_PATH_MAX];
    size_t i;

    snprintf(work, sizeof(work), "%s", dir);
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        mkdir(work, 0755);
        work[i] = '/';
    }
    mkdir(work, 0755);
}

int radar_store_save(const struct radar_record *record)
{
    uint8_t blob[RADAR_RECORD_SIZE];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;
    int written;

    if (!record) {
        return -1;
    }
    written = radar_record_encode(record, blob, sizeof(blob));
    if (written < 0) {
        return -1;
    }
    make_dirs(radar_store_dir());
    path = radar_store_path();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(blob, 1, (size_t)written, f) != (size_t)written ||
        fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int radar_store_load(struct radar_record *record)
{
    uint8_t blob[RADAR_RECORD_SIZE + 1];
    FILE *f;
    size_t got;

    if (!record) {
        return -1;
    }
    f = fopen(radar_store_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    /* A record is exactly RADAR_RECORD_SIZE bytes; anything else is
     * truncated, padded or from another version. */
    if (got != RADAR_RECORD_SIZE) {
        return -1;
    }
    return radar_record_decode(record, blob, got);
}

int radar_store_clear(void)
{
    if (unlink(radar_store_path()) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
}
