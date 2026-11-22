/*
 * Card games: the Lamplight deck (docs/design/card-games, direction A).
 *
 * What is shared between the games' card renderers: the face below the
 * corner index (pip layouts, the ace, the court frame), the back, and a cache
 * that turns any card a game draws into a composed image plus a shadow, so a
 * redraw is one blit (section 8 of the proposal; measured on Unit A in
 * milestone 0). Each game keeps its own corner index, because its layout
 * depends on it.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_DECK_H
#define CG_DECK_H

#include "cg_cards.h"
#include "lvgl.h"

#include <stdint.h>

/* The inks a game's palette gives the deck. */
struct cg_deck_inks {
    lv_color_t paper, edge, black, red, back, back_line;
};

/* A suit centred at (cx, cy) in a box `size` pixels high. */
void cg_deck_suit(lv_layer_t *layer, int cx, int cy, int size, enum cg_suit suit, lv_color_t color);
/* The card's paper: a rounded sheet, a shade darker toward its foot. */
void cg_deck_paper(lv_layer_t *layer, const lv_area_t *area, int radius, const struct cg_deck_inks *inks);
/* The face below `top` pixels from the card's top edge (the game's corner
 * index is above and left of it): pips in their places from two to ten, one
 * large pip for the ace, a framed letter for the court cards, and a small
 * suit in the far corner. `left_pct` and `right_pct` are how far in from each
 * side the outer pip columns stand, in percent of the width (a corner index
 * with its pip below the rank needs the left one further in); `court_font` draws the letter. Small cards (under
 * 96 px wide) get one large pip or the letter alone. */
void cg_deck_body(lv_layer_t *layer, const lv_area_t *area, int top, int left_pct, int right_pct, cg_card_t card,
                  const lv_font_t *court_font, const struct cg_deck_inks *inks);
void cg_deck_back(lv_layer_t *layer, const lv_area_t *area, int radius, const struct cg_deck_inks *inks);

/* ---- the cache ------------------------------------------------------------------ */

#define CG_DECK_IDS (CG_DECK + 1)
#define CG_DECK_ID_BACK CG_DECK

/* Draw image `id` of a set straight onto the layer. */
typedef void (*cg_deck_compose_cb)(lv_layer_t *layer, const lv_area_t *area, int id);

/* Draw image `id` (a card byte, or CG_DECK_ID_BACK) at `area`. A set is one
 * game's deck in one look: `set` names it and `stamp` changes when its look
 * does (a font, a theme mode). The first draws of a set at a size go through
 * `compose` directly while the set is composed in the background; after that
 * each draw is one image. */
void cg_deck_draw(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, int id,
                  cg_deck_compose_cb compose);
/* The soft shadow a card of that set lying at `area` casts on the cloth.
 * Draw the shadows of a pile or a hand before its cards, so no card's
 * shadow falls across its neighbour's index. Nothing until the set is
 * composed. */
void cg_deck_shadow(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, cg_deck_compose_cb compose);
/* Forget every composed set (tests; a low-memory hook later). */
void cg_deck_purge(void);

#endif
