/*
 * Card games: a shoe of one to four decks with its own shuffle stream.
 *
 * Blackjack plays from two decks. A game that wants one deck and no refill
 * can use cg_deck_fill and cg_deck_shuffle (cg_cards.h) instead.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_SHOE_H
#define CG_SHOE_H

#include "cg_cards.h"

#include <stdint.h>

#define CG_SHOE_DECKS_MAX 4
#define CG_SHOE_MAX (CG_SHOE_DECKS_MAX * CG_DECK)

struct cg_shoe {
    uint8_t n;                 /* cards left; card[n - 1] is dealt next */
    uint8_t decks;             /* 1..CG_SHOE_DECKS_MAX */
    cg_card_t card[CG_SHOE_MAX];
    struct cg_rng rng;
    uint32_t shuffles;
};

/* Zero the shoe, seed its generator and fill it with `decks` shuffled decks
 * (clamped to 1..CG_SHOE_DECKS_MAX). */
void cg_shoe_init(struct cg_shoe *shoe, int decks, uint32_t seed);
/* Replace the shoe with a full, freshly shuffled one. */
void cg_shoe_shuffle(struct cg_shoe *shoe);
/* As cg_shoe_shuffle, but leave out the cards that lie elsewhere:
 * elsewhere[c] copies of card c, at most `decks` each. NULL leaves none out. */
void cg_shoe_refill(struct cg_shoe *shoe, const uint8_t elsewhere[CG_DECK]);
/* The next card; 0xFF from an empty shoe. */
cg_card_t cg_shoe_draw(struct cg_shoe *shoe);

#endif
