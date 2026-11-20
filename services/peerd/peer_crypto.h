/*
 * peerd cryptography (docs/decisions/ADR-017, sections 2-5): the one file
 * that calls libsodium. No primitive is implemented here; this is the choice
 * of functions, the order of their arguments and the checking of every
 * return value.
 *
 *   identity       Ed25519 (crypto_sign); the public key is the identity
 *   key agreement  X25519, both keys converted from the Ed25519 ones
 *   session keys   crypto_kx: one key per direction; the device with the
 *                  bytewise lower identity takes the client role
 *   sealing        XChaCha20-Poly1305 (IETF) with a random 24-byte nonce
 *   signed frames  crypto_sign_detached over "DOORS-PEERD-1" | frame
 *
 * Secrets live in sodium_malloc memory and are wiped when released.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PEERD_CRYPTO_H
#define PEERD_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define PC_KEY 32
#define PC_SECRET 64        /* an Ed25519 secret key as libsodium keeps it: seed | public */
#define PC_SESSION 32
#define PC_NONCE 24
#define PC_TAG 16
#define PC_SIG 64

struct pc_identity {
    uint8_t pk[PC_KEY];         /* Ed25519 public: the identity */
    uint8_t x_pk[PC_KEY];       /* X25519 public, converted */
    uint8_t *sk;                /* PC_SECRET bytes, guarded memory */
    uint8_t *x_sk;              /* PC_KEY bytes, guarded memory */
};

/* The two keys of a pair of devices; rx opens what the peer sealed with its tx. */
struct pc_session {
    uint8_t rx[PC_SESSION];
    uint8_t tx[PC_SESSION];
};

/* Once, before anything else. 0, or -1 when libsodium cannot start. */
int pc_init(void);

/* A new identity from the system's random source. 0, or -1. */
int pc_identity_generate(struct pc_identity *id);
/* An identity from a stored secret key. -1 when the stored public half does
 * not belong to the stored seed, or the key cannot be converted. */
int pc_identity_load(struct pc_identity *id, const uint8_t sk[PC_SECRET]);
/* Copy the secret key out, for storing. */
void pc_identity_secret(const struct pc_identity *id, uint8_t sk[PC_SECRET]);
void pc_identity_wipe(struct pc_identity *id);

/* 1 when a peer's identity can be used at all: it converts to X25519
 * (libsodium refuses small-order and off-subgroup points) and is not ours. */
int pc_peer_ok(const struct pc_identity *id, const uint8_t peer[PC_KEY]);

/* The session keys with a peer. 0, or -1 for a key that cannot be used or a
 * shared secret libsodium refuses. */
int pc_session(const struct pc_identity *id, const uint8_t peer[PC_KEY], struct pc_session *out);
void pc_session_wipe(struct pc_session *s);

/* Seal pt (ptlen bytes) under key with ad as associated data. Writes the
 * nonce (PC_NONCE random bytes) and ptlen + PC_TAG bytes of ciphertext.
 * 0, or -1. */
int pc_seal(const uint8_t key[PC_SESSION], const uint8_t *ad, size_t adlen, const uint8_t *pt,
            size_t ptlen, uint8_t nonce[PC_NONCE], uint8_t *ct);
/* The same with the nonce given: pc_seal() is this after drawing a random
 * nonce. For known-answer tests only - a nonce must never be used twice
 * under one key, and nothing but a fresh random one guarantees that here. */
int pc_seal_with_nonce(const uint8_t key[PC_SESSION], const uint8_t *ad, size_t adlen,
                       const uint8_t *pt, size_t ptlen, const uint8_t nonce[PC_NONCE], uint8_t *ct);
/* Open ct (ctlen bytes, tag included) into pt (ctlen - PC_TAG bytes).
 * 0, or -1 when it does not verify; pt is then not to be used. */
int pc_open(const uint8_t key[PC_SESSION], const uint8_t *ad, size_t adlen,
            const uint8_t nonce[PC_NONCE], const uint8_t *ct, size_t ctlen, uint8_t *pt);

/* Sign body (len bytes) as a peerd frame. 0, or -1. */
int pc_sign(const struct pc_identity *id, const uint8_t *body, size_t len, uint8_t sig[PC_SIG]);
/* 0 when sig is pk's signature of body as a peerd frame, else -1. */
int pc_verify(const uint8_t pk[PC_KEY], const uint8_t *body, size_t len, const uint8_t sig[PC_SIG]);

void pc_random(void *buf, size_t n);
void pc_wipe(void *buf, size_t n);

#endif
