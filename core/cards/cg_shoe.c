/*
 * Card games: the shoe. See cg_shoe.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_shoe.h"

#include <string.h>

void cg_shoe_refill(struct cg_shoe *shoe, const uint8_t elsewhere[CG_DECK])
{
    int n = 0;
    int c;
    int i;

    if (!shoe) {
        return;
    }
    for (c = 0; c < CG_DECK; c++) {
        int copies = shoe->decks - (elsewhere ? elsewhere[c] : 0);

        for (i = 0; i < copies; i++) {
            shoe->card[n++] = (cg_card_t)c;
        }
    }
    /* Fisher-Yates over the whole shoe, one draw per position. */
    for (i = n - 1; i > 0; i--) {
        int j = (int)cg_rng_below(&shoe->rng, (uint32_t)i + 1u);
        cg_card_t t = shoe->card[i];

        shoe->card[i] = shoe->card[j];
        shoe->card[j] = t;
    }
    shoe->n = (uint8_t)n;
    shoe->shuffles++;
}

void cg_shoe_init(struct cg_shoe *shoe, int decks, uint32_t seed)
{
    if (!shoe) {
        return;
    }
    memset(shoe, 0, sizeof(*shoe));
    shoe->decks = (uint8_t)(decks < 1 ? 1 : decks > CG_SHOE_DECKS_MAX ? CG_SHOE_DECKS_MAX : decks);
    cg_rng_seed(&shoe->rng, seed);
    cg_shoe_refill(shoe, NULL);
}

void cg_shoe_shuffle(struct cg_shoe *shoe)
{
    cg_shoe_refill(shoe, NULL);
}

cg_card_t cg_shoe_draw(struct cg_shoe *shoe)
{
    return (shoe && shoe->n > 0) ? shoe->card[--shoe->n] : 0xFF;
}
