/*
 * The two sounds RIFT makes, and the seam they are made through.
 *
 * RIFT asks for a short, restrained sound when a message arrives
 * (rift_notify.h decides when): one for a direct message, a different one
 * for a channel message. It does not open the sound card itself: apps never
 * touch hardware (ADR-002).
 *
 * So the sound goes through a backend, the way PocketClock's alert does
 * (apps/clock/clock_alert.h). The built-in one plays through Doors's
 * existing audio path - pos-record, the per-operation helper on
 * core/pocketaudio that the Recorder plays WAV files with (ADR-010, and its
 * Amendment 1 for RIFT) - so the audio lock, the route, the amplifier, the
 * level ceiling and the recovery after a crash are all the ones that path
 * already has, and nothing in RIFT opens audio (rift_sound_helper.c). The
 * silent backend (rift_sound_set_backend(NULL)) says why nothing is heard.
 *
 * A backend's play must return at once (no audio work on the LVGL thread,
 * ADR-004) and stop must leave nothing open: RIFT calls it when it closes.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_SOUND_H
#define RIFT_SOUND_H

#include <stddef.h>
#include <stdint.h>

/* Which sound: they are told apart by ear (rift_sound_tone_render). */
enum rift_sound_kind {
    RIFT_SOUND_DM = 0,      /* two short notes, rising */
    RIFT_SOUND_CHANNEL = 1, /* one softer, lower note */
    RIFT_SOUND_KINDS = 2,
};

struct rift_sound_backend {
    const char *name;
    /* 1 when this backend can make a sound on this device now. NULL: never. */
    int (*available)(void);
    /* Start one sound at this volume (1..100 percent of the system level)
     * and return at once. 0 when it was started, -1 when not. */
    int (*play)(enum rift_sound_kind kind, int volume_percent);
    /* Stop anything still sounding and release what play took. May be NULL. */
    void (*stop)(void);
    /* One line for a reader: what this backend does, or why it cannot. */
    const char *why;
};

/* Install a backend. NULL installs the silent one; the default, before any
 * call, is the pos-record backend (rift_sound_helper.c). The pointer is
 * kept, so it must outlive RIFT - a static is the shape. */
void rift_sound_set_backend(const struct rift_sound_backend *backend);
const char *rift_sound_backend_name(void);
const char *rift_sound_why(void);

int rift_sound_available(void);
/* Ask for a sound. -1 when the backend has none or would not start it. */
int rift_sound_play(enum rift_sound_kind kind, int volume_percent);
void rift_sound_stop(void);

/* ---- the pos-record backend (rift_sound_helper.c) ------------------------ */

extern const struct rift_sound_backend rift_sound_pos_record;

/* The sounds, as 48 kHz mono 16-bit samples: a short lead-in of silence (the
 * amplifier coming up), then the notes, each with a soft attack and decay so
 * nothing clicks. Pure. Returns the frames written, or the frames needed
 * when out is NULL. */
size_t rift_sound_tone_render(enum rift_sound_kind kind, int16_t *out, size_t max_frames);
/* The WAV a kind is played from: written once into RIFT's runtime directory
 * ($POCKETOS_RUNTIME_DIR/rift/) when first needed, atomically. NULL when it
 * could not be written. */
const char *rift_sound_wav_path(enum rift_sound_kind kind);

#endif
