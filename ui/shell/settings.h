/*
 * Flat key=value settings store for the shell (DS v0.1 §8 persistence).
 *
 * File: $POCKETOS_CONFIG_DIR/settings.conf, default /etc/pocketos. One
 * "key=value" per line, '#' comments. Unknown or malformed lines are ignored
 * on read and dropped on write. Writes are atomic (temp file + rename).
 * Pure C, no LVGL, so it is unit-tested natively.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SETTINGS_H
#define POCKETOS_SETTINGS_H

#include <stddef.h>

#define SETTINGS_DEFAULT_DIR "/etc/pocketos"
#define SETTINGS_FILE "settings.conf"
#define SETTINGS_MAX_KEYS 64
#define SETTINGS_KEY_MAX 32
#define SETTINGS_VALUE_MAX 128

/* Load the file. Returns 0 when read, 1 when absent (empty store), -1 when
 * present but unreadable (empty store, caller logs). */
int settings_init(void);
const char *settings_path(void);
/* Value for key, or fallback (may be NULL) when unset. The pointer stays
 * valid until the key is set again. */
const char *settings_get(const char *key, const char *fallback);
/* Set and persist. Returns 0, or -1 when the file could not be written (the
 * in-memory value is still updated). value NULL removes the key. */
int settings_set(const char *key, const char *value);
int settings_count(void);

#endif
