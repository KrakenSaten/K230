/*
 * PG Solitaire rules: the deck and shuffle, the deal, tableau building,
 * colour and rank refusals, moving runs, kings to empty columns, the
 * foundations, the stock and waste, winning, and random play that must
 * never lose or copy a card.
 *
 * Positions are built card by card from the ruleset in sol_rules.h; the
 * expected answers are written out, never computed by the code under test.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sol_rules.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

#define S SOL_SPADES
#define H SOL_HEARTS
#define C SOL_CLUBS
#define D SOL_DIAMONDS

/* Cards as (rank, suit) pairs, terminated by rank 0. */
static void set_pile(struct sol_game *g, int pile, int down, ...)
{
    struct sol_stack *s = &g->pile[pile];
    va_list ap;

    s->n = 0;
    va_start(ap, down);
    for (;;) {
        int rank = va_arg(ap, int);
        int suit;

        if (rank == 0) {
            break;
        }
        suit = va_arg(ap, int);
        s->card[s->n++] = sol_card(rank, (enum sol_suit)suit);
    }
    va_end(ap);
    s->down = (uint8_t)down;
}

static void empty_game(struct sol_game *g)
{
    memset(g, 0, sizeof(*g));
}

/* ---- cards and deck ------------------------------------------------------- */

static void test_cards(void)
{
    sol_card_t deck[SOL_DECK];
    sol_card_t again[SOL_DECK];
    uint8_t seen[SOL_DECK];
    struct sol_rng rng;
    int i;
    int moved = 0;
    int differ = 0;
    uint32_t seed;

    check("ace of spades is card 0", sol_card(SOL_ACE, S) == 0);
    check("king of diamonds is card 51", sol_card(SOL_KING, D) == 51);
    check("rank and suit come back", sol_card_rank(sol_card(7, C)) == 7 && sol_card_suit(sol_card(7, C)) == C);
    check("hearts and diamonds are red", sol_card_is_red(sol_card(3, H)) && sol_card_is_red(sol_card(3, D)));
    check("spades and clubs are black", !sol_card_is_red(sol_card(3, S)) && !sol_card_is_red(sol_card(3, C)));
    check("rank 0 and 14 are not cards", sol_card(0, S) == 0xFF && sol_card(14, S) == 0xFF);
    check("a fifth suit is not a card", sol_card(1, (enum sol_suit)4) == 0xFF);
    check("an invalid card has no rank and no colour", sol_card_rank(0xFF) == 0 && !sol_card_is_red(0xFF));
    check("rank names", strcmp(sol_rank_text(1), "A") == 0 && strcmp(sol_rank_text(10), "10") == 0 &&
                            strcmp(sol_rank_text(11), "J") == 0 && strcmp(sol_rank_text(13), "K") == 0 &&
                            strcmp(sol_rank_text(14), "") == 0);

    sol_deck_fill(deck);
    sol_rng_seed(&rng, 2026);
    sol_deck_shuffle(deck, &rng);
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < SOL_DECK; i++) {
        if (sol_card_valid(deck[i])) {
            seen[deck[i]]++;
        }
        moved += deck[i] != i;
    }
    {
        int unique = 1;

        for (i = 0; i < SOL_DECK; i++) {
            unique &= seen[i] == 1;
        }
        check("a shuffled deck holds exactly the 52 cards, once each", unique);
    }
    check("and is actually shuffled", moved > 40);
    sol_deck_fill(again);
    sol_rng_seed(&rng, 2026);
    sol_deck_shuffle(again, &rng);
    check("the same seed shuffles the same way", memcmp(deck, again, sizeof(deck)) == 0);
    for (seed = 1; seed <= 20; seed++) {
        sol_deck_fill(again);
        sol_rng_seed(&rng, seed);
        sol_deck_shuffle(again, &rng);
        differ += memcmp(deck, again, sizeof(deck)) != 0;
    }
    check("other seeds shuffle differently", differ == 20);

    /* Every card reaches every position about equally often: 52 000 shuffles,
     * so each (card, position) pair expects 1000; bounds are over six
     * standard deviations. */
    {
        static int count[SOL_DECK][SOL_DECK];
        int worst_lo = 100000;
        int worst_hi = 0;
        int k;
        int j;

        sol_rng_seed(&rng, 777);
        for (k = 0; k < 52000; k++) {
            sol_deck_fill(deck);
            sol_deck_shuffle(deck, &rng);
            for (j = 0; j < SOL_DECK; j++) {
                count[deck[j]][j]++;
            }
        }
        for (i = 0; i < SOL_DECK; i++) {
            for (j = 0; j < SOL_DECK; j++) {
                worst_lo = count[i][j] < worst_lo ? count[i][j] : worst_lo;
                worst_hi = count[i][j] > worst_hi ? count[i][j] : worst_hi;
            }
        }
        check("the shuffle is uniform over positions", worst_lo > 800 && worst_hi < 1200);
    }
}

