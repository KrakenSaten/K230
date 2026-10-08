/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "poker.h"
#include <string.h>

bool poker_playing(const struct poker_game *g)
{
    return g && g->phase >= POKER_PREFLOP && g->phase <= POKER_RIVER;
}
uint32_t poker_pot_total(const struct poker_game *g)
{
    uint32_t total = 0;
    if (g)
        for (int i = 0; i < POKER_SEATS; ++i)
            total += g->player[i].committed;
    return total;
}
static int funded(const struct poker_game *g)
{
    int n = 0;
    for (int i = 0; i < POKER_SEATS; ++i)
        n += g->player[i].stack > 0;
    return n;
}
static int live(const struct poker_game *g)
{
    int n = 0;
    for (int i = 0; i < POKER_SEATS; ++i)
        n += g->player[i].in_hand && !g->player[i].folded;
    return n;
}
static int can_act(const struct poker_player *p)
{
    return p->in_hand && !p->folded && p->stack > 0;
}
static int next_funded(const struct poker_game *g, int after)
{
    for (int k = 1; k <= POKER_SEATS; ++k) {
        int seat = (after + k) % POKER_SEATS;
        if (g->player[seat].stack)
            return seat;
    }
    return POKER_NO_SEAT;
}
static bool needs_action(const struct poker_game *g, int seat)
{
    const struct poker_player *p = &g->player[seat];
    return can_act(p) && (!p->acted || p->bet < g->current_bet);
}
static void pay(struct poker_player *p, uint32_t amount)
{
    if (amount > p->stack)
        amount = p->stack;
    p->stack -= amount;
    p->bet += amount;
    p->committed += amount;
}
void poker_restart(struct poker_game *g, uint32_t seed)
{
    if (!g)
        return;
    memset(g, 0, sizeof(*g));
    bj_rng_seed(&g->rng, seed);
    for (int i = 0; i < POKER_SEATS; ++i)
        g->player[i].stack = POKER_START_STACK;
    g->button = POKER_SEATS - 1;
    g->actor = g->small_blind = g->big_blind = POKER_NO_SEAT;
    g->phase = POKER_READY;
}

