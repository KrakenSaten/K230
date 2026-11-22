/*
 * Card games: card primitives. See cg_cards.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_cards.h"

cg_card_t cg_card(int rank, enum cg_suit suit)
{
    if (rank < CG_ACE || rank > CG_KING || (int)suit < 0 || (int)suit >= CG_SUITS) {
        return 0xFF;
    }
    return (cg_card_t)((int)suit * CG_RANKS + rank - 1);
}

int cg_card_valid(cg_card_t c)
{
    return c < CG_DECK;
}

int cg_card_rank(cg_card_t c)
{
    return cg_card_valid(c) ? c % CG_RANKS + 1 : 0;
}

enum cg_suit cg_card_suit(cg_card_t c)
{
    return (enum cg_suit)(cg_card_valid(c) ? c / CG_RANKS : 0);
}

int cg_card_is_red(cg_card_t c)
{
    enum cg_suit s = cg_card_suit(c);

    return cg_card_valid(c) && (s == CG_HEARTS || s == CG_DIAMONDS);
}

const char *cg_rank_text(int rank)
{
    static const char *const text[] = { "", "A", "2", "3", "4", "5", "6", "7",
                                         "8", "9", "10", "J", "Q", "K" };

    return (rank >= CG_ACE && rank <= CG_KING) ? text[rank] : "";
}

const char *cg_suit_name(enum cg_suit suit)
{
    static const char *const names[] = { "SPADES", "HEARTS", "CLUBS", "DIAMONDS" };

    return ((int)suit >= 0 && (int)suit < CG_SUITS) ? names[suit] : "";
}

void cg_deck_fill(cg_card_t deck[CG_DECK])
{
    int i;

    for (i = 0; i < CG_DECK; i++) {
        deck[i] = (cg_card_t)i;
    }
}

void cg_deck_shuffle(cg_card_t deck[CG_DECK], struct cg_rng *rng)
{
    int i;

    for (i = CG_DECK - 1; i > 0; i--) {
        int j = (int)cg_rng_below(rng, (uint32_t)i + 1u);
        cg_card_t t = deck[i];

        deck[i] = deck[j];
        deck[j] = t;
    }
}
