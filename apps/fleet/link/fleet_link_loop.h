/*
 * A virtual opponent in the same process (fleet_link.h): a second match state
 * machine, with PocketFleet's AI playing it, on the far side of a channel that
 * can lose, duplicate and delay. Everything is the real protocol; only the
 * radio is missing.
 *
 * It is the fake transport the UI is built and tested against (P3), and a way
 * to play a whole multiplayer match in the simulator with one shell. It is
 * never used unless POCKETFLEET_MP_FAKE asks for it (fleet_app.c), and it
 * puts nothing on any air.
 *
 * Pure C, no LVGL and no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_LINK_LOOP_H
#define POCKETFLEET_LINK_LOOP_H

#include "fleet_link.h"

struct fleet_link_loop_cfg {
    int loss_pct;           /* each packet, each way */
    int dup_pct;
    int delay_ms;           /* plus up to as much again at random */
    int invite_after_ms;    /* the opponent invites us after this long idle; 0: never */
    int think_ms;           /* the opponent's time per move, and up to twice it */
    int difficulty;         /* enum fleet_difficulty */
    int decline;            /* the opponent declines every invite */
    int silent;             /* the opponent never answers anything */
    int cut_after_ms;       /* the channel goes dead this long after opening; 0: never */
    int chat;               /* the opponent answers every line of chat, after think_ms */
    int crowd;              /* bystanders listed beside the opponent, never answering */
    uint32_t seed;
};

/* The most bystanders a loop lists. */
#define FLEET_LINK_LOOP_CROWD_MAX 8

/* Parse "loss=20,dup=5,delay=800,invite=3000,think=2500,level=3,decline,
 * silent,cut=60000,chat,crowd=3,seed=7" (any subset, any order) over the
 * defaults. Unknown words are ignored. Returns 0. */
int fleet_link_loop_parse(const char *spec, struct fleet_link_loop_cfg *cfg);
void fleet_link_loop_defaults(struct fleet_link_loop_cfg *cfg);

/* A link backed by a virtual opponent. NULL when out of memory. */
struct fleet_link *fleet_link_loop_open(const struct fleet_link_loop_cfg *cfg);

/* Development aid and tests: the channel goes dead (1) or comes back (0). */
void fleet_link_loop_set_cut(struct fleet_link *link, int cut);
/* For tests: the opponent's own match. */
const struct fleet_match *fleet_link_loop_peer(const struct fleet_link *link);
/* Development aid and tests: the opponent says something. Returns what
 * fleet_match_chat_send() does. */
int fleet_link_loop_say(struct fleet_link *link, const char *text);
/* Tests: a node is heard - 0 the opponent, 1..crowd a bystander - and moves
 * to the top of the list, as meshcored lists the most recently heard first. */
void fleet_link_loop_hear(struct fleet_link *link, int who);

#endif
