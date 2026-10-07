/*
 * PG Blackjack save file. See bj_store.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "bj_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PATH_MAX 512
#define SHOE_AT 38
#define PLAYER_AT (SHOE_AT + 1 + BJ_SHOE)
#define DEALER_AT (PLAYER_AT + 1 + BJ_HAND_MAX)
#define SUM_AT (DEALER_AT + 1 + BJ_HAND_MAX)
#define NO_CARD 0xFF

_Static_assert(SUM_AT + 4 == BJ_SAVE_SIZE, "bj_store.h layout and BJ_SAVE_SIZE disagree");

static const uint8_t magic[4] = { 'P', 'G', 'B', 'J' };

static char dir_buf[STORE_PATH_MAX];
static char path_buf[STORE_PATH_MAX + 32];

const char *bj_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s", (base && *base) ? base : BJ_STORE_DEFAULT_DIR, BJ_STORE_SUBDIR);
    return dir_buf;
}

const char *bj_store_path(void)
{
    snprintf(path_buf, sizeof(path_buf), "%s/%s", bj_store_dir(), BJ_STORE_FILE);
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

static void put_cards(uint8_t *p, const bj_card_t *card, uint8_t n, int slots)
{
    p[0] = n;
    memset(p + 1, NO_CARD, (size_t)slots);
    memcpy(p + 1, card, n);
}

/* 0 when the count fits and every slot past it is empty. */
static int get_cards(const uint8_t *p, bj_card_t *card, uint8_t *n, int slots)
{
    int i;

    if (p[0] > slots) {
        return -1;
    }
    for (i = p[0]; i < slots; i++) {
        if (p[1 + i] != NO_CARD) {
            return -1;
        }
    }
    *n = p[0];
    memcpy(card, p + 1, p[0]);
    return 0;
}

int bj_save_encode(const struct bj_game *g, uint8_t *buf, size_t n)
{
    if (!g || !buf || n < BJ_SAVE_SIZE || !bj_game_valid(g)) {
        return -1;
    }
    memcpy(buf, magic, 4);
    buf[4] = (uint8_t)BJ_SAVE_VERSION;
    buf[5] = 0;
    put_u32(buf + 6, g->bankroll);
    put_u32(buf + 10, g->bet);
    put_u32(buf + 14, g->stake);
    put_u32(buf + 18, (uint32_t)g->last_delta);
    put_u32(buf + 22, g->rounds);
    buf[26] = (uint8_t)g->phase;
    buf[27] = (uint8_t)g->outcome;
    buf[28] = g->doubled;
    buf[29] = g->reshuffled;
    put_u32(buf + 30, g->shoe.rng.state);
    put_u32(buf + 34, g->shoe.shuffles);
    put_cards(buf + SHOE_AT, g->shoe.card, g->shoe.n, BJ_SHOE);
    put_cards(buf + PLAYER_AT, g->player.card, g->player.n, BJ_HAND_MAX);
    put_cards(buf + DEALER_AT, g->dealer.card, g->dealer.n, BJ_HAND_MAX);
    put_u32(buf + SUM_AT, fnv1a(buf, SUM_AT));
    return BJ_SAVE_SIZE;
}

int bj_save_decode(struct bj_game *g, const uint8_t *buf, size_t n)
{
    struct bj_game tmp;

    if (!g || !buf || n != BJ_SAVE_SIZE) {
        return -1;
    }
    if (memcmp(buf, magic, 4) != 0 || get_u32(buf + SUM_AT) != fnv1a(buf, SUM_AT)) {
        return -1;
    }
    if (buf[4] != BJ_SAVE_VERSION || buf[5] != 0 || buf[26] > BJ_SETTLED || buf[27] > BJ_DEALER_BLACKJACK) {
        return -1;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.bankroll = get_u32(buf + 6);
    tmp.bet = get_u32(buf + 10);
    tmp.stake = get_u32(buf + 14);
    tmp.last_delta = (int32_t)get_u32(buf + 18);
    tmp.rounds = get_u32(buf + 22);
    tmp.phase = (enum bj_phase)buf[26];
    tmp.outcome = (enum bj_outcome)buf[27];
    tmp.doubled = buf[28];
    tmp.reshuffled = buf[29];
    tmp.shoe.rng.state = get_u32(buf + 30);
    tmp.shoe.shuffles = get_u32(buf + 34);
    if (get_cards(buf + SHOE_AT, tmp.shoe.card, &tmp.shoe.n, BJ_SHOE) != 0 ||
        get_cards(buf + PLAYER_AT, tmp.player.card, &tmp.player.n, BJ_HAND_MAX) != 0 ||
        get_cards(buf + DEALER_AT, tmp.dealer.card, &tmp.dealer.n, BJ_HAND_MAX) != 0) {
        return -1;
    }
    if (!bj_game_valid(&tmp)) {
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

int bj_store_save(const struct bj_game *g)
{
    uint8_t blob[BJ_SAVE_SIZE];
    char tmp[sizeof(path_buf) + 8];
    const char *path;
    FILE *f;

    if (bj_save_encode(g, blob, sizeof(blob)) != BJ_SAVE_SIZE) {
        return -1;
    }
    make_dirs(bj_store_dir());
    path = bj_store_path();
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

int bj_store_load(struct bj_game *g)
{
    uint8_t blob[BJ_SAVE_SIZE + 1];
    FILE *f;
    size_t got;

    if (!g) {
        return -1;
    }
    f = fopen(bj_store_path(), "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }
    got = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    if (got != BJ_SAVE_SIZE) {
        return -1;
    }
    return bj_save_decode(g, blob, got);
}
