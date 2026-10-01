/*
 * RIFT's DM sound: whether a direct message that has just arrived is worth
 * a sound now, and when it is not.
 *
 * Two questions are kept apart, and this file answers only the second:
 *
 *   - Did a direct message genuinely just arrive? The model decides that,
 *     once, where the message is filed (rift_model_apply_live_message): an
 *     incoming direct message from a live event, new to the window, above
 *     every id this run has shown, and not a retransmission of one of the
 *     last few. History reloads, snapshots, state updates, replays, channel
 *     messages and this device's own messages never count. It says so by
 *     raising m->dm_arrivals.
 *   - Should that make a sound? Here: only when the reader has the DM sound
 *     on, only when the platform can play one and is not muted, and at most
 *     one sound in RIFT_NOTIFY_GAP_MS however many arrive - a burst of
 *     twenty is one sound, not twenty, and nothing is queued to play later.
 *
 * Every arrival is consumed by the poll that sees it, whatever the answer:
 * one that fell in the gap, or arrived while the sound was off or muted, is
 * counted and dropped, never replayed when the setting or the volume comes
 * back.
 *
 * No LVGL, no I/O, no clock of its own: host-tested by
 * tests/rift_notify_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_NOTIFY_H
#define RIFT_NOTIFY_H

#include "rift_model.h"

#include <stdint.h>

/* The quiet time after a sound. Long enough that a conversation going back
 * and forth is not a sound per line, short enough that a message a minute
 * later is heard. */
#define RIFT_NOTIFY_GAP_MS 10000

struct rift_notify {
    int enabled;          /* the reader's setting */
    unsigned seen;        /* m->dm_arrivals as last read */
    int have_last;
    int64_t last_ms;      /* when the last sound was asked for */
    /* What became of the arrivals, for tests and for the screen. */
    unsigned played;      /* a sound was asked for */
    unsigned coalesced;   /* inside the gap after one: no second sound */
    unsigned off;         /* the setting was off */
    unsigned silent;      /* the platform could not play one, or was muted */
};

/* Start counting from what the model already holds: arrivals before this
 * are not news to a listener that was not there for them. */
void rift_notify_init(struct rift_notify *n, const struct rift_model *m, int enabled);
void rift_notify_set_enabled(struct rift_notify *n, int enabled);

/* Read the arrivals since the last call. can_sound is whether a sound could
 * be played at all right now (a backend that is available, a volume that is
 * not muted). Returns 1 when a sound should be played now - and records it
 * as played - or 0. */
int rift_notify_poll(struct rift_notify *n, const struct rift_model *m, int64_t now_ms,
                     int can_sound);

#endif
