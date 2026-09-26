/*
 * Wave persistence: two small files under $POCKETOS_STATE_DIR/wave, and one
 * short-lived capture under $POCKETOS_RUNTIME_DIR/wave.
 *
 *   wave.conf   the selected preset, as its id. "wave-prefs 1", then
 *               key=value lines; unknown keys are ignored, so a later
 *               version can add some.
 *   history     wave_history.h's text form, at most WAVE_HISTORY_MAX
 *               entries (about 8 KB). Written after each change the app
 *               makes, never from a timer.
 *
 * Both writes are the Notes and Clock one: temporary file, fsync, rename, so
 * a reader sees one whole version or the other. Both directories are 0700 and
 * the files 0600: received messages are somebody's words.
 *
 * THE CAPTURE. A capture is recorded by the helper to
 * $POCKETOS_RUNTIME_DIR/wave/capture.wav, decoded from there, and removed:
 * after the decode, when the app starts and when it closes. /run is a tmpfs
 * that starts empty on every boot, so audio never reaches flash and cannot
 * outlive a power cut even if the app is killed between the two steps. At
 * the longest capture it is 2.9 MB of RAM for a few seconds.
 *
 * Nothing else is kept: not the listen toggle (the microphone never turns
 * itself on when the app opens), not the typed message, not audio.
 *
 * This is the only file in the app that touches the filesystem
 * (tests/wave_lint.sh). Tested against a temporary state directory in
 * tests/wave_store_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETWAVE_STORE_H
#define POCKETWAVE_STORE_H

#include "wave_history.h"

#include <stddef.h>

#define WAVE_STORE_SUBDIR "wave"
#define WAVE_PREFS_FILE "wave.conf"
#define WAVE_PREFS_MAGIC "wave-prefs 1"
#define WAVE_HISTORY_FILE "history"
#define WAVE_CAPTURE_FILE "capture.wav"
/* The largest file either reader accepts; anything larger is not ours. */
#define WAVE_STORE_FILE_MAX (WAVE_HISTORY_TEXT_MAX + 1024)

struct wave_prefs {
    char preset[WAVE_PRESET_ID_MAX]; /* "" when none is stored */
};

/* $POCKETOS_STATE_DIR/wave. */
int wave_store_dir(char *out, size_t n);

/* 0 loaded, 1 nothing stored yet, -1 a file that could not be used (prefs
 * are then empty). */
int wave_store_load_prefs(struct wave_prefs *p);
int wave_store_save_prefs(const struct wave_prefs *p);

/* 0 loaded (skipped lines counted in *skipped), 1 none yet, -1 unusable (h
 * is then empty). */
int wave_store_load_history(struct wave_history *h, int *skipped);
/* An empty history removes the file rather than writing an empty one. */
int wave_store_save_history(const struct wave_history *h);

/* $POCKETOS_RUNTIME_DIR/wave/capture.wav, its directory created (0700). 0,
 * or -1 when the path would not fit or the directory cannot be made. */
int wave_store_capture_path(char *out, size_t n);
/* Remove the capture if there is one. Never fails loudly: there is nothing a
 * caller could do differently. */
void wave_store_capture_remove(void);

#endif
