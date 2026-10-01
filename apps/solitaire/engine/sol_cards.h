/*
 * PG Solitaire card primitives: a card, its rank, suit and colour, and a
 * 52-card deck with a deterministic shuffle.
 *
 * These are the candidates for a later shared Pocket Games card module
 * (docs/apps/PGSOLITAIRE.md, "Pocket Cards"). They are deliberately small
 * and carry the game's own prefix, so this branch and PG Blackjack can each
 * keep a copy and still link together if both are merged before the
 * extraction happens.
 *
 * A card is one byte, 0..51: suit * 13 + (rank - 1). Rank 1 is the ace, 11
 * jack, 12 queen, 13 king. Face up or down is not part of a card: it belongs
 * to where the card lies (sol_rules.h).
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PGSOL_CARDS_H
#define PGSOL_CARDS_H

#include "sol_rng.h"

#include <stdint.h>

#define SOL_SUITS 4
#define SOL_RANKS 13
#define SOL_DECK (SOL_SUITS * SOL_RANKS)

#define SOL_ACE 1
#define SOL_JACK 11
#define SOL_QUEEN 12
#define SOL_KING 13

/* Ordered so the foundation row alternates black, red, black, red. */
enum sol_suit {
    SOL_SPADES = 0,
    SOL_HEARTS,
    SOL_CLUBS,
    SOL_DIAMONDS
};

typedef uint8_t sol_card_t;

/* Build a card; 0xFF when the rank or suit is out of range. */
sol_card_t sol_card(int rank, enum sol_suit suit);
int sol_card_rank(sol_card_t c);          /* 1..13, 0 for no card */
enum sol_suit sol_card_suit(sol_card_t c);
int sol_card_is_red(sol_card_t c);        /* hearts and diamonds */
int sol_card_valid(sol_card_t c);

/* "A", "2" .. "10", "J", "Q", "K"; "" for an invalid rank. */
const char *sol_rank_text(int rank);
/* "SPADES", "HEARTS", "CLUBS", "DIAMONDS". */
const char *sol_suit_name(enum sol_suit suit);

/* The 52 cards in order: spades A..K, hearts, clubs, diamonds. */
void sol_deck_fill(sol_card_t deck[SOL_DECK]);
/* Fisher-Yates from the top, one draw per position: the same generator state
 * gives the same order. */
void sol_deck_shuffle(sol_card_t deck[SOL_DECK], struct sol_rng *rng);

#endif
