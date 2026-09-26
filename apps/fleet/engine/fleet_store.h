/*
 * PocketFleet save file. The only part of the game that touches the
 * filesystem; everything else works on buffers.
 *
 * Location: $POCKETOS_STATE_DIR/fleet/save.v1, default
 * /var/lib/pocketos/fleet/save.v1. This is app-owned storage: PocketOS has
 * no storage.* service yet and the shell's settings store is for short
 * non-secret preferences, not game state. Whether /var/lib is writable on
 * the K230 is DOCUMENTED from the Buildroot defconfig (ext4 rootfs, no
 * read-only setting) and ASSUMED until it is verified on hardware.
 *
 * Persistence never blocks play. Every call reports failure and does
 * nothing else; PocketFleet then continues as a session-only game and
 * offers no Resume. Writes are atomic (temp file, fsync, rename), so a
 * power loss leaves either the previous save or the new one, never a
 * half-written file.
 *
 * The save holds no secrets: a board layout and a shot history.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_STORE_H
#define POCKETFLEET_STORE_H

#include "fleet_rules.h"

#define FLEET_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define FLEET_STORE_SUBDIR "fleet"
#define FLEET_STORE_FILE "save.v1"

/* Directory and full path of the save file. The pointers stay valid until
 * the next call. */
const char *fleet_store_dir(void);
const char *fleet_store_path(void);
/* Write the match. Returns 0, or -1 when it could not be stored (the caller
 * carries on without persistence). */
int fleet_store_save(const struct fleet_game *game);
/* Read the match. Returns 0 on success, 1 when there is no save, and -1 when
 * one exists but is unreadable or unusable; game is untouched unless 0 is
 * returned. */
int fleet_store_load(struct fleet_game *game);
/* Remove the save. Returns 0 when there is no save afterwards, -1 otherwise. */
int fleet_store_clear(void);
/* 1 when a save file is present and decodes, else 0. Cheap enough for the
 * menu to decide whether Resume is offered. */
int fleet_store_has_save(void);

/* ---- multiplayer: match.v1 (docs/apps/FLEET_MULTIPLAYER.md) ---------------
 *
 * The multiplayer match, beside the single-player save and independent of
 * it. The blob is apps/fleet/net/fleet_match_save.c's; this only stores it.
 *
 * Stricter than save.v1, because here a packet may already have left on the
 * strength of a write: the file is flushed, renamed over the old one, and then
 * the directory itself is flushed, so the new name survives a power cut too.
 * A failed directory flush is a failed write. */
#define FLEET_STORE_MATCH_FILE "match.v1"
#define FLEET_STORE_MATCH_MAX 1024

const char *fleet_store_match_path(void);
/* Returns 0 when the blob is durably written, -1 otherwise. */
int fleet_store_match_save(const uint8_t *blob, size_t n);
/* Read up to max bytes into buf. Returns the length read, 0 when there is no
 * file, and -1 when there is one that cannot be read. */
int fleet_store_match_load(uint8_t *buf, size_t max);

#endif
