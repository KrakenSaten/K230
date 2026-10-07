/*
 * PocketFleet multiplayer view model: a match turned into the words the
 * screens show (docs/apps/FLEET_MULTIPLAYER.md, UX states). No LVGL, so the
 * wording is tested natively (tests/fleet_view_mp_test.c) and the screens
 * hold layout, not phrasing.
 *
 * Every function writes at most n bytes including the terminator and returns
 * 0, or -1 when the arguments are unusable (buf then holds "" when it can).
 * `peer` is the opponent's name as the player knows it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_VIEW_MP_H
#define POCKETFLEET_VIEW_MP_H

#include "../link/fleet_link.h"
#include "../net/fleet_match.h"

/* Where the match stands, as the player needs to hear it first. Read off the
 * match and the link and nothing else, so it can never disagree with them:
 * there is no status of its own to fall out of step. In order of precedence:
 * the match is over; the mesh service is gone; our key is not known yet (m
 * NULL) or the session is being set up; the opponent is out of reach; the
 * records are being compared; a packet is being retried; deploying; whose
 * turn it is. */
enum fleet_mp_status {
    FLEET_STATUS_IDLE = 0,          /* no match in hand: "MULTIPLAYER" */
    FLEET_STATUS_CONNECTING,        /* the service, or the opponent, being reached */
    FLEET_STATUS_OFFLINE,           /* the mesh service or its radio is not there */
    FLEET_STATUS_DEPLOYING,         /* our fleet is still to be placed */
    FLEET_STATUS_SYNCING,           /* comparing records with the opponent */
    FLEET_STATUS_RECONNECTING,      /* a packet of the game is being retried */
    FLEET_STATUS_DISCONNECTED,      /* out of reach: paused, not lost */
    FLEET_STATUS_YOUR_TURN,
    FLEET_STATUS_WAITING,           /* for the opponent: to deploy, to answer, to fire */
    FLEET_STATUS_GAME_OVER,
};

enum fleet_mp_status fleet_view_mp_state(const struct fleet_match *m, enum fleet_link_state link);

/* The status in words, for the header: "YOUR TURN · SHOT 14", "WAITING FOR
 * ANNA", "OPPONENT DISCONNECTED", "GAME OVER". m NULL: our key is not known
 * yet. */
int fleet_view_mp_status(const struct fleet_match *m, enum fleet_link_state link,
                         const char *peer, char *buf, size_t n);

/* ---- chat ----------------------------------------------------------------- */

/* One line as the history shows it: "ANNA · Nice shot", "YOU · Hello",
 * with how ours is doing after it - " · SENDING", " · NOT DELIVERED". */
int fleet_view_mp_chat_line(const struct fleet_chat_line *l, const char *peer, char *buf,
                            size_t n);
/* The chat button beside FIRE: its title ("CHAT", "CHAT · 2 NEW") and the
 * newest line, or what to do when there is none. */
int fleet_view_mp_chat_tile(const struct fleet_match *m, const char *peer, char *title,
                            size_t tn, char *preview, size_t pn);

/* What Battle says when the player is not aiming: waiting for the opponent to
 * deploy, a shot on its way, the opponent aiming, a link problem, a resync,
 * the airtime limit, the opponent unreachable. Empty when it is the player's
 * turn and the link is well: the screen then says what aiming says. now_ms
 * dates the silence. */
int fleet_view_mp_note(const struct fleet_match *m, const char *peer, int64_t now_ms,
                       char *buf, size_t n);

/* "YOU D7 HIT · ANNA B3 MISS": the last shot each way. */
int fleet_view_mp_exchange(const struct fleet_match *m, const char *peer, char *buf, size_t n);

/* The session line in the lobby: inviting, invited, joining, or what became
 * of the last invite. */
int fleet_view_mp_lobby(const struct fleet_match *m, const char *peer, char *buf, size_t n);

/* Result: the heading ("Enemy fleet destroyed", "Fleet lost", "No result";
 * "Opponent forfeited", "You forfeited" when the match ended by forfeit),
 * how it ended as a short caption ("ALL SHIPS SUNK", "THEY FORFEITED",
 * "VOID · RECORDS DIFFER"), and the verification of the opponent's
 * fleet. */
const char *fleet_view_mp_outcome(const struct fleet_match *m);
const char *fleet_view_mp_ended(const struct fleet_match *m);
const char *fleet_view_mp_verify(const struct fleet_match *m);

/* Shots and hits by one side ("47 % · 8 OF 17"), from the log. */
int fleet_view_mp_accuracy(const struct fleet_match *m, int mine, char *buf, size_t n);

/* "ANNA · 2 HOPS · HEARD 3 MIN AGO", a lobby row. */
int fleet_view_mp_peer_row(const struct fleet_link_peer *p, int64_t now_ms, char *buf, size_t n);

#endif
