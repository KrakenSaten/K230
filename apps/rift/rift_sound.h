/*
 * The one sound RIFT makes, and the seam it is made through.
 *
 * RIFT asks for a short, restrained message sound when a direct message
 * arrives (rift_notify.h decides when). It does not open the sound card
 * itself: apps never touch hardware (ADR-002), and the one exception there
 * is - pocketaudio driven by a per-operation pos-wave helper - is Wave's and
 * no one else's (ADR-004, "a later need is decided against ADR-002, not by
 * extending this"). ADR-004 names system sounds as the point at which audio
 * moves to a platform owner.
 *
 * So the sound goes through a backend, the way PocketClock's alert does
 * (apps/clock/clock_alert.h). This build has no platform sound to hand it
 * to, and the built-in backend says so: not available, and why, in words the
 * screen shows beside the setting - a DM sound that is switched on and cannot
 * be heard must not look like one that can. When the platform grows a
 * notification sound, a backend that asks it to play is registered here and
 * nothing in the policy, the setting or the screens changes. The requirement
 * is written down in docs/apps/RIFT.md, "The DM sound".
 *
 * A backend's play must return at once (no audio work on the LVGL thread,
 * ADR-004) and stop must leave nothing open: RIFT calls it when it closes.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_SOUND_H
#define RIFT_SOUND_H

struct rift_sound_backend {
    const char *name;
    /* 1 when this backend can make a sound on this device now. NULL: never. */
    int (*available)(void);
    /* Start the short message sound at this volume (1..100 percent of the
     * system level) and return at once. 0 when it was started, -1 when not. */
    int (*play)(int volume_percent);
    /* Stop anything still sounding and release what play took. May be NULL. */
    void (*stop)(void);
    /* One line for a reader: what this backend does, or why it cannot. */
    const char *why;
};

/* Install a backend. NULL restores the built-in one, which has no sound.
 * The pointer is kept, so it must outlive RIFT - a static is the shape. */
void rift_sound_set_backend(const struct rift_sound_backend *backend);
const char *rift_sound_backend_name(void);
const char *rift_sound_why(void);

int rift_sound_available(void);
/* Ask for the sound. -1 when the backend has none or would not start it. */
int rift_sound_play(int volume_percent);
void rift_sound_stop(void);

#endif
