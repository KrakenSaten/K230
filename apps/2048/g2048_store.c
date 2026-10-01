/*
 * PG 2048 save file. See g2048_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "g2048_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PATH_MAX 512

#define FLAG_WON 0x01u
#define FLAG_KEEP_GOING 0x02u
#define FLAG_OVER 0x04u

static const uint8_t magic[4] = { 'P', 'G', '4', '8' };

static char dir_buf[STORE_PATH_MAX];
static char path_buf[STORE_PATH_MAX + 32];

const char *g2048_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : G2048_STORE_DEFAULT_DIR, G2048_STORE_SUBDIR);
    return dir_buf;
}

const char *g2048_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", g2048_store_dir(), G2048_STORE_FILE);
    return path_buf;
}

/* ---- codec ------------------------------------------------------------ */

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

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (uint16_t)(v & 0xFFFFu));
    put_u16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16);
}

int g2048_save_encode(const struct g2048_game *g, uint8_t *buf, size_t n)
{
    if (!g || !buf || n < G2048_SAVE_SIZE || !g2048_game_valid(g)) {
        return -1;
    }
    memcpy(buf, magic, 4);
    put_u16(buf + 4, G2048_SAVE_VERSION);
    memcpy(buf + 6, g->cell, G2048_CELLS);
    put_u32(buf + 22, g->score);
    put_u32(buf + 26, g->best);
    put_u32(buf + 30, g->moves);
    put_u32(buf + 34, g->rng.state);
    buf[38] = (uint8_t)((g->won ? FLAG_WON : 0u) | (g->keep_going ? FLAG_KEEP_GOING : 0u) |
                        (g->over ? FLAG_OVER : 0u));
    put_u32(buf + 39, fnv1a(buf, 39));
    return G2048_SAVE_SIZE;
}

int g2048_save_decode(struct g2048_game *g, const uint8_t *buf, size_t n)
{
    struct g2048_game tmp;

    if (!g || !buf || n != G2048_SAVE_SIZE) {
        return -1;
    }
    if (memcmp(buf, magic, 4) != 0 || get_u32(buf + 39) != fnv1a(buf, 39)) {
        return -1;
    }
    if (get_u16(buf + 4) != G2048_SAVE_VERSION) {
        return -1;
    }
    if (buf[38] & ~(FLAG_WON | FLAG_KEEP_GOING | FLAG_OVER)) {
        return -1; /* a flag this version never writes */
    }
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.cell, buf + 6, G2048_CELLS);
    tmp.score = get_u32(buf + 22);
    tmp.best = get_u32(buf + 26);
    tmp.moves = get_u32(buf + 30);
    tmp.rng.state = get_u32(buf + 34);
    tmp.won = (buf[38] & FLAG_WON) != 0;
    tmp.keep_going = (buf[38] & FLAG_KEEP_GOING) != 0;
    tmp.over = (buf[38] & FLAG_OVER) != 0;
    if (!g2048_game_valid(&tmp)) {
        return -1;
    }
    *g = tmp;
    return 0;
}

/* ---- file ------------------------------------------------------------- */

/* Create every missing component of the directory. Best effort: a failure
 * shows up as a failed write, which the caller already tolerates. */
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

int g2048_store_save(const struct g2048_game *g)
{
    uint8_t blob[G2048_SAVE_SIZE];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;

    if (g2048_save_encode(g, blob, sizeof(blob)) != G2048_SAVE_SIZE) {
        return -1;
    }
    make_dirs(g2048_store_dir());
    path = g2048_store_path();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(blob, 1, sizeof(blob), f) != sizeof(blob) || fflush(f) != 0 ||
        fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int g2048_store_load(struct g2048_game *g)
{
    uint8_t blob[G2048_SAVE_SIZE + 1];
    FILE *f;
    size_t got;

    if (!g) {
        return -1;
    }
    f = fopen(g2048_store_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    /* Exactly G2048_SAVE_SIZE bytes; a longer read means a longer file. */
    if (got != G2048_SAVE_SIZE) {
        return -1;
    }
    return g2048_save_decode(g, blob, got);
}
