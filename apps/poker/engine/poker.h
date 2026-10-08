/*
 * Offline four-seat no-limit Texas Hold'em. Integer chips, fixed-size state,
 * no LVGL, I/O or platform clock. Reuses Doors' existing card/RNG primitives.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_POKER_H
#define DOORS_POKER_H

#include "bj_cards.h"
#include <stdbool.h>
#include <stdint.h>

#define POKER_SEATS 4
#define POKER_START_STACK 1000u
#define POKER_SMALL_BLIND 10u
#define POKER_BIG_BLIND 20u
#define POKER_NO_SEAT (-1)

enum poker_phase {
    POKER_READY,
    POKER_PREFLOP,
    POKER_FLOP,
    POKER_TURN,
    POKER_RIVER,
    POKER_FINISHED,
    POKER_GAME_OVER
};
enum poker_action { POKER_FOLD, POKER_CHECK, POKER_CALL, POKER_RAISE, POKER_ALL_IN };
enum poker_rank {
    POKER_HIGH_CARD,
    POKER_PAIR,
    POKER_TWO_PAIR,
    POKER_TRIPS,
    POKER_STRAIGHT,
    POKER_FLUSH,
    POKER_FULL_HOUSE,
    POKER_QUADS,
    POKER_STRAIGHT_FLUSH
};

struct poker_player {
    uint32_t stack, bet, committed, payout;
    uint32_t refunded;  /* included in payout, excluded from contested winnings */
    uint32_t reopen_at; /* cumulative short raises can reopen a player's bet */
    bj_card_t hole[2];
    bool in_hand, folded, acted, checked_zero;
    enum poker_action last_action;
};
struct poker_pot {
    uint32_t amount;
    uint8_t eligible, winners;
    bool refund; /* unmatched chips are returned, not a contested pot */
};
struct poker_game {
    struct poker_player player[POKER_SEATS];
    struct bj_rng rng;
    bj_card_t deck[BJ_DECK], board[5];
    uint8_t next_card, board_count, pot_count, winners;
    struct poker_pot pots[POKER_SEATS];
    uint32_t current_bet, min_raise, hand_number, settled_pot;
    uint32_t rank[POKER_SEATS];
    int button, small_blind, big_blind, actor;
    enum poker_phase phase;
    bool showdown;
};
struct poker_options {
    uint32_t to_call, call_cost, min_raise_to, max_raise_to;
    bool can_check, can_raise, can_all_in;
};
struct poker_decision {
    enum poker_action action;
    uint32_t raise_to;
};

void poker_restart(struct poker_game *g, uint32_t seed);
bool poker_new_hand(struct poker_game *g);
bool poker_playing(const struct poker_game *g);
uint32_t poker_pot_total(const struct poker_game *g);
struct poker_options poker_options(const struct poker_game *g);
/* raise_to is the total street bet, including chips already posted. Invalid
 * actions leave every byte of state, including the RNG, unchanged. */
bool poker_act(struct poker_game *g, enum poker_action action, uint32_t raise_to);
/* Best five out of 5..7 distinct cards; category then five descending rank
 * nibbles, so unsigned comparison breaks ties. Invalid input returns zero. */
uint32_t poker_evaluate(const bj_card_t *cards, unsigned count);
enum poker_rank poker_rank_category(uint32_t rank);
const char *poker_rank_name(uint32_t rank);
const char *poker_phase_name(enum poker_phase phase);
/* Bounded strength/pot-odds policy. Reads only this seat's hole cards and the
 * exposed board. Never reads other hole cards or the shuffled deck. */
struct poker_decision poker_ai(struct poker_game *g);

#endif
