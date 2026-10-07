/*
 * PG Solitaire rules. See sol_rules.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sol_rules.h"

#include <string.h>

int sol_is_foundation(int pile)
{
    return pile >= SOL_F0 && pile < SOL_F0 + SOL_FOUNDATIONS;
}

int sol_is_column(int pile)
{
    return pile >= SOL_T0 && pile < SOL_T0 + SOL_COLUMNS;
}

static int pile_ok(int pile)
{
    return pile >= 0 && pile < SOL_PILES;
}

sol_card_t sol_top(const struct sol_game *g, int pile)
{
    if (!g || !pile_ok(pile) || g->pile[pile].n == 0) {
        return 0xFF;
    }
    return g->pile[pile].card[g->pile[pile].n - 1];
}

int sol_cards_home(const struct sol_game *g)
{
    int n = 0;
    int f;

    for (f = 0; g && f < SOL_FOUNDATIONS; f++) {
        n += g->pile[SOL_F0 + f].n;
    }
    return n;
}

void sol_deal(struct sol_game *g, uint32_t seed)
{
    sol_card_t deck[SOL_DECK];
    struct sol_rng rng;
    int next = 0;
    int c;
    int i;

    if (!g) {
        return;
    }
    memset(g, 0, sizeof(*g));
    g->seed = seed;
    sol_rng_seed(&rng, seed);
    sol_deck_fill(deck);
    sol_deck_shuffle(deck, &rng);
    for (c = 0; c < SOL_COLUMNS; c++) {
        struct sol_stack *col = &g->pile[SOL_T0 + c];

        for (i = 0; i <= c; i++) {
            col->card[col->n++] = deck[next++];
        }
        col->down = (uint8_t)c;
    }
    while (next < SOL_DECK) {
        struct sol_stack *stock = &g->pile[SOL_STOCK];

        stock->card[stock->n++] = deck[next++];
    }
    g->pile[SOL_STOCK].down = g->pile[SOL_STOCK].n;
}

/* Does card `upper` go on `lower` in the tableau? */
static int builds_on(sol_card_t upper, sol_card_t lower)
{
    return sol_card_rank(upper) + 1 == sol_card_rank(lower) &&
           sol_card_is_red(upper) != sol_card_is_red(lower);
}

enum sol_result sol_can_pick(const struct sol_game *g, int pile, int index)
{
    const struct sol_stack *s;
    int i;

    if (!g || !pile_ok(pile)) {
        return SOL_ERR_BAD_PILE;
    }
    if (g->won) {
        return SOL_ERR_WON;
    }
    if (pile == SOL_STOCK) {
        return SOL_ERR_STOCK;
    }
    s = &g->pile[pile];
    if (s->n == 0) {
        return SOL_ERR_EMPTY;
    }
    if (index < 0 || index >= s->n) {
        return SOL_ERR_BAD_PILE;
    }
    if (index < s->down) {
        return SOL_ERR_FACE_DOWN;
    }
    if (!sol_is_column(pile)) {
        return index == s->n - 1 ? SOL_OK : SOL_ERR_NOT_TOP;
    }
    for (i = index; i + 1 < s->n; i++) {
        if (!builds_on(s->card[i + 1], s->card[i])) {
            return SOL_ERR_NOT_A_RUN;
        }
    }
    return SOL_OK;
}

enum sol_result sol_can_move(const struct sol_game *g, int from, int index, int to)
{
    enum sol_result r = sol_can_pick(g, from, index);
    const struct sol_stack *src;
    const struct sol_stack *dst;
    sol_card_t moving;

    if (r != SOL_OK) {
        return r;
    }
    if (!pile_ok(to)) {
        return SOL_ERR_BAD_PILE;
    }
    if (to == from) {
        return SOL_ERR_SAME_PILE;
    }
    if (to == SOL_STOCK) {
        return SOL_ERR_STOCK;
    }
    if (to == SOL_WASTE) {
        return SOL_ERR_BAD_PILE;
    }
    src = &g->pile[from];
    dst = &g->pile[to];
    moving = src->card[index];
    if (sol_is_foundation(to)) {
        if (sol_is_foundation(from)) {
            return SOL_ERR_BAD_PILE;
        }
        if (index != src->n - 1) {
            return SOL_ERR_ONE_CARD;
        }
        if ((int)sol_card_suit(moving) != to - SOL_F0) {
            return SOL_ERR_SUIT;
        }
        if (sol_card_rank(moving) != dst->n + 1) {
            return SOL_ERR_RANK;
        }
        return SOL_OK;
    }
    /* A tableau column. */
    if (dst->n == 0) {
        return sol_card_rank(moving) == SOL_KING ? SOL_OK : SOL_ERR_KING_ONLY;
    }
    {
        sol_card_t onto = dst->card[dst->n - 1];

        if (sol_card_rank(moving) + 1 != sol_card_rank(onto)) {
            return SOL_ERR_RANK;
        }
        if (sol_card_is_red(moving) == sol_card_is_red(onto)) {
            return SOL_ERR_COLOUR;
        }
    }
    return SOL_OK;
}

