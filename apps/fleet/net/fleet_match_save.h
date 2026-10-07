/*
 * PocketFleet multiplayer save codec: the saved half of struct fleet_match to
 * and from a fixed-size byte blob (match.v1). Pure encoding, no I/O: the file
 * is fleet_store.c's, as save.v1 is.
 *
 * Field by field, little-endian, so it depends on no struct layout. The
 * FNV-1a trailer catches accidental corruption and nothing else; what keeps
 * an impossible match out of the game is fleet_match_restore(), which checks
 * the commitment, replays the log against the own fleet and checks every
 * answer, and refuses a state the protocol could not have produced.
 *
 * A match that has not started (inviting, invited, accepting) is saved as
 * idle: pre-start state is deliberately not persisted (ADR-008, point 10).
 * The tombstones are saved in every phase.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_MATCH_SAVE_H
#define POCKETFLEET_MATCH_SAVE_H

#include "fleet_match.h"

#define FLEET_MATCH_SAVE_VERSION 1
#define FLEET_MATCH_TOMB_BYTES (4 + FLEET_MATCH_PEER_PREFIX + 1 + 1)
#define FLEET_MATCH_SAVE_SIZE (4 + 2 + FLEET_KEY_BYTES + 1 + 1 + 4 + FLEET_KEY_BYTES + \
                               FLEET_MATCH_NAME_MAX + 1 + 1 + FLEET_LAYOUT_BYTES + \
                               FLEET_SALT_BYTES + FLEET_COMMIT_BYTES + 1 + \
                               FLEET_COMMIT_BYTES + 1 + 1 + 2 * FLEET_PROTO_PLY_MAX + 1 + \
                               1 + FLEET_LAYOUT_BYTES + FLEET_SALT_BYTES + 1 + 5 + \
                               FLEET_MATCH_TOMBSTONES * FLEET_MATCH_TOMB_BYTES + 4)

/* Encode. Returns the bytes written (FLEET_MATCH_SAVE_SIZE), or -1. */
int fleet_match_save_encode(const struct fleet_match *m, uint8_t *buf, size_t n);
/* Decode the saved fields into m, which must have been fleet_match_init()ed
 * for this node. Returns 0; -1 for a blob that is not a match.v1 (bad size,
 * magic, version, checksum or field); -2 when it was saved by a different
 * node identity. On failure m is untouched. Call fleet_match_restore() after
 * a 0. */
int fleet_match_save_decode(struct fleet_match *m, const uint8_t *buf, size_t n);

#endif
