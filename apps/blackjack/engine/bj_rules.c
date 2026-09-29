/*
 * PG Blackjack rules. See bj_rules.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "bj_rules.h"

#include <string.h>

/* ---- hands --------------------------------------------------------------- */

int bj_card_points(bj_card_t c)
{
    int rank = bj_card_rank(c);

    return rank >= 10 ? 10 : rank;
}

int bj_hand_total(const struct bj_hand *h, int *soft)
{
    int total = 0;
    int aces = 0;
    int i;

    if (soft) {
        *soft = 0;
    }
    if (!h) {
        return 0;
    }
    for (i = 0; i < h->n; i++) {
        total += bj_card_points(h->card[i]);
        aces += bj_card_rank(h->card[i]) == BJ_ACE;
    }
    /* At most one ace can ever count as 11: two would be 22. */
    if (aces > 0 && total + 10 <= 21) {
        total += 10;
        if (soft) {
            *soft = 1;
        }
    }
    return total;
}

int bj_is_blackjack(const struct bj_hand *h)
{
    return h && h->n == 2 && bj_hand_total(h, NULL) == 21;
}

int bj_is_bust(const struct bj_hand *h)
{
    return bj_hand_total(h, NULL) > 21;
}

/* ---- the shoe ---------------------------------------------------------------- */

static void shoe_fill_shuffled(struct bj_shoe *shoe, const uint8_t *exclude_count)
{
    int copies[BJ_DECK];
    int n = 0;
    int c;
    int i;

    for (c = 0; c < BJ_DECK; c++) {
        copies[c] = BJ_DECKS - (exclude_count ? exclude_count[c] : 0);
    }
    for (c = 0; c < BJ_DECK; c++) {
        for (i = 0; i < copies[c]; i++) {
            shoe->card[n++] = (bj_card_t)c;
        }
    }
    /* Fisher-Yates over the whole shoe, one draw per position. */
    for (i = n - 1; i > 0; i--) {
        int j = (int)bj_rng_below(&shoe->rng, (uint32_t)i + 1u);
        bj_card_t t = shoe->card[i];

        shoe->card[i] = shoe->card[j];
        shoe->card[j] = t;
    }
    shoe->n = (uint8_t)n;
    shoe->shuffles++;
}

void bj_shoe_init(struct bj_shoe *shoe, uint32_t seed)
{
    if (!shoe) {
        return;
    }
    memset(shoe, 0, sizeof(*shoe));
    bj_rng_seed(&shoe->rng, seed);
    shoe_fill_shuffled(shoe, NULL);
}

void bj_shoe_shuffle(struct bj_shoe *shoe)
{
    if (shoe) {
        shoe_fill_shuffled(shoe, NULL);
    }
}

/* The next card. A shoe that has run out mid-round is refilled with every
 * card that is not on the table, so no card is ever in two places. */
static bj_card_t draw(struct bj_game *g)
{
    if (g->shoe.n == 0) {
        uint8_t on_table[BJ_DECK];
        int i;

        memset(on_table, 0, sizeof(on_table));
        for (i = 0; i < g->player.n; i++) {
            on_table[g->player.card[i]]++;
        }
        for (i = 0; i < g->dealer.n; i++) {
            on_table[g->dealer.card[i]]++;
        }
        shoe_fill_shuffled(&g->shoe, on_table);
    }
    return g->shoe.card[--g->shoe.n];
}

static void give(struct bj_game *g, struct bj_hand *h)
{
    if (h->n < BJ_HAND_MAX) {
        /* Drawn first, in its own statement: draw() counts the hands to
         * refill an empty shoe, and `h->card[h->n++] = draw(g)` leaves it
         * unspecified whether n has already grown by then - it had, and a
         * stale slot was counted as a card on the table (tests/
         * bj_rules_test.c, the empty shoe). */
        bj_card_t c = draw(g);

        h->card[h->n++] = c;
    }
}

/* ---- chips ---------------------------------------------------------------------- */

uint32_t bj_chips(const struct bj_game *g)
{
    return g ? g->bankroll + (g->phase == BJ_PLAYER ? g->stake : 0) : 0;
}

uint32_t bj_bet_ceiling(const struct bj_game *g)
{
    uint32_t cap;

    if (!g) {
        return 0;
    }
    cap = g->bankroll - g->bankroll % BJ_BET_STEP;
    return cap < BJ_BET_MAX ? cap : BJ_BET_MAX;
}

static int between_rounds(const struct bj_game *g)
{
    return g->phase == BJ_BETTING || g->phase == BJ_SETTLED;
}

int bj_can_deal(const struct bj_game *g)
{
    return g && between_rounds(g) && g->bankroll >= BJ_BET_MIN;
}

void bj_new_session(struct bj_game *g, uint32_t seed)
{
    if (!g) {
        return;
    }
    memset(g, 0, sizeof(*g));
    bj_shoe_init(&g->shoe, seed);
    g->bankroll = BJ_BANKROLL_START;
    g->bet = BJ_BET_MIN;
    g->phase = BJ_BETTING;
}

