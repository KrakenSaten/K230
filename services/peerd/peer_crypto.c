/*
 * peerd cryptography over libsodium (peer_crypto.h, ADR-017).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_crypto.h"

#include <sodium.h>
#include <string.h>

/* What this file assumes of the library, checked where it is compiled. */
_Static_assert(crypto_sign_PUBLICKEYBYTES == PC_KEY, "Ed25519 public key size");
_Static_assert(crypto_sign_SECRETKEYBYTES == PC_SECRET, "Ed25519 secret key size");
_Static_assert(crypto_sign_BYTES == PC_SIG, "Ed25519 signature size");
_Static_assert(crypto_scalarmult_curve25519_BYTES == PC_KEY, "X25519 key size");
_Static_assert(crypto_kx_PUBLICKEYBYTES == PC_KEY && crypto_kx_SECRETKEYBYTES == PC_KEY,
               "crypto_kx key sizes");
_Static_assert(crypto_kx_SESSIONKEYBYTES == PC_SESSION, "crypto_kx session key size");
_Static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES == PC_SESSION, "AEAD key size");
_Static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == PC_NONCE, "AEAD nonce size");
_Static_assert(crypto_aead_xchacha20poly1305_ietf_ABYTES == PC_TAG, "AEAD tag size");

#define SIGN_PREFIX "DOORS-PEERD-1"
#define SIGN_PREFIX_LEN 13
#define SIGN_BODY_MAX 256

int pc_init(void)
{
    return sodium_init() < 0 ? -1 : 0;
}

void pc_random(void *buf, size_t n)
{
    randombytes_buf(buf, n);
}

void pc_wipe(void *buf, size_t n)
{
    sodium_memzero(buf, n);
}

void pc_identity_wipe(struct pc_identity *id)
{
    if (!id) {
        return;
    }
    if (id->sk) {
        sodium_free(id->sk);    /* zeroes before it frees */
    }
    if (id->x_sk) {
        sodium_free(id->x_sk);
    }
    memset(id, 0, sizeof(*id));
}

/* Guarded memory for the two secrets, and the X25519 pair from the Ed25519
 * one. id->pk and id->sk's contents are set by the caller first. */
static int identity_finish(struct pc_identity *id)
{
    if (crypto_sign_ed25519_sk_to_curve25519(id->x_sk, id->sk) != 0 ||
        crypto_sign_ed25519_pk_to_curve25519(id->x_pk, id->pk) != 0) {
        return -1;
    }
    return 0;
}

static int identity_alloc(struct pc_identity *id)
{
    memset(id, 0, sizeof(*id));
    id->sk = sodium_malloc(PC_SECRET);
    id->x_sk = sodium_malloc(PC_KEY);
    if (!id->sk || !id->x_sk) {
        pc_identity_wipe(id);
        return -1;
    }
    return 0;
}

int pc_identity_generate(struct pc_identity *id)
{
    if (!id || identity_alloc(id) != 0) {
        return -1;
    }
    if (crypto_sign_keypair(id->pk, id->sk) != 0 || identity_finish(id) != 0) {
        pc_identity_wipe(id);
        return -1;
    }
    return 0;
}

int pc_identity_load(struct pc_identity *id, const uint8_t sk[PC_SECRET])
{
    uint8_t pk[PC_KEY];
    uint8_t *check;
    int ok;

    if (!id || !sk || identity_alloc(id) != 0) {
        return -1;
    }
    /* The stored key is seed | public. Derive the pair again from the seed
     * and hold the stored public half to it: a file whose two halves do not
     * belong together is not an identity. */
    check = sodium_malloc(PC_SECRET);
    if (!check) {
        pc_identity_wipe(id);
        return -1;
    }
    ok = crypto_sign_seed_keypair(pk, check, sk) == 0 && sodium_memcmp(check, sk, PC_SECRET) == 0;
    sodium_free(check);
    if (!ok) {
        pc_identity_wipe(id);
        return -1;
    }
    memcpy(id->sk, sk, PC_SECRET);
    memcpy(id->pk, pk, PC_KEY);
    if (identity_finish(id) != 0) {
        pc_identity_wipe(id);
        return -1;
    }
    return 0;
}

void pc_identity_secret(const struct pc_identity *id, uint8_t sk[PC_SECRET])
{
    memcpy(sk, id->sk, PC_SECRET);
}

