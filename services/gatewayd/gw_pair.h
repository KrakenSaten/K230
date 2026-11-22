/*
 * gatewayd: the pairing state machine (ADR-019 rules 14 and 20 to 22;
 * docs/design/companion/PAIRING.md §3). Pure: no I/O, no OpenSSL, the clock
 * and the random token passed in, so tests/gw_pair_test.c walks every path.
 *
 *   IDLE ── begin ──▶ OPEN ── a TLS client with an unknown key ──▶ CANDIDATE
 *                      ▲                                             │
 *                      └── it left, was wrong or too slow (a failure)─┤
 *                                                    the right token  ▼
 *   IDLE ◀── decided, the candidate left, the window ended ────── WAITING
 *
 * What it guarantees:
 *   - a window opens only by gw_pair_begin(), which only the local control
 *     socket calls; nothing a network peer sends reaches it;
 *   - one candidate at a time;
 *   - the token is compared in constant time, is accepted once, and is
 *     wiped the moment the window leaves OPEN/CANDIDATE for good;
 *   - GW_PAIR_MAX_BAD wrong tokens close the window. A candidate that
 *     leaves, or shows no token in GW_PAIR_TOKEN_MS, is dropped and frees
 *     the door without counting: it presented nothing;
 *   - a right token pairs nobody: only gw_pair_decide(allow) does, and only
 *     while that candidate is still connected and the window still open.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_PAIR_H
#define GATEWAYD_GW_PAIR_H

#include "gw.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum gw_pair_state {
    GW_PAIR_IDLE = 0,
    GW_PAIR_OPEN,       /* window open, nobody at the door */
    GW_PAIR_CANDIDATE,  /* an unknown key is connected, no token yet */
    GW_PAIR_WAITING,    /* it showed the token; a person decides */
};

struct gw_pair {
    enum gw_pair_state state;
    int64_t until_ms;               /* the window's end */
    uint8_t token[GW_TOKEN_LEN];
    unsigned bad;
    uint64_t session;               /* the candidate's connection */
    int64_t token_by_ms;
    uint8_t fp[GW_FP_LEN];
    char name[GW_NAME_MAX + 1];
};

enum gw_pair_token_result {
    GW_PAIR_TOKEN_OK = 0,   /* now WAITING */
    GW_PAIR_TOKEN_BAD,      /* drop this candidate; the window may have closed */
    GW_PAIR_TOKEN_REFUSED,  /* not the candidate, or no window: drop it */
};

void gw_pair_init(struct gw_pair *p);
/* Opens a window. 0, or -1 when one is already open. */
int gw_pair_begin(struct gw_pair *p, const uint8_t token[GW_TOKEN_LEN], int64_t now_ms);
/* Closes it. Returns the candidate's session to drop, or 0. */
uint64_t gw_pair_cancel(struct gw_pair *p);

/* May a client with an unknown key be let through the handshake now? */
bool gw_pair_admits(const struct gw_pair *p, int64_t now_ms);
/* It was. Only after gw_pair_admits() said yes. */
void gw_pair_candidate(struct gw_pair *p, uint64_t session, const uint8_t fp[GW_FP_LEN], int64_t now_ms);

enum gw_pair_token_result gw_pair_token(struct gw_pair *p, uint64_t session, const uint8_t *token,
                                        size_t token_len, const char *name, int64_t now_ms);

/* The person's answer. 1: allowed, pair p->fp / p->name now. 0: refused.
 * -1: nobody is waiting. The window is closed in every case but -1;
 * *session is the candidate to answer. */
int gw_pair_decide(struct gw_pair *p, bool allow, int64_t now_ms, uint64_t *session);

/* A connection ended. A candidate that had not shown the token frees the
 * door; one that had ends the window (its token is spent). */
void gw_pair_gone(struct gw_pair *p, uint64_t session);

/* Deadlines. Returns a session to drop (a candidate out of time, or whoever
 * was at the door when the window ended), or 0. */
uint64_t gw_pair_tick(struct gw_pair *p, int64_t now_ms);

bool gw_pair_open(const struct gw_pair *p);

#endif
