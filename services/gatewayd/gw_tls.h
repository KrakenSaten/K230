/*
 * gatewayd: TLS (ADR-019 rules 10 to 17; docs/design/companion/PAIRING.md
 * §2). The one file that calls OpenSSL. No primitive is implemented here;
 * this is the configuration, the trust decision and the checking of what
 * the library reports.
 *
 *   version     TLS 1.3 and nothing else; no tickets, no session cache, no
 *               early data, no renegotiation, no compression
 *   identity    ECDSA P-256 on both sides; the certificate is only the
 *               container the protocol needs for a public key
 *   trust       the SHA-256 of the peer's SubjectPublicKeyInfo, computed
 *               here from the curve point (one encoding per key), handed to
 *               the caller's admit function. That function is the whole
 *               decision: no CA, no chain, no date, no name
 *   possession  TLS 1.3's CertificateVerify, which is OpenSSL's and is not
 *               touched
 *
 * There is no way to configure this module into accepting an unpinned key:
 * peer verification is always required, the verification callback fails
 * unless the admit function says yes, and no flag or environment variable
 * is read. tests/gatewayd_lint.sh holds the text of that.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef GATEWAYD_GW_TLS_H
#define GATEWAYD_GW_TLS_H

#include "gw.h"

#include <openssl/ssl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_TLS_IDENTITY_FILE "identity.v1"

enum gw_tls_identity_result {
    GW_TLS_ID_OK = 0,
    GW_TLS_ID_CREATED,
    GW_TLS_ID_FAULT,    /* missing beside existing state, unreadable, or not P-256 */
};

/* The trust decision for one handshake: 1 to admit the peer whose key has
 * this fingerprint on this connection, 0 to refuse. */
typedef int (*gw_tls_admit_fn)(void *user, SSL *ssl, const uint8_t fp[GW_FP_LEN]);

/* Once. 0, or -1 when the library is older than 3.0 or lacks TLS 1.3. */
int gw_tls_init(void);

/* Loads <dir>/identity.v1, or creates it when may_create and it is absent
 * (0600, temp-flush-rename, the directory flushed). *key is the caller's to
 * EVP_PKEY_free(). Never regenerates over an existing file. */
enum gw_tls_identity_result gw_tls_identity(const char *dir, bool may_create, EVP_PKEY **key);

/* The fingerprint of a P-256 public key. 0, or -1 for any other key, a key
 * given by explicit curve parameters, or the point at infinity. */
int gw_tls_fingerprint(EVP_PKEY *key, uint8_t fp[GW_FP_LEN]);

/* A server context for this key. NULL on any failure; nothing half-set is
 * returned. `admit` is called once per handshake that presents a single
 * P-256 certificate. */
SSL_CTX *gw_tls_ctx(EVP_PKEY *key, gw_tls_admit_fn admit, void *user);

/* After SSL_accept() returned 1: is this connection what rule 10 and 11
 * require (TLS 1.3, not resumed, a peer certificate whose key is `fp`)?
 * The verification callback already decided; this refuses to rely on it
 * alone. */
bool gw_tls_established_ok(SSL *ssl, const uint8_t fp[GW_FP_LEN]);

/* Random bytes from the library's generator. 0, or -1. */
int gw_tls_random(uint8_t *out, size_t n);

#endif
