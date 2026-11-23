/*
 * Card games: a card, its rank, suit and colour, and a 52-card deck with a
 * deterministic shuffle. Shared by Blackjack, Solitaire and Poker; each game
 * used to carry its own copy (bj_cards, sol_cards).
 *
 * A card is one byte, 0..51: suit * 13 + (rank - 1). Rank 1 is the ace, 11
 * jack, 12 queen, 13 king. Face up or down is not part of a card: it belongs
 * to where the card lies, which is each game's business. A shoe of several
 * decks (cg_shoe.h) holds the same card byte once per deck.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_CARDS_H
#define CG_CARDS_H

#include "cg_rng.h"

#include <stdint.h>

#define CG_SUITS 4
#define CG_RANKS 13
#define CG_DECK (CG_SUITS * CG_RANKS)

#define CG_ACE 1
#define CG_JACK 11
#define CG_QUEEN 12
#define CG_KING 13

/* Ordered so a row of the four suits alternates black, red, black, red. */
enum cg_suit {
    CG_SPADES = 0,
    CG_HEARTS,
    CG_CLUBS,
    CG_DIAMONDS
};

typedef uint8_t cg_card_t;

/* Build a card; 0xFF when the rank or suit is out of range. */
cg_card_t cg_card(int rank, enum cg_suit suit);
int cg_card_rank(cg_card_t c);          /* 1..13, 0 for no card */
enum cg_suit cg_card_suit(cg_card_t c);
int cg_card_is_red(cg_card_t c);        /* hearts and diamonds */
int cg_card_valid(cg_card_t c);

/* "A", "2" .. "10", "J", "Q", "K"; "" for an invalid rank. */
const char *cg_rank_text(int rank);
/* "SPADES", "HEARTS", "CLUBS", "DIAMONDS". */
const char *cg_suit_name(enum cg_suit suit);

/* The 52 cards in order: spades A..K, hearts, clubs, diamonds. */
void cg_deck_fill(cg_card_t deck[CG_DECK]);
/* Fisher-Yates from the top, one draw per position: the same generator state
 * gives the same order. */
void cg_deck_shuffle(cg_card_t deck[CG_DECK], struct cg_rng *rng);

#endif
