/*
 * PocketFleet save codec. See fleet_save.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_save.h"

#include <string.h>

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

/* ---- writing ---------------------------------------------------------- */

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

static void put_bytes(struct cursor *c, const uint8_t *data, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        put_u8(c, data[i]);
    }
}

/* ---- reading ---------------------------------------------------------- */

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

static void get_bytes(struct cursor *c, uint8_t *data, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        data[i] = get_u8(c);
    }
}

/* ---- encode ----------------------------------------------------------- */

size_t fleet_save_size(void)
{
    return FLEET_SAVE_SIZE;
}

static void encode_board(struct cursor *c, const struct fleet_board *b)
{
    int i;

    put_bytes(c, b->ship_at, FLEET_CELLS);
    put_bytes(c, b->shot, FLEET_CELLS);
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        put_u8(c, b->ships[i].row);
        put_u8(c, b->ships[i].col);
        put_u8(c, b->ships[i].orient);
        put_u8(c, b->ships[i].hits);
        put_u8(c, b->ships[i].placed);
    }
    put_u8(c, b->ships_placed);
    put_u8(c, b->ships_afloat);
}

int fleet_save_encode(const struct fleet_game *game, uint8_t *buf, size_t n)
{
    struct cursor c;
    int side;

    if (!game || !buf || n < FLEET_SAVE_SIZE) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.buf = buf;
    c.n = n;

    put_u8(&c, FLEET_SAVE_MAGIC0);
    put_u8(&c, FLEET_SAVE_MAGIC1);
    put_u8(&c, FLEET_SAVE_MAGIC2);
    put_u8(&c, FLEET_SAVE_MAGIC3);
    put_u16(&c, FLEET_SAVE_VERSION);
    put_u8(&c, game->difficulty);
    put_u8(&c, game->phase);
    put_u8(&c, game->winner);
    put_u16(&c, game->turn);
    put_u32(&c, game->seed);
    put_u32(&c, game->rng_setup.state);
    put_u32(&c, game->rng_ai.state);
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        encode_board(&c, &game->board[side]);
    }
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        put_u16(&c, game->stats[side].shots);
        put_u16(&c, game->stats[side].hits);
    }
    put_u8(&c, game->ai.difficulty);
    put_u16(&c, game->ai.shots);
    put_bytes(&c, game->ai.shot, FLEET_CELLS);
    put_bytes(&c, game->ai.result, FLEET_CELLS);
    put_bytes(&c, game->ai.sunk, FLEET_SHIP_COUNT);
    put_bytes(&c, game->ai.resolved, FLEET_CELLS);
    put_bytes(&c, game->ai.queue, FLEET_CELLS);
    put_u8(&c, game->ai.queue_len);
    put_u32(&c, fnv1a(buf, c.at));
    if (c.bad || c.at != FLEET_SAVE_SIZE) {
        return -1;
    }
    return (int)c.at;
}

/* ---- decode ----------------------------------------------------------- */

static void decode_board(struct cursor *c, struct fleet_board *b)
{
    int i;

    get_bytes(c, b->ship_at, FLEET_CELLS);
    get_bytes(c, b->shot, FLEET_CELLS);
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        b->ships[i].row = get_u8(c);
        b->ships[i].col = get_u8(c);
        b->ships[i].orient = get_u8(c);
        b->ships[i].hits = get_u8(c);
        b->ships[i].placed = get_u8(c);
    }
    b->ships_placed = get_u8(c);
    b->ships_afloat = get_u8(c);
}

/* Every invariant fleet_rules.c maintains. A save that fails any of them was
 * damaged or forged and must not be resumed. */
static int board_valid(const struct fleet_board *b)
{
    uint8_t covered[FLEET_CELLS];
    int placed = 0;
    int afloat = 0;
    int i;

    memset(covered, FLEET_NO_SHIP, sizeof(covered));
    for (i = 0; i < FLEET_CELLS; i++) {
        if (b->ship_at[i] != FLEET_NO_SHIP && b->ship_at[i] >= FLEET_SHIP_COUNT) {
            return 0;
        }
        if (b->shot[i] > 1) {
            return 0;
        }
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        const struct fleet_ship_state *s = &b->ships[i];
        int length = fleet_ship_length((enum fleet_ship)i);
        int n;

        if (s->placed > 1) {
            return 0;
        }
        if (!s->placed) {
            /* An unplaced ship must be blank and must own no cell. */
            if (s->row || s->col || s->orient || s->hits) {
                return 0;
            }
            continue;
        }
        if (s->orient > FLEET_VERTICAL || s->hits > length) {
            return 0;
        }
        placed++;
        afloat += s->hits < length;
        for (n = 0; n < length; n++) {
            int row = s->row + (s->orient == FLEET_VERTICAL ? n : 0);
            int col = s->col + (s->orient == FLEET_VERTICAL ? 0 : n);
            int idx = fleet_index(row, col);

            if (idx < 0 || b->ship_at[idx] != (uint8_t)i || covered[idx] != FLEET_NO_SHIP) {
                return 0;
            }
            covered[idx] = (uint8_t)i;
        }
    }
    /* ship_at may not claim cells no ship covers. */
    for (i = 0; i < FLEET_CELLS; i++) {
        if (b->ship_at[i] != covered[i]) {
            return 0;
        }
    }
    if (b->ships_placed != placed) {
        return 0;
    }
    /* Ships never placed still count as afloat, exactly as fleet_board_clear
     * leaves them. */
    return b->ships_afloat == FLEET_SHIP_COUNT - (placed - afloat);
}

