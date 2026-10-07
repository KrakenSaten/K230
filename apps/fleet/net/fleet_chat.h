/*
 * PocketFleet multiplayer: the chat lines of the match in hand
 * (docs/apps/FLEET_MULTIPLAYER.md, "Chat").
 *
 * Bookkeeping only, and every part of it bounded: the lines kept while the
 * match is open, which of ours are still owed a receipt, and the peer's
 * recent lines so a copy is never shown twice. When a line goes on the air,
 * how often it is tried and what it may cost is the match's business
 * (fleet_match.c), because only the match knows what the game needs first.
 *
 * Nothing here is saved. A chat line lives as long as the app has the match
 * open, and a new session starts with none.
 *
 * Pure C: no LVGL, no IPC, no filesystem, no clock (tests/fleet_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_CHAT_H
#define POCKETFLEET_CHAT_H

#include "fleet_proto.h"

#include <stddef.h>
#include <stdint.h>

/* Lines kept, theirs and ours together. The oldest settled one goes first. */
#define FLEET_CHAT_HISTORY 16
/* Ours not yet confirmed or given up. A fifth is refused until one settles. */
#define FLEET_CHAT_OUTGOING 4
/* The peer's most recent lines, remembered by id and check, so a copy that
 * arrives late is known for one. */
#define FLEET_CHAT_SEEN 16

enum fleet_chat_state {
    FLEET_CHAT_RECEIVED = 0,    /* theirs */
    FLEET_CHAT_QUEUED,          /* ours, not yet on the air */
    FLEET_CHAT_SENDING,         /* ours, on the air, no receipt yet */
    FLEET_CHAT_DELIVERED,       /* ours, and the peer confirmed it */
    FLEET_CHAT_FAILED,          /* ours, never confirmed: given up */
};

struct fleet_chat_line {
    uint8_t mine;
    uint8_t state;              /* enum fleet_chat_state */
    uint8_t id;
    uint8_t len;
    uint16_t check;
    char text[FLEET_CHAT_TEXT_MAX + 1];
};

struct fleet_chat {
    struct fleet_chat_line line[FLEET_CHAT_HISTORY];    /* oldest first */
    uint8_t count;
    uint8_t unread;             /* theirs, since the player last looked */
    uint8_t next_id;
    /* the one of ours on the air: how often it has gone, and when next */
    uint8_t tries;
    int64_t next_try;
    uint8_t held;               /* the line due is waiting for the governor */
    uint8_t seen_id[FLEET_CHAT_SEEN];
    uint16_t seen_check[FLEET_CHAT_SEEN];
    uint8_t seen_count;
    uint8_t seen_next;
};

/* Empty it. first_id: where our ids start, so a restarted app does not
 * reuse the ids its last run gave the same peer. */
void fleet_chat_reset(struct fleet_chat *c, uint8_t first_id);

/* Queue a line of ours. Spaces and tabs at either end are dropped first.
 * Returns 0, -1 when what is left is empty, too long or not a line CHAT can
 * carry (fleet_chat_text_ok), or -2 when FLEET_CHAT_OUTGOING are waiting. */
int fleet_chat_add_mine(struct fleet_chat *c, const char *text);

/* A line of theirs arrived. Returns 1 when it is new and was kept, 0 when it
 * is a copy of one already shown (it is not kept again), -1 when it is not a
 * line at all. */
int fleet_chat_add_theirs(struct fleet_chat *c, uint8_t id, const uint8_t *text, size_t n);

/* The line of ours to work on: the oldest one queued or on the air. There is
 * never more than one on the air. NULL when there is none. */
struct fleet_chat_line *fleet_chat_outgoing(struct fleet_chat *c);
/* How many of ours are queued or on the air. */
int fleet_chat_pending(const struct fleet_chat *c);

/* A receipt arrived. Returns 1 when it confirmed the line on the air, 0 when
 * it names no line of ours that is waiting (late, or someone else's). */
int fleet_chat_acked(struct fleet_chat *c, uint8_t id, uint16_t check);

/* Give up on every line of ours still waiting. Returns how many. */
int fleet_chat_fail_pending(struct fleet_chat *c);

/* The player has seen every line. */
void fleet_chat_seen(struct fleet_chat *c);

/* The newest line, or NULL. */
const struct fleet_chat_line *fleet_chat_last(const struct fleet_chat *c);

#endif
