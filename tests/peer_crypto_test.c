/*
 * peerd cryptography (services/peerd/peer_crypto.h, ADR-017 sections 2-5).
 *
 * Known answers first, from sources outside this code:
 *   - the Ed25519 -> X25519 conversion: libsodium's own test vector
 *     (test/default/ed25519_convert);
 *   - XChaCha20-Poly1305: the vector of draft-irtf-cfrg-xchacha, section
 *     A.3.1, as libsodium's test carries it;
 *   - Ed25519: RFC 8032, section 7.1, test 1;
 *   - keys that must be refused: the five small-order points, one point of
 *     mixed order (a valid key plus a point of order 8) and one encoding
 *     that is not a point at all, computed with independent arithmetic
 *     (docs/api/peer.md, "Test vectors").
 * Then peerd's own constructions, held to vectors recorded from this
 * implementation (so a change in what goes on the wire cannot pass
 * unnoticed), and the properties ADR-017 claims of them.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_crypto.h"
#include "peer_wire.h"

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void unhex(const char *hex, uint8_t *out, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned v = 0;

        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static int is_hex(const uint8_t *got, size_t n, const char *want)
{
    char text[2 * 256 + 1];
    size_t i;

    for (i = 0; i < n && i < 256; i++) {
        snprintf(text + 2 * i, 3, "%02x", got[i]);
    }
    if (strcmp(text, want) != 0) {
        printf("     got  %s\n     want %s\n", text, want);
        return 0;
    }
    return 1;
}

/* An identity from a 32-byte seed, as peer_store reads one back. */
static int from_seed(struct pc_identity *id, const char *seed_hex)
{
    uint8_t seed[32];
    uint8_t pk[32];
    uint8_t sk[64];

    unhex(seed_hex, seed, 32);
    crypto_sign_seed_keypair(pk, sk, seed);
    return pc_identity_load(id, sk);
}

#define SEED_A "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60" /* RFC 8032 test 1 */
#define SEED_B "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb" /* RFC 8032 test 2 */
#define SEED_C "421151a459faeade3d247115f94aedae42318124095afabe4d1451a559faedee" /* libsodium convert */

/* ---- published vectors -------------------------------------------------------- */

