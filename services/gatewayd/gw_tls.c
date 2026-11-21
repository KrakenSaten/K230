/*
 * gatewayd: TLS. See gw_tls.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gw_tls.h"

#include "gw_util.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
#error "gatewayd needs OpenSSL 3.0 or later"
#endif

#define IDENTITY_MAX 4096
#define PATH_MAX_ 512

static gw_tls_admit_fn g_admit;
static void *g_admit_user;

int gw_tls_init(void)
{
    if (OpenSSL_version_num() < 0x30000000UL) {
        return -1;
    }
    /* No openssl.cnf: nothing outside this file configures the library. */
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL) != 1) {
        return -1;
    }
    return 0;
}

int gw_tls_random(uint8_t *out, size_t n)
{
    return RAND_bytes(out, (int)n) == 1 ? 0 : -1;
}

/* ---- keys -------------------------------------------------------------- */

/* A P-256 key given by the curve's name. A key that spells the curve out
 * in explicit parameters is refused even when the numbers are P-256's: one
 * key, one description. */
static bool is_p256(EVP_PKEY *key)
{
    char group[32];
    size_t len = 0;
    int explicit_params = 1;

    if (key == NULL || !EVP_PKEY_is_a(key, "EC")) {
        return false;
    }
    if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), &len) != 1 ||
        strcmp(group, "prime256v1") != 0) {
        return false;
    }
    if (EVP_PKEY_get_int_param(key, OSSL_PKEY_PARAM_EC_DECODED_FROM_EXPLICIT_PARAMS, &explicit_params) != 1 ||
        explicit_params != 0) {
        return false;
    }
    return true;
}

int gw_tls_fingerprint(EVP_PKEY *key, uint8_t fp[GW_FP_LEN])
{
    /* SubjectPublicKeyInfo of an uncompressed P-256 point: this prefix,
     * then 04 | X | Y. Built here rather than taken from the certificate's
     * bytes, so a key has one fingerprint however its sender encoded it. */
    static const uint8_t prefix[26] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
                                       0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
                                       0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};
    uint8_t spki[sizeof(prefix) + 65];
    BIGNUM *x = NULL, *y = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    int rc = -1;

    if (!is_p256(key)) {
        return -1;
    }
    pctx = EVP_PKEY_CTX_new_from_pkey(NULL, key, NULL);
    if (pctx == NULL || EVP_PKEY_public_check_quick(pctx) != 1) {
        goto out;
    }
    if (EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_X, &x) != 1 ||
        EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_EC_PUB_Y, &y) != 1) {
        goto out;
    }
    memcpy(spki, prefix, sizeof(prefix));
    spki[sizeof(prefix)] = 0x04;
    if (BN_bn2binpad(x, spki + sizeof(prefix) + 1, 32) != 32 ||
        BN_bn2binpad(y, spki + sizeof(prefix) + 33, 32) != 32) {
        goto out;
    }
    if (SHA256(spki, sizeof(spki), fp) == NULL) {
        goto out;
    }
    rc = 0;
out:
    BN_free(x);
    BN_free(y);
    EVP_PKEY_CTX_free(pctx);
    return rc;
}

/* An encrypted identity file is not one this service wrote; never prompt. */
static int no_passphrase(char *buf, int size, int rwflag, void *u)
{
    (void)buf;
    (void)size;
    (void)rwflag;
    (void)u;
    return -1;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static EVP_PKEY *identity_load(const char *path)
{
    char buf[IDENTITY_MAX];
    struct stat sb;
    EVP_PKEY *key = NULL;
    BIO *bio;
    size_t len = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

    if (fd < 0) {
        return NULL;
    }
    /* A key file others can read is not a private key any more. */
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || (sb.st_mode & 077) != 0 || sb.st_size <= 0 ||
        sb.st_size > IDENTITY_MAX) {
        close(fd);
        return NULL;
    }
    while (len < (size_t)sb.st_size) {
        ssize_t got = read(fd, buf + len, (size_t)sb.st_size - len);

        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        len += (size_t)got;
    }
    close(fd);
    if (len == (size_t)sb.st_size) {
        bio = BIO_new_mem_buf(buf, (int)len);
        if (bio != NULL) {
            key = PEM_read_bio_PrivateKey(bio, NULL, no_passphrase, NULL);
            BIO_free(bio);
        }
    }
    OPENSSL_cleanse(buf, sizeof(buf));
    if (key != NULL && !is_p256(key)) {
        EVP_PKEY_free(key);
        key = NULL;
    }
    return key;
}

