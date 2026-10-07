/*
 * PocketTimber record file. See timber_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "timber_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PATH_MAX 512

static char dir_buf[STORE_PATH_MAX];
static char path_buf[STORE_PATH_MAX + 32];

/* ---- the records ------------------------------------------------------ */

void timber_records_init(struct timber_records *records)
{
    if (!records) {
        return;
    }
    memset(records, 0, sizeof(*records));
    timber_record_init(&records->core);
}

int timber_records_note_run(struct timber_records *records, const struct timber_score *score,
                            int cause)
{
    int best;

    if (!records || !score) {
        return 0;
    }
    best = timber_record_note_run(&records->core, score);
    if (score->pulls > records->best_pulls) {
        records->best_pulls = score->pulls;
    }
    if (cause == TIMBER_CAUSE_NONE) {
        records->summits++;
    } else {
        records->collapses++;
        if (cause > TIMBER_CAUSE_NONE && cause < TIMBER_CAUSE_COUNT) {
            records->causes[cause - 1]++;
        }
    }
    return best;
}

uint32_t timber_records_score_ceiling(void)
{
    /* The engine prices the richest pull there is: a STUCK block from the
     * bottom of the tallest tower, clean, untested, with the streak at its
     * cap. Times the most pulls a run can hold, plus every layer bonus a
     * run could earn. Generous by construction, and it moves with the
     * scoring constants because it is computed from them. */
    int64_t per_pull = timber_score_value(TIMBER_CLASS_STUCK, TIMBER_LAYERS_MAX - 1, 1, 0,
                                          TIMBER_SCORE_STREAK_CAP);
    int64_t pulls = (int64_t)TIMBER_SLOTS * TIMBER_LAYERS_MAX;
    int64_t total = per_pull * pulls + (int64_t)TIMBER_LAYERS_MAX * TIMBER_SCORE_LAYER;

    if (total < 0 || total > 0xFFFFFFFFll) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)total;
}

const char *timber_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : TIMBER_STORE_DEFAULT_DIR, TIMBER_STORE_SUBDIR);
    return dir_buf;
}

const char *timber_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", timber_store_dir(), TIMBER_STORE_FILE);
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

int timber_records_encode(const struct timber_records *records, uint8_t *buf, size_t n)
{
    struct cursor c;
    int i;

    if (!records || !buf || n < TIMBER_RECORDS_SIZE) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.buf = buf;
    c.n = n;
    put_u8(&c, TIMBER_RECORDS_MAGIC0);
    put_u8(&c, TIMBER_RECORDS_MAGIC1);
    put_u8(&c, TIMBER_RECORDS_MAGIC2);
    put_u8(&c, TIMBER_RECORDS_MAGIC3);
    put_u16(&c, TIMBER_RECORDS_VERSION);
    put_u32(&c, records->core.best_score);
    put_u16(&c, records->core.best_height);
    put_u16(&c, records->core.best_streak);
    put_u16(&c, records->best_pulls);
    put_u32(&c, records->core.runs);
    put_u32(&c, records->core.pulls);
    put_u32(&c, records->collapses);
    put_u32(&c, records->summits);
    for (i = 0; i < TIMBER_RECORDS_CAUSES; i++) {
        put_u32(&c, records->causes[i]);
    }
    put_u32(&c, fnv1a(buf, c.at));
    if (c.bad) {
        return -1;
    }
    return (int)c.at;
}

/* Could the game have produced this lifetime? A record that decodes but
 * describes something the rules cannot reach is refused: the checksum only
 * proves the bytes survived the disk, not that they ever meant anything.
 * Every check here follows from the bookkeeping above or from the tower's
 * dimensions; none of them encodes a rule, so a legitimate record is never
 * refused by a rule that later changed. */
