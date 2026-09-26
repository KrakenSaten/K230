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
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_VIEW_MP_H
#define POCKETFLEET_VIEW_MP_H

#include "../link/fleet_link.h"
#include "../net/fleet_match.h"

/* "VS ANNA · SHOT 14", for the header. */
int fleet_view_mp_status(const struct fleet_match *m, const char *peer, char *buf, size_t n);

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
