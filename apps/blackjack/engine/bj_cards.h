/*
 * PG Blackjack card primitives: a card, its rank, suit and colour, and a
 * 52-card deck with a deterministic shuffle.
 *
 * A copy of PG Solitaire's sol_cards.h (branch feature/game-solitaire) under
 * this game's prefix, so the two branches stay independent and still link
 * together if both are merged before the shared Pocket Cards module is
 * extracted (docs/apps/PGBLACKJACK.md, "Pocket Cards"). Keep the two copies
 * the same until then.
 *
 * A card is one byte, 0..51: suit * 13 + (rank - 1). Rank 1 is the ace, 11
 * jack, 12 queen, 13 king. Face up or down is not part of a card: it belongs
 * to where the card lies (bj_rules.h). A shoe holds several decks, so in
 * Blackjack the same card byte appears once per deck.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PGBJ_CARDS_H
#define PGBJ_CARDS_H

#include "bj_rng.h"

#include <stdint.h>

#define BJ_SUITS 4
#define BJ_RANKS 13
#define BJ_DECK (BJ_SUITS * BJ_RANKS)

#define BJ_ACE 1
#define BJ_JACK 11
#define BJ_QUEEN 12
#define BJ_KING 13

/* Alternating black and red, as in the Solitaire copy. */
enum bj_suit {
    BJ_SPADES = 0,
    BJ_HEARTS,
    BJ_CLUBS,
    BJ_DIAMONDS
};

typedef uint8_t bj_card_t;

/* Build a card; 0xFF when the rank or suit is out of range. */
bj_card_t bj_card(int rank, enum bj_suit suit);
int bj_card_rank(bj_card_t c);          /* 1..13, 0 for no card */
enum bj_suit bj_card_suit(bj_card_t c);
int bj_card_is_red(bj_card_t c);        /* hearts and diamonds */
int bj_card_valid(bj_card_t c);

/* "A", "2" .. "10", "J", "Q", "K"; "" for an invalid rank. */
const char *bj_rank_text(int rank);
/* "SPADES", "HEARTS", "CLUBS", "DIAMONDS". */
const char *bj_suit_name(enum bj_suit suit);

/* The 52 cards in order: spades A..K, hearts, clubs, diamonds. */
void bj_deck_fill(bj_card_t deck[BJ_DECK]);
/* Fisher-Yates from the top, one draw per position: the same generator state
 * gives the same order. */
void bj_deck_shuffle(bj_card_t deck[BJ_DECK], struct bj_rng *rng);

#endif
