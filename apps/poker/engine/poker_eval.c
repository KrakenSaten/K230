/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker.h"

static uint32_t five(const bj_card_t c[5])
{
    int count[15] = {0}, ranks[5], pairs[2] = {0}, np = 0, triple = 0, quad = 0;
    int flush = 1, straight = 0, n = 0, run = 0;
    enum poker_rank category;
    uint32_t result;
    for (int i = 0; i < 5; ++i) {
        int rank = bj_card_rank(c[i]);
        if (rank == BJ_ACE)
            rank = 14;
        ++count[rank];
        if (bj_card_suit(c[i]) != bj_card_suit(c[0]))
            flush = 0;
    }
    for (int r = 14; r >= 2; --r) {
        if (count[r]) {
            ++run;
            if (run == 5 && !straight)
                straight = r + 4;
        } else
            run = 0;
        if (count[r] == 4)
            quad = r;
        if (count[r] == 3)
            triple = r;
        if (count[r] == 2)
            pairs[np++] = r;
    }
    if (!straight && count[14] && count[5] && count[4] && count[3] && count[2])
        straight = 5;
    if (flush && straight) {
        category = POKER_STRAIGHT_FLUSH;
        ranks[n++] = straight;
    } else if (quad) {
        category = POKER_QUADS;
        ranks[n++] = quad;
    } else if (triple && np) {
        category = POKER_FULL_HOUSE;
        ranks[n++] = triple;
        ranks[n++] = pairs[0];
    } else if (flush)
        category = POKER_FLUSH;
    else if (straight) {
        category = POKER_STRAIGHT;
        ranks[n++] = straight;
    } else if (triple) {
        category = POKER_TRIPS;
        ranks[n++] = triple;
    } else if (np == 2) {
        category = POKER_TWO_PAIR;
        ranks[n++] = pairs[0];
        ranks[n++] = pairs[1];
    } else if (np) {
        category = POKER_PAIR;
        ranks[n++] = pairs[0];
    } else
        category = POKER_HIGH_CARD;
    if (category != POKER_STRAIGHT && category != POKER_STRAIGHT_FLUSH && category != POKER_FULL_HOUSE) {
        for (int r = 14; r >= 2; --r) {
            if (count[r] && (category == POKER_FLUSH || category == POKER_HIGH_CARD || count[r] == 1))
                ranks[n++] = r;
        }
    }
    result = (uint32_t)category << 20;
    for (int i = 0; i < n; ++i)
        result |= (uint32_t)ranks[i] << (16 - 4 * i);
    return result;
}

uint32_t poker_evaluate(const bj_card_t *cards, unsigned count)
{
    uint64_t seen = 0;
    uint32_t best = 0;
    bj_card_t hand[5];
    if (!cards || count < 5 || count > 7)
        return 0;
    for (unsigned i = 0; i < count; ++i) {
        if (!bj_card_valid(cards[i]) || (seen & (UINT64_C(1) << cards[i])))
            return 0;
        seen |= UINT64_C(1) << cards[i];
    }
    for (unsigned a = 0; a + 4 < count; ++a)
        for (unsigned b = a + 1; b + 3 < count; ++b)
            for (unsigned c = b + 1; c + 2 < count; ++c)
                for (unsigned d = c + 1; d + 1 < count; ++d)
                    for (unsigned e = d + 1; e < count; ++e) {
                        uint32_t value;
                        hand[0] = cards[a];
                        hand[1] = cards[b];
                        hand[2] = cards[c];
                        hand[3] = cards[d];
                        hand[4] = cards[e];
                        value = five(hand);
                        if (value > best)
                            best = value;
                    }
    return best;
}

enum poker_rank poker_rank_category(uint32_t rank)
{
    return (enum poker_rank)(rank >> 20);
}
const char *poker_rank_name(uint32_t rank)
{
    static const char *const names[] = {"High card",       "Pair",           "Two pair",
                                        "Three of a kind", "Straight",       "Flush",
                                        "Full house",      "Four of a kind", "Straight flush"};
    unsigned category = poker_rank_category(rank);
    return category < sizeof(names) / sizeof(names[0]) ? names[category] : "Unknown";
}