int pc_peer_ok(const struct pc_identity *id, const uint8_t peer[PC_KEY])
{
    uint8_t x[PC_KEY];

    if (!id || !peer || memcmp(id->pk, peer, PC_KEY) == 0) {
        return 0;
    }
    return crypto_sign_ed25519_pk_to_curve25519(x, peer) == 0;
}

int pc_session(const struct pc_identity *id, const uint8_t peer[PC_KEY], struct pc_session *out)
{
    uint8_t peer_x[PC_KEY];
    int order;
    int rc;

    if (!id || !peer || !out) {
        return -1;
    }
    order = memcmp(id->pk, peer, PC_KEY);
    if (order == 0 || crypto_sign_ed25519_pk_to_curve25519(peer_x, peer) != 0) {
        return -1;
    }
    /* The lower identity is the client. Both sides apply the same rule to
     * the same two keys, so they take opposite roles, and client rx is
     * server tx: one key per direction. */
    if (order < 0) {
        rc = crypto_kx_client_session_keys(out->rx, out->tx, id->x_pk, id->x_sk, peer_x);
    } else {
        rc = crypto_kx_server_session_keys(out->rx, out->tx, id->x_pk, id->x_sk, peer_x);
    }
    if (rc != 0) {
        pc_session_wipe(out);
        return -1;
    }
    return 0;
}

void pc_session_wipe(struct pc_session *s)
{
    if (s) {
        sodium_memzero(s, sizeof(*s));
    }
}

int pc_seal_with_nonce(const uint8_t key[PC_SESSION], const uint8_t *ad, size_t adlen,
                       const uint8_t *pt, size_t ptlen, const uint8_t nonce[PC_NONCE], uint8_t *ct)
{
    unsigned long long clen = 0;

    if (crypto_aead_xchacha20poly1305_ietf_encrypt(ct, &clen, pt, ptlen, ad, adlen, NULL, nonce,
                                                   key) != 0 ||
        clen != ptlen + PC_TAG) {
        return -1;
    }
    return 0;
}

int pc_seal(const uint8_t key[PC_SESSION], const uint8_t *ad, size_t adlen, const uint8_t *pt,
            size_t ptlen, uint8_t nonce[PC_NONCE], uint8_t *ct)
{
    randombytes_buf(nonce, PC_NONCE);
    return pc_seal_with_nonce(key, ad, adlen, pt, ptlen, nonce, ct);
}

int pc_open(const uint8_t key[PC_SESSION], const uint8_t *ad, size_t adlen,
            const uint8_t nonce[PC_NONCE], const uint8_t *ct, size_t ctlen, uint8_t *pt)
{
    unsigned long long plen = 0;

    if (ctlen < PC_TAG) {
        return -1;
    }
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(pt, &plen, NULL, ct, ctlen, ad, adlen, nonce,
                                                   key) != 0 ||
        plen != ctlen - PC_TAG) {
        return -1;
    }
    return 0;
}

/* "DOORS-PEERD-1" | body: the prefix keeps a peerd signature from being a
 * signature of anything else this key might ever sign. */
static int prefixed(const uint8_t *body, size_t len, uint8_t *out)
{
    if (!body || len == 0 || len > SIGN_BODY_MAX) {
        return -1;
    }
    memcpy(out, SIGN_PREFIX, SIGN_PREFIX_LEN);
    memcpy(out + SIGN_PREFIX_LEN, body, len);
    return 0;
}

int pc_sign(const struct pc_identity *id, const uint8_t *body, size_t len, uint8_t sig[PC_SIG])
{
    uint8_t msg[SIGN_PREFIX_LEN + SIGN_BODY_MAX];
    unsigned long long siglen = 0;

    if (!id || prefixed(body, len, msg) != 0) {
        return -1;
    }
    if (crypto_sign_detached(sig, &siglen, msg, SIGN_PREFIX_LEN + len, id->sk) != 0 ||
        siglen != PC_SIG) {
        return -1;
    }
    return 0;
}

int pc_verify(const uint8_t pk[PC_KEY], const uint8_t *body, size_t len, const uint8_t sig[PC_SIG])
{
    uint8_t msg[SIGN_PREFIX_LEN + SIGN_BODY_MAX];

    if (!pk || !sig || prefixed(body, len, msg) != 0) {
        return -1;
    }
    return crypto_sign_verify_detached(sig, msg, SIGN_PREFIX_LEN + len, pk) == 0 ? 0 : -1;
}