static EVP_PKEY *identity_create(const char *dir, const char *path)
{
    char tmp[PATH_MAX_];
    EVP_PKEY *key = EVP_EC_gen("P-256");
    BIO *bio = BIO_new(BIO_s_mem());
    char *pem = NULL;
    long len;
    int fd = -1, dfd, ok = 0;

    if (key == NULL || bio == NULL || !is_p256(key) ||
        snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", dir, GW_TLS_IDENTITY_FILE) >= (int)sizeof(tmp) ||
        PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) != 1) {
        goto out;
    }
    len = BIO_get_mem_data(bio, &pem);
    if (len <= 0) {
        goto out;
    }
    (void)unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || write_all(fd, pem, (size_t)len) != 0 || fsync(fd) != 0) {
        goto out;
    }
    close(fd);
    fd = -1;
    /* link, not rename: an identity that appeared meanwhile is never
     * written over. */
    if (link(tmp, path) != 0) {
        goto out;
    }
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ok = fsync(dfd) == 0;
        close(dfd);
    }
out:
    if (fd >= 0) {
        close(fd);
    }
    (void)unlink(tmp);
    if (bio != NULL) {
        /* The PEM text of the key is in the BIO's buffer. */
        if (BIO_get_mem_data(bio, &pem) > 0) {
            OPENSSL_cleanse(pem, (size_t)BIO_get_mem_data(bio, &pem));
        }
        BIO_free(bio);
    }
    if (!ok) {
        EVP_PKEY_free(key);
        key = NULL;
    }
    return key;
}

enum gw_tls_identity_result gw_tls_identity(const char *dir, bool may_create, EVP_PKEY **key)
{
    char path[PATH_MAX_];
    struct stat sb;

    *key = NULL;
    if (snprintf(path, sizeof(path), "%s/%s", dir, GW_TLS_IDENTITY_FILE) >= (int)sizeof(path)) {
        return GW_TLS_ID_FAULT;
    }
    if (lstat(path, &sb) == 0) {
        *key = identity_load(path);
        return *key != NULL ? GW_TLS_ID_OK : GW_TLS_ID_FAULT;
    }
    if (errno != ENOENT || !may_create) {
        return GW_TLS_ID_FAULT;
    }
    *key = identity_create(dir, path);
    return *key != NULL ? GW_TLS_ID_CREATED : GW_TLS_ID_FAULT;
}

/* ---- the certificate: a container for the key -------------------------- */

static X509 *make_cert(EVP_PKEY *key)
{
    X509 *x = X509_new();
    X509_NAME *name;
    BIGNUM *bn = BN_new();
    uint8_t serial[8];
    int ok = 0;

    if (x == NULL || bn == NULL || gw_tls_random(serial, sizeof(serial)) != 0) {
        goto out;
    }
    serial[0] &= 0x7f; /* positive */
    serial[0] |= 0x01; /* and not zero */
    name = X509_get_subject_name(x);
    /* Fixed dates: nothing on either side reads them, and nothing may start
     * to (PAIRING.md §2). 1970 to "no well-defined expiration". */
    if (BN_bin2bn(serial, sizeof(serial), bn) == NULL ||
        BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x)) == NULL || X509_set_version(x, 2) != 1 ||
        ASN1_TIME_set_string_X509(X509_getm_notBefore(x), "19700101000000Z") != 1 ||
        ASN1_TIME_set_string_X509(X509_getm_notAfter(x), "99991231235959Z") != 1 ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"doors-gateway", -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(x, name) != 1 || X509_set_pubkey(x, key) != 1 ||
        X509_sign(x, key, EVP_sha256()) <= 0) {
        goto out;
    }
    ok = 1;
out:
    BN_free(bn);
    if (!ok) {
        X509_free(x);
        x = NULL;
    }
    return x;
}

/* ---- the trust decision ------------------------------------------------- */
/*
 * This replaces OpenSSL's chain building, and it is stricter than what it
 * replaces. It returns 1 on exactly one path: one certificate was sent, its
 * key is a named-curve P-256 key, and the admit function accepted that
 * key's fingerprint for this connection. Every other path returns 0, which
 * aborts the handshake.
 */
