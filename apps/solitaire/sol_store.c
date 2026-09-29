/*
 * PG Solitaire save file. See sol_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "sol_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PATH_MAX 512
#define PILES_AT 19
#define CARDS_AT (PILES_AT + 2 * SOL_PILES)
#define SUM_AT (CARDS_AT + SOL_DECK)

static const uint8_t magic[4] = { 'P', 'G', 'S', 'L' };

static char dir_buf[STORE_PATH_MAX];
static char path_buf[STORE_PATH_MAX + 32];

const char *sol_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s", (base && *base) ? base : SOL_STORE_DEFAULT_DIR, SOL_STORE_SUBDIR);
    return dir_buf;
}

const char *sol_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", sol_store_dir(), SOL_STORE_FILE);
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

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int sol_save_encode(const struct sol_game *g, uint8_t *buf, size_t n)
{
    int at = CARDS_AT;
    int p;

    if (!g || !buf || n < SOL_SAVE_SIZE || !sol_game_valid(g)) {
        return -1;
    }
    memcpy(buf, magic, 4);
    buf[4] = (uint8_t)SOL_SAVE_VERSION;
    buf[5] = 0;
    put_u32(buf + 6, g->seed);
    put_u32(buf + 10, g->moves);
    put_u32(buf + 14, g->passes);
    buf[18] = g->won;
    for (p = 0; p < SOL_PILES; p++) {
        buf[PILES_AT + 2 * p] = g->pile[p].n;
        buf[PILES_AT + 2 * p + 1] = g->pile[p].down;
        memcpy(buf + at, g->pile[p].card, g->pile[p].n);
        at += g->pile[p].n;
    }
    put_u32(buf + SUM_AT, fnv1a(buf, SUM_AT));
    return SOL_SAVE_SIZE;
}

int sol_save_decode(struct sol_game *g, const uint8_t *buf, size_t n)
{
    struct sol_game tmp;
    int at = CARDS_AT;
    int p;

    if (!g || !buf || n != SOL_SAVE_SIZE) {
        return -1;
    }
    if (memcmp(buf, magic, 4) != 0 || get_u32(buf + SUM_AT) != fnv1a(buf, SUM_AT)) {
        return -1;
    }
    if (buf[4] != SOL_SAVE_VERSION || buf[5] != 0 || buf[18] > 1) {
        return -1;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.seed = get_u32(buf + 6);
    tmp.moves = get_u32(buf + 10);
    tmp.passes = get_u32(buf + 14);
    tmp.won = buf[18];
    for (p = 0; p < SOL_PILES; p++) {
        uint8_t count = buf[PILES_AT + 2 * p];

        /* Counts that run past the cards would read the checksum as cards;
         * refuse before copying anything. */
        if (count > SOL_DECK || at + count > SUM_AT) {
            return -1;
        }
        tmp.pile[p].n = count;
        tmp.pile[p].down = buf[PILES_AT + 2 * p + 1];
        memcpy(tmp.pile[p].card, buf + at, count);
        at += count;
    }
    if (at != SUM_AT || !sol_game_valid(&tmp)) {
        return -1;
    }
    *g = tmp;
    return 0;
}

/* ---- file ------------------------------------------------------------- */

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

int sol_store_save(const struct sol_game *g)
{
    uint8_t blob[SOL_SAVE_SIZE];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;

    if (sol_save_encode(g, blob, sizeof(blob)) != SOL_SAVE_SIZE) {
        return -1;
    }
    make_dirs(sol_store_dir());
    path = sol_store_path();
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(blob, 1, sizeof(blob), f) != sizeof(blob) || fflush(f) != 0 || fsync(fileno(f)) != 0) {
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

int sol_store_load(struct sol_game *g)
{
    uint8_t blob[SOL_SAVE_SIZE + 1];
    FILE *f;
    size_t got;

    if (!g) {
        return -1;
    }
    f = fopen(sol_store_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    if (got != SOL_SAVE_SIZE) {
        return -1;
    }
    return sol_save_decode(g, blob, got);
}
