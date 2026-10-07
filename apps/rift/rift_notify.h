/*
 * RIFT's message sounds: whether a message that has just arrived is worth a
 * sound now, which of the two sounds, and when it is not.
 *
 * Two questions are kept apart, and this file answers only the second:
 *
 *   - Did a message genuinely just arrive? The model decides that, once,
 *     where the message is filed (rift_model_apply_live_message): an
 *     incoming message from a live event, new to the window, above every id
 *     of its kind this run has shown, and not a retransmission of one of the
 *     last few. History reloads, snapshots, repaints, state updates, replays
 *     and this device's own messages never count. It says so by raising
 *     m->dm_arrivals or m->ch_arrivals (with the channel's conversation).
 *   - Should that make a sound? Here, and there are two sounds: one for a
 *     direct message, a different one for a channel message. A direct
 *     message sounds only while the reader has DM sounds on; a channel
 *     message only while channel sounds are on and that channel is not
 *     muted. Neither sounds when the platform cannot play one or is muted.
 *     And at most one sound of either kind in RIFT_NOTIFY_GAP_MS however
 *     many arrive - a burst of twenty is one sound, not twenty, and nothing
 *     is queued to play later. When a direct and a channel message arrive in
 *     the same pass, the direct one's sound is the one played.
 *
 * Every arrival is consumed by the poll that sees it, whatever the answer:
 * one that fell in the gap, or arrived while its sound was off, its channel
 * muted or the device muted, is counted and dropped, never replayed when the
 * setting or the volume comes back.
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

/* What a poll asks to be played. */
enum rift_notify_sound {
    RIFT_NOTIFY_NONE = 0,
    RIFT_NOTIFY_DM = 1,
    RIFT_NOTIFY_CHANNEL = 2,
};

/* Whether a channel (its conversation key) is muted: the reader's own
 * choice, kept by the app (rift_store.h). NULL: nothing is muted. */
typedef int (*rift_notify_muted_fn)(const char *conv_key, void *user);

struct rift_notify {
    int enabled;          /* the DM sound's setting */
    int ch_enabled;       /* the channel sound's setting */
    rift_notify_muted_fn muted;
    void *muted_user;
    unsigned seen;        /* m->dm_arrivals as last read */
    unsigned ch_seen;     /* m->ch_arrivals as last read */
    int have_last;
    int64_t last_ms;      /* when the last sound of either kind was asked for */
    /* What became of the arrivals, for tests and for the screen. */
    unsigned played;      /* a DM sound was asked for */
    unsigned coalesced;   /* inside the gap after a sound: no second one */
    unsigned off;         /* the DM setting was off */
    unsigned silent;      /* the platform could not play one, or was muted */
    unsigned ch_played;   /* a channel sound was asked for */
    unsigned ch_off;      /* the channel setting was off */
    unsigned ch_muted;    /* that channel is muted */
};

/* Start counting from what the model already holds: arrivals before this
 * are not news to a listener that was not there for them. Channel sounds
 * start on; rift_notify_set_channel says otherwise. */
void rift_notify_init(struct rift_notify *n, const struct rift_model *m, int enabled);
void rift_notify_set_enabled(struct rift_notify *n, int enabled);
void rift_notify_set_channel(struct rift_notify *n, int enabled, rift_notify_muted_fn muted,
                             void *user);

/* Read the arrivals since the last call. can_sound is whether a sound could
 * be played at all right now (a backend that is available, a volume that is
 * not muted). Returns the sound to play now - and records it as played - or
 * RIFT_NOTIFY_NONE. */
int rift_notify_poll(struct rift_notify *n, const struct rift_model *m, int64_t now_ms,
                     int can_sound);

#endif