static void settle(struct poker_game *g)
{
    uint32_t lower = 0;
    int remaining = live(g), sole = 0;
    g->settled_pot = poker_pot_total(g);
    g->showdown = remaining > 1;
    for (int i = 0; i < POKER_SEATS; ++i) {
        bj_card_t cards[7];
        if (!g->player[i].in_hand || g->player[i].folded)
            continue;
        sole = i;
        if (g->showdown) {
            memcpy(cards, g->board, 5);
            memcpy(cards + 5, g->player[i].hole, 2);
            g->rank[i] = poker_evaluate(cards, 7);
        }
    }
    /* Every distinct contribution level creates one main/side pot. Folded
     * players contribute but can never win. Odd chips go clockwise from the
     * button to the first tied winners, one chip at a time. */
    while (g->pot_count < POKER_SEATS) {
        uint32_t upper = UINT32_MAX, best = 0;
        int contributors = 0, donor = 0, winners = 0;
        struct poker_pot *pot = &g->pots[g->pot_count];
        for (int i = 0; i < POKER_SEATS; ++i) {
            uint32_t c = g->player[i].committed;
            if (c > lower && c < upper)
                upper = c;
        }
        if (upper == UINT32_MAX)
            break;
        for (int i = 0; i < POKER_SEATS; ++i) {
            const struct poker_player *p = &g->player[i];
            if (p->committed >= upper) {
                ++contributors;
                donor = i;
                if (p->in_hand && !p->folded)
                    pot->eligible |= 1u << i;
            }
        }
        pot->amount = (upper - lower) * (uint32_t)contributors;
        if (contributors == 1) {
            pot->refund = true;
            pot->winners = 1u << donor;
            g->player[donor].refunded += pot->amount;
            g->settled_pot -= pot->amount;
        } else if (remaining == 1) {
            pot->winners = 1u << sole;
        } else {
            for (int i = 0; i < POKER_SEATS; ++i)
                if ((pot->eligible & (1u << i)) && g->rank[i] > best)
                    best = g->rank[i];
            for (int i = 0; i < POKER_SEATS; ++i)
                if ((pot->eligible & (1u << i)) && g->rank[i] == best)
                    pot->winners |= 1u << i;
        }
        for (int i = 0; i < POKER_SEATS; ++i)
            winners += (pot->winners >> i) & 1u;
        if (winners) {
            uint32_t share = pot->amount / (uint32_t)winners, odd = pot->amount % (uint32_t)winners;
            for (int k = 1; k <= POKER_SEATS; ++k) {
                int i = (g->button + k) % POKER_SEATS;
                if (pot->winners & (1u << i))
                    g->player[i].payout += share + (odd ? (--odd, 1u) : 0u);
            }
            if (!pot->refund)
                g->winners |= pot->winners;
        }
        ++g->pot_count;
        lower = upper;
    }
    for (int i = 0; i < POKER_SEATS; ++i) {
        g->player[i].stack += g->player[i].payout;
        g->player[i].committed = g->player[i].bet = 0;
    }
    g->actor = POKER_NO_SEAT;
    g->current_bet = 0;
    g->phase = !g->player[0].stack || funded(g) < 2 ? POKER_GAME_OVER : POKER_FINISHED;
}
static void street(struct poker_game *g)
{
    ++g->next_card; /* standard burn before each community street */
    if (g->phase == POKER_PREFLOP) {
        for (int i = 0; i < 3; ++i)
            g->board[g->board_count++] = g->deck[g->next_card++];
        g->phase = POKER_FLOP;
    } else {
        g->board[g->board_count++] = g->deck[g->next_card++];
        g->phase = g->phase == POKER_FLOP ? POKER_TURN : POKER_RIVER;
    }
    g->current_bet = 0;
    g->min_raise = POKER_BIG_BLIND;
    for (int i = 0; i < POKER_SEATS; ++i) {
        g->player[i].bet = 0;
        g->player[i].acted = false;
        g->player[i].checked_zero = false;
        g->player[i].reopen_at = 0;
    }
}
static void advance(struct poker_game *g, int after)
{
    for (;;) {
        int actors = 0, single = POKER_NO_SEAT;
        if (live(g) == 1) {
            settle(g);
            return;
        }
        for (int i = 0; i < POKER_SEATS; ++i)
            if (can_act(&g->player[i])) {
                ++actors;
                single = i;
            }
        /* No betting into a dry side pot. A sole player with chips must
         * still answer a bet, but need not check through all-in opponents. */
        if (actors > 1 || (actors == 1 && g->player[single].bet < g->current_bet)) {
            for (int k = 1; k <= POKER_SEATS; ++k) {
                int i = (after + k) % POKER_SEATS;
                if (needs_action(g, i)) {
                    g->actor = i;
                    return;
                }
            }
        }
        if (g->phase == POKER_RIVER) {
            settle(g);
            return;
        }
        street(g);
        after = g->button;
    }
}
bool poker_new_hand(struct poker_game *g)
{
    if (!g || (g->phase != POKER_READY && g->phase != POKER_FINISHED))
        return false;
    if (funded(g) < 2 || !g->player[0].stack) {
        g->phase = POKER_GAME_OVER;
        return false;
    }
    if (funded(g) == 2 && g->big_blind != POKER_NO_SEAT) {
        /* On the transition to heads-up the big blind must advance, even
         * when advancing only the button would charge the same seat twice. */
        g->big_blind = next_funded(g, g->big_blind);
        g->button = g->small_blind = next_funded(g, g->big_blind);
    } else {
        g->button = next_funded(g, g->button);
        g->small_blind = funded(g) == 2 ? g->button : next_funded(g, g->button);
        g->big_blind = next_funded(g, g->small_blind);
    }
    g->board_count = g->pot_count = g->winners = g->next_card = 0;
    g->showdown = false;
    g->settled_pot = 0;
    memset(g->board, 0, sizeof(g->board));
    memset(g->pots, 0, sizeof(g->pots));
    memset(g->rank, 0, sizeof(g->rank));
    for (int i = 0; i < POKER_SEATS; ++i) {
        uint32_t stack = g->player[i].stack;
        memset(&g->player[i], 0, sizeof(g->player[i]));
        g->player[i].stack = stack;
        g->player[i].in_hand = stack > 0;
    }
    bj_deck_fill(g->deck);
    bj_deck_shuffle(g->deck, &g->rng);
    for (int round = 0; round < 2; ++round)
        for (int k = 1; k <= POKER_SEATS; ++k) {
            int i = (g->button + k) % POKER_SEATS;
            if (g->player[i].in_hand)
                g->player[i].hole[round] = g->deck[g->next_card++];
        }
    pay(&g->player[g->small_blind], POKER_SMALL_BLIND);
    pay(&g->player[g->big_blind], POKER_BIG_BLIND);
    g->current_bet = g->min_raise = POKER_BIG_BLIND;
    g->phase = POKER_PREFLOP;
    ++g->hand_number;
    advance(g, g->big_blind);
    return true;
}
struct poker_options poker_options(const struct poker_game *g)
{
    struct poker_options o = {0};
    const struct poker_player *p;
    bool opponent = false;
    if (!poker_playing(g) || g->actor < 0 || g->actor >= POKER_SEATS)
        return o;
    p = &g->player[g->actor];
    o.to_call = g->current_bet > p->bet ? g->current_bet - p->bet : 0;
    o.call_cost = o.to_call < p->stack ? o.to_call : p->stack;
    o.can_check = o.to_call == 0;
    o.min_raise_to = g->current_bet < POKER_BIG_BLIND ? POKER_BIG_BLIND : g->current_bet + g->min_raise;
    o.max_raise_to = p->bet + p->stack;
    for (int i = 0; i < POKER_SEATS; ++i)
        if (i != g->actor && can_act(&g->player[i]))
            opponent = true;
    o.can_raise = opponent && o.max_raise_to > g->current_bet &&
                  (!p->acted || p->checked_zero || g->current_bet >= p->reopen_at);
    o.can_all_in = p->stack && (o.max_raise_to <= g->current_bet || o.can_raise);
    return o;
}
bool poker_act(struct poker_game *g, enum poker_action action, uint32_t raise_to)
{
    struct poker_options o;
    struct poker_player *p;
    int actor;
    uint32_t target;
    if (!poker_playing(g) || g->actor < 0 || g->actor >= POKER_SEATS)
        return false;
    actor = g->actor;
    p = &g->player[actor];
    o = poker_options(g);
    if (!can_act(p))
        return false;
    switch (action) {
    case POKER_FOLD:
        p->folded = true;
        break;
    case POKER_CHECK:
        if (!o.can_check)
            return false;
        break;
    case POKER_CALL:
        if (!o.to_call)
            return false;
        pay(p, o.call_cost);
        break;
    case POKER_RAISE:
    case POKER_ALL_IN:
        target = action == POKER_ALL_IN ? o.max_raise_to : raise_to;
        if (action == POKER_ALL_IN && !o.can_all_in)
            return false;
        if (target <= g->current_bet) {
            if (action != POKER_ALL_IN || target <= p->bet)
                return false;
            pay(p, p->stack);
        } else {
            uint32_t increment;
            if (!o.can_raise || target > o.max_raise_to ||
                (target < o.min_raise_to && target != o.max_raise_to))
                return false;
            increment = g->current_bet < POKER_BIG_BLIND ? target : target - g->current_bet;
            if (target >= o.min_raise_to)
                g->min_raise = increment;
            pay(p, target - p->bet);
            g->current_bet = target;
        }
        break;
    default:
        return false;
    }
    p->last_action = action;
    p->acted = true;
    p->checked_zero = action == POKER_CHECK && !g->current_bet;
    p->reopen_at = g->current_bet + g->min_raise;
    advance(g, actor);
    return true;
}
const char *poker_phase_name(enum poker_phase phase)
{
    static const char *const names[] = {"New table", "Preflop",       "Flop",     "Turn",
                                        "River",     "Hand complete", "Game over"};
    return (unsigned)phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "Unknown";
}