/* ---- the deal ------------------------------------------------------------ */

static void test_deal(void)
{
    struct sol_game g;
    struct sol_game h;
    int c;
    int ok = 1;

    sol_deal(&g, 42);
    for (c = 0; c < SOL_COLUMNS; c++) {
        ok &= g.pile[SOL_T0 + c].n == c + 1 && g.pile[SOL_T0 + c].down == c;
    }
    check("columns hold 1..7 cards with only the top face up", ok);
    check("the stock holds the other 24, all face down",
          g.pile[SOL_STOCK].n == 24 && g.pile[SOL_STOCK].down == 24);
    check("waste and foundations start empty", g.pile[SOL_WASTE].n == 0 && sol_cards_home(&g) == 0);
    check("nothing counted yet", g.moves == 0 && g.passes == 0 && !g.won && g.seed == 42);
    check("a deal is a valid position", sol_game_valid(&g));
    sol_deal(&h, 42);
    check("the same seed deals the same game", memcmp(&g, &h, sizeof(g)) == 0);
    sol_deal(&h, 43);
    check("another seed deals another", memcmp(g.pile, h.pile, sizeof(g.pile)) != 0);
    /* A new game after play is a clean deal, not a leftover. */
    sol_draw(&h);
    sol_draw(&h);
    sol_deal(&h, 42);
    check("dealing again resets everything", memcmp(&g, &h, sizeof(g)) == 0);
}

/* ---- the tableau ------------------------------------------------------------ */