enum bj_result bj_new_bankroll(struct bj_game *g)
{
    if (!g || !between_rounds(g)) {
        return BJ_ERR_PHASE;
    }
    g->bankroll = BJ_BANKROLL_START;
    g->bet = BJ_BET_MIN;
    g->stake = 0;
    g->phase = BJ_BETTING;
    g->outcome = BJ_NO_OUTCOME;
    g->last_delta = 0;
    g->player.n = 0;
    g->dealer.n = 0;
    return BJ_OK;
}

enum bj_result bj_bet_up(struct bj_game *g)
{
    if (!g || !between_rounds(g)) {
        return BJ_ERR_PHASE;
    }
    if (g->bet + BJ_BET_STEP > bj_bet_ceiling(g)) {
        return BJ_ERR_LIMIT;
    }
    g->bet += BJ_BET_STEP;
    return BJ_OK;
}

enum bj_result bj_bet_down(struct bj_game *g)
{
    if (!g || !between_rounds(g)) {
        return BJ_ERR_PHASE;
    }
    if (g->bet <= BJ_BET_MIN) {
        return BJ_ERR_LIMIT;
    }
    g->bet -= BJ_BET_STEP;
    return BJ_OK;
}

/* ---- a round ------------------------------------------------------------------------ */

static void settle(struct bj_game *g, enum bj_outcome outcome)
{
    uint32_t back = 0;

    switch (outcome) {
    case BJ_PLAYER_BLACKJACK:
        back = g->stake + g->stake / 2 * 3;
        break;
    case BJ_PLAYER_WINS:
    case BJ_DEALER_BUSTS:
        back = 2 * g->stake;
        break;
    case BJ_PUSH:
        back = g->stake;
        break;
    default:
        back = 0;
        break;
    }
    g->bankroll += back;
    g->last_delta = (int32_t)back - (int32_t)g->stake;
    g->outcome = outcome;
    g->phase = BJ_SETTLED;
    g->rounds++;
    /* The next bet stays what the player chose, unless the bankroll can no
     * longer cover it. */
    if (g->bet > bj_bet_ceiling(g)) {
        g->bet = bj_bet_ceiling(g) >= BJ_BET_MIN ? bj_bet_ceiling(g) : BJ_BET_MIN;
    }
}

static void dealer_plays(struct bj_game *g)
{
    int dealer;
    int player = bj_hand_total(&g->player, NULL);

    /* Stand on every 17, soft 17 included. */
    while (bj_hand_total(&g->dealer, NULL) < 17) {
        give(g, &g->dealer);
    }
    dealer = bj_hand_total(&g->dealer, NULL);
    if (dealer > 21) {
        settle(g, BJ_DEALER_BUSTS);
    } else if (player > dealer) {
        settle(g, BJ_PLAYER_WINS);
    } else if (player == dealer) {
        settle(g, BJ_PUSH);
    } else {
        settle(g, BJ_DEALER_WINS);
    }
}

enum bj_result bj_deal(struct bj_game *g)
{
    int player_bj;
    int dealer_bj;

    if (!g || !between_rounds(g)) {
        return BJ_ERR_PHASE;
    }
    if (g->bankroll < BJ_BET_MIN) {
        return BJ_ERR_BROKE;
    }
    if (g->bet > bj_bet_ceiling(g)) {
        g->bet = bj_bet_ceiling(g);
    }
    if (g->bet < BJ_BET_MIN) {
        g->bet = BJ_BET_MIN;
    }
    g->reshuffled = 0;
    if (g->shoe.n < BJ_RESHUFFLE_AT) {
        bj_shoe_shuffle(&g->shoe);
        g->reshuffled = 1;
    }
    g->player.n = 0;
    g->dealer.n = 0;
    g->doubled = 0;
    g->outcome = BJ_NO_OUTCOME;
    g->last_delta = 0;
    g->bankroll -= g->bet;
    g->stake = g->bet;
    g->phase = BJ_PLAYER;
    give(g, &g->player);
    give(g, &g->dealer);
    give(g, &g->player);
    give(g, &g->dealer);

    player_bj = bj_is_blackjack(&g->player);
    dealer_bj = bj_is_blackjack(&g->dealer);
    if (player_bj && dealer_bj) {
        settle(g, BJ_PUSH);
    } else if (player_bj) {
        settle(g, BJ_PLAYER_BLACKJACK);
    } else if (dealer_bj) {
        settle(g, BJ_DEALER_BLACKJACK);
    }
    return BJ_OK;
}

enum bj_result bj_hit(struct bj_game *g)
{
    int total;

    if (!g || g->phase != BJ_PLAYER) {
        return BJ_ERR_PHASE;
    }
    give(g, &g->player);
    total = bj_hand_total(&g->player, NULL);
    if (total > 21) {
        settle(g, BJ_PLAYER_BUSTS);
    } else if (total == 21) {
        dealer_plays(g);
    }
    return BJ_OK;
}

enum bj_result bj_stand(struct bj_game *g)
{
    if (!g || g->phase != BJ_PLAYER) {
        return BJ_ERR_PHASE;
    }
    dealer_plays(g);
    return BJ_OK;
}

