/*
 * PG Blackjack card primitives. See bj_cards.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "bj_cards.h"

bj_card_t bj_card(int rank, enum bj_suit suit)
{
    if (rank < BJ_ACE || rank > BJ_KING || (int)suit < 0 || (int)suit >= BJ_SUITS) {
        return 0xFF;
    }
    return (bj_card_t)((int)suit * BJ_RANKS + rank - 1);
}

int bj_card_valid(bj_card_t c)
{
    return c < BJ_DECK;
}

int bj_card_rank(bj_card_t c)
{
    return bj_card_valid(c) ? c % BJ_RANKS + 1 : 0;
}

enum bj_suit bj_card_suit(bj_card_t c)
{
    return (enum bj_suit)(bj_card_valid(c) ? c / BJ_RANKS : 0);
}

int bj_card_is_red(bj_card_t c)
{
    enum bj_suit s = bj_card_suit(c);

    return bj_card_valid(c) && (s == BJ_HEARTS || s == BJ_DIAMONDS);
}

const char *bj_rank_text(int rank)
{
    static const char *const text[] = { "", "A", "2", "3", "4", "5", "6", "7",
                                         "8", "9", "10", "J", "Q", "K" };

    return (rank >= BJ_ACE && rank <= BJ_KING) ? text[rank] : "";
}

const char *bj_suit_name(enum bj_suit suit)
{
    static const char *const names[] = { "SPADES", "HEARTS", "CLUBS", "DIAMONDS" };

    return ((int)suit >= 0 && (int)suit < BJ_SUITS) ? names[suit] : "";
}

void bj_deck_fill(bj_card_t deck[BJ_DECK])
{
    int i;

    for (i = 0; i < BJ_DECK; i++) {
        deck[i] = (bj_card_t)i;
    }
}

void bj_deck_shuffle(bj_card_t deck[BJ_DECK], struct bj_rng *rng)
{
    int i;

    for (i = BJ_DECK - 1; i > 0; i--) {
        int j = (int)bj_rng_below(rng, (uint32_t)i + 1u);
        bj_card_t t = deck[i];

        deck[i] = deck[j];
        deck[j] = t;
    }
}
