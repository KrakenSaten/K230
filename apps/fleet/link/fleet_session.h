/*
 * PocketFleet multiplayer session: the match state machine, a link and the
 * save file joined up (docs/apps/FLEET_MULTIPLAYER.md).
 *
 * The one place the rules of the joint are kept:
 *
 *   - packets are pumped only once the session is engaged - the player opened
 *     Multiplayer or pressed Resume. Opening Fleet transmits nothing, and does
 *     not even read the inbox, so a stale packet waiting there cannot draw a
 *     reply out of a device whose owner is playing single player;
 *   - after every call into the match, unsaved state is saved before a single
 *     packet is taken from its outbox (fleet_match_pop refuses until then);
 *   - a failed save turns the gate off for the session and says so, and the
 *     match plays on;
 *   - random bits (session ids, commitment salts) come from the caller's
 *     entropy source, never from a clock.
 *
 * Pure C, no LVGL: the app drives it from a timer, and tests/fleet_session_test
 * drives it with the loop link and an in-memory store.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_SESSION_H
#define POCKETFLEET_SESSION_H

#include "fleet_link.h"

#include "../net/fleet_match.h"
#include "../net/fleet_match_save.h"

struct fleet_session {
    struct fleet_match m;
    int ready;                          /* the match exists: our key is known */
    struct fleet_link *link;
    /* Save the blob; 0 when it is durably written. */
    int (*save)(void *user, const uint8_t *blob, size_t n);
    /* Fill buf with random bytes; 0 on success. */
    int (*entropy)(void *user, void *buf, size_t n);
    void *user;
    uint8_t saved[FLEET_MATCH_SAVE_SIZE];
    int have_saved;                     /* what the store held at start */
    int store_fault;                    /* it held something unreadable */
    int identity_changed;               /* it was saved by another identity */
    int engaged;
    int save_failed;
    unsigned sent;
    unsigned received;
    unsigned busy;
    int64_t now;
};

void fleet_session_init(struct fleet_session *s, struct fleet_link *link,
                        int (*save)(void *, const uint8_t *, size_t),
                        int (*entropy)(void *, void *, size_t), void *user);
/* What the store held when the app opened: a blob (NULL when there was none)
 * and whether reading it failed. */
void fleet_session_set_saved(struct fleet_session *s, const uint8_t *blob, size_t n, int fault);
/* Start pumping packets. Idempotent. */
void fleet_session_engage(struct fleet_session *s, int64_t now);
/* Do everything due: link I/O, received packets, timers, saving, sending. */
void fleet_session_poll(struct fleet_session *s, int64_t now);
/* The match's revision, or 0 before it exists: changes whenever something a
 * screen shows has changed. */
unsigned fleet_session_revision(const struct fleet_session *s);

/* User actions. 0 when done, -1 when not possible now. */
int fleet_session_invite(struct fleet_session *s, const uint8_t peer[FLEET_KEY_BYTES],
                         const char *name, int64_t now);
int fleet_session_accept(struct fleet_session *s, int64_t now);
int fleet_session_decline(struct fleet_session *s, int64_t now);
int fleet_session_cancel(struct fleet_session *s, int64_t now);
int fleet_session_deploy(struct fleet_session *s, const struct fleet_board *board, int64_t now);
int fleet_session_fire(struct fleet_session *s, int row, int col, int64_t now);
int fleet_session_forfeit(struct fleet_session *s, int64_t now);
int fleet_session_resume(struct fleet_session *s, int64_t now);
void fleet_session_dismiss(struct fleet_session *s, int64_t now);

/* The name to show for the opponent: the match's, the mesh's, or its key. */
const char *fleet_session_peer_name(struct fleet_session *s, char *buf, size_t n);

#endif