static void test_tableau(void)
{
    struct sol_game g;
    struct sol_moved m;

    empty_game(&g);
    set_pile(&g, SOL_T0, 0, 8, S, 0);          /* black 8 */
    set_pile(&g, SOL_T0 + 1, 0, 7, H, 0);      /* red 7 */
    set_pile(&g, SOL_T0 + 2, 0, 7, C, 0);      /* black 7 */
    set_pile(&g, SOL_T0 + 3, 0, 6, D, 0);      /* red 6 */
    set_pile(&g, SOL_T0 + 4, 0, 9, H, 0);      /* red 9 */

    check("a red 7 goes on a black 8", sol_can_move(&g, SOL_T0 + 1, 0, SOL_T0) == SOL_OK);
    check("a black 7 does not: same colour", sol_can_move(&g, SOL_T0 + 2, 0, SOL_T0) == SOL_ERR_COLOUR);
    check("a red 6 does not go on a black 8: wrong rank", sol_can_move(&g, SOL_T0 + 3, 0, SOL_T0) == SOL_ERR_RANK);
    check("a black 8 does not go on a red 7: building is downward",
          sol_can_move(&g, SOL_T0, 0, SOL_T0 + 1) == SOL_ERR_RANK);
    check("a black 8 goes on a red 9", sol_can_move(&g, SOL_T0, 0, SOL_T0 + 4) == SOL_OK);
    check("a card does not move onto its own pile", sol_can_move(&g, SOL_T0, 0, SOL_T0) == SOL_ERR_SAME_PILE);

    check("the legal move happens", sol_move(&g, SOL_T0 + 1, 0, SOL_T0, &m) == SOL_OK && m.count == 1);
    check("the 7 now lies on the 8", g.pile[SOL_T0].n == 2 && sol_top(&g, SOL_T0) == sol_card(7, H));
    check("and its column is empty", g.pile[SOL_T0 + 1].n == 0);
    check("a move is counted", g.moves == 1);
    {
        struct sol_game before = g;

        check("an illegal move is refused", sol_move(&g, SOL_T0 + 2, 0, SOL_T0 + 3, &m) == SOL_ERR_RANK);
        check("and changes nothing", memcmp(&before, &g, sizeof(g)) == 0 && m.count == 0);
    }

    /* A run: the 8-7 moves onto the red 9 as one. */
    check("the run 8 7 can be picked from its 8", sol_can_pick(&g, SOL_T0, 0) == SOL_OK);
    check("and moves onto the red 9", sol_move(&g, SOL_T0, 0, SOL_T0 + 4, &m) == SOL_OK && m.count == 2);
    check("the column reads 9 8 7", g.pile[SOL_T0 + 4].n == 3 &&
                                        g.pile[SOL_T0 + 4].card[0] == sol_card(9, H) &&
                                        g.pile[SOL_T0 + 4].card[1] == sol_card(8, S) &&
                                        g.pile[SOL_T0 + 4].card[2] == sol_card(7, H));
    /* Part of a run: the 7 alone. */
    check("the top card of a run is picked on its own", sol_can_pick(&g, SOL_T0 + 4, 2) == SOL_OK);
    check("and a red 7 on a black 7 is refused on rank", sol_can_move(&g, SOL_T0 + 4, 2, SOL_T0 + 2) == SOL_ERR_RANK);
    check("an index past the top is not a card", sol_can_pick(&g, SOL_T0 + 4, 3) == SOL_ERR_BAD_PILE);
    check("a negative index is not a card", sol_can_pick(&g, SOL_T0 + 4, -1) == SOL_ERR_BAD_PILE);

    /* A column whose face-up part is not a run cannot be lifted from below. */
    empty_game(&g);
    set_pile(&g, SOL_T0, 0, 9, S, 5, H, 0);
    check("cards that do not descend in alternating colours are not a run",
          sol_can_pick(&g, SOL_T0, 0) == SOL_ERR_NOT_A_RUN);
    check("but the top card alone can be picked", sol_can_pick(&g, SOL_T0, 1) == SOL_OK);

    /* Face-down cards, and the flip. */
    empty_game(&g);
    set_pile(&g, SOL_T0, 2, 2, C, 13, D, 4, H, 0); /* two down, 4H up */
    set_pile(&g, SOL_T0 + 1, 0, 5, S, 0);
    check("a face-down card cannot be picked", sol_can_pick(&g, SOL_T0, 1) == SOL_ERR_FACE_DOWN);
    check("moving the last face-up card uncovers one", sol_move(&g, SOL_T0, 2, SOL_T0 + 1, &m) == SOL_OK);
    check("which turns face up", m.flipped == 1 && g.pile[SOL_T0].n == 2 && g.pile[SOL_T0].down == 1);
    check("and can now be played", sol_can_pick(&g, SOL_T0, 1) == SOL_OK);

    /* Kings and empty columns. */
    empty_game(&g);
    set_pile(&g, SOL_T0, 0, 0);
    set_pile(&g, SOL_T0 + 1, 1, 3, S, 12, H, 0);
    set_pile(&g, SOL_T0 + 2, 0, 13, C, 12, D, 11, S, 0);
    set_pile(&g, SOL_WASTE, 0, 13, H, 0);
    check("a queen does not go to an empty column", sol_can_move(&g, SOL_T0 + 1, 1, SOL_T0) == SOL_ERR_KING_ONLY);
    check("a king from the waste does", sol_can_move(&g, SOL_WASTE, 0, SOL_T0) == SOL_OK);
    check("a run headed by a king does too", sol_move(&g, SOL_T0 + 2, 0, SOL_T0, &m) == SOL_OK && m.count == 3);
    check("leaving its old column empty, with nothing to flip", g.pile[SOL_T0 + 2].n == 0 && m.flipped == 0);
    check("a run headed by a queen does not go to an empty column",
          sol_can_move(&g, SOL_T0, 1, SOL_T0 + 2) == SOL_ERR_KING_ONLY);
}

/* ---- foundations ------------------------------------------------------------ */

