/*
 * PG Solitaire card primitives. See sol_cards.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "sol_cards.h"

sol_card_t sol_card(int rank, enum sol_suit suit)
{
    if (rank < SOL_ACE || rank > SOL_KING || (int)suit < 0 || (int)suit >= SOL_SUITS) {
        return 0xFF;
    }
    return (sol_card_t)((int)suit * SOL_RANKS + rank - 1);
}

int sol_card_valid(sol_card_t c)
{
    return c < SOL_DECK;
}

int sol_card_rank(sol_card_t c)
{
    return sol_card_valid(c) ? c % SOL_RANKS + 1 : 0;
}

enum sol_suit sol_card_suit(sol_card_t c)
{
    return (enum sol_suit)(sol_card_valid(c) ? c / SOL_RANKS : 0);
}

int sol_card_is_red(sol_card_t c)
{
    enum sol_suit s = sol_card_suit(c);

    return sol_card_valid(c) && (s == SOL_HEARTS || s == SOL_DIAMONDS);
}

const char *sol_rank_text(int rank)
{
    static const char *const text[] = { "", "A", "2", "3", "4", "5", "6", "7",
                                         "8", "9", "10", "J", "Q", "K" };

    return (rank >= SOL_ACE && rank <= SOL_KING) ? text[rank] : "";
}

const char *sol_suit_name(enum sol_suit suit)
{
    static const char *const names[] = { "SPADES", "HEARTS", "CLUBS", "DIAMONDS" };

    return ((int)suit >= 0 && (int)suit < SOL_SUITS) ? names[suit] : "";
}

void sol_deck_fill(sol_card_t deck[SOL_DECK])
{
    int i;

    for (i = 0; i < SOL_DECK; i++) {
        deck[i] = (sol_card_t)i;
    }
}

void sol_deck_shuffle(sol_card_t deck[SOL_DECK], struct sol_rng *rng)
{
    int i;

    for (i = SOL_DECK - 1; i > 0; i--) {
        int j = (int)sol_rng_below(rng, (uint32_t)i + 1u);
        sol_card_t t = deck[i];

        deck[i] = deck[j];
        deck[j] = t;
    }
}
