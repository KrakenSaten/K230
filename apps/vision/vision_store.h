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
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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

/* The detector files Vision knows by name (docs/apps/VISION.md, "The
 * model"), in /usr/share/doors/vision/: R0, which the image carries from
 * 0.3.6, and UPSTREAM, which only a bench unit is given by hand. Each is
 * overridable for the simulator ($POCKETOS_VISION_MODEL_R0,
 * $POCKETOS_VISION_MODEL_UPSTREAM). Not "yolo*": the image's refusal of the
 * SDK's Ultralytics models goes by that name. */
const char *vision_store_detector_path(enum vision_detector d);
/* The detector files on the unit now: a bit per enum vision_detector. */
uint32_t vision_store_detector_files(void);

#endif