static int verify_peer(X509_STORE_CTX *sctx, void *arg)
{
    SSL *ssl = X509_STORE_CTX_get_ex_data(sctx, SSL_get_ex_data_X509_STORE_CTX_idx());
    X509 *leaf = X509_STORE_CTX_get0_cert(sctx);
    STACK_OF(X509) *sent = X509_STORE_CTX_get0_untrusted(sctx);
    uint8_t fp[GW_FP_LEN];
    int admitted = 0;

    (void)arg;
    X509_STORE_CTX_set_error(sctx, X509_V_ERR_APPLICATION_VERIFICATION);
    if (ssl == NULL || leaf == NULL || g_admit == NULL) {
        return 0;
    }
    /* What the peer sent, its own certificate included: exactly that one. */
    if (sent == NULL || sk_X509_num(sent) != 1) {
        return 0;
    }
    if (gw_tls_fingerprint(X509_get0_pubkey(leaf), fp) != 0) {
        return 0;
    }
    admitted = g_admit(g_admit_user, ssl, fp);
    OPENSSL_cleanse(fp, sizeof(fp));
    if (admitted != 1) {
        return 0;
    }
    X509_STORE_CTX_set_error(sctx, X509_V_OK);
    return 1;
}

SSL_CTX *gw_tls_ctx(EVP_PKEY *key, gw_tls_admit_fn admit, void *user)
{
    const int verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    SSL_CTX *ctx = NULL;
    X509 *cert = NULL;

    if (admit == NULL || !is_p256(key)) {
        return NULL;
    }
    cert = make_cert(key);
    ctx = SSL_CTX_new(TLS_server_method());
    if (cert == NULL || ctx == NULL) {
        goto fail;
    }
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_ciphersuites(
            ctx, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256") != 1 ||
        SSL_CTX_set1_groups_list(ctx, "P-256:X25519") != 1 ||
        SSL_CTX_set1_sigalgs_list(ctx, "ecdsa_secp256r1_sha256") != 1 ||
        SSL_CTX_set_num_tickets(ctx, 0) != 1 || SSL_CTX_set_max_early_data(ctx, 0) != 1 ||
        SSL_CTX_set_recv_max_early_data(ctx, 0) != 1 || SSL_CTX_use_certificate(ctx, cert) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, key) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
        goto fail;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
#if defined(SSL_OP_NO_RX_CERTIFICATE_COMPRESSION) && defined(SSL_OP_NO_TX_CERTIFICATE_COMPRESSION)
    /* OpenSSL 3.2 and later: no decompressor in front of authentication. */
    SSL_CTX_set_options(ctx, SSL_OP_NO_RX_CERTIFICATE_COMPRESSION | SSL_OP_NO_TX_CERTIFICATE_COMPRESSION);
#endif
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_max_cert_list(ctx, 4096);
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    g_admit = admit;
    g_admit_user = user;
    SSL_CTX_set_verify(ctx, verify_mode, NULL);
    SSL_CTX_set_verify_depth(ctx, 0);
    SSL_CTX_set_cert_verify_callback(ctx, verify_peer, NULL);
    /* Read back what was set. A library that silently kept another value is
     * a library this service does not run on. */
    if (SSL_CTX_get_min_proto_version(ctx) != TLS1_3_VERSION ||
        SSL_CTX_get_max_proto_version(ctx) != TLS1_3_VERSION || SSL_CTX_get_verify_mode(ctx) != verify_mode ||
        SSL_CTX_get_num_tickets(ctx) != 0 || (SSL_CTX_get_options(ctx) & SSL_OP_NO_TICKET) == 0 ||
        SSL_CTX_get_session_cache_mode(ctx) != SSL_SESS_CACHE_OFF) {
        goto fail;
    }
    X509_free(cert);
    return ctx;
fail:
    X509_free(cert);
    SSL_CTX_free(ctx);
    return NULL;
}

bool gw_tls_established_ok(SSL *ssl, const uint8_t fp[GW_FP_LEN])
{
    uint8_t now[GW_FP_LEN];
    X509 *peer;
    bool ok = false;

    if (SSL_version(ssl) != TLS1_3_VERSION || SSL_session_reused(ssl) != 0 ||
        SSL_get_verify_result(ssl) != X509_V_OK) {
        return false;
    }
    peer = SSL_get1_peer_certificate(ssl);
    if (peer == NULL) {
        return false;
    }
    if (gw_tls_fingerprint(X509_get0_pubkey(peer), now) == 0) {
        ok = gw_ct_eq(now, fp, GW_FP_LEN) == 1;
    }
    X509_free(peer);
    return ok;
}
