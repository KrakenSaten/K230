/*
 * PG Blackjack rules: hand totals with soft and hard aces, naturals, the
 * dealer's 16/17 boundary including soft 17, busts, pushes, doubling, the
 * bet and the bankroll, reshuffling and a shoe that runs dry mid-round, new
 * rounds, determinism, and thousands of random rounds that must never lose,
 * copy or create a card or a chip.
 *
 * Hands are dealt from a stacked shoe, so every outcome here is written out
 * from the ruleset in bj_rules.h, never computed by the code under test.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "bj_rules.h"

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

#define S BJ_SPADES
#define H BJ_HEARTS
#define C BJ_CLUBS
#define D BJ_DIAMONDS

static void hand(struct bj_hand *h, ...)
{
    va_list ap;

    h->n = 0;
    va_start(ap, h);
    for (;;) {
        int rank = va_arg(ap, int);

        if (rank == 0) {
            break;
        }
        h->card[h->n++] = bj_card(rank, (enum bj_suit)va_arg(ap, int));
    }
    va_end(ap);
}

/* Put these ranks on top of the shoe in the order they will be dealt: player,
 * dealer, player, dealer, then any hits and dealer draws. Suits cycle so no
 * id repeats more than a shoe allows. Filler below keeps the shoe above the
 * reshuffle mark. */
static void stack(struct bj_game *g, ...)
{
    int ranks[40];
    int k = 0;
    int i;
    va_list ap;

    va_start(ap, g);
    for (;;) {
        int r = va_arg(ap, int);

        if (r == 0) {
            break;
        }
        ranks[k++] = r;
    }
    va_end(ap);
    g->shoe.n = (uint8_t)(40 + k);
    for (i = 0; i < 40; i++) {
        g->shoe.card[i] = bj_card(2 + i % 3, (enum bj_suit)(i % 4)); /* 2s, 3s, 4s */
    }
    for (i = 0; i < k; i++) {
        g->shoe.card[g->shoe.n - 1 - i] = bj_card(ranks[i], (enum bj_suit)((i + 1) % 4));
    }
}

static void fresh(struct bj_game *g)
{
    bj_new_session(g, 1234);
}

/* ---- totals --------------------------------------------------------------------- */

static void test_totals(void)
{
    struct bj_hand h;
    int soft;

    check("courts count ten", bj_card_points(bj_card(11, S)) == 10 && bj_card_points(bj_card(13, H)) == 10);
    check("an ace counts one before aces are decided", bj_card_points(bj_card(1, C)) == 1);
    hand(&h, 1, S, 13, H, 0);
    check("A K is 21, soft", bj_hand_total(&h, &soft) == 21 && soft);
    check("and a blackjack", bj_is_blackjack(&h));
    hand(&h, 1, S, 1, H, 0);
    check("A A is soft 12", bj_hand_total(&h, &soft) == 12 && soft);
    hand(&h, 1, S, 1, H, 1, D, 0);
    check("A A A is soft 13", bj_hand_total(&h, &soft) == 13 && soft);
    hand(&h, 1, S, 6, H, 0);
    check("A 6 is soft 17", bj_hand_total(&h, &soft) == 17 && soft);
    hand(&h, 1, S, 6, H, 10, C, 0);
    check("A 6 10 is hard 17: the ace drops to one", bj_hand_total(&h, &soft) == 17 && !soft);
    hand(&h, 1, S, 1, H, 9, C, 0);
    check("A A 9 is soft 21", bj_hand_total(&h, &soft) == 21 && soft);
    check("but three cards are not a blackjack", !bj_is_blackjack(&h));
    hand(&h, 10, S, 6, H, 1, C, 0);
    check("10 6 A is hard 17", bj_hand_total(&h, &soft) == 17 && !soft);
    hand(&h, 1, S, 1, H, 1, D, 1, C, 7, S, 0);
    check("four aces and a 7 are soft 21", bj_hand_total(&h, &soft) == 21 && soft);
    hand(&h, 1, S, 1, H, 1, D, 1, C, 7, S, 10, H, 0);
    check("and a 10 more is hard 21", bj_hand_total(&h, &soft) == 21 && !soft && !bj_is_bust(&h));
    hand(&h, 10, S, 12, H, 0);
    check("10 Q is 20, not a blackjack", bj_hand_total(&h, NULL) == 20 && !bj_is_blackjack(&h));
    hand(&h, 10, S, 6, H, 6, C, 0);
    check("10 6 6 busts at 22", bj_hand_total(&h, &soft) == 22 && !soft && bj_is_bust(&h));
    hand(&h, 0);
    check("an empty hand is 0", bj_hand_total(&h, &soft) == 0 && !soft);
    check("NULL is 0", bj_hand_total(NULL, NULL) == 0);
}

