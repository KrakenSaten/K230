/*
 * PG Blackjack card rendering: a card face, a card back, an empty slot and a
 * suit pip, drawn as shapes into an LVGL draw layer at any size.
 *
 * Shapes rather than bitmaps, so a card is as sharp at the size portrait
 * gives it as at whatever size a landscape table will, with no art per size.
 * The suits are built from triangles and discs (the product fonts carry no
 * suit glyphs), the ranks are the product font's own glyphs taken from the
 * role styles, and every colour is the card world palette (bj_palette.h).
 *
 * A copy of PG Solitaire's sol_card_draw.[ch] under this game's prefix: the
 * Pocket Cards renderer both games should share once it is extracted. The
 * slot marks for foundations, columns and the stock are Solitaire's; this
 * game uses only the plain slot, and they are kept so the copies stay close.
 *
 * ONE DELIBERATE DIFFERENCE: bj_draw_face() takes an index layout. Solitaire
 * fans cards vertically, so its corner puts the pip beside the rank, in the
 * top strip that stays visible; a Blackjack hand overlaps cards sideways,
 * where that pip is covered, so the pip goes below the rank. A shared
 * renderer needs both (docs/apps/PGBLACKJACK.md, "Pocket Cards").
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_CARD_DRAW_H
#define PGBJ_CARD_DRAW_H

#include "lvgl.h"
#include "bj_cards.h"

/* The corner radius a card of this width gets. */
int bj_card_radius(int card_w);

/* Where the corner index puts the suit: beside the rank (a vertical fan shows
 * the top strip) or below it (a sideways overlap shows the left strip). */
enum bj_index {
    BJ_INDEX_BESIDE = 0,
    BJ_INDEX_BELOW
};
void bj_draw_face(lv_layer_t *layer, const lv_area_t *area, bj_card_t card, enum bj_index index);
void bj_draw_back(lv_layer_t *layer, const lv_area_t *area);

enum bj_slot_mark {
    BJ_SLOT_PLAIN = 0,
    BJ_SLOT_SUIT,    /* a foundation: its suit as a ghost */
    BJ_SLOT_KING,    /* an empty column: a ghost K */
    BJ_SLOT_TURN     /* an empty stock with cards in the waste: the turn-over mark */
};
void bj_draw_slot(lv_layer_t *layer, const lv_area_t *area, enum bj_slot_mark mark, enum bj_suit suit);

/* A suit centred at (cx, cy) in a box `size` pixels high. */
void bj_draw_suit(lv_layer_t *layer, int cx, int cy, int size, enum bj_suit suit, lv_color_t color);

#endif
