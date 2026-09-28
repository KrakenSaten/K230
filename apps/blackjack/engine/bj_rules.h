/*
 * PG Blackjack rules: one player against the dealer, with a shoe, a bet and
 * a small bankroll.
 *
 * THE RULESET (docs/apps/PGBLACKJACK.md):
 *
 *  - A shoe of BJ_DECKS standard decks, shuffled from a seed. Before a round
 *    is dealt, a shoe with fewer than BJ_RESHUFFLE_AT cards left is replaced
 *    by a freshly shuffled full one. Should it run out mid-round anyway, the
 *    cards not on the table are shuffled back in.
 *  - Values: 2-10 their rank, J Q K 10, an ace 1 or 11. A hand's total counts
 *    one ace as 11 when that does not go over 21 (a "soft" total).
 *  - A blackjack is an ace and a ten-value card as the first two cards.
 *  - The bet is taken when the round is dealt: player, dealer, player,
 *    dealer. The dealer's second card is face down.
 *  - The dealer checks for blackjack at once. If either side has one, the
 *    round is settled there: both is a push, the player's alone pays 3:2,
 *    the dealer's alone takes the bet. No insurance is offered.
 *  - Otherwise the player hits (one card) or stands. A total over 21 busts
 *    and loses at once. A total of exactly 21 stands by itself.
 *  - Double down: on the first two cards only, when the bankroll covers a
 *    second bet of the same size; exactly one more card, then stand.
 *  - No splitting, surrender, insurance or side bets.
 *  - The dealer then turns the hole card and draws while under 17, standing
 *    on every 17 including soft 17.
 *  - Higher total wins; equal totals push; a dealer bust pays every standing
 *    hand. A win pays 1:1, a blackjack 3:2, a push returns the stake.
 *
 * CHIPS. The bankroll starts at BJ_BANKROLL_START. The bet is BJ_BET_MIN to
 * BJ_BET_MAX in steps of BJ_BET_STEP and never above the bankroll. Bets are
 * multiples of 10, so 3:2 is always a whole number of chips. A bankroll below
 * the minimum bet cannot deal; bj_new_bankroll() starts again.
 *
 * Pure C: no LVGL, no I/O, no floating point, no clock (tests/bj_lint.sh).
 * Nothing here knows where a card is drawn.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_RULES_H
#define PGBJ_RULES_H

#include "bj_cards.h"

#include <stdint.h>

#define BJ_DECKS 2
#define BJ_SHOE (BJ_DECKS * BJ_DECK)
/* A round never needs more than this; reshuffling below it at the deal means
 * a round in progress does not normally run the shoe dry. */
#define BJ_RESHUFFLE_AT 26
/* The most cards a hand can hold: eight aces and six twos make 20, and one
 * more card is the last one it can take. */
#define BJ_HAND_MAX 16

#define BJ_BANKROLL_START 1000u
#define BJ_BET_MIN 10u
#define BJ_BET_STEP 10u
#define BJ_BET_MAX 200u

struct bj_hand {
    uint8_t n;
    bj_card_t card[BJ_HAND_MAX];
};

struct bj_shoe {
    uint8_t n;                 /* cards left; card[n - 1] is dealt next */
    bj_card_t card[BJ_SHOE];
    struct bj_rng rng;
    uint32_t shuffles;
};

enum bj_phase {
    BJ_BETTING = 0, /* before the first round, or after one: bet and deal */
    BJ_PLAYER,      /* the player's hand is live */
    BJ_SETTLED      /* the round is over; the hands stay on the table */
};

enum bj_outcome {
    BJ_NO_OUTCOME = 0,
    BJ_PLAYER_BLACKJACK, /* 3:2 */
    BJ_PLAYER_WINS,      /* 1:1, the higher total */
    BJ_DEALER_BUSTS,     /* 1:1 */
    BJ_PUSH,             /* stake returned */
    BJ_DEALER_WINS,      /* the higher total */
    BJ_PLAYER_BUSTS,
    BJ_DEALER_BLACKJACK
};

enum bj_result {
    BJ_OK = 0,
    BJ_ERR_PHASE,     /* not now */
    BJ_ERR_BROKE,     /* the bankroll does not cover the minimum bet */
    BJ_ERR_FUNDS,     /* the bankroll does not cover a double */
    BJ_ERR_NOT_FIRST, /* doubling is for the first two cards only */
    BJ_ERR_LIMIT      /* the bet is already at its limit */
};

struct bj_game {
    struct bj_shoe shoe;
    struct bj_hand player;
    struct bj_hand dealer;
    enum bj_phase phase;
    enum bj_outcome outcome;
    uint32_t bankroll;  /* chips not on the table */
    uint32_t bet;       /* the next round's bet */
    uint32_t stake;     /* chips on the table this round (bet, or twice it doubled) */
    int32_t last_delta; /* what the last settled round won (+) or lost (-) */
    uint8_t doubled;
    uint8_t reshuffled; /* this round's deal started a new shoe */
    uint32_t rounds;
};

/* ---- hands --------------------------------------------------------------- */

/* The value a card adds before aces are decided: aces 1, courts 10. */
int bj_card_points(bj_card_t c);
/* The best total not over 21 if there is one, else the lowest; *soft is 1
 * when an ace is being counted as 11 (soft may be NULL). */
int bj_hand_total(const struct bj_hand *h, int *soft);
int bj_is_blackjack(const struct bj_hand *h);
int bj_is_bust(const struct bj_hand *h);

/* ---- the shoe ---------------------------------------------------------------- */

void bj_shoe_init(struct bj_shoe *shoe, uint32_t seed);
/* Replace the shoe with a full, freshly shuffled one. */
void bj_shoe_shuffle(struct bj_shoe *shoe);

/* ---- a session ------------------------------------------------------------------ */

/* A new session: a shuffled shoe, the starting bankroll, the minimum bet. */
void bj_new_session(struct bj_game *g, uint32_t seed);
/* Start again with the starting bankroll, keeping the shoe. Allowed between
 * rounds. */
enum bj_result bj_new_bankroll(struct bj_game *g);

enum bj_result bj_bet_up(struct bj_game *g);
enum bj_result bj_bet_down(struct bj_game *g);
/* The largest bet allowed now: BJ_BET_MAX, or the bankroll rounded down to a
 * step if that is less. */
uint32_t bj_bet_ceiling(const struct bj_game *g);
/* Can a round be dealt at all. */
int bj_can_deal(const struct bj_game *g);
int bj_can_double(const struct bj_game *g);

/* Deal a round at the current bet (clamped to the ceiling). */
enum bj_result bj_deal(struct bj_game *g);
enum bj_result bj_hit(struct bj_game *g);
enum bj_result bj_stand(struct bj_game *g);
enum bj_result bj_double(struct bj_game *g);

/* Every chip the session has: the bankroll plus what is on the table. */
uint32_t bj_chips(const struct bj_game *g);

/* Whether g is a position these rules can reach: every card a real one and
 * none in the shoe and on the table more often than BJ_DECKS; a bet in range
 * and in steps; nothing on the table between rounds before the first; a live
 * hand that has not stood by itself, with the dealer on two cards, no
 * blackjack and the stake equal to the bet; a settled round whose outcome
 * matches the hands and whose result matches the stake. Used to vet a saved
 * game (bj_store.h); a game played through these functions is always valid. */
int bj_game_valid(const struct bj_game *g);

#endif
