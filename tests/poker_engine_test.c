/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker.h"
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            ++failed;                                                                                        \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                                      \
        }                                                                                                    \
    } while (0)
static bj_card_t card(int rank, int suit)
{
    return bj_card(rank, (enum bj_suit)suit);
}
static uint32_t total(const struct poker_game *g)
{
    uint32_t sum = poker_pot_total(g);
    for (int i = 0; i < 4; ++i)
        sum += g->player[i].stack;
    return sum;
}
static void passive(struct poker_game *g)
{
    struct poker_options o = poker_options(g);
    CHECK(poker_act(g, o.can_check ? POKER_CHECK : POKER_CALL, 0));
}
static void finish(struct poker_game *g)
{
    int steps = 0;
    while (poker_playing(g) && steps++ < 200)
        passive(g);
    CHECK(!poker_playing(g));
}
static void rejected(struct poker_game *g, enum poker_action action, uint32_t amount)
{
    struct poker_game before = *g;
    CHECK(!poker_act(g, action, amount));
    CHECK(memcmp(&before, g, sizeof(*g)) == 0);
}
static void deck_test(void)
{
    struct poker_game a, b;
    for (unsigned seed = 0; seed < 128; ++seed) {
        uint64_t seen = 0;
        poker_restart(&a, seed);
        poker_restart(&b, seed);
        CHECK(poker_new_hand(&a));
        CHECK(poker_new_hand(&b));
        CHECK(memcmp(a.deck, b.deck, 52) == 0);
        for (int i = 0; i < 52; ++i) {
            CHECK(bj_card_valid(a.deck[i]));
            CHECK(!(seen & (UINT64_C(1) << a.deck[i])));
            seen |= UINT64_C(1) << a.deck[i];
        }
        CHECK(seen == ((UINT64_C(1) << 52) - 1));
        CHECK(a.next_card == 8);
    }
    poker_restart(&a, 1);
    poker_restart(&b, 2);
    poker_new_hand(&a);
    poker_new_hand(&b);
    CHECK(memcmp(a.deck, b.deck, 52) != 0);
}
static void ranking_test(void)
{
    bj_card_t royal[] = {card(1, 0),  card(13, 0), card(12, 0), card(11, 0),
                         card(10, 0), card(2, 1),  card(3, 2)};
    bj_card_t wheel[] = {card(1, 0), card(2, 1),  card(3, 2), card(4, 3),
                         card(5, 0), card(13, 1), card(12, 2)};
    bj_card_t six[] = {card(2, 0), card(3, 1), card(4, 2), card(5, 3), card(6, 0)};
    bj_card_t house[] = {card(13, 0), card(13, 1), card(13, 2), card(12, 0),
                         card(12, 1), card(12, 2), card(1, 0)};
    bj_card_t two[] = {card(1, 0),  card(1, 1),  card(13, 0), card(13, 1),
                       card(12, 0), card(12, 1), card(2, 2)};
    bj_card_t a[] = {card(9, 0), card(9, 1), card(1, 2), card(13, 0), card(12, 1)};
    bj_card_t b[] = {card(9, 2), card(9, 3), card(1, 3), card(13, 2), card(11, 1)};
    CHECK(poker_rank_category(poker_evaluate(royal, 7)) == POKER_STRAIGHT_FLUSH);
    CHECK(poker_rank_category(poker_evaluate(wheel, 7)) == POKER_STRAIGHT);
    CHECK(poker_evaluate(six, 5) > poker_evaluate(wheel, 7));
    CHECK(poker_rank_category(poker_evaluate(house, 7)) == POKER_FULL_HOUSE);
    CHECK((poker_evaluate(house, 7) & 0xfffffu) == 0xdc000u);
    CHECK((poker_evaluate(two, 7) & 0xfffffu) == 0xedc00u);
    CHECK(poker_evaluate(a, 5) > poker_evaluate(b, 5));
    for (int i = 0; i < 5; ++i)
        b[i] = a[4 - i];
    CHECK(poker_evaluate(a, 5) == poker_evaluate(b, 5));
    CHECK(!poker_evaluate(NULL, 7));
    CHECK(!poker_evaluate(a, 4));
    a[1] = a[0];
    CHECK(!poker_evaluate(a, 5));
    a[1] = 255;
    CHECK(!poker_evaluate(a, 5));
    /* Exhaustive, independent published five-card category frequencies.
     * This catches wheel/flush, pair multiplicity and category ordering bugs
     * across all 2,598,960 combinations, not just curated examples. */
    uint32_t counts[9] = {0};
    const uint32_t expected[9] = {1302540, 1098240, 123552, 54912, 10200, 5108, 3744, 624, 40};
    for (int i = 0; i < 48; ++i)
        for (int j = i + 1; j < 49; ++j)
            for (int k = j + 1; k < 50; ++k)
                for (int l = k + 1; l < 51; ++l)
                    for (int m = l + 1; m < 52; ++m) {
                        bj_card_t hand[] = {(bj_card_t)i, (bj_card_t)j, (bj_card_t)k, (bj_card_t)l,
                                            (bj_card_t)m};
                        ++counts[poker_rank_category(poker_evaluate(hand, 5))];
                    }
    for (int i = 0; i < 9; ++i)
        CHECK(counts[i] == expected[i]);
}
static void rounds_test(void)
{
    struct poker_game g;
    poker_restart(&g, 5);
    CHECK(poker_new_hand(&g));
    CHECK(g.button == 0 && g.small_blind == 1 && g.big_blind == 2 && g.actor == 3);
    CHECK(g.player[1].stack == 990 && g.player[2].stack == 980 && poker_pot_total(&g) == 30);
    rejected(&g, POKER_CHECK, 0);
    rejected(&g, POKER_RAISE, 39);
    rejected(&g, POKER_RAISE, 1001);
    CHECK(!poker_new_hand(&g));
    passive(&g);
    passive(&g);
    passive(&g);
    CHECK(g.actor == 2 && g.phase == POKER_PREFLOP); /* big blind's option */
    passive(&g);
    CHECK(g.phase == POKER_FLOP && g.board_count == 3 && g.actor == 1);
    CHECK(g.next_card == 12 && g.board[0] == g.deck[9]);
    for (int i = 0; i < 4; ++i)
        CHECK(g.player[i].bet == 0 && !g.player[i].acted);
    for (int i = 0; i < 4; ++i)
        passive(&g);
    CHECK(g.phase == POKER_TURN && g.board_count == 4 && g.next_card == 14);
    for (int i = 0; i < 4; ++i)
        passive(&g);
    CHECK(g.phase == POKER_RIVER && g.board_count == 5 && g.next_card == 16);
    for (int i = 0; i < 4; ++i)
        passive(&g);
    CHECK(g.phase == POKER_FINISHED && g.showdown && total(&g) == 4000);
    CHECK(poker_new_hand(&g));
    CHECK(g.button == 1 && g.small_blind == 2 && g.big_blind == 3 && g.actor == 0);
    finish(&g);
    poker_restart(&g, 5);
    CHECK(g.phase == POKER_READY && g.hand_number == 0);
    for (int i = 0; i < 4; ++i)
        CHECK(g.player[i].stack == 1000 && !g.player[i].in_hand);
    rejected(&g, POKER_CALL, 0);
    g.player[1].stack = g.player[3].stack = 0;
    CHECK(poker_new_hand(&g));
    CHECK(g.button == 0 && g.small_blind == 0 && g.big_blind == 2 && g.actor == 0);
    passive(&g);
    passive(&g);
    CHECK(g.phase == POKER_FLOP && g.actor == 2); /* heads-up order reverses postflop */
    finish(&g);
    CHECK(poker_new_hand(&g));
    CHECK(g.button == 2 && g.small_blind == 2 && g.big_blind == 0);

    poker_restart(&g, 51);
    for (int i = 0; i < 4; ++i) {
        CHECK(poker_new_hand(&g));
        finish(&g);
    }
    CHECK(g.button == 3 && g.big_blind == 1);
    g.player[2].stack = g.player[3].stack = 0;
    CHECK(poker_new_hand(&g));
    CHECK(g.button == 1 && g.small_blind == 1 && g.big_blind == 0 && g.actor == 1);
    finish(&g);
    CHECK(poker_new_hand(&g));
    CHECK(g.button == 0 && g.big_blind == 1);
}
static void raises_test(void)
{
    struct poker_game g;
    poker_restart(&g, 6);
    poker_new_hand(&g);
    CHECK(poker_act(&g, POKER_RAISE, 60));
    CHECK(g.min_raise == 40);
    CHECK(poker_options(&g).min_raise_to == 100);
    rejected(&g, POKER_RAISE, 99);
    CHECK(poker_act(&g, POKER_RAISE, 100));
    CHECK(g.min_raise == 40);
    passive(&g);
    passive(&g);
    passive(&g);
    CHECK(g.phase == POKER_FLOP && g.actor == 1); /* raiser need not act again */

    /* One short all-in leaves the caller's raising rights closed. */
    poker_restart(&g, 7);
    g.player[1].stack = 70;
    poker_new_hand(&g);
    CHECK(poker_act(&g, POKER_RAISE, 60));
    passive(&g);
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.current_bet == 70 && g.min_raise == 40);
    passive(&g);
    CHECK(g.actor == 3 && !poker_options(&g).can_raise);
    rejected(&g, POKER_RAISE, 110);
    rejected(&g, POKER_ALL_IN, 0);
    passive(&g);
    CHECK(g.actor == 0 && !poker_options(&g).can_raise);
    passive(&g);
    CHECK(g.phase == POKER_FLOP);

    /* Cumulative short raises of 20 reopen a previous 20 raise. */
    poker_restart(&g, 8);
    g.player[1].stack = 50;
    g.player[2].stack = 60;
    poker_new_hand(&g);
    CHECK(poker_act(&g, POKER_RAISE, 40));
    passive(&g);
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.actor == 3 && poker_options(&g).can_raise && poker_options(&g).min_raise_to == 80);
    CHECK(poker_act(&g, POKER_RAISE, 80));
    passive(&g);
    CHECK(g.phase == POKER_FLOP);

    /* A check before an incomplete opening bet keeps raising rights. */
    poker_restart(&g, 9);
    poker_new_hand(&g);
    while (g.phase == POKER_PREFLOP)
        passive(&g);
    CHECK(g.actor == 1);
    passive(&g);
    g.player[2].stack = 5;
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.current_bet == 5 && poker_options(&g).min_raise_to == 20);
    passive(&g);
    passive(&g);
    CHECK(g.actor == 1 && poker_options(&g).can_raise);
    CHECK(poker_act(&g, POKER_RAISE, 20));
    CHECK(g.min_raise == 20);
    finish(&g);
    /* A short blind is all-in, but the bring-in remains the full big blind. */
    poker_restart(&g, 10);
    g.player[2].stack = 7;
    poker_new_hand(&g);
    CHECK(g.current_bet == 20 && poker_options(&g).to_call == 20);
}
static void fixture(struct poker_game *g)
{
    const int ranks[] = {1, 13, 12, 11};
    for (int i = 0; i < 4; ++i) {
        g->player[i].hole[0] = card(ranks[i], 0);
        g->player[i].hole[1] = card(ranks[i], 1);
    }
    g->deck[9] = card(2, 2);
    g->deck[10] = card(3, 3);
    g->deck[11] = card(4, 2);
    g->deck[13] = card(8, 3);
    g->deck[15] = card(9, 2);
}
static void settlement_test(void)
{
    struct poker_game g;
    poker_restart(&g, 11);
    g.player[0].stack = 50;
    g.player[1].stack = 100;
    g.player[2].stack = 200;
    g.player[3].stack = 200;
    poker_new_hand(&g);
    fixture(&g);
    CHECK(poker_act(&g, POKER_ALL_IN, 0)); /* seat 3 bets 200 */
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(!poker_playing(&g) && g.showdown && g.board_count == 5 && g.pot_count == 3);
    CHECK(g.pots[0].amount == 200 && g.pots[0].winners == 1);
    CHECK(g.pots[1].amount == 150 && g.pots[1].winners == 2);
    CHECK(g.pots[2].amount == 200 && g.pots[2].winners == 4);
    CHECK(g.player[0].stack == 200 && g.player[1].stack == 150 && g.player[2].stack == 200 &&
          !g.player[3].stack);
    CHECK(total(&g) == 550 && g.settled_pot == 550);

    /* Each contribution tier splits independently on a board-only royal
     * flush. Nobody can win beyond their side-pot eligibility. */
    poker_restart(&g, 111);
    g.player[0].stack = 50;
    g.player[1].stack = 100;
    g.player[2].stack = 200;
    g.player[3].stack = 200;
    poker_new_hand(&g);
    fixture(&g);
    g.deck[9] = card(1, 2);
    g.deck[10] = card(13, 2);
    g.deck[11] = card(12, 2);
    g.deck[13] = card(11, 2);
    g.deck[15] = card(10, 2);
    for (int i = 0; i < 4; ++i)
        CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.pot_count == 3 && g.pots[0].winners == 15 && g.pots[1].winners == 14 && g.pots[2].winners == 12);
    CHECK(g.player[0].payout == 50 && g.player[1].payout == 100 && g.player[2].payout == 200 &&
          g.player[3].payout == 200);
    CHECK(total(&g) == 550);

    /* A board-only royal flush splits a 123-chip pot three ways; one folded
     * player's dead chip makes the main pot odd. Odd chips start left of D. */
    poker_restart(&g, 12);
    for (int i = 0; i < 4; ++i)
        g.player[i].stack = 41;
    poker_new_hand(&g);
    for (int i = 0; i < 4; ++i) {
        g.player[i].hole[0] = card(2 + i, 0);
        g.player[i].hole[1] = card(2 + i, 1);
    }
    g.deck[9] = card(1, 2);
    g.deck[10] = card(13, 2);
    g.deck[11] = card(12, 2);
    g.deck[13] = card(11, 2);
    g.deck[15] = card(10, 2);
    CHECK(poker_act(&g, POKER_FOLD, 0));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.player[0].stack == 41 && g.player[1].stack == 41 && g.player[2].stack == 41);
    CHECK(g.winners == 7 && total(&g) == 164);

    /* Three-way tie with folded small blind: 70 chips -> 24,23,23. */
    poker_restart(&g, 13);
    poker_new_hand(&g);
    for (int i = 0; i < 4; ++i) {
        g.player[i].hole[0] = card(2 + i, 0);
        g.player[i].hole[1] = card(2 + i, 1);
    }
    g.deck[9] = card(1, 2);
    g.deck[10] = card(13, 2);
    g.deck[11] = card(12, 2);
    g.deck[13] = card(11, 2);
    g.deck[15] = card(10, 2);
    passive(&g);
    passive(&g);
    CHECK(poker_act(&g, POKER_FOLD, 0));
    finish(&g);
    CHECK(g.player[0].payout == 23 && g.player[2].payout == 24 && g.player[3].payout == 23);
    CHECK(g.player[1].payout == 0 && total(&g) == 4000);

    /* Uncalled excess is returned, folded money still belongs to the winner. */
    poker_restart(&g, 14);
    poker_new_hand(&g);
    CHECK(poker_act(&g, POKER_RAISE, 200));
    for (int i = 0; i < 3; ++i)
        CHECK(poker_act(&g, POKER_FOLD, 0));
    CHECK(g.phase == POKER_FINISHED && !g.showdown && g.winners == 8);
    CHECK(g.player[3].stack == 1030 && g.player[3].payout == 230 && total(&g) == 4000);
    CHECK(g.pots[g.pot_count - 1].refund);
    CHECK(g.settled_pot == 50 && g.player[3].refunded == 180);

    /* Only one player can respond: calling/folding is required, raising into
     * a dry side pot is forbidden, then the board runs out automatically. */
    poker_restart(&g, 15);
    g.player[1].stack = g.player[2].stack = 0;
    g.player[3].stack = 40;
    poker_new_hand(&g);
    CHECK(g.actor == 0);
    CHECK(poker_act(&g, POKER_RAISE, 40));
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(!poker_playing(&g) && g.board_count == 5);
    CHECK(total(&g) == 1040);
    poker_restart(&g, 151);
    g.player[1].stack = g.player[2].stack = 0;
    g.player[3].stack = 40;
    poker_new_hand(&g);
    passive(&g);
    CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.actor == 0 && !poker_options(&g).can_raise && !poker_options(&g).can_all_in);
    rejected(&g, POKER_RAISE, 60);
    rejected(&g, POKER_ALL_IN, 0);
    passive(&g);
    CHECK(!poker_playing(&g) && g.board_count == 5 && total(&g) == 1040);
    poker_restart(&g, 16);
    g.player[0].stack = 0;
    CHECK(!poker_new_hand(&g) && g.phase == POKER_GAME_OVER);
    poker_restart(&g, 17);
    CHECK(g.phase == POKER_READY && total(&g) == 4000);
    CHECK(sizeof(g) <= 512); /* bounded per-session footprint */

    /* Winning the table and busting the human both end the game; neither
     * terminal state can deal again until restart. */
    poker_new_hand(&g);
    fixture(&g);
    for (int i = 0; i < 4; ++i)
        CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.phase == POKER_GAME_OVER && g.player[0].stack == 4000);
    CHECK(!poker_new_hand(&g));
    poker_restart(&g, 18);
    poker_new_hand(&g);
    fixture(&g);
    bj_card_t tmp[2];
    memcpy(tmp, g.player[0].hole, 2);
    memcpy(g.player[0].hole, g.player[1].hole, 2);
    memcpy(g.player[1].hole, tmp, 2);
    for (int i = 0; i < 4; ++i)
        CHECK(poker_act(&g, POKER_ALL_IN, 0));
    CHECK(g.phase == POKER_GAME_OVER && !g.player[0].stack && g.player[1].stack == 4000);
    CHECK(!poker_new_hand(&g));
    poker_restart(&g, 19);
    CHECK(g.phase == POKER_READY && total(&g) == 4000 && poker_new_hand(&g));
}
static void ai_test(void)
{
    struct poker_game a, b;
    for (unsigned seed = 1; seed <= 30; ++seed) {
        poker_restart(&a, seed);
        poker_new_hand(&a);
        int steps = 0;
        while (poker_playing(&a) && steps++ < 400) {
            struct poker_decision d = poker_ai(&a);
            CHECK(poker_act(&a, d.action, d.raise_to));
            CHECK(total(&a) == 4000);
        }
        CHECK(!poker_playing(&a));
    }
    /* Hidden information must not affect decisions, on any street. */
    poker_restart(&a, 123);
    poker_new_hand(&a);
    while (a.phase == POKER_PREFLOP)
        passive(&a);
    b = a;
    for (int i = 0; i < 4; ++i)
        if (i != b.actor) {
            b.player[i].hole[0] = 255;
            b.player[i].hole[1] = 255;
        }
    memset(b.deck, 255, sizeof(b.deck));
    struct poker_decision da = poker_ai(&a), db = poker_ai(&b);
    CHECK(da.action == db.action && da.raise_to == db.raise_to && a.rng.state == b.rng.state);
    /* Thousands of legal actions, chip conservation, turn legality and
     * tournament restarts under unequal stacks/all-in/raise/fold pressure. */
    poker_restart(&a, 999);
    for (int hand = 0; hand < 300; ++hand) {
        if (a.phase == POKER_GAME_OVER)
            poker_restart(&a, (uint32_t)hand + 1000);
        CHECK(poker_new_hand(&a));
        int steps = 0;
        while (poker_playing(&a) && steps++ < 400) {
            struct poker_options o = poker_options(&a);
            unsigned pick = bj_rng_below(&a.rng, 10);
            enum poker_action action = o.can_check ? POKER_CHECK : POKER_CALL;
            uint32_t amount = 0;
            if (pick == 0)
                action = POKER_FOLD;
            else if (pick == 1 && o.can_all_in)
                action = POKER_ALL_IN;
            else if (pick < 4 && o.can_raise && o.max_raise_to >= o.min_raise_to) {
                action = POKER_RAISE;
                amount = o.min_raise_to;
            }
            CHECK(poker_act(&a, action, amount));
            CHECK(total(&a) == 4000);
        }
        CHECK(!poker_playing(&a));
    }
}
int main(void)
{
    deck_test();
    ranking_test();
    rounds_test();
    raises_test();
    settlement_test();
    ai_test();
    printf("poker_engine_test: %d checks, %d failures\n", checks, failed);
    return failed != 0;
}
