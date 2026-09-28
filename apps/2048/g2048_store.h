/*
 * PG 2048 save file: the game in progress and the best score.
 *
 * This is the only part of PG 2048 that touches the filesystem. The engine
 * under apps/2048/engine does no I/O at all (tests/g2048_lint.sh), which is
 * what lets all of it be unit-tested natively.
 *
 * Location: $POCKETOS_STATE_DIR/2048/game.v1, default
 * /var/lib/pocketos/2048/game.v1. App-owned storage in the pattern PocketFleet
 * established (docs/apps/POCKETFLEET.md, D2) and PocketRadar and PocketTimber
 * follow. PocketFleet's approval covers PocketFleet only; this app records the
 * same arrangement as a deviation pending the owner (docs/apps/PG2048.md).
 *
 * A game of 2048 can last an hour, so unlike PocketRadar it is resumed: the
 * whole game is kept - sixteen cells, score, best, move count, the generator
 * state and three flags - so a resumed game continues exactly as it would
 * have, new tiles included. 43 bytes, little-endian, one field at a time.
 *
 *   0  magic "PG48"        4
 *   4  version (1)          2
 *   6  cells, exponents    16
 *  22  score                4
 *  26  best                 4
 *  30  moves                4
 *  34  generator state      4
 *  38  flags                1   bit 0 won, bit 1 keep going, bit 2 over
 *  39  FNV-1a of 0..38      4
 *
 * Persistence never blocks play. A save that fails is reported and the game
 * goes on; a file that is missing starts a new game; a file that is damaged,
 * from another version or describes a game the rules could not have produced
 * is refused, left where it is, and replaced by the next save. Writes are
 * atomic (temp file, fsync, rename).
 *
 * Nothing in it is secret: numbers about a game.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PG2048_STORE_H
#define PG2048_STORE_H

#include "engine/g2048_rules.h"

#include <stddef.h>

#define G2048_STORE_DEFAULT_DIR "/var/lib/pocketos"
#define G2048_STORE_SUBDIR "2048"
#define G2048_STORE_FILE "game.v1"

#define G2048_SAVE_VERSION 1
#define G2048_SAVE_SIZE 43

/* Directory and full path of the save file. Valid until the next call. */
const char *g2048_store_dir(void);
const char *g2048_store_path(void);

/* Encode a game. Returns the number of bytes written (G2048_SAVE_SIZE), or
 * -1 when buf is too small or the game is not valid. Pure, no I/O. */
int g2048_save_encode(const struct g2048_game *g, uint8_t *buf, size_t n);
/* Decode into g. Returns 0, or -1 leaving g untouched when the blob is not
 * a save of this version, is damaged, or fails g2048_game_valid(). The
 * checksum catches accidents, not intent; the validity check is what keeps
 * an impossible game out. */
int g2048_save_decode(struct g2048_game *g, const uint8_t *buf, size_t n);

/* Write the game. 0, or -1 when it could not be stored. */
int g2048_store_save(const struct g2048_game *g);
/* Read the game: 0 on success, 1 when there is no file, -1 when there is one
 * that cannot be used. g is untouched unless 0 is returned. */
int g2048_store_load(struct g2048_game *g);

#endif