/* ---- naturals ---------------------------------------------------------------------- */

static void test_naturals(void)
{
    struct bj_game g;

    fresh(&g);
    stack(&g, 1, 9, 13, 7, 0); /* player A K, dealer 9 7 */
    check("deal", bj_deal(&g) == BJ_OK);
    check("a player blackjack settles at once", g.phase == BJ_SETTLED && g.outcome == BJ_PLAYER_BLACKJACK);
    check("paying 3:2: +15 on 10", g.last_delta == 15 && g.bankroll == 1015);
    check("the dealer draws nothing", g.dealer.n == 2);
    check("no hit or stand after it", bj_hit(&g) == BJ_ERR_PHASE && bj_stand(&g) == BJ_ERR_PHASE);

    fresh(&g);
    stack(&g, 1, 1, 12, 11, 0); /* player A Q, dealer A J */
    bj_deal(&g);
    check("two blackjacks push", g.outcome == BJ_PUSH && g.last_delta == 0 && g.bankroll == 1000);

    fresh(&g);
    stack(&g, 10, 1, 9, 13, 0); /* player 10 9, dealer A K */
    bj_deal(&g);
    check("a dealer blackjack takes the bet before the player acts",
          g.outcome == BJ_DEALER_BLACKJACK && g.phase == BJ_SETTLED && g.last_delta == -10 && g.bankroll == 990);
    check("so the player never doubles into it", bj_double(&g) == BJ_ERR_PHASE && g.player.n == 2);

    fresh(&g);
    g.bet = 30;
    stack(&g, 13, 5, 1, 6, 0);
    bj_deal(&g);
    check("3:2 on 30 is 45", g.outcome == BJ_PLAYER_BLACKJACK && g.last_delta == 45);
}

/* ---- the dealer -------------------------------------------------------------------------- */

