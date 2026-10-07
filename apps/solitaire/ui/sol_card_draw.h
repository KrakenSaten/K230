/*
 * PG Solitaire card rendering: a card face, a card back, an empty slot and a
 * suit pip, drawn as shapes into an LVGL draw layer at any size.
 *
 * Shapes rather than bitmaps, so a card is as sharp at 68 px wide in portrait
 * as at whatever width a landscape table gives it, with no art per size. The
 * suits are built from triangles and discs (the product fonts carry no suit
 * glyphs), the ranks are the product font's own glyphs taken from the role
 * styles, and every colour is the card world palette (sol_palette.h).
 *
 * This and sol_cards.h are the Pocket Cards candidates: PG Blackjack carries
 * a copy under its own prefix until a shared module is extracted.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PGSOL_CARD_DRAW_H
#define PGSOL_CARD_DRAW_H

#include "lvgl.h"
#include "sol_cards.h"

/* The corner radius a card of this width gets. */
int sol_card_radius(int card_w);

void sol_draw_face(lv_layer_t *layer, const lv_area_t *area, sol_card_t card);
void sol_draw_back(lv_layer_t *layer, const lv_area_t *area);

enum sol_slot_mark {
    SOL_SLOT_PLAIN = 0,
    SOL_SLOT_SUIT,    /* a foundation: its suit as a ghost */
    SOL_SLOT_KING,    /* an empty column: a ghost K */
    SOL_SLOT_TURN     /* an empty stock with cards in the waste: the turn-over mark */
};
void sol_draw_slot(lv_layer_t *layer, const lv_area_t *area, enum sol_slot_mark mark, enum sol_suit suit);

/* A suit centred at (cx, cy) in a box `size` pixels high. */
void sol_draw_suit(lv_layer_t *layer, int cx, int cy, int size, enum sol_suit suit, lv_color_t color);

#endif