static void test_published(void)
{
    struct pc_identity id;
    uint8_t pk[32];
    uint8_t sk[64];
    uint8_t seed[32];
    uint8_t sig[64];

    check("published: an identity loads from libsodium's conversion seed", from_seed(&id, SEED_C) == 0);
    check("published: its X25519 public key is libsodium's vector",
          is_hex(id.x_pk, 32, "f1814f0e8ff1043d8a44d25babff3cedcae6c22c3edaa48f857ae70de2baae50"));
    check("published: and its X25519 secret key",
          is_hex(id.x_sk, 32, "8052030376d47112be7f73ed7a019293dd12ad910b654455798b4667d73de166"));
    pc_identity_wipe(&id);

    unhex(SEED_A, seed, 32);
    crypto_sign_seed_keypair(pk, sk, seed);
    check("published: RFC 8032 test 1, the public key",
          is_hex(pk, 32, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    crypto_sign_detached(sig, NULL, (const uint8_t *)"", 0, sk);
    check("published: RFC 8032 test 1, the signature of the empty message",
          is_hex(sig, 64,
                 "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                 "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"));

    {
        /* draft-irtf-cfrg-xchacha-03, A.3.1 */
        static const char *msg = "Ladies and Gentlemen of the class of '99: If I could offer you "
                                 "only one tip for the future, sunscreen would be it.";
        static const uint8_t want[130] = {
            0xf8,0xeb,0xea,0x48,0x75,0x04,0x40,0x66,0xfc,0x16,0x2a,0x06,0x04,0xe1,0x71,0xfe,
            0xec,0xfb,0x3d,0x20,0x42,0x52,0x48,0x56,0x3b,0xcf,0xd5,0xa1,0x55,0xdc,0xc4,0x7b,
            0xbd,0xa7,0x0b,0x86,0xe5,0xab,0x9b,0x55,0x00,0x2b,0xd1,0x27,0x4c,0x02,0xdb,0x35,
            0x32,0x1a,0xcd,0x7a,0xf8,0xb2,0xe2,0xd2,0x50,0x15,0xe1,0x36,0xb7,0x67,0x94,0x58,
            0xe9,0xf4,0x32,0x43,0xbf,0x71,0x9d,0x63,0x9b,0xad,0xb5,0xfe,0xac,0x03,0xf8,0x0a,
            0x19,0xa9,0x6e,0xf1,0x0c,0xb1,0xd1,0x53,0x33,0xa8,0x37,0xb9,0x09,0x46,0xba,0x38,
            0x54,0xee,0x74,0xda,0x3f,0x25,0x85,0xef,0xc7,0xe1,0xe1,0x70,0xe1,0x7e,0x15,0xe5,
            0x63,0xe7,0x76,0x01,0xf4,0xf8,0x5c,0xaf,0xa8,0xe5,0x87,0x76,0x14,0xe1,0x43,0xe6,
            0x84,0x20,
        };
        uint8_t key[32];
        uint8_t nonce[24];
        uint8_t ad[12];
        uint8_t ct[130];
        uint8_t pt[114];
        int i;

        for (i = 0; i < 32; i++) {
            key[i] = (uint8_t)(0x80 + i);
        }
        unhex("07000000404142434445464748494a4b4c4d4e4f50515253", nonce, 24);
        unhex("50515253c0c1c2c3c4c5c6c7", ad, 12);
        check("published: XChaCha20-Poly1305 seals the draft's message to the draft's bytes",
              strlen(msg) == 114 &&
              pc_seal_with_nonce(key, ad, sizeof(ad), (const uint8_t *)msg, 114, nonce, ct) == 0 &&
              memcmp(ct, want, sizeof(want)) == 0);
        check("published: and opens them again",
              pc_open(key, ad, sizeof(ad), nonce, want, sizeof(want), pt) == 0 &&
              memcmp(pt, msg, 114) == 0);
    }
}

/* ---- keys that must never become a node ------------------------------------------ */

static void test_refused_keys(void)
{
    static const struct {
        const char *what;
        const char *hex;
    } bad[] = {
        { "the identity point (order 1)", "0100000000000000000000000000000000000000000000000000000000000000" },
        { "the point of order 2", "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f" },
        { "a point of order 4", "0000000000000000000000000000000000000000000000000000000000000000" },
        { "a point of order 8", "26e8958fc2b227b045c3f489f2ef98f0d5dfac05d3c63339b13802886d53fc05" },
        { "the other point of order 8", "c7176a703d4dd84fba3c0b760d10670f2a2053fa2c39ccc64ec7fd7792ac037a" },
        { "a point of mixed order (a valid key plus one of order 8)",
          "3b5b475c4b82dd1572799fc546f4c6c03e478c6654aa4c7f945b347ea32af60d" },
        { "bytes that are not a point", "0200000000000000000000000000000000000000000000000000000000000000" },
    };
    struct pc_identity id;
    struct pc_session s;
    uint8_t key[32];
    char label[160];
    size_t i;

    from_seed(&id, SEED_A);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        unhex(bad[i].hex, key, 32);
        snprintf(label, sizeof(label), "refused: %s is not a peer, and no session is made with it",
                 bad[i].what);
        check(label, pc_peer_ok(&id, key) == 0 && pc_session(&id, key, &s) == -1);
    }
    check("refused: our own key is not a peer", pc_peer_ok(&id, id.pk) == 0 &&
          pc_session(&id, id.pk, &s) == -1);
    unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", key, 32); /* RFC 8032 test 2 */
    check("refused: (a genuine key is accepted)", pc_peer_ok(&id, key) == 1 && pc_session(&id, key, &s) == 0);
    pc_identity_wipe(&id);
}

/* ---- identities ------------------------------------------------------------------- */

static void test_identity(void)
{
    struct pc_identity a;
    struct pc_identity b;
    uint8_t sk[64];
    uint8_t sk2[64];

    check("identity: one is generated", pc_identity_generate(&a) == 0);
    check("identity: and another is a different one", pc_identity_generate(&b) == 0 &&
          memcmp(a.pk, b.pk, 32) != 0);
    pc_identity_secret(&a, sk);
    pc_identity_wipe(&b);
    check("identity: its secret loads back as the same identity",
          pc_identity_load(&b, sk) == 0 && memcmp(a.pk, b.pk, 32) == 0 &&
          memcmp(a.x_pk, b.x_pk, 32) == 0 && memcmp(a.x_sk, b.x_sk, 32) == 0);
    pc_identity_wipe(&b);
    memcpy(sk2, sk, 64);
    sk2[40] ^= 1;
    check("identity: a secret whose public half was altered is refused", pc_identity_load(&b, sk2) == -1);
    memcpy(sk2, sk, 64);
    sk2[3] ^= 1;
    check("identity: a secret whose seed was altered is refused", pc_identity_load(&b, sk2) == -1);
    check("identity: nothing to load is refused", pc_identity_load(&b, NULL) == -1);
    pc_identity_wipe(&a);
    check("identity: wiped, it holds nothing", a.sk == NULL && a.x_sk == NULL && a.pk[0] == 0);
    pc_identity_wipe(&a);
    pc_identity_wipe(NULL);
    check("identity: wiping twice, or nothing, is harmless", 1);
}

/* ---- session keys ------------------------------------------------------------------- */

static void test_sessions(void)
{
    struct pc_identity a;
    struct pc_identity b;
    struct pc_identity c;
    struct pc_session ab;
    struct pc_session ba;
    struct pc_session ac;
    struct pc_session ab2;

    from_seed(&a, SEED_A);
    from_seed(&b, SEED_B);
    from_seed(&c, SEED_C);
    check("session: both ends make one", pc_session(&a, b.pk, &ab) == 0 && pc_session(&b, a.pk, &ba) == 0);
    check("session: what one sends with, the other receives with",
          memcmp(ab.tx, ba.rx, 32) == 0 && memcmp(ab.rx, ba.tx, 32) == 0);
    check("session: the two directions have different keys", memcmp(ab.tx, ab.rx, 32) != 0);
    check("session: another peer, other keys",
          pc_session(&a, c.pk, &ac) == 0 && memcmp(ac.tx, ab.tx, 32) != 0 &&
          memcmp(ac.rx, ab.rx, 32) != 0 && memcmp(ac.tx, ab.rx, 32) != 0);
    check("session: made again it is the same (nothing to re-establish after a restart)",
          pc_session(&a, b.pk, &ab2) == 0 && memcmp(&ab, &ab2, sizeof(ab)) == 0);
    /* Recorded from this implementation (libsodium 1.0.18): identities from
     * the seeds of RFC 8032 tests 1 and 2. */
    check("session: the recorded keys for the two RFC 8032 identities, one way",
          is_hex(ab.tx, 32, "8d41cd3903dee578f316749d53389ac8cf5bb66268e8e8db0bcce138cff74a95"));
    check("session: and the other", is_hex(ab.rx, 32, "c88586864dae1405676d55f629332837d17c9ed3060d8f6d80cf20611014103b"));
    pc_session_wipe(&ab);
    check("session: wiped, it holds nothing", ab.rx[0] == 0 && ab.tx[31] == 0);
    pc_identity_wipe(&a);
    pc_identity_wipe(&b);
    pc_identity_wipe(&c);
}

/* ---- sealing -------------------------------------------------------------------------- */

static void test_sealing(void)
{
    struct pc_identity a;
    struct pc_identity b;
    struct pc_session ab;
    struct pc_session ba;
    uint8_t ad[PW_HEADER];
    uint8_t nonce[PC_NONCE];
    uint8_t nonce2[PC_NONCE];
    uint8_t ct[64 + PC_TAG];
    uint8_t ct2[64 + PC_TAG];
    uint8_t pt[64];
    uint8_t back[64];
    size_t i;
    int bad;

    from_seed(&a, SEED_A);
    from_seed(&b, SEED_B);
    pc_session(&a, b.pk, &ab);
    pc_session(&b, a.pk, &ba);
    for (i = 0; i < sizeof(ad); i++) {
        ad[i] = (uint8_t)i;
    }
    for (i = 0; i < sizeof(pt); i++) {
        pt[i] = (uint8_t)(200 - i);
    }
    check("seal: sealed by one end", pc_seal(ab.tx, ad, sizeof(ad), pt, sizeof(pt), nonce, ct) == 0);
    check("seal: opened by the other", pc_open(ba.rx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == 0 &&
          memcmp(back, pt, sizeof(pt)) == 0);
    check("seal: the ciphertext is not the plaintext", memcmp(ct, pt, sizeof(pt)) != 0);
    check("seal: sealed again, the nonce and the bytes are different",
          pc_seal(ab.tx, ad, sizeof(ad), pt, sizeof(pt), nonce2, ct2) == 0 &&
          memcmp(nonce, nonce2, PC_NONCE) != 0 && memcmp(ct, ct2, sizeof(ct)) != 0);
    /* Reflection and the wrong direction: the sender cannot open its own
     * datagram, and it does not open as if it had travelled the other way. */
    check("seal: its sender cannot open it (no reflection)",
          pc_open(ab.rx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == -1);
    check("seal: nor does it open with the receiver's sending key",
          pc_open(ba.tx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == -1);

    bad = 0;
    for (i = 0; i < sizeof(ad); i++) {
        ad[i] ^= 0x01;
        bad += pc_open(ba.rx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == 0;
        ad[i] ^= 0x01;
    }
    check("seal: any one of the 58 header bytes altered, and it does not open", bad == 0);
    bad = 0;
    for (i = 0; i < PC_NONCE; i++) {
        nonce[i] ^= 0x80;
        bad += pc_open(ba.rx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == 0;
        nonce[i] ^= 0x80;
    }
    check("seal: any nonce byte altered, and it does not open", bad == 0);
    bad = 0;
    for (i = 0; i < sizeof(ct); i++) {
        ct[i] ^= 0x10;
        bad += pc_open(ba.rx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == 0;
        ct[i] ^= 0x10;
    }
    check("seal: any byte of the ciphertext or the tag altered, and it does not open", bad == 0);
    check("seal: cut short it does not open",
          pc_open(ba.rx, ad, sizeof(ad), nonce, ct, sizeof(ct) - 1, back) == -1 &&
          pc_open(ba.rx, ad, sizeof(ad), nonce, ct, PC_TAG - 1, back) == -1);
    check("seal: with less associated data it does not open",
          pc_open(ba.rx, ad, sizeof(ad) - 1, nonce, ct, sizeof(ct), back) == -1);
    check("seal: (and untouched it still opens)",
          pc_open(ba.rx, ad, sizeof(ad), nonce, ct, sizeof(ct), back) == 0);
    {
        /* Recorded from this implementation: the first 16 header bytes
         * 00..0f as associated data, "peerd" as plaintext, a nonce of 24
         * bytes 0xA5, under the recorded session key above. */
        uint8_t n[PC_NONCE];
        uint8_t out[5 + PC_TAG];

        memset(n, 0xA5, sizeof(n));
        check("seal: the recorded ciphertext for a fixed key, nonce, header and text",
              pc_seal_with_nonce(ab.tx, ad, 16, (const uint8_t *)"peerd", 5, n, out) == 0 &&
              is_hex(out, sizeof(out), "873a98fff7e0a4fd0386ea205db4353f10937081e5"));
    }
    pc_identity_wipe(&a);
    pc_identity_wipe(&b);
}

/* ---- signed frames ---------------------------------------------------------------------- */

static void test_signing(void)
{
    struct pc_identity a;
    struct pc_identity b;
    uint8_t body[PW_SIGNED_BODY];
    uint8_t sig[PC_SIG];
    uint8_t raw[PC_SIG];
    size_t i;
    int bad;

    from_seed(&a, SEED_A);
    from_seed(&b, SEED_B);
    for (i = 0; i < sizeof(body); i++) {
        body[i] = (uint8_t)(i * 3 + 1);
    }
    check("sign: a frame is signed", pc_sign(&a, body, sizeof(body), sig) == 0);
    check("sign: and verifies under its signer's key", pc_verify(a.pk, body, sizeof(body), sig) == 0);
    check("sign: not under another key", pc_verify(b.pk, body, sizeof(body), sig) == -1);
    bad = 0;
    for (i = 0; i < sizeof(body); i++) {
        body[i] ^= 0x04;
        bad += pc_verify(a.pk, body, sizeof(body), sig) == 0;
        body[i] ^= 0x04;
    }
    check("sign: any one of the 146 signed bytes altered, and it does not verify", bad == 0);
    bad = 0;
    for (i = 0; i < sizeof(sig); i++) {
        sig[i] ^= 0x01;
        bad += pc_verify(a.pk, body, sizeof(body), sig) == 0;
        sig[i] ^= 0x01;
    }
    check("sign: any signature byte altered, and it does not verify", bad == 0);
    check("sign: a shorter body does not verify", pc_verify(a.pk, body, sizeof(body) - 1, sig) == -1);
    /* The prefix: a peerd signature is not a signature of the bare bytes,
     * and a signature of the bare bytes is not a peerd signature. */
    crypto_sign_detached(raw, NULL, body, sizeof(body), a.sk);
    check("sign: a signature of the same bytes without the peerd prefix is not accepted",
          pc_verify(a.pk, body, sizeof(body), raw) == -1);
    check("sign: and a peerd signature is not one of the bare bytes",
          crypto_sign_verify_detached(sig, body, sizeof(body), a.pk) != 0);
    check("sign: Ed25519 is deterministic - signed again, the same signature", ({
              uint8_t again[PC_SIG];

              pc_sign(&a, body, sizeof(body), again) == 0 && memcmp(again, sig, PC_SIG) == 0;
          }));
    check("sign: the recorded signature for RFC 8032's first identity over a fixed body",
          is_hex(sig, PC_SIG, "884c75376c5d7cc9751005bbe32b9347e1af9e490146484ed7811311232a0bfa"
                 "5a96eb66e40985f531ff8e62b0e25e760d6775efc2c7c1ac5ca6ba6476ef7306"));
    check("sign: nothing, or too much, is not signed",
          pc_sign(&a, body, 0, sig) == -1 && pc_sign(&a, NULL, 10, sig) == -1 &&
          pc_sign(&a, body, 257, sig) == -1 && pc_verify(a.pk, body, 0, sig) == -1);
    {
        /* A signature under a small-order key: Ed25519 has signatures that
         * verify for such keys; libsodium refuses the key itself. */
        uint8_t small[32];
        uint8_t zero_sig[PC_SIG];

        unhex("0100000000000000000000000000000000000000000000000000000000000000", small, 32);
        memset(zero_sig, 0, sizeof(zero_sig));
        zero_sig[0] = 1;
        check("sign: nothing verifies under the identity point",
              pc_verify(small, body, sizeof(body), zero_sig) == -1 &&
              pc_verify(small, body, sizeof(body), sig) == -1);
    }
    pc_identity_wipe(&a);
    pc_identity_wipe(&b);
}

static void test_random(void)
{
    uint8_t a[32];
    uint8_t b[32];

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    pc_random(a, sizeof(a));
    pc_random(b, sizeof(b));
    check("random: two draws differ, and neither is all zero",
          memcmp(a, b, 32) != 0 && sodium_is_zero(a, 32) == 0 && sodium_is_zero(b, 32) == 0);
    pc_wipe(a, sizeof(a));
    check("random: wiped is zero", sodium_is_zero(a, 32) == 1);
}

int main(void)
{
    if (pc_init() != 0) {
        printf("FAIL libsodium cannot start\n");
        return 1;
    }
    printf("     libsodium %s%s\n", sodium_version_string(),
           sodium_library_minimal() ? " (minimal build)" : " (full build)");
    test_published();
    test_refused_keys();
    test_identity();
    test_sessions();
    test_sealing();
    test_signing();
    test_random();
    printf("peer_crypto_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
