/*
 * SHA-256 (FIPS 180-4) for the multiplayer board commitment.
 *
 * First-party and deliberately small: the shell links no cryptographic hash
 * and this is the only thing that needs one (ADR-008). It is used to bind a
 * player to a fleet layout before play, not to protect a secret key, so
 * constant-time behaviour is not a requirement here. tests/fleet_sha256_test
 * holds it to the NIST example vectors.
 *
 * Pure C, no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_SHA256_H
#define POCKETFLEET_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define FLEET_SHA256_BYTES 32

struct fleet_sha256 {
    uint32_t h[8];
    uint64_t length;        /* bytes hashed so far */
    uint8_t block[64];
    size_t used;            /* bytes waiting in block */
};

void fleet_sha256_init(struct fleet_sha256 *s);
void fleet_sha256_update(struct fleet_sha256 *s, const void *data, size_t n);
void fleet_sha256_final(struct fleet_sha256 *s, uint8_t out[FLEET_SHA256_BYTES]);
/* One call for a whole buffer. */
void fleet_sha256(const void *data, size_t n, uint8_t out[FLEET_SHA256_BYTES]);

#endif
