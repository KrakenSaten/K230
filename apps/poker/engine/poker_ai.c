/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker.h"
#include <string.h>

/* Sixteen bounded trials, no hidden-card knowledge and no heap allocations.
 * A trial draws a plausible river and each live opponent's unseen hole cards.
 * The policy uses shared equity (ties included) and the price of calling. */
static unsigned strength(struct poker_game *g)
{
    const struct poker_player *p = &g->player[g->actor];
    if (!g->board_count) {
        int a = bj_card_rank(p->hole[0]), b = bj_card_rank(p->hole[1]);
        unsigned score;
        if (a == 1)
            a = 14;
        if (b == 1)
            b = 14;
        if (a == b)
            return 48 + (unsigned)a * 3;
        score = (unsigned)(a + b) * 2;
        if (bj_card_suit(p->hole[0]) == bj_card_suit(p->hole[1]))
            score += 8;
        if (a - b == 1 || b - a == 1)
            score += 6;
        return score;
    }
    unsigned equity = 0;
    uint64_t known = (UINT64_C(1) << p->hole[0]) | (UINT64_C(1) << p->hole[1]);
    bj_card_t pool[52], cards[7];
    unsigned n = 0;
    for (int i = 0; i < g->board_count; ++i)
        known |= UINT64_C(1) << g->board[i];
    for (int i = 0; i < 52; ++i)
        if (!(known & (UINT64_C(1) << i)))
            pool[n++] = (bj_card_t)i;
    for (unsigned trial = 0; trial < 16; ++trial) {
        bj_card_t sample[52];
        unsigned at = 0, ties = 1;
        bool beaten = false;
        uint32_t ours;
        memcpy(sample, pool, n);
        /* Partial Fisher-Yates: at most eleven unknown cards are needed. */
        for (unsigned i = 0; i < 11 && i < n; ++i) {
            unsigned j = i + bj_rng_below(&g->rng, n - i);
            bj_card_t tmp = sample[i];
            sample[i] = sample[j];
            sample[j] = tmp;
        }
        memcpy(cards, g->board, g->board_count);
        for (unsigned i = g->board_count; i < 5; ++i)
            cards[i] = sample[at++];
        memcpy(cards + 5, p->hole, 2);
        ours = poker_evaluate(cards, 7);
        for (int seat = 0; seat < POKER_SEATS; ++seat) {
            if (seat == g->actor || !g->player[seat].in_hand || g->player[seat].folded)
                continue;
            cards[5] = sample[at++];
            cards[6] = sample[at++];
            uint32_t theirs = poker_evaluate(cards, 7);
            if (theirs > ours)
                beaten = true;
            else if (theirs == ours)
                ++ties;
        }
        if (!beaten)
            equity += 100 / ties;
    }
    return equity / 16;
}
struct poker_decision poker_ai(struct poker_game *g)
{
    struct poker_decision d = {POKER_CHECK, 0};
    struct poker_options o;
    unsigned equity, variation;
    uint32_t pot;
    if (!poker_playing(g) || g->actor < 0)
        return d;
    o = poker_options(g);
    equity = strength(g);
    variation = bj_rng_below(&g->rng, 100);
    pot = poker_pot_total(g);
    d.action = o.can_check ? POKER_CHECK : POKER_CALL;
    if (o.to_call && equity < 45 && (uint64_t)equity * (pot + o.call_cost) < (uint64_t)o.call_cost * 115)
        d.action = POKER_FOLD;
    else if (o.can_raise && equity > 62 && variation < 55) {
        uint32_t extra = pot / 2;
        d.raise_to = g->current_bet + (extra > g->min_raise ? extra : g->min_raise);
        if (d.raise_to < o.min_raise_to)
            d.raise_to = o.min_raise_to;
        if (d.raise_to >= o.max_raise_to) {
            d.action = POKER_ALL_IN;
            d.raise_to = 0;
        } else
            d.action = POKER_RAISE;
    }
    return d;
}
