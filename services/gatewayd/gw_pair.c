/*
 * gatewayd: the pairing state machine. See gw_pair.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_pair.h"

#include "gw_util.h"

#include <string.h>

/* memset that the compiler may not drop: the token is a secret while the
 * window is open. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;

    while (n-- > 0) {
        *v++ = 0;
    }
}

static void close_window(struct gw_pair *p)
{
    wipe(p->token, sizeof(p->token));
    p->state = GW_PAIR_IDLE;
    p->until_ms = 0;
    p->bad = 0;
    p->session = 0;
    p->token_by_ms = 0;
    memset(p->fp, 0, sizeof(p->fp));
    p->name[0] = '\0';
}

void gw_pair_init(struct gw_pair *p)
{
    memset(p, 0, sizeof(*p));
}

bool gw_pair_open(const struct gw_pair *p)
{
    return p->state != GW_PAIR_IDLE;
}

int gw_pair_begin(struct gw_pair *p, const uint8_t token[GW_TOKEN_LEN], int64_t now_ms)
{
    if (p->state != GW_PAIR_IDLE) {
        return -1;
    }
    close_window(p);
    memcpy(p->token, token, GW_TOKEN_LEN);
    p->until_ms = now_ms + GW_PAIR_WINDOW_MS;
    p->state = GW_PAIR_OPEN;
    return 0;
}

uint64_t gw_pair_cancel(struct gw_pair *p)
{
    uint64_t s = p->session;

    close_window(p);
    return s;
}

bool gw_pair_admits(const struct gw_pair *p, int64_t now_ms)
{
    return p->state == GW_PAIR_OPEN && now_ms < p->until_ms;
}

void gw_pair_candidate(struct gw_pair *p, uint64_t session, const uint8_t fp[GW_FP_LEN], int64_t now_ms)
{
    if (!gw_pair_admits(p, now_ms) || session == 0) {
        return;
    }
    p->state = GW_PAIR_CANDIDATE;
    p->session = session;
    p->token_by_ms = now_ms + GW_PAIR_TOKEN_MS;
    memcpy(p->fp, fp, GW_FP_LEN);
    p->name[0] = '\0';
}

/* The door is free again for the next candidate. */
static void reopen(struct gw_pair *p)
{
    p->state = GW_PAIR_OPEN;
    p->session = 0;
    p->token_by_ms = 0;
    memset(p->fp, 0, sizeof(p->fp));
    p->name[0] = 0;
}

/* A candidate showed a wrong token: back to OPEN, or closed after the
 * third. Only a presented token counts (rule 21). A candidate that left or
 * ran out of time presented nothing, and counting it would let anyone on
 * the network close the owner's window with three connections. */
static void failure(struct gw_pair *p)
{
    p->bad++;
    if (p->bad >= GW_PAIR_MAX_BAD) {
        close_window(p);
        return;
    }
    p->state = GW_PAIR_OPEN;
    p->session = 0;
    p->token_by_ms = 0;
    memset(p->fp, 0, sizeof(p->fp));
    p->name[0] = '\0';
}

enum gw_pair_token_result gw_pair_token(struct gw_pair *p, uint64_t session, const uint8_t *token,
                                        size_t token_len, const char *name, int64_t now_ms)
{
    uint8_t padded[GW_TOKEN_LEN];
    int ok;

    if (p->state != GW_PAIR_CANDIDATE || session == 0 || session != p->session) {
        return GW_PAIR_TOKEN_REFUSED;
    }
    if (now_ms >= p->until_ms) {
        close_window(p);
        return GW_PAIR_TOKEN_REFUSED;
    }
    /* A token of the wrong length is compared all the same, so the answer's
     * timing says nothing about the length. */
    memset(padded, 0, sizeof(padded));
    if (token != NULL) {
        memcpy(padded, token, token_len < GW_TOKEN_LEN ? token_len : GW_TOKEN_LEN);
    }
    ok = gw_ct_eq(padded, p->token, GW_TOKEN_LEN) & (token != NULL && token_len == GW_TOKEN_LEN);
    wipe(padded, sizeof(padded));
    if (!ok || !gw_name_ok(name)) {
        failure(p);
        return GW_PAIR_TOKEN_BAD;
    }
    wipe(p->token, sizeof(p->token)); /* accepted once */
    memcpy(p->name, name, strlen(name) + 1);
    p->state = GW_PAIR_WAITING;
    p->token_by_ms = 0;
    return GW_PAIR_TOKEN_OK;
}

int gw_pair_decide(struct gw_pair *p, bool allow, int64_t now_ms, uint64_t *session)
{
    if (p->state != GW_PAIR_WAITING || now_ms >= p->until_ms) {
        return -1;
    }
    *session = p->session;
    if (!allow) {
        close_window(p);
        return 0;
    }
    /* The caller reads p->fp and p->name, then calls gw_pair_cancel(). */
    return 1;
}

void gw_pair_gone(struct gw_pair *p, uint64_t session)
{
    if (session == 0 || session != p->session) {
        return;
    }
    if (p->state == GW_PAIR_CANDIDATE) {
        reopen(p);
    } else if (p->state == GW_PAIR_WAITING) {
        close_window(p);
    }
}

uint64_t gw_pair_tick(struct gw_pair *p, int64_t now_ms)
{
    uint64_t s;

    if (p->state == GW_PAIR_IDLE) {
        return 0;
    }
    if (now_ms >= p->until_ms) {
        s = p->session;
        close_window(p);
        return s;
    }
    if (p->state == GW_PAIR_CANDIDATE && now_ms >= p->token_by_ms) {
        s = p->session;
        reopen(p);
        return s;
    }
    return 0;
}