static void test_foundations(void)
{
    struct sol_game g;
    struct sol_moved m;

    empty_game(&g);
    set_pile(&g, SOL_WASTE, 0, 1, H, 0);
    set_pile(&g, SOL_T0, 0, 2, H, 0);
    set_pile(&g, SOL_T0 + 1, 0, 3, H, 2, S, 0);
    set_pile(&g, SOL_T0 + 2, 0, 1, S, 0);
    set_pile(&g, SOL_T0 + 3, 0, 2, D, 0);

    check("a 2 does not start a foundation", sol_can_move(&g, SOL_T0, 0, SOL_F0 + H) == SOL_ERR_RANK);
    check("an ace does not go on another suit's foundation", sol_can_move(&g, SOL_WASTE, 0, SOL_F0 + S) == SOL_ERR_SUIT);
    check("the ace of hearts starts hearts", sol_move(&g, SOL_WASTE, 0, SOL_F0 + H, &m) == SOL_OK);
    check("the 2 of hearts follows", sol_move(&g, SOL_T0, 0, SOL_F0 + H, &m) == SOL_OK);
    check("a 2 of diamonds does not go on hearts", sol_can_move(&g, SOL_T0 + 3, 0, SOL_F0 + H) == SOL_ERR_SUIT);
    check("a buried card cannot go up: one card at a time",
          sol_can_move(&g, SOL_T0 + 1, 0, SOL_F0 + H) == SOL_ERR_NOT_A_RUN ||
              sol_can_move(&g, SOL_T0 + 1, 0, SOL_F0 + H) == SOL_ERR_ONE_CARD);
    check("the foundation target of a playable card is found",
          sol_foundation_target(&g, SOL_T0 + 2, 0) == SOL_F0 + S);
    check("and there is none for a card that cannot go", sol_foundation_target(&g, SOL_T0 + 3, 0) == -1);
    check("two in the foundation", sol_cards_home(&g) == 2);
    check("the top of a foundation may come back to the tableau",
          sol_can_pick(&g, SOL_F0 + H, 1) == SOL_OK);
    set_pile(&g, SOL_T0 + 4, 0, 3, S, 0);
    check("the 2 of hearts goes back onto a black 3", sol_move(&g, SOL_F0 + H, 1, SOL_T0 + 4, &m) == SOL_OK);
    check("only the top card of a foundation", sol_can_pick(&g, SOL_F0 + H, 0) == SOL_OK &&
                                                   g.pile[SOL_F0 + H].n == 1);
    check("foundation to foundation is not a move", sol_can_move(&g, SOL_F0 + H, 0, SOL_F0 + S) == SOL_ERR_BAD_PILE);
    check("a run of two cannot go to a foundation",
          sol_can_move(&g, SOL_T0 + 4, 0, SOL_F0 + S) == SOL_ERR_ONE_CARD);
    set_pile(&g, SOL_WASTE, 0, 5, C, 6, H, 0);
    check("only the waste's top card can be picked", sol_can_pick(&g, SOL_WASTE, 0) == SOL_ERR_NOT_TOP &&
                                                        sol_can_pick(&g, SOL_WASTE, 1) == SOL_OK);
    check("nothing goes onto the waste", sol_can_move(&g, SOL_T0 + 2, 0, SOL_WASTE) == SOL_ERR_BAD_PILE);
    check("nothing goes onto the stock", sol_can_move(&g, SOL_T0 + 2, 0, SOL_STOCK) == SOL_ERR_STOCK);
    check("nothing is picked from the stock", sol_can_pick(&g, SOL_STOCK, 0) == SOL_ERR_STOCK);
    check("an empty pile has nothing to pick", sol_can_pick(&g, SOL_T0 + 5, 0) == SOL_ERR_EMPTY);
    check("pile numbers outside the table are refused", sol_can_pick(&g, SOL_PILES, 0) == SOL_ERR_BAD_PILE &&
                                                            sol_can_move(&g, SOL_T0 + 2, 0, -1) == SOL_ERR_BAD_PILE &&
                                                            sol_can_move(&g, SOL_T0 + 2, 0, 99) == SOL_ERR_BAD_PILE);
    check("a NULL game is refused", sol_can_pick(NULL, SOL_T0, 0) == SOL_ERR_BAD_PILE);
}

/* ---- stock and waste --------------------------------------------------------- */

static void test_stock(void)
{
    struct sol_game g;
    sol_card_t first_pass[24];
    int i;
    int ok = 1;

    sol_deal(&g, 9);
    for (i = 0; i < 24; i++) {
        sol_card_t expect = g.pile[SOL_STOCK].card[g.pile[SOL_STOCK].n - 1];

        ok &= sol_draw(&g) == SOL_DREW && sol_top(&g, SOL_WASTE) == expect;
        first_pass[i] = expect;
    }
    check("draw-1: each draw turns the stock's top card onto the waste", ok);
    check("24 draws empty the stock", g.pile[SOL_STOCK].n == 0 && g.pile[SOL_WASTE].n == 24);
    check("each draw is a move", g.moves == 24);
    check("the waste is face up", g.pile[SOL_WASTE].down == 0 && sol_game_valid(&g));
    check("drawing from an empty stock turns the waste over", sol_draw(&g) == SOL_TURNED);
    check("all 24 go back, face down, and the waste is empty",
          g.pile[SOL_STOCK].n == 24 && g.pile[SOL_STOCK].down == 24 && g.pile[SOL_WASTE].n == 0);
    check("a pass is counted", g.passes == 1 && g.moves == 25);
    ok = 1;
    for (i = 0; i < 24; i++) {
        ok &= sol_draw(&g) == SOL_DREW && sol_top(&g, SOL_WASTE) == first_pass[i];
    }
    check("the second pass deals the same cards in the same order", ok);
    for (i = 0; i < 5; i++) {
        sol_draw(&g);
        while (g.pile[SOL_STOCK].n) {
            sol_draw(&g);
        }
    }
    check("passes are unlimited", g.passes == 6 && sol_game_valid(&g));

    empty_game(&g);
    set_pile(&g, SOL_T0, 0, 13, S, 0);
    check("with stock and waste both empty a draw does nothing", sol_draw(&g) == SOL_NOTHING && g.moves == 0);
    check("a NULL game draws nothing", sol_draw(NULL) == SOL_NOTHING);
}

