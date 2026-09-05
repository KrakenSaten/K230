/*
 * PocketFleet save codec: a match to and from a self-describing byte blob.
 *
 * Pure encoding, no I/O (that is fleet_store.c), so it is unit-tested
 * natively. The layout is written field by field in little-endian order
 * rather than by copying the struct, so it does not depend on padding,
 * alignment or the host's byte order, and it stays readable when the engine
 * grows.
 *
 * Decoding is defensive. A blob that fails the magic, version, length,
 * checksum or any rules invariant is rejected and the caller's match is left
 * untouched: PocketFleet then starts a new game rather than resuming into a
 * state the rules could never have produced.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_SAVE_H
#define POCKETFLEET_SAVE_H

#include "fleet_rules.h"

#define FLEET_SAVE_MAGIC0 'P'
#define FLEET_SAVE_MAGIC1 'F'
#define FLEET_SAVE_MAGIC2 'S'
#define FLEET_SAVE_MAGIC3 '1'
#define FLEET_SAVE_VERSION 1

/* magic + version + difficulty/phase/winner + turn + seed + two streams */
#define FLEET_SAVE_HEADER_BYTES (4 + 2 + 3 + 2 + 4 + 4 + 4)
#define FLEET_SAVE_BOARD_BYTES (FLEET_CELLS + FLEET_CELLS + FLEET_SHIP_COUNT * 5 + 2)
#define FLEET_SAVE_AI_BYTES (1 + 2 + FLEET_CELLS * 4 + FLEET_SHIP_COUNT + 1)
#define FLEET_SAVE_SIZE (FLEET_SAVE_HEADER_BYTES + \
                         FLEET_SIDE_COUNT * FLEET_SAVE_BOARD_BYTES + \
                         FLEET_SIDE_COUNT * 4 + FLEET_SAVE_AI_BYTES + 4)

/* Bytes an encoded match occupies. Constant for a given save version. */
size_t fleet_save_size(void);
/* Encode into buf. Returns the number of bytes written, or -1 when buf is
 * too small. */
int fleet_save_encode(const struct fleet_game *game, uint8_t *buf, size_t n);
/* Decode into game. Returns 0 on success. Returns -1 and leaves game
 * untouched when the blob is not a save, is from another version, is
 * damaged, or describes a state the rules could not produce. */
int fleet_save_decode(struct fleet_game *game, const uint8_t *buf, size_t n);

#endif
