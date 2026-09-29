/*
 * Vision's files: the only part of the Vision app that touches the
 * filesystem.
 *
 *   $POCKETOS_STATE_DIR/vision/settings.v1   the settings (vision_settings.h)
 *
 * default /var/lib/pocketos/vision: app-owned storage in the pattern
 * PocketFleet established and RIFT, the games and DeskBuddy follow. The
 * directory is 0700 (it is also where the helper keeps the owner's face
 * profile, which is biometric) and the file 0600. Writes are atomic (temp
 * file, fsync, rename), so a power cut leaves the old file or the new one.
 * A failure is reported to the caller and Vision carries on with what it
 * has.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef VISION_STORE_H
#define VISION_STORE_H

#include "vision_settings.h"

#define VISION_STORE_SUBDIR "vision"
#define VISION_STORE_SETTINGS "settings.v1"

const char *vision_store_dir(void);

/* 0 read (refused values left at their defaults), 1 no file (defaults), -1
 * unreadable (defaults). */
int vision_store_load(struct vision_settings *s);
int vision_store_save(const struct vision_settings *s);

#endif