/* ---- winning -------------------------------------------------------------------- */

static void test_win(void)
{
    struct sol_game g;
    struct sol_moved m;
    int s;
    int r;

    empty_game(&g);
    for (s = 0; s < SOL_SUITS; s++) {
        for (r = 1; r <= 13; r++) {
            if (!(s == D && r == 13)) {
                g.pile[SOL_F0 + s].card[g.pile[SOL_F0 + s].n++] = sol_card(r, (enum sol_suit)s);
            }
        }
    }
    set_pile(&g, SOL_T0 + 6, 0, 13, D, 0);
    check("51 home and one king left is valid and not won", sol_game_valid(&g) && !g.won && sol_cards_home(&g) == 51);
    check("the last card home wins", sol_move(&g, SOL_T0 + 6, 0, SOL_F0 + D, &m) == SOL_OK && m.won && g.won);
    check("all 52 are home", sol_cards_home(&g) == 52 && sol_game_valid(&g));
    check("a won game takes no more moves", sol_can_pick(&g, SOL_F0 + D, 12) == SOL_ERR_WON &&
                                                sol_draw(&g) == SOL_NOTHING);
    g.won = 0;
    check("a full foundation row not marked won is invalid", !sol_game_valid(&g));
}

/* ---- conservation under random play ------------------------------------------ */

static void test_random_play(void)
{
    struct sol_rng pick;
    uint32_t seed;
    int broken = 0;
    int wins = 0;
    long moves = 0;

    sol_rng_seed(&pick, 11);
    for (seed = 1; seed <= 400; seed++) {
        struct sol_game g;
        int turn;

        sol_deal(&g, seed);
        for (turn = 0; turn < 1500 && !g.won; turn++) {
            int from = (int)sol_rng_below(&pick, SOL_PILES);
            int to = (int)sol_rng_below(&pick, SOL_PILES);
            struct sol_game before;
            enum sol_result can;
            enum sol_result did;
            int index;
            int target;

            /* Draw now and then, so games get somewhere. */
            if (sol_rng_below(&pick, 3) == 0) {
                sol_draw(&g);
            }
            index = g.pile[from].n ? (int)sol_rng_below(&pick, g.pile[from].n) : 0;
            before = g;
            can = sol_can_move(&g, from, index, to);
            did = sol_move(&g, from, index, to, NULL);
            if (did != can) {
                broken++;
            }
            if (did != SOL_OK && memcmp(&before, &g, sizeof(g)) != 0) {
                broken++;
            }
            for (target = 0; target < SOL_PILES; target++) {
                int t = sol_foundation_target(&g, target, g.pile[target].n - 1);

                if (t >= 0) {
                    sol_move(&g, target, g.pile[target].n - 1, t, NULL);
                }
            }
            if (!sol_game_valid(&g)) {
                broken++;
                printf("FAIL seed %u turn %d: position invalid\n", (unsigned)seed, turn);
                break;
            }
        }
        wins += g.won;
        moves += g.moves;
    }
    check("400 random games never lose, copy or misplace a card", broken == 0);
    check("and random play does move cards", moves > 400 * 50);
    printf("note: random play won %d of 400 deals\n", wins);

    {
        struct sol_game g;

        sol_deal(&g, 5);
        g.pile[SOL_T0].card[0] = g.pile[SOL_T0 + 1].card[1];
        check("a duplicated card is invalid", !sol_game_valid(&g));
        sol_deal(&g, 5);
        g.pile[SOL_STOCK].n--;
        check("a lost card is invalid", !sol_game_valid(&g));
        sol_deal(&g, 5);
        g.pile[SOL_T0 + 3].down = 4;
        check("a column showing a face-down top card is invalid", !sol_game_valid(&g));
        sol_deal(&g, 5);
        g.pile[SOL_STOCK].down = 3;
        check("a face-up card in the stock is invalid", !sol_game_valid(&g));
        check("NULL is not a game", !sol_game_valid(NULL));
    }
}

int main(void)
{
    test_cards();
    test_deal();
    test_tableau();
    test_foundations();
    test_stock();
    test_win();
    test_random_play();
    printf("sol_rules_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