static void test_dealer(void)
{
    struct bj_game g;

    fresh(&g);
    stack(&g, 10, 10, 8, 6, 2, 0); /* player 18; dealer 10 6 = 16, draws the 2 */
    bj_deal(&g);
    check("the player's hand is live", g.phase == BJ_PLAYER && g.outcome == BJ_NO_OUTCOME);
    check("with the bet on the table", g.bankroll == 990 && g.stake == 10 && bj_chips(&g) == 1000);
    bj_stand(&g);
    check("a dealer on 16 draws", g.dealer.n == 3 && bj_hand_total(&g.dealer, NULL) == 18);
    check("18 against 18 pushes", g.outcome == BJ_PUSH && g.bankroll == 1000);

    fresh(&g);
    stack(&g, 10, 10, 8, 7, 2, 0); /* dealer 17 */
    bj_deal(&g);
    bj_stand(&g);
    check("a dealer on hard 17 stands", g.dealer.n == 2);
    check("18 beats 17 and pays 1:1", g.outcome == BJ_PLAYER_WINS && g.last_delta == 10 && g.bankroll == 1010);

    fresh(&g);
    stack(&g, 10, 1, 7, 6, 3, 0); /* player 17; dealer A 6 = soft 17 */
    bj_deal(&g);
    bj_stand(&g);
    check("a dealer on soft 17 stands too", g.dealer.n == 2 && bj_hand_total(&g.dealer, NULL) == 17);
    check("17 against 17 pushes", g.outcome == BJ_PUSH);

    fresh(&g);
    stack(&g, 10, 10, 7, 6, 6, 0); /* player 17; dealer 16 draws 6 = 22 */
    bj_deal(&g);
    bj_stand(&g);
    check("a dealer who draws past 21 busts", g.outcome == BJ_DEALER_BUSTS && g.last_delta == 10);

    fresh(&g);
    /* Player 10 8 = 18. Dealer 5 6 = 11, draws 2 (13), an ace (14: as 11
     * it would be 24, so it counts one), then 4 (18) and stands. */
    stack(&g, 10, 5, 8, 6, 2, 1, 4, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("a dealer keeps drawing while under 17", g.dealer.n == 5 && bj_hand_total(&g.dealer, NULL) == 18);
    check("18 against 18 pushes", g.outcome == BJ_PUSH);

    fresh(&g);
    stack(&g, 10, 10, 6, 9, 0); /* player 16, dealer 19 */
    bj_deal(&g);
    bj_stand(&g);
    check("the higher dealer total wins", g.outcome == BJ_DEALER_WINS && g.last_delta == -10 && g.bankroll == 990);
}

/* ---- the player ----------------------------------------------------------------------------- */

static void test_player(void)
{
    struct bj_game g;

    fresh(&g);
    stack(&g, 10, 10, 6, 7, 9, 0); /* player 16 hits a 9 */
    bj_deal(&g);
    check("a hit gives one card", bj_hit(&g) == BJ_OK && g.player.n == 3);
    check("over 21 busts and loses at once", g.outcome == BJ_PLAYER_BUSTS && g.phase == BJ_SETTLED &&
                                                 g.last_delta == -10);
    check("the dealer does not draw against a bust", g.dealer.n == 2);

    fresh(&g);
    stack(&g, 5, 10, 6, 7, 10, 0); /* player 11 hits a 10 = 21 */
    bj_deal(&g);
    bj_hit(&g);
    check("21 stands by itself and the dealer plays", g.phase == BJ_SETTLED && g.outcome == BJ_PLAYER_WINS);

    fresh(&g);
    stack(&g, 1, 10, 5, 7, 5, 10, 0); /* A 5 soft 16, hit 5 = soft 21 */
    bj_deal(&g);
    bj_hit(&g);
    check("a soft 21 after a hit stands as well", g.phase == BJ_SETTLED && bj_hand_total(&g.player, NULL) == 21);

    fresh(&g);
    stack(&g, 1, 10, 6, 7, 9, 0); /* A 6 soft 17 hits 9 = hard 16 */
    bj_deal(&g);
    bj_hit(&g);
    check("a soft hand that takes a big card turns hard instead of busting",
          g.phase == BJ_PLAYER && bj_hand_total(&g.player, NULL) == 16);

    /* Doubling. */
    fresh(&g);
    stack(&g, 6, 10, 5, 7, 10, 0); /* player 11 doubles on a 10; dealer 17 */
    bj_deal(&g);
    check("a double is allowed on two cards", bj_can_double(&g));
    check("it succeeds", bj_double(&g) == BJ_OK);
    check("with exactly one more card, then the dealer plays", g.player.n == 3 && g.phase == BJ_SETTLED);
    check("and the stake doubled: 21 beats 17 for +20", g.outcome == BJ_PLAYER_WINS && g.last_delta == 20 &&
                                                            g.bankroll == 1020 && g.doubled);

    fresh(&g);
    stack(&g, 10, 10, 6, 7, 10, 0); /* 16 doubles into a bust */
    bj_deal(&g);
    bj_double(&g);
    check("a doubled bust loses twice the bet", g.outcome == BJ_PLAYER_BUSTS && g.last_delta == -20 &&
                                                    g.bankroll == 980 && g.dealer.n == 2);

    fresh(&g);
    stack(&g, 2, 10, 3, 7, 2, 0);
    bj_deal(&g);
    bj_hit(&g);
    check("no double after a hit", !bj_can_double(&g) && bj_double(&g) == BJ_ERR_NOT_FIRST);

    fresh(&g);
    g.bankroll = 30;
    g.bet = 20;
    stack(&g, 2, 10, 3, 7, 0);
    bj_deal(&g);
    check("no double the bankroll cannot cover", !bj_can_double(&g) && bj_double(&g) == BJ_ERR_FUNDS &&
                                                     g.bankroll == 10 && g.stake == 20);
    check("no actions outside a round", bj_double(NULL) == BJ_ERR_PHASE && bj_hit(NULL) == BJ_ERR_PHASE);
}

/* ---- chips ---------------------------------------------------------------------------------- */

static void test_chips(void)
{
    struct bj_game g;
    int i;

    fresh(&g);
    check("a session starts with 1000 chips and the minimum bet", g.bankroll == 1000 && g.bet == 10 &&
                                                                     g.phase == BJ_BETTING);
    for (i = 0; i < 30; i++) {
        bj_bet_up(&g);
    }
    check("the bet stops at 200", g.bet == 200 && bj_bet_up(&g) == BJ_ERR_LIMIT);
    for (i = 0; i < 30; i++) {
        bj_bet_down(&g);
    }
    check("and at 10 going down", g.bet == 10 && bj_bet_down(&g) == BJ_ERR_LIMIT);
    g.bankroll = 155;
    for (i = 0; i < 30; i++) {
        bj_bet_up(&g);
    }
    check("never above the bankroll, in whole steps", g.bet == 150 && bj_bet_ceiling(&g) == 150);

    fresh(&g);
    stack(&g, 10, 10, 8, 9, 0);
    bj_deal(&g);
    check("the bet cannot change mid-round", bj_bet_up(&g) == BJ_ERR_PHASE && bj_bet_down(&g) == BJ_ERR_PHASE);
    check("nor can a new round be dealt", bj_deal(&g) == BJ_ERR_PHASE && !bj_can_deal(&g));
    bj_stand(&g);
    check("after it settles the bet can change again", bj_bet_up(&g) == BJ_OK);

    /* A lost bet that leaves less than the bet clamps the next one. */
    fresh(&g);
    g.bankroll = 60;
    g.bet = 50;
    stack(&g, 10, 10, 6, 9, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("losing 50 of 60 leaves 10 and clamps the bet to it", g.bankroll == 10 && g.bet == 10);
    stack(&g, 10, 10, 6, 9, 0);
    bj_deal(&g);
    bj_stand(&g);
    check("losing the last 10 leaves nothing", g.bankroll == 0);
    check("which cannot deal", !bj_can_deal(&g) && bj_deal(&g) == BJ_ERR_BROKE && g.phase == BJ_SETTLED);
    check("a new bankroll starts again", bj_new_bankroll(&g) == BJ_OK && g.bankroll == 1000 && g.bet == 10 &&
                                            bj_can_deal(&g) && g.player.n == 0);
    stack(&g, 10, 10, 8, 9, 0);
    bj_deal(&g);
    check("but not in the middle of a round", bj_new_bankroll(&g) == BJ_ERR_PHASE);
}

/* ---- the shoe ---------------------------------------------------------------------------------- */

static void count_ids(const struct bj_game *g, int *counts, int include_shoe)
{
    int i;

    memset(counts, 0, sizeof(int) * BJ_DECK);
    for (i = 0; include_shoe && i < g->shoe.n; i++) {
        counts[g->shoe.card[i]]++;
    }
    for (i = 0; i < g->player.n; i++) {
        counts[g->player.card[i]]++;
    }
    for (i = 0; i < g->dealer.n; i++) {
        counts[g->dealer.card[i]]++;
    }
}

static void test_shoe(void)
{
    struct bj_game g;
    struct bj_game h;
    int counts[BJ_DECK];
    int ok = 1;
    int i;

    fresh(&g);
    count_ids(&g, counts, 1);
    for (i = 0; i < BJ_DECK; i++) {
        ok &= counts[i] == BJ_DECKS;
    }
    check("a new shoe holds every card exactly twice", ok && g.shoe.n == BJ_SHOE && g.shoe.shuffles == 1);

    fresh(&h);
    check("the same seed shuffles the same shoe", memcmp(&g.shoe, &h.shoe, sizeof(g.shoe)) == 0);
    bj_new_session(&h, 99);
    check("another seed another", memcmp(g.shoe.card, h.shoe.card, sizeof(g.shoe.card)) != 0);

    fresh(&g);
    g.shoe.n = BJ_RESHUFFLE_AT - 1;
    bj_deal(&g);
    check("a deal from a shoe under the mark reshuffles first", g.reshuffled && g.shoe.shuffles == 2 &&
                                                                  g.shoe.n == BJ_SHOE - 4);
    g.phase = BJ_SETTLED;
    g.shoe.n = BJ_RESHUFFLE_AT;
    bj_deal(&g);
    check("a shoe at the mark does not", !g.reshuffled && g.shoe.shuffles == 2);

    /* Run the shoe dry in the middle of a round. */
    fresh(&g);
    stack(&g, 2, 10, 2, 5, 0);
    bj_deal(&g);
    g.shoe.n = 0;
    check("a hit from an empty shoe still deals a card", bj_hit(&g) == BJ_OK && g.player.n == 3);
    count_ids(&g, counts, 1);
    ok = 1;
    for (i = 0; i < BJ_DECK; i++) {
        ok &= counts[i] == BJ_DECKS;
    }
    check("refilled from everything not on the table: each card still exactly twice", ok);
    check("and counted as a shuffle", g.shoe.shuffles == 2);

    /* Round after round through a whole shoe: no card is dealt more often
     * than the shoe holds it between two shuffles. */
    fresh(&g);
    {
        int seen[BJ_DECK];
        uint32_t shuffles = g.shoe.shuffles;
        int too_many = 0;
        int rounds = 0;

        memset(seen, 0, sizeof(seen));
        while (rounds < 400) {
            int before_p;
            int before_d;

            bj_deal(&g);
            if (g.shoe.shuffles != shuffles) {
                memset(seen, 0, sizeof(seen));
                shuffles = g.shoe.shuffles;
            }
            while (g.phase == BJ_PLAYER && bj_hand_total(&g.player, NULL) < 15) {
                bj_hit(&g);
            }
            if (g.phase == BJ_PLAYER) {
                bj_stand(&g);
            }
            if (g.shoe.shuffles != shuffles) {
                /* refilled mid-round: cannot happen above the mark */
                too_many++;
            }
            before_p = g.player.n;
            before_d = g.dealer.n;
            for (i = 0; i < before_p; i++) {
                too_many += ++seen[g.player.card[i]] > BJ_DECKS;
            }
            for (i = 0; i < before_d; i++) {
                too_many += ++seen[g.dealer.card[i]] > BJ_DECKS;
            }
            if (g.bankroll < BJ_BET_MIN) {
                bj_new_bankroll(&g);
            }
            rounds++;
        }
        check("400 rounds never deal a card more times than the shoe holds it", too_many == 0);
        check("and go through several shoes", g.shoe.shuffles >= 4);
    }
}

/* ---- rounds, determinism, conservation --------------------------------------------------------- */

static void test_rounds(void)
{
    struct bj_game g;
    struct bj_game h;
    struct bj_rng pick;
    int i;
    int bad = 0;
    long outcomes[8];

    fresh(&g);
    stack(&g, 6, 10, 5, 7, 10, 0);
    bj_deal(&g);
    bj_double(&g);
    stack(&g, 10, 10, 8, 9, 0);
    check("a new round deals from clean hands", bj_deal(&g) == BJ_OK && g.player.n == 2 && g.dealer.n == 2);
    check("without the last round's double, outcome or delta", !g.doubled && g.outcome == BJ_NO_OUTCOME &&
                                                                  g.last_delta == 0 && g.stake == g.bet);

    bj_new_session(&g, 7);
    bj_new_session(&h, 7);
    for (i = 0; i < 200; i++) {
        bj_deal(&g);
        bj_deal(&h);
        if (i % 3 == 0) {
            bj_double(&g);
            bj_double(&h);
        }
        bj_hit(&g);
        bj_hit(&h);
        bj_stand(&g);
        bj_stand(&h);
        if (g.bankroll < BJ_BET_MIN) {
            bj_new_bankroll(&g);
            bj_new_bankroll(&h);
        }
    }
    check("the same seed and the same choices are the same session", memcmp(&g, &h, sizeof(g)) == 0);

    memset(outcomes, 0, sizeof(outcomes));
    bj_rng_seed(&pick, 31);
    bj_new_session(&g, 555);
    for (i = 0; i < 20000; i++) {
        uint32_t chips = bj_chips(&g);

        if (bj_rng_below(&pick, 4) == 0) {
            bj_bet_up(&g);
        }
        if (bj_rng_below(&pick, 5) == 0) {
            bj_bet_down(&g);
        }
        if (bj_deal(&g) != BJ_OK) {
            bj_new_bankroll(&g);
            continue;
        }
        if (bj_chips(&g) != chips && g.phase == BJ_PLAYER) {
            bad++;
        }
        while (g.phase == BJ_PLAYER) {
            switch (bj_rng_below(&pick, 3)) {
            case 0:
                bj_hit(&g);
                break;
            case 1:
                bj_stand(&g);
                break;
            default:
                if (bj_double(&g) != BJ_OK) {
                    bj_stand(&g);
                }
                break;
            }
        }
        outcomes[g.outcome]++;
        if ((int64_t)bj_chips(&g) != (int64_t)chips + g.last_delta || g.phase != BJ_SETTLED ||
            g.outcome == BJ_NO_OUTCOME) {
            bad++;
        }
        if (g.outcome == BJ_PLAYER_BUSTS && !bj_is_bust(&g.player)) {
            bad++;
        }
        if ((g.outcome == BJ_PLAYER_WINS || g.outcome == BJ_DEALER_WINS || g.outcome == BJ_PUSH) &&
            !bj_is_blackjack(&g.player) && bj_hand_total(&g.dealer, NULL) < 17) {
            bad++; /* a dealer compared below 17 did not play out */
        }
        if (g.bet % BJ_BET_STEP || g.bet < BJ_BET_MIN || g.bet > BJ_BET_MAX) {
            bad++;
        }
        {
            int counts[BJ_DECK];
            int k;

            count_ids(&g, counts, 1);
            for (k = 0; k < BJ_DECK; k++) {
                bad += counts[k] > BJ_DECKS;
            }
        }
    }
    check("20 000 random rounds: chips conserved, every round settled, every outcome sound", bad == 0);
    check("and every outcome happens", outcomes[BJ_PLAYER_BLACKJACK] && outcomes[BJ_PLAYER_WINS] &&
                                           outcomes[BJ_DEALER_BUSTS] && outcomes[BJ_PUSH] &&
                                           outcomes[BJ_DEALER_WINS] && outcomes[BJ_PLAYER_BUSTS] &&
                                           outcomes[BJ_DEALER_BLACKJACK]);
    printf("note: 20000 rounds: blackjack %ld, win %ld, dealer bust %ld, push %ld, dealer win %ld, bust %ld, "
           "dealer blackjack %ld\n", outcomes[1], outcomes[2], outcomes[3], outcomes[4], outcomes[5], outcomes[6],
           outcomes[7]);
}

/* ---- reachable positions ------------------------------------------------------------------ */

/* As stack(), but fair: the shoe is every card not on the table, each id at
 * most BJ_DECKS times, with the wanted ranks on top. */
static void stack_fair(struct bj_game *g, ...)
{
    uint8_t left[BJ_DECK];
    bj_card_t picked[40];
    int k = 0;
    int n = 0;
    int c;
    int i;
    va_list ap;

    memset(left, BJ_DECKS, sizeof(left));
    for (i = 0; i < g->player.n; i++) {
        left[g->player.card[i]]--;
    }
    for (i = 0; i < g->dealer.n; i++) {
        left[g->dealer.card[i]]--;
    }
    va_start(ap, g);
    for (;;) {
        int r = va_arg(ap, int);
        int s;

        if (r == 0) {
            break;
        }
        for (s = 0; s < BJ_SUITS && !left[bj_card(r, (enum bj_suit)s)]; s++) {
        }
        picked[k] = bj_card(r, (enum bj_suit)(s % BJ_SUITS));
        left[picked[k++]]--;
    }
    va_end(ap);
    for (c = 0; c < BJ_DECK; c++) {
        for (i = 0; i < left[c]; i++) {
            g->shoe.card[n++] = (bj_card_t)c;
        }
    }
    for (i = k - 1; i >= 0; i--) {
        g->shoe.card[n++] = picked[i];
    }
    g->shoe.n = (uint8_t)n;
}

/* Move card c out of the shoe into h, so a changed hand copies no card. */
static int from_shoe(struct bj_game *g, struct bj_hand *h, bj_card_t c)
{
    int i;

    for (i = 0; i < g->shoe.n; i++) {
        if (g->shoe.card[i] == c) {
            g->shoe.card[i] = g->shoe.card[--g->shoe.n];
            h->card[h->n++] = c;
            return 1;
        }
    }
    return 0;
}

static void test_valid(void)
{
    struct bj_game g;
    struct bj_game v;
    struct bj_rng pick;
    int bad = 0;
    int i;

    fresh(&g);
    check("a new session is a valid position", bj_game_valid(&g));
    check("nothing is", !bj_game_valid(NULL));

    /* Every position random play reaches, after every single command. */
    bj_rng_seed(&pick, 77);
    bj_new_session(&g, 4242);
    for (i = 0; i < 60000; i++) {
        switch (bj_rng_below(&pick, 8)) {
        case 0:
            bj_bet_up(&g);
            break;
        case 1:
            bj_bet_down(&g);
            break;
        case 2:
            bj_deal(&g);
            break;
        case 3:
        case 4:
            bj_hit(&g);
            break;
        case 5:
            bj_stand(&g);
            break;
        case 6:
            bj_double(&g);
            break;
        default:
            if (!bj_can_deal(&g)) {
                bj_new_bankroll(&g);
            }
            break;
        }
        if (bj_rng_below(&pick, 500) == 0 && g.phase == BJ_PLAYER) {
            g.shoe.n = 0; /* and now and then a shoe runs dry mid-round */
        }
        bad += !bj_game_valid(&g);
    }
    check("60 000 random commands, shoes run dry included: every position is valid", bad == 0);
    check("and the session went through rounds and shoes", g.rounds > 3000 && g.shoe.shuffles > 50);

    /* The kinds of position, each valid, then broken one way at a time. */
    fresh(&g);
    stack_fair(&g, 10, 9, 6, 7, 0); /* player 16, dealer 16 */
    bj_deal(&g);
    check("a live hand is valid", g.phase == BJ_PLAYER && bj_game_valid(&g));
    v = g;
    check("a live hand of 21 is not: it stands by itself",
          from_shoe(&v, &v.player, bj_card(5, BJ_CLUBS)) && bj_game_valid(&g) && !bj_game_valid(&v));
    v = g;
    v.dealer.n = 0;
    check("nor one where the dealer holds a blackjack", from_shoe(&v, &v.dealer, bj_card(BJ_KING, BJ_CLUBS)) &&
                                                            from_shoe(&v, &v.dealer, bj_card(BJ_ACE, BJ_CLUBS)) &&
                                                            !bj_game_valid(&v));
    v = g;
    check("nor one where the dealer has drawn", from_shoe(&v, &v.dealer, bj_card(2, BJ_CLUBS)) && !bj_game_valid(&v));
    v = g;
    v.stake = 20;
    check("nor one whose stake is not the bet", !bj_game_valid(&v));
    v = g;
    v.doubled = 1;
    check("nor a doubled hand still live", !bj_game_valid(&v));
    v = g;
    v.player.n = 1;
    check("nor a hand of one card", !bj_game_valid(&v));
    v = g;
    v.shoe.card[v.shoe.n - 1] = v.player.card[0];
    v.shoe.card[v.shoe.n - 2] = v.player.card[0];
    check("a card more often than the shoe holds it is refused", !bj_game_valid(&v));
    v = g;
    v.shoe.card[0] = 52;
    check("so is a card that does not exist", !bj_game_valid(&v));
    v = g;
    v.shoe.n = BJ_SHOE + 1;
    check("and a shoe over-full", !bj_game_valid(&v));
    v = g;
    v.player.n = BJ_HAND_MAX + 1;
    check("and a hand past its slots", !bj_game_valid(&v));
    v = g;
    v.shoe.rng.state = 0;
    check("a stuck generator is refused", !bj_game_valid(&v));
    v = g;
    v.shoe.shuffles = 0;
    check("so is a shoe never shuffled", !bj_game_valid(&v));
    v = g;
    v.reshuffled = 2;
    check("and a flag that is not 0 or 1", !bj_game_valid(&v));
    v = g;
    v.phase = (enum bj_phase)3;
    check("and a phase that does not exist", !bj_game_valid(&v));

    bj_stand(&g); /* the dealer draws the shoe's top, a king: 26 */
    check("a settled round is valid", g.phase == BJ_SETTLED && g.outcome == BJ_DEALER_BUSTS && bj_game_valid(&g));
    v = g;
    v.last_delta = -v.last_delta;
    check("one whose result does not match the stake is not", !bj_game_valid(&v));
    v = g;
    v.outcome = BJ_DEALER_WINS;
    v.last_delta = -(int32_t)v.stake;
    check("nor one whose outcome the hands contradict, however it pays", !bj_game_valid(&v));
    v = g;
    v.outcome = BJ_NO_OUTCOME;
    check("nor a settled round with no outcome", !bj_game_valid(&v));
    v = g;
    v.doubled = 1;
    check("nor a double that took no card", !bj_game_valid(&v));
    v = g;
    v.bet = 15;
    check("a bet off the step is refused", !bj_game_valid(&v));
    v = g;
    v.bet = BJ_BET_MAX + BJ_BET_STEP;
    check("so is one over the limit", !bj_game_valid(&v));
    v = g;
    v.bankroll = 30;
    v.bet = 50;
    check("and one the chips do not cover between rounds", !bj_game_valid(&v));

    /* Doubled, busted, blackjacks, broke, a new bankroll. */
    fresh(&g);
    stack_fair(&g, 6, 10, 5, 7, 10, 0);
    bj_deal(&g);
    bj_double(&g);
    check("a doubled win is valid", g.doubled && g.outcome == BJ_PLAYER_WINS && g.stake == 20 && bj_game_valid(&g));
    v = g;
    v.stake = 30;
    check("a doubled stake must be twice a bet", !bj_game_valid(&v));
    stack_fair(&g, 10, 10, 6, 9, 9, 0);
    bj_deal(&g);
    bj_hit(&g);
    check("a bust is valid", g.outcome == BJ_PLAYER_BUSTS && bj_game_valid(&g));
    v = g;
    v.player.n = 2;
    check("a bust on 16 is not", !bj_game_valid(&v));
    stack_fair(&g, 1, 9, 13, 7, 0);
    bj_deal(&g);
    check("a player blackjack is valid", g.outcome == BJ_PLAYER_BLACKJACK && g.last_delta == 15 && bj_game_valid(&g));
    v = g;
    v.last_delta = 10;
    check("paid 1:1 it is not", !bj_game_valid(&v));
    stack_fair(&g, 10, 1, 9, 12, 0);
    bj_deal(&g);
    check("a dealer blackjack is valid", g.outcome == BJ_DEALER_BLACKJACK && bj_game_valid(&g));
    stack_fair(&g, 1, 1, 12, 13, 0);
    bj_deal(&g);
    check("two blackjacks push, valid", g.outcome == BJ_PUSH && g.last_delta == 0 && bj_game_valid(&g));
    g.bankroll = 40;
    g.bet = 20;
    stack_fair(&g, 6, 10, 5, 7, 2, 0); /* 11 doubles on a 2: 13 against 17 */
    bj_deal(&g);
    bj_double(&g);
    check("out of chips on a lost double is valid", g.bankroll == 0 && g.doubled && g.bet == BJ_BET_MIN &&
                                                        bj_game_valid(&g));
    bj_new_bankroll(&g);
    check("so is the new bankroll after it, with the round's double flag left over",
          g.phase == BJ_BETTING && g.doubled && bj_game_valid(&g));
    v = g;
    check("but not with a card on the table before a round", from_shoe(&v, &v.player, v.shoe.card[0]) &&
                                                                 !bj_game_valid(&v));
    v = g;
    v.stake = 10;
    check("or a stake", !bj_game_valid(&v));
    v = g;
    v.last_delta = 10;
    check("or a result", !bj_game_valid(&v));
}

int main(void)
{
    test_totals();
    test_naturals();
    test_dealer();
    test_player();
    test_chips();
    test_shoe();
    test_rounds();
    test_valid();
    printf("bj_rules_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
