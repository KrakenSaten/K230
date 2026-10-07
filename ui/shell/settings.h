/*
 * Flat key=value settings store for the shell (DS v0.1 §8 persistence).
 *
 * File: $POCKETOS_CONFIG_DIR/settings.conf, default /etc/pocketos. One
 * "key=value" per line, '#' comments. Unknown or malformed lines are ignored
 * on read and dropped on write. Writes are atomic (temp file + rename) and
 * settings_set() is transactional: the in-memory value changes only when the
 * file was written. Pure C, no LVGL, so it is unit-tested natively.
 *
 * Standard keys: theme (DS theme id), display_mode (normal|outdoor|night),
 * reduced_motion (0|1, default 0; DS §12, see pocketos_shell_reduced_motion()),
 * display_brightness (10..100 percent; brightness.h, unset = as booted),
 * audio_volume (10..100 step 10, default 100), audio_muted (0|1; volume.h),
 * text_size (small|medium|large, default small; DS §46, shell.c),
 * screen_off_s and auto_lock_s (seconds, 0 never; power_policy.h),
 * lock_screen (0|1, default 1), timezone (an IANA name; tz_zones.h) and
 * debug_overlay (0|1, default 0; shell_overlay.h) - DS §52.
 *
 * SECURITY: this store is for non-secret preferences (theme, display mode
 * and the like). It is a world-readable plain-text file with no integrity
 * protection. It MUST NOT hold passwords, private keys, Wi-Fi credentials,
 * API tokens or any other secret until PocketOS has a dedicated credential
 * storage design (see docs/ARCHITECTURE.md, "Not yet decided").
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_SETTINGS_H
#define POCKETOS_SETTINGS_H

#include "pocketpaths.h"

#include <stddef.h>

/* Kept as the historical name; the value belongs to pocketpaths.h. */
#define SETTINGS_DEFAULT_DIR POCKETOS_CONFIG_DIR_DEFAULT
#define SETTINGS_FILE "settings.conf"
#define SETTINGS_MAX_KEYS 64
#define SETTINGS_KEY_MAX 32
#define SETTINGS_VALUE_MAX 128

/* Load the file. Returns 0 when read, 1 when absent (empty store), -1 when
 * present but not read whole: it could not be opened or read (EACCES, EIO,
 * ...) or holds more than SETTINGS_MAX_KEYS keys. After -1 the table holds
 * what was read, if anything, and settings_set() refuses to write (returns
 * -1, table unchanged) so the file is never replaced by a partial table; a
 * later settings_init() that reads it whole lifts that. The caller logs. */
int settings_init(void);
const char *settings_path(void);
/* Value for key, or fallback (may be NULL) when unset. The pointer stays
 * valid until the key is set again. */
const char *settings_get(const char *key, const char *fallback);
/* Set and persist. Returns 0, or -1 when the file could not be written or
 * was not read whole (the in-memory table is then unchanged). value NULL
 * removes the key. */
int settings_set(const char *key, const char *value);
int settings_count(void);

#endif
