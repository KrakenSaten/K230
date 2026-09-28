/*
 * PG Solitaire rules: Klondike, draw one.
 *
 * THE RULESET (one variant, used everywhere, docs/apps/PGSOLITAIRE.md):
 *
 *  - One standard 52-card deck, shuffled from a seed.
 *  - Deal: seven tableau columns holding 1..7 cards, only the top card of
 *    each face up; the remaining 24 cards form the stock, face down.
 *  - Stock: drawing turns ONE card onto the waste (draw-1). When the stock is
 *    empty, the waste is turned over to become the stock again, in the order
 *    that deals the same cards in the same sequence. Passes are unlimited.
 *  - Waste: only its top card can be played.
 *  - Foundations: one per suit, in a fixed slot (spades, hearts, clubs,
 *    diamonds). A foundation takes its suit's ace first, then that suit's
 *    cards in ascending order. One card at a time.
 *  - Tableau: a card or a face-up run goes onto a card one rank higher of the
 *    opposite colour. An empty column takes only a king, or a run headed by
 *    a king. Any face-up run whose cards descend in alternating colours may
 *    move as a whole.
 *  - When a move uncovers a face-down tableau card, it turns face up.
 *  - The top card of a foundation may come back down onto the tableau.
 *  - The game is won when all 52 cards are on the foundations.
 *  - Moves counts every card move, draw and turn of the waste.
 *
 * No scoring beyond the move count, no undo, no auto-complete: out of scope
 * for this version.
 *
 * PILES. Thirteen, numbered for storage and for the view: stock, waste, four
 * foundations, seven tableau columns. A pile is bottom-first: card[0] lies
 * on the table, card[n - 1] is the top. The first `down` cards are face
 * down; only tableau columns and the stock have any. Nothing here knows
 * where a pile is drawn.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock (tests/sol_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGSOL_RULES_H
#define PGSOL_RULES_H

#include "sol_cards.h"

#include <stdint.h>

#define SOL_FOUNDATIONS 4
#define SOL_COLUMNS 7

enum sol_pile {
    SOL_STOCK = 0,
    SOL_WASTE = 1,
    SOL_F0 = 2, /* spades; SOL_F0 + suit */
    SOL_T0 = SOL_F0 + SOL_FOUNDATIONS,
    SOL_PILES = SOL_T0 + SOL_COLUMNS,
    SOL_NO_PILE = 0xFF
};

struct sol_stack {
    uint8_t n;
    uint8_t down;
    sol_card_t card[SOL_DECK];
};

struct sol_game {
    struct sol_stack pile[SOL_PILES];
    uint32_t seed;
    uint32_t moves;
    uint32_t passes; /* times the waste was turned back into the stock */
    uint8_t won;
};

/* Why a move is refused. SOL_OK is 0, so a result reads as "is there a
 * problem". The view turns these into words. */
enum sol_result {
    SOL_OK = 0,
    SOL_ERR_BAD_PILE,    /* not a pile, or not a pile cards go to or from */
    SOL_ERR_EMPTY,       /* nothing there to take */
    SOL_ERR_FACE_DOWN,   /* the card is face down */
    SOL_ERR_NOT_TOP,     /* only the top card of this pile can be taken */
    SOL_ERR_NOT_A_RUN,   /* the cards above it do not descend in alternating colours */
    SOL_ERR_SAME_PILE,   /* the cards are already there */
    SOL_ERR_ONE_CARD,    /* a foundation takes one card at a time */
    SOL_ERR_SUIT,        /* not this foundation's suit */
    SOL_ERR_RANK,        /* not the next rank */
    SOL_ERR_COLOUR,      /* the same colour as the card it would go on */
    SOL_ERR_KING_ONLY,   /* an empty column takes only a king */
    SOL_ERR_STOCK,       /* cards leave the stock by drawing, and never go back to it */
    SOL_ERR_WON          /* the game is over */
};

enum sol_draw {
    SOL_DREW = 1,     /* one card turned onto the waste */
    SOL_TURNED = 2,   /* the waste went back to the stock */
    SOL_NOTHING = 0   /* both are empty */
};

/* What a successful move did, for the view. */
struct sol_moved {
    uint8_t count;   /* cards moved */
    uint8_t flipped; /* 1 when a tableau card was turned face up */
    uint8_t won;     /* 1 when this move finished the game */
};

/* Deal a new game from a seed. */
void sol_deal(struct sol_game *g, uint32_t seed);

/* Can the card at `index` of `pile` (and the cards above it) be picked up? */
enum sol_result sol_can_pick(const struct sol_game *g, int pile, int index);
/* Can it go onto `to`? Includes the pick check. */
enum sol_result sol_can_move(const struct sol_game *g, int from, int index, int to);
/* Move it. On success fills `moved` (may be NULL). A refused move changes
 * nothing. */
enum sol_result sol_move(struct sol_game *g, int from, int index, int to, struct sol_moved *moved);
/* Draw from the stock, or turn the waste back when the stock is empty. */
enum sol_draw sol_draw(struct sol_game *g);

/* The foundation the card at `index` of `pile` can go to right now, or -1. */
int sol_foundation_target(const struct sol_game *g, int pile, int index);

int sol_cards_home(const struct sol_game *g);
int sol_is_foundation(int pile);
int sol_is_column(int pile);
/* The top card, or 0xFF for an empty pile. */
sol_card_t sol_top(const struct sol_game *g, int pile);

/* Could the rules have produced this position? All 52 cards exactly once,
 * foundations in suit and order, face-up tableau runs descending in
 * alternating colours, face-down counts within their piles, only tableau
 * and stock face down (and the stock entirely), `won` exactly when all
 * cards are home. 1 valid, 0 not. */
int sol_game_valid(const struct sol_game *g);

#endif