static int game_valid(const struct fleet_game *g)
{
    int side;
    int i;

    if (g->difficulty >= FLEET_DIFFICULTY_COUNT || g->phase > FLEET_PHASE_OVER) {
        return 0;
    }
    if (g->winner != FLEET_SIDE_NONE && g->winner >= FLEET_SIDE_COUNT) {
        return 0;
    }
    if ((g->phase == FLEET_PHASE_OVER) != (g->winner != FLEET_SIDE_NONE)) {
        return 0;
    }
    /* The turn counter advances after the opponent's shot, so it can reach
     * one more than the largest possible number of shots per side. */
    if (g->turn > FLEET_CELLS + 1) {
        return 0;
    }
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        if (!board_valid(&g->board[side])) {
            return 0;
        }
        if (g->stats[side].shots > FLEET_CELLS ||
            g->stats[side].hits > g->stats[side].shots) {
            return 0;
        }
    }
    if (g->ai.difficulty != g->difficulty || g->ai.shots > FLEET_CELLS ||
        g->ai.queue_len > FLEET_AI_QUEUE_MAX) {
        return 0;
    }
    for (i = 0; i < FLEET_CELLS; i++) {
        if (g->ai.shot[i] > 1 || g->ai.resolved[i] > 1 ||
            g->ai.result[i] > FLEET_SHOT_SUNK) {
            return 0;
        }
        /* Every cell the AI believes it fired at was fired at for real. */
        if (g->ai.shot[i] && !g->board[FLEET_SIDE_PLAYER].shot[i]) {
            return 0;
        }
        if (!g->ai.shot[i] && g->ai.result[i] != FLEET_SHOT_INVALID) {
            return 0;
        }
    }
    for (i = 0; i < g->ai.queue_len; i++) {
        if (g->ai.queue[i] >= FLEET_CELLS) {
            return 0;
        }
    }
    for (i = 0; i < FLEET_SHIP_COUNT; i++) {
        if (g->ai.sunk[i] > 1) {
            return 0;
        }
    }
    return 1;
}

int fleet_save_decode(struct fleet_game *game, const uint8_t *buf, size_t n)
{
    struct fleet_game tmp;
    struct cursor c;
    int side;

    if (!game || !buf || n < FLEET_SAVE_SIZE) {
        return -1;
    }
    if (buf[0] != FLEET_SAVE_MAGIC0 || buf[1] != FLEET_SAVE_MAGIC1 ||
        buf[2] != FLEET_SAVE_MAGIC2 || buf[3] != FLEET_SAVE_MAGIC3) {
        return -1;
    }
    if (fnv1a(buf, FLEET_SAVE_SIZE - 4) !=
        ((uint32_t)buf[FLEET_SAVE_SIZE - 4] |
         ((uint32_t)buf[FLEET_SAVE_SIZE - 3] << 8) |
         ((uint32_t)buf[FLEET_SAVE_SIZE - 2] << 16) |
         ((uint32_t)buf[FLEET_SAVE_SIZE - 1] << 24))) {
        return -1;
    }
    memset(&tmp, 0, sizeof(tmp));
    memset(&c, 0, sizeof(c));
    c.src = buf;
    c.n = n;
    c.at = 4;
    if (get_u16(&c) != FLEET_SAVE_VERSION) {
        return -1;
    }
    tmp.difficulty = get_u8(&c);
    tmp.phase = get_u8(&c);
    tmp.winner = get_u8(&c);
    tmp.turn = get_u16(&c);
    tmp.seed = get_u32(&c);
    tmp.rng_setup.state = get_u32(&c);
    tmp.rng_ai.state = get_u32(&c);
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        decode_board(&c, &tmp.board[side]);
    }
    for (side = 0; side < FLEET_SIDE_COUNT; side++) {
        tmp.stats[side].shots = get_u16(&c);
        tmp.stats[side].hits = get_u16(&c);
    }
    tmp.ai.difficulty = get_u8(&c);
    tmp.ai.shots = get_u16(&c);
    get_bytes(&c, tmp.ai.shot, FLEET_CELLS);
    get_bytes(&c, tmp.ai.result, FLEET_CELLS);
    get_bytes(&c, tmp.ai.sunk, FLEET_SHIP_COUNT);
    get_bytes(&c, tmp.ai.resolved, FLEET_CELLS);
    get_bytes(&c, tmp.ai.queue, FLEET_CELLS);
    tmp.ai.queue_len = get_u8(&c);
    if (c.bad || !game_valid(&tmp)) {
        return -1;
    }
    *game = tmp;
    return 0;
}