enum sol_result sol_move(struct sol_game *g, int from, int index, int to, struct sol_moved *moved)
{
    enum sol_result r = sol_can_move(g, from, index, to);
    struct sol_stack *src;
    struct sol_stack *dst;
    int count;

    if (moved) {
        memset(moved, 0, sizeof(*moved));
    }
    if (r != SOL_OK) {
        return r;
    }
    src = &g->pile[from];
    dst = &g->pile[to];
    count = src->n - index;
    memcpy(&dst->card[dst->n], &src->card[index], (size_t)count);
    dst->n = (uint8_t)(dst->n + count);
    src->n = (uint8_t)index;
    if (src->down > src->n) {
        src->down = src->n;
    }
    if (moved) {
        moved->count = (uint8_t)count;
    }
    if (sol_is_column(from) && src->n > 0 && src->down == src->n) {
        src->down--;
        if (moved) {
            moved->flipped = 1;
        }
    }
    g->moves++;
    if (sol_cards_home(g) == SOL_DECK) {
        g->won = 1;
        if (moved) {
            moved->won = 1;
        }
    }
    return SOL_OK;
}

enum sol_draw sol_draw(struct sol_game *g)
{
    struct sol_stack *stock;
    struct sol_stack *waste;

    if (!g || g->won) {
        return SOL_NOTHING;
    }
    stock = &g->pile[SOL_STOCK];
    waste = &g->pile[SOL_WASTE];
    if (stock->n > 0) {
        waste->card[waste->n++] = stock->card[--stock->n];
        stock->down = stock->n;
        g->moves++;
        return SOL_DREW;
    }
    if (waste->n == 0) {
        return SOL_NOTHING;
    }
    /* Turning the waste over: its top card becomes the bottom of the stock,
     * so the next pass deals the cards in the order the last one did. */
    while (waste->n > 0) {
        stock->card[stock->n++] = waste->card[--waste->n];
    }
    stock->down = stock->n;
    g->passes++;
    g->moves++;
    return SOL_TURNED;
}

int sol_foundation_target(const struct sol_game *g, int pile, int index)
{
    sol_card_t c;
    int to;

    if (!g || !pile_ok(pile) || index < 0 || index >= g->pile[pile].n) {
        return -1;
    }
    c = g->pile[pile].card[index];
    to = SOL_F0 + (int)sol_card_suit(c);
    return sol_can_move(g, pile, index, to) == SOL_OK ? to : -1;
}

int sol_game_valid(const struct sol_game *g)
{
    uint8_t seen[SOL_DECK];
    int total = 0;
    int p;
    int i;

    if (!g) {
        return 0;
    }
    memset(seen, 0, sizeof(seen));
    for (p = 0; p < SOL_PILES; p++) {
        const struct sol_stack *s = &g->pile[p];

        if (s->n > SOL_DECK || s->down > s->n) {
            return 0;
        }
        for (i = 0; i < s->n; i++) {
            if (!sol_card_valid(s->card[i]) || seen[s->card[i]]) {
                return 0;
            }
            seen[s->card[i]] = 1;
        }
        total += s->n;
        if (p == SOL_STOCK && s->down != s->n) {
            return 0;
        }
        if ((p == SOL_WASTE || sol_is_foundation(p)) && s->down != 0) {
            return 0;
        }
        if (sol_is_foundation(p)) {
            if (s->n > SOL_RANKS) {
                return 0;
            }
            for (i = 0; i < s->n; i++) {
                if ((int)sol_card_suit(s->card[i]) != p - SOL_F0 || sol_card_rank(s->card[i]) != i + 1) {
                    return 0;
                }
            }
        }
        if (sol_is_column(p)) {
            /* A column never shows a face-down card on top. */
            if (s->n > 0 && s->down == s->n) {
                return 0;
            }
            for (i = s->down; i + 1 < s->n; i++) {
                if (!builds_on(s->card[i + 1], s->card[i])) {
                    return 0;
                }
            }
        }
    }
    if (total != SOL_DECK) {
        return 0;
    }
    if (g->won != (sol_cards_home(g) == SOL_DECK)) {
        return 0;
    }
    return 1;
}
