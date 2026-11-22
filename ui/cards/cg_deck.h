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

#include <stddef.h>
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
 * large pip for the ace, a double-ended court figure in a ruled frame for the
 * jack, queen and king, and a small suit in the far corner. `left_pct` and
 * `right_pct` are how far in from each side the outer pip columns stand, in
 * percent of the width (a corner index with its pip below the rank needs the
 * left one further in). Small cards (under 96 px wide) get one large pip, or
 * the court's head and shoulders; `court_font` draws the letter only where a
 * card is too small even for that. */
void cg_deck_body(lv_layer_t *layer, const lv_area_t *area, int top, int left_pct, int right_pct, cg_card_t card,
                  const lv_font_t *court_font, const struct cg_deck_inks *inks);
/* A court figure in `box`: the jack with a feathered cap and a halberd, the
 * queen with a tiara and a flower, the king crowned and bearded with a
 * sceptre, robed in the suit's colour with the suit on the breast. Double
 * ended (the lower half the same figure turned) or, for a small card, one
 * figure filling the box. Drawn with shapes, once, when a set is composed. */
void cg_deck_court(lv_layer_t *layer, const lv_area_t *box, cg_card_t card, int double_ended,
                   const struct cg_deck_inks *inks);
void cg_deck_back(lv_layer_t *layer, const lv_area_t *area, int radius, const struct cg_deck_inks *inks);

/* The card's own type, fixed whatever the text size (owner decision 4,
 * 2026-11-22): the faces the Medium text size gives the DS title (28
 * semibold), label (24) and hero-40 roles. */
enum cg_deck_type {
    CG_TYPE_INDEX = 0,   /* the corner rank on a card 56 px wide or more */
    CG_TYPE_INDEX_SMALL, /* the corner rank on a smaller card */
    CG_TYPE_LARGE        /* a large letter (an empty column's K) */
};
const lv_font_t *cg_deck_font(enum cg_deck_type type);

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
/* The same, squashed across to `scale_x` 256ths of its width about its
 * centre line: a card turning over. Nothing at 0; full width while the set
 * is not composed. */
void cg_deck_draw_scaled(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, int id,
                         cg_deck_compose_cb compose, int scale_x);
/* Night: every card drawn from now on is darkened by `opa` of black (0 for
 * none), as the cloth under it is. */
void cg_deck_set_dim(lv_opa_t opa);
/* The bytes the composed sets and their shadows hold now. */
size_t cg_deck_bytes(void);
/* The soft shadow a card of that set lying at `area` casts on the cloth.
 * Draw the shadows of a pile or a hand before its cards, so no card's
 * shadow falls across its neighbour's index. Nothing until the set is
 * composed. */
void cg_deck_shadow(lv_layer_t *layer, const lv_area_t *area, int set, uintptr_t stamp, cg_deck_compose_cb compose);
/* How far past a card `card_w` wide its shadow can reach (for motion). */
int cg_deck_shadow_reach(int card_w);
/* Forget every composed set. Each game calls it when it closes, so no deck
 * outlives the game that drew it. */
void cg_deck_purge(void);

#endif
