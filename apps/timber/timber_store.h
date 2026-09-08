/*
 * PocketTimber record file: the best score and a few lifetime counters.
 *
 * This is the only part of PocketTimber that touches the filesystem. The
 * engine under apps/timber/engine does no I/O at all, which is what lets
 * the whole of it be unit-tested natively; tests/timber_lint.sh enforces
 * both halves of that split. The store knows no rule: it counts what the
 * engine reported about a finished run and keeps the numbers safe.
 *
 * Location: $POCKETOS_STATE_DIR/timber/record.v1, default
 * /var/lib/pocketos/timber/record.v1. App-owned storage, as PocketRadar's
 * D1 and PocketFleet's D2 established: PocketOS has no storage service and
 * the shell's settings store is for short preferences, not app data. That
 * /var/lib is writable on the K230 is DOCUMENTED from the Buildroot
 * defconfig and ASSUMED until hardware confirms it.
 *
 * What is kept is what outlives a run: the engine's own record (best
 * score, tallest tower, longest streak, runs, lifetime pulls), the most
 * blocks pulled in one run, how many runs fell and how many stood at the
 * summit, and which cause felled the ones that fell. Nothing here describes
 * a run in progress: the tower, the run, the generator and the replay log
 * are deliberately session-only, and a run interrupted is simply lost.
 *
 * Persistence never blocks play. Every call reports failure and does
 * nothing else; the app then runs session-only. Writes are atomic (temp
 * file, fsync, rename), so a power loss leaves either the previous record
 * or the new one, never a half-written file. A file that is absent, short,
 * long, foreign, from another version, damaged or impossible is refused
 * and left where it is; the app starts from nothing and the next finished
 * run replaces it.
 *
 * The record holds no secrets: a dozen numbers about a game.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_STORE_H
#define POCKETTIMBER_STORE_H

#include "engine/timber_score.h"
#include "engine/timber_types.h"

#include <stddef.h>
#include <stdint.h>

#define TIMBER_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define TIMBER_STORE_SUBDIR "timber"
#define TIMBER_STORE_FILE "record.v1"

#define TIMBER_RECORDS_MAGIC0 'P'
#define TIMBER_RECORDS_MAGIC1 'T'
#define TIMBER_RECORDS_MAGIC2 'R'
#define TIMBER_RECORDS_MAGIC3 '1'
#define TIMBER_RECORDS_VERSION 1
/* One counter per way a tower can fall: TIP, JOLT, PLACEMENT, SWAY. */
#define TIMBER_RECORDS_CAUSES (TIMBER_CAUSE_COUNT - 1)
/* magic + version + best score + best height + best streak + best pulls +
 * runs + pulls + collapses + summits + the cause counters + checksum */
#define TIMBER_RECORDS_SIZE (4 + 2 + 4 + 2 + 2 + 2 + 4 + 4 + 4 + 4 + 4 * TIMBER_RECORDS_CAUSES + 4)

struct timber_records {
    struct timber_record core;      /* the engine's record: best score, height, streak, runs, pulls */
    uint16_t best_pulls;            /* the most blocks pulled free in one run */
    uint32_t collapses;             /* runs that ended with the tower down */
    uint32_t summits;               /* runs that ended standing, every block on top */
    uint32_t causes[TIMBER_RECORDS_CAUSES]; /* collapses by cause, TIMBER_CAUSE_TIP first */
};

void timber_records_init(struct timber_records *records);
/* Fold a finished run into the records: the engine folds its own part,
 * this counts the outcome. cause is what timber_run_cause() said when the
 * run was over: TIMBER_CAUSE_NONE means it ended standing. Returns 1 when
 * the run set a new best score, the one thing the app announces. */
int timber_records_note_run(struct timber_records *records, const struct timber_score *score,
                            int cause);
/* The most points a run could possibly hold, from the engine's own scoring
 * at every cap; a stored score above it was never earned. */
uint32_t timber_records_score_ceiling(void);

/* Directory and full path of the record file. The pointers stay valid
 * until the next call. */
const char *timber_store_dir(void);
const char *timber_store_path(void);

/* Encode into buf. Returns the number of bytes written, or -1 when buf is
 * too small. Pure encoding, no I/O, so the format is tested without a
 * filesystem. Fields go out one at a time in little-endian order rather
 * than by copying the struct, so the file does not depend on padding,
 * alignment or the host's byte order. */
int timber_records_encode(const struct timber_records *records, uint8_t *buf, size_t n);
/* Decode into records. Returns 0 on success. Returns -1 and leaves records
 * untouched when the blob is not a record, is from another version, is
 * damaged, or describes a lifetime the game could not have produced. The
 * FNV-1a checksum catches accidental corruption and is not a cryptographic
 * digest; it is the invariant checks that keep an impossible record out of
 * the game. */
int timber_records_decode(struct timber_records *records, const uint8_t *buf, size_t n);

/* Write the records. Returns 0, or -1 when they could not be stored (the
 * caller carries on without persistence). */
int timber_store_save(const struct timber_records *records);
/* Read the records. Returns 0 on success, 1 when there is no file, and -1
 * when one exists but is unreadable or unusable. records is untouched
 * unless 0 is returned, so the caller initialises it first and simply
 * keeps the empty one on 1 or -1. A refused file is left in place. */
int timber_store_load(struct timber_records *records);
/* Remove the records. Returns 0 when there is none afterwards, -1
 * otherwise. */
int timber_store_clear(void);

#endif
