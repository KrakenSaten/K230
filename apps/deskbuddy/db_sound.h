/*
 * The one sound DeskBuddy makes, and the seam it is made through.
 *
 * An armed Desk Guard with "Visit sound" on plays one short chime when it
 * logs a new visit (db_brain_take_visit_alert decides when). DeskBuddy does
 * not open the sound card itself: apps never touch hardware (ADR-002).
 *
 * The sound goes through a backend, as RIFT's message sounds do
 * (apps/rift/rift_sound.h). The built-in one plays through Doors's existing
 * audio path - `pos-record play`, the per-operation helper on
 * core/pocketaudio that the Recorder and RIFT already use (ADR-010 and its
 * amendments) - so the audio lock, the route, the amplifier, the level
 * ceiling and the recovery after a crash are that path's. The silent
 * backend (db_sound_set_backend(NULL)) says why nothing is heard.
 *
 * A backend's play must return at once (no audio work on the LVGL thread,
 * ADR-004) and stop must leave nothing open: DeskBuddy calls it when it
 * closes.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_SOUND_H
#define DB_SOUND_H

#include <stddef.h>
#include <stdint.h>

struct db_sound_backend {
    const char *name;
    /* 1 when this backend can make a sound on this device now. NULL: never. */
    int (*available)(void);
    /* Start the visit sound at this volume (1..100 percent of the system
     * level) and return at once. 0 when it was started, -1 when not. */
    int (*play)(int volume_percent);
    /* Stop anything still sounding and release what play took. May be NULL. */
    void (*stop)(void);
    /* One line for a reader: what this backend does, or why it cannot. */
    const char *why;
};

/* Install a backend. NULL installs the silent one; the default, before any
 * call, is the pos-record backend (db_sound_helper.c). The pointer is kept,
 * so it must outlive DeskBuddy - a static is the shape. */
void db_sound_set_backend(const struct db_sound_backend *backend);
const char *db_sound_backend_name(void);
const char *db_sound_why(void);

int db_sound_available(void);
/* Ask for the visit sound. -1 when the volume is 0, the backend has none,
 * or it would not start one (one is still sounding). */
int db_sound_play(int volume_percent);
void db_sound_stop(void);

/* ---- the pos-record backend (db_sound_helper.c) -------------------------- */

extern const struct db_sound_backend db_sound_pos_record;

/* The visit sound, as 48 kHz mono 16-bit samples: a short lead-in of silence
 * (the amplifier coming up), then three notes, each with a soft attack and
 * decay so nothing clicks. Pure. Returns the frames written, or the frames
 * needed when out is NULL; 0 when out is too small. */
size_t db_sound_tone_render(int16_t *out, size_t max_frames);
/* The WAV it is played from: written once into DeskBuddy's runtime
 * directory ($POCKETOS_RUNTIME_DIR/deskbuddy/visit.wav) when first needed,
 * atomically. NULL when it could not be written. */
const char *db_sound_wav_path(void);

#endif