static int records_valid(const struct timber_records *r)
{
    uint32_t caused = 0;
    int i;

    /* The tower cannot be taller than the summit, and a run cannot pull
     * more blocks than it takes to build every layer up to it. */
    if (r->core.best_height > TIMBER_LAYERS_MAX) {
        return 0;
    }
    if (r->best_pulls > TIMBER_SLOTS * TIMBER_LAYERS_MAX) {
        return 0;
    }
    /* A streak is made of pulls in one run, and the lifetime holds every
     * run's pulls. */
    if (r->core.best_streak > r->best_pulls) {
        return 0;
    }
    if (r->core.pulls < r->best_pulls) {
        return 0;
    }
    /* Points only ever come from a block pulled free. */
    if (r->core.best_score > 0 && r->core.pulls == 0) {
        return 0;
    }
    if (r->core.best_score > timber_records_score_ceiling()) {
        return 0;
    }
    /* Every run ended one way or the other, and every fall had at most one
     * cause. */
    if (r->collapses + r->summits < r->collapses) {
        return 0; /* the counters wrapped, so they were never ours */
    }
    if (r->core.runs != r->collapses + r->summits) {
        return 0;
    }
    for (i = 0; i < TIMBER_RECORDS_CAUSES; i++) {
        if (caused + r->causes[i] < caused) {
            return 0;
        }
        caused += r->causes[i];
    }
    if (caused > r->collapses) {
        return 0;
    }
    /* Nothing is counted without a run behind it. */
    if (r->core.runs == 0 &&
        (r->core.best_score || r->core.best_height || r->core.best_streak || r->core.pulls ||
         r->best_pulls)) {
        return 0;
    }
    return 1;
}

int timber_records_decode(struct timber_records *records, const uint8_t *buf, size_t n)
{
    struct timber_records tmp;
    struct cursor c;
    uint32_t stored;
    int i;

    if (!records || !buf || n != TIMBER_RECORDS_SIZE) {
        return -1;
    }
    if (buf[0] != TIMBER_RECORDS_MAGIC0 || buf[1] != TIMBER_RECORDS_MAGIC1 ||
        buf[2] != TIMBER_RECORDS_MAGIC2 || buf[3] != TIMBER_RECORDS_MAGIC3) {
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
    if (get_u16(&c) != TIMBER_RECORDS_VERSION) {
        return -1;
    }
    timber_records_init(&tmp);
    tmp.core.best_score = get_u32(&c);
    tmp.core.best_height = get_u16(&c);
    tmp.core.best_streak = get_u16(&c);
    tmp.best_pulls = get_u16(&c);
    tmp.core.runs = get_u32(&c);
    tmp.core.pulls = get_u32(&c);
    tmp.collapses = get_u32(&c);
    tmp.summits = get_u32(&c);
    for (i = 0; i < TIMBER_RECORDS_CAUSES; i++) {
        tmp.causes[i] = get_u32(&c);
    }
    if (c.bad || c.at != c.n || !records_valid(&tmp)) {
        return -1;
    }
    *records = tmp;
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

int timber_store_save(const struct timber_records *records)
{
    uint8_t blob[TIMBER_RECORDS_SIZE];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;
    int written;

    if (!records) {
        return -1;
    }
    written = timber_records_encode(records, blob, sizeof(blob));
    if (written < 0) {
        return -1;
    }
    make_dirs(timber_store_dir());
    path = timber_store_path();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    /* "wb" truncates whatever an interrupted earlier write left behind. */
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
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int timber_store_load(struct timber_records *records)
{
    uint8_t blob[TIMBER_RECORDS_SIZE + 1];
    FILE *f;
    size_t got;

    if (!records) {
        return -1;
    }
    f = fopen(timber_store_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    /* A record is exactly TIMBER_RECORDS_SIZE bytes; anything else is
     * truncated, padded or from another version. */
    if (got != TIMBER_RECORDS_SIZE) {
        return -1;
    }
    return timber_records_decode(records, blob, got);
}

int timber_store_clear(void)
{
    if (unlink(timber_store_path()) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
}