int bj_can_double(const struct bj_game *g)
{
    return g && g->phase == BJ_PLAYER && g->player.n == 2 && g->bankroll >= g->stake;
}

enum bj_result bj_double(struct bj_game *g)
{
    if (!g || g->phase != BJ_PLAYER) {
        return BJ_ERR_PHASE;
    }
    if (g->player.n != 2) {
        return BJ_ERR_NOT_FIRST;
    }
    if (g->bankroll < g->stake) {
        return BJ_ERR_FUNDS;
    }
    g->bankroll -= g->stake;
    g->stake *= 2;
    g->doubled = 1;
    give(g, &g->player);
    if (bj_is_bust(&g->player)) {
        settle(g, BJ_PLAYER_BUSTS);
    } else {
        dealer_plays(g);
    }
    return BJ_OK;
}

/* ---- a reachable position -------------------------------------------------------------- */

static int cards_ok(const bj_card_t *card, int n, uint8_t copies[BJ_DECK])
{
    int i;

    for (i = 0; i < n; i++) {
        if (!bj_card_valid(card[i]) || ++copies[card[i]] > BJ_DECKS) {
            return 0;
        }
    }
    return 1;
}

static int bet_ok(uint32_t bet)
{
    return bet >= BJ_BET_MIN && bet <= BJ_BET_MAX && bet % BJ_BET_STEP == 0;
}

/* The outcome settle() was given, told again from the hands. */
static int outcome_matches_hands(const struct bj_game *g)
{
    int p = bj_hand_total(&g->player, NULL);
    int d = bj_hand_total(&g->dealer, NULL);
    int p_bj = bj_is_blackjack(&g->player);
    int d_bj = bj_is_blackjack(&g->dealer);
    int dealer_played = !p_bj && !d_bj && p <= 21 && d >= 17;

    switch (g->outcome) {
    case BJ_PLAYER_BLACKJACK:
        return p_bj && !d_bj;
    case BJ_DEALER_BLACKJACK:
        return d_bj && !p_bj;
    case BJ_PUSH:
        return p == d && ((p_bj && d_bj) || (dealer_played && d <= 21));
    case BJ_PLAYER_BUSTS:
        return p > 21 && g->dealer.n == 2 && !d_bj;
    case BJ_DEALER_BUSTS:
        return dealer_played && d > 21;
    case BJ_PLAYER_WINS:
        return dealer_played && d <= 21 && p > d;
    case BJ_DEALER_WINS:
        return dealer_played && d <= 21 && d > p;
    default:
        return 0;
    }
}

static int64_t delta_for(enum bj_outcome outcome, uint32_t stake)
{
    switch (outcome) {
    case BJ_PLAYER_BLACKJACK:
        return (int64_t)(stake / 2 * 3);
    case BJ_PLAYER_WINS:
    case BJ_DEALER_BUSTS:
        return (int64_t)stake;
    case BJ_PUSH:
        return 0;
    default:
        return -(int64_t)stake;
    }
}

int bj_game_valid(const struct bj_game *g)
{
    uint8_t copies[BJ_DECK];

    if (!g || g->shoe.n > BJ_SHOE || g->player.n > BJ_HAND_MAX || g->dealer.n > BJ_HAND_MAX) {
        return 0;
    }
    memset(copies, 0, sizeof(copies));
    if (!cards_ok(g->shoe.card, g->shoe.n, copies) || !cards_ok(g->player.card, g->player.n, copies) ||
        !cards_ok(g->dealer.card, g->dealer.n, copies)) {
        return 0;
    }
    /* xorshift32 never leaves zero, and every shoe has been shuffled once. */
    if (g->shoe.rng.state == 0 || g->shoe.shuffles == 0 || g->doubled > 1 || g->reshuffled > 1 || !bet_ok(g->bet)) {
        return 0;
    }
    switch (g->phase) {
    case BJ_BETTING:
        return g->player.n == 0 && g->dealer.n == 0 && g->stake == 0 && g->outcome == BJ_NO_OUTCOME &&
               g->last_delta == 0 && g->bet <= (bj_bet_ceiling(g) > BJ_BET_MIN ? bj_bet_ceiling(g) : BJ_BET_MIN);
    case BJ_PLAYER:
        /* 21 stands by itself and a blackjack settles on the deal. */
        return g->player.n >= 2 && bj_hand_total(&g->player, NULL) <= 20 && g->dealer.n == 2 &&
               !bj_is_blackjack(&g->dealer) && g->stake == g->bet && !g->doubled && g->outcome == BJ_NO_OUTCOME &&
               g->last_delta == 0;
    case BJ_SETTLED:
        if (g->player.n < 2 || g->dealer.n < 2 || !outcome_matches_hands(g)) {
            return 0;
        }
        if (g->doubled ? (g->player.n != 3 || g->stake % 2 || !bet_ok(g->stake / 2)) : !bet_ok(g->stake)) {
            return 0;
        }
        return (int64_t)g->last_delta == delta_for(g->outcome, g->stake) &&
               g->bet <= (bj_bet_ceiling(g) > BJ_BET_MIN ? bj_bet_ceiling(g) : BJ_BET_MIN);
    default:
        return 0;
    }
}
