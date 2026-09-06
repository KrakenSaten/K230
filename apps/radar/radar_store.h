/*
 * PocketRadar record file: the best score and a few lifetime counters.
 *
 * This is the only part of PocketRadar that touches the filesystem. The
 * engine under apps/radar/engine does no I/O at all, which is what lets the
 * whole of it be unit-tested natively; tests/radar_lint.sh enforces both
 * halves of that split.
 *
 * Location: $POCKETOS_STATE_DIR/radar/record.v1, default
 * /var/lib/pocketos/radar/record.v1. This is app-owned storage, following
 * the pattern PocketFleet established (docs/apps/POCKETFLEET.md, deviation
 * D2): PocketOS has no storage.* service yet, and the shell's settings store
 * is for short non-secret preferences, not for app data. Whether /var/lib is
 * writable on the K230 is DOCUMENTED from the Buildroot defconfig (ext4
 * rootfs, no read-only setting) and ASSUMED until it is verified on
 * hardware.
 *
 * A radar run is two to five minutes long, so there is deliberately no
 * resume and nothing here describes a run in progress. Only what outlives
 * one is kept: the best score, the longest streak, the highest level
 * reached and three counters. A run that is interrupted is simply lost,
 * which is the right trade for a game this short and is what keeps this
 * file thirty bytes and its validation obvious.
 *
 * Persistence never blocks play. Every call reports failure and does
 * nothing else; the app then runs session-only, with no best score carried
 * in or out. Writes are atomic (temp file, fsync, rename), so a power loss
 * leaves either the previous record or the new one, never a half-written
 * file.
 *
 * The record holds no secrets: six numbers about a game.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETRADAR_STORE_H
#define POCKETRADAR_STORE_H

#include "engine/radar_score.h"

#define RADAR_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define RADAR_STORE_SUBDIR "radar"
#define RADAR_STORE_FILE "record.v1"

#define RADAR_RECORD_MAGIC0 'P'
#define RADAR_RECORD_MAGIC1 'R'
#define RADAR_RECORD_MAGIC2 'R'
#define RADAR_RECORD_MAGIC3 '1'
#define RADAR_RECORD_VERSION 1
/* magic + version + the six fields + checksum */
#define RADAR_RECORD_SIZE (4 + 2 + 4 + 2 + 2 + 4 + 4 + 4 + 4)

/* Directory and full path of the record file. The pointers stay valid until
 * the next call. */
const char *radar_store_dir(void);
const char *radar_store_path(void);

/* Encode a record into buf. Returns the number of bytes written, or -1 when
 * buf is too small. Pure encoding, no I/O, so the format is tested without
 * a filesystem. Fields go out one at a time in little-endian order rather
 * than by copying the struct, so the file does not depend on padding,
 * alignment or the host's byte order. */
int radar_record_encode(const struct radar_record *record, uint8_t *buf, size_t n);
/* Decode into record. Returns 0 on success. Returns -1 and leaves record
 * untouched when the blob is not a record, is from another version, is
 * damaged, or describes a lifetime the game could not have produced. The
 * FNV-1a checksum catches accidental corruption - a truncated write, a bad
 * block - and is not a cryptographic digest: anyone who can write the file
 * can recompute it. Nothing here is secret, so that is the right trade, and
 * it is the invariant checks rather than the checksum that keep an
 * impossible record out of the game. */
int radar_record_decode(struct radar_record *record, const uint8_t *buf, size_t n);

/* Write the record. Returns 0, or -1 when it could not be stored (the
 * caller carries on without persistence). */
int radar_store_save(const struct radar_record *record);
/* Read the record. Returns 0 on success, 1 when there is no record, and -1
 * when one exists but is unreadable or unusable. record is untouched unless
 * 0 is returned, so the caller initialises it with radar_record_init()
 * first and simply keeps the empty one on 1 or -1. A damaged file is left
 * in place rather than deleted, matching the settings store's policy. */
int radar_store_load(struct radar_record *record);
/* Remove the record. Returns 0 when there is none afterwards, -1
 * otherwise. */
int radar_store_clear(void);

#endif
