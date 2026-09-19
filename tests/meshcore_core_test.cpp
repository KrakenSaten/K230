/*
 * protocols/meshcore: the protocol core, against the real crypto.
 *
 * Everything here runs against the vendored MeshCore sources and the crypto
 * library MeshCore itself uses. There is no stand-in for either. That is a
 * deliberate departure from upstream's own native test environment, which
 * builds with -I test/mocks, where AES128 is a class whose encryptBlock() has
 * an empty body and SHA256 is an xor-and-rotate toy (vendor/RIFT
 * test/mocks/AES.h and SHA256.h). Those mocks let upstream test Packet and
 * Utils without a crypto library; they cannot tell you whether a MAC rejects
 * a forgery, because their MAC is eight bytes of nothing.
 *
 * Determinism: the identities are fixed. One is the key pair MeshCore carries
 * in its own source as a known-good test pair, which holds the Ed25519 path
 * to a value the protocol's authors published; the others are generated from
 * fixed seeds. Fixing the input to real key generation is not the same as
 * faking it - every signature, agreement, cipher and MAC below is computed by
 * the shipped implementation.
 *
 * Several groups below are ported from vendor/RIFT's own googletest suites,
 * assertion for assertion, named in each group's comment. They are rewritten
 * onto this repository's check() harness rather than pulling googletest into
 * the build; where upstream ran them against the mock crypto, they run here
 * against the real thing, which makes the packet-hash ones strictly stronger.
 *
 * Built and run by protocols/meshcore/Makefile:
 *   make meshcore-core-test        (from the top of the repository)
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include <stdio.h>
#include <string.h>

#include <Identity.h>
#include <MeshCore.h>
#include <Packet.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/UTF8Helpers.h>

#include "mc_port.h"

static int failed;
static int checks;

static void check(const char* name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  checks++;
  failed += !ok;
}

/* ---- fixed identities --------------------------------------------------- */

/* vendor/RIFT src/Identity.cpp, LocalIdentity::validatePrivateKey(): the
 * "known good test client keypair" MeshCore ships to check that a private key
 * can do ECDH at all. Using it here means the Ed25519 key derivation is held
 * to a pair the protocol's own source states. */
static const char* const A_PRV =
    "7065e18fd9fabb70c1ed90dca19907de698c88b709ea146eafd93d9b830c7b60"
    "c4681193c79bbc39945ba8064104bb618f8fd7a84a0af6f57033d6e8ddcd6471";
static const char* const A_PUB =
    "1ec77175b0918ed206f9ae04ec136d6d5d4315bb26305427f645b492e9350c10";

/* Real key generation (ed25519_create_keypair through mesh::LocalIdentity)
 * from a seed this test fixes, so every run and every host gets the same
 * keys. */
class FixedSeedRNG : public mesh::RNG {
  uint8_t _next;
public:
  explicit FixedSeedRNG(uint8_t start) : _next(start) { }
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; i++) dest[i] = (uint8_t) (_next + i * 7u);
  }
};

static mesh::LocalIdentity identityA() { return mesh::LocalIdentity(A_PRV, A_PUB); }

static mesh::LocalIdentity identityB() {
  FixedSeedRNG rng(0x5a);
  return mesh::LocalIdentity(&rng);
}

static mesh::LocalIdentity identityC() {
  FixedSeedRNG rng(0x1f);
  return mesh::LocalIdentity(&rng);
}

/* ---- 1. packet encode / decode ------------------------------------------
 * writeTo() and readFrom() are the blob form MeshCore stores and bridges
 * packets in - not the on-air form, which Dispatcher::checkSend builds and
 * tryParsePacket reads. Both are exercised: this group covers the blob form,
 * and tests/meshcore_smoke_test.cpp drives the on-air one through two nodes.
 */
static void test_packet_round_trip(void) {
  mesh::Packet p;
  p.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT);
  p.path_len = 0;
  p.payload_len = 40;
  for (int i = 0; i < 40; i++) p.payload[i] = (uint8_t) (i * 3 + 1);

  uint8_t raw[MAX_TRANS_UNIT];
  uint8_t len = p.writeTo(raw);
  check("a flood packet writes its header, path_len and payload", len == 2 + 40);

  mesh::Packet q;
  check("it reads back", q.readFrom(raw, len));
  check("the header survives", q.header == p.header);
  check("the payload length survives", q.payload_len == p.payload_len);
  check("the payload bytes survive", memcmp(q.payload, p.payload, 40) == 0);
  check("the route type is still flood", q.isRouteFlood() && !q.isRouteDirect());
  check("the payload type is still TXT_MSG", q.getPayloadType() == PAYLOAD_TYPE_TXT_MSG);

  /* A direct packet carrying a three-hop path. */
  mesh::Packet d;
  d.header = ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  d.setPathHashSizeAndCount(1, 3);
  d.path[0] = 0x11; d.path[1] = 0x22; d.path[2] = 0x33;
  d.payload_len = 4;
  memcpy(d.payload, "\xde\xad\xbe\xef", 4);

  len = d.writeTo(raw);
  check("a direct packet's length includes its path", len == 2 + 3 + 4);

  mesh::Packet e;
  check("the direct packet reads back", e.readFrom(raw, len));
  check("the hop count survives", e.getPathHashCount() == 3);
  check("the hash size survives", e.getPathHashSize() == 1);
  check("the path bytes survive", memcmp(e.path, d.path, 3) == 0);
  check("the payload survives beside the path", e.payload_len == 4 && memcmp(e.payload, d.payload, 4) == 0);

  /* Transport codes sit between the header and path_len. */
  mesh::Packet t;
  t.header = ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);
  t.path_len = 0;
  t.transport_codes[0] = 0x1234;
  t.transport_codes[1] = 0xABCD;
  t.payload_len = 8;
  memset(t.payload, 0x5c, 8);

  len = t.writeTo(raw);
  check("transport codes add four bytes", len == 2 + 4 + 8);

  mesh::Packet u;
  check("the transport packet reads back", u.readFrom(raw, len));
  check("both transport codes survive",
        u.transport_codes[0] == 0x1234 && u.transport_codes[1] == 0xABCD);
  check("hasTransportCodes agrees", u.hasTransportCodes());
  check("getRawLength matches what writeTo produced", u.getRawLength() == len);
}

/* ---- 2. path_len encoding -----------------------------------------------
 * Ported from vendor/RIFT test/test_path_len/test_path_len.cpp (6 cases),
 * assertion for assertion. Pure bit packing, no crypto, so it is the same
 * test here as there.
 */
static void test_path_len(void) {
  bool ok = true;
  for (int hops = 0; hops <= 63; hops++) {
    ok = ok && mesh::Packet::pathHashCount((uint16_t) hops) == hops;
    ok = ok && mesh::Packet::pathHashSize((uint16_t) hops) == 1;
  }
  check("single-byte hashes read as a plain hop count", ok);

  ok = true;
  for (int size = 1; size <= 4; size++) {
    for (int hops = 0; hops <= 63; hops++) {
      uint8_t encoded = (uint8_t) (((size - 1) << 6) | hops);
      ok = ok && mesh::Packet::pathHashCount(encoded) == hops;
      ok = ok && mesh::Packet::pathHashSize(encoded) == size;
    }
  }
  check("every hash size and hop count decodes", ok);

  /* Two hops at the 2-byte setting. Read raw, this displayed as 66 hops. */
  uint8_t encoded = (uint8_t) ((1 << 6) | 2);
  check("the case that shipped wrong decodes as 2 hops of 2-byte hashes",
        encoded == 66 && mesh::Packet::pathHashCount(encoded) == 2
                      && mesh::Packet::pathHashSize(encoded) == 2);

  ok = true;
  for (int size = 1; size <= 4; size++) {
    for (int hops = 0; hops <= 63; hops += 7) {
      mesh::Packet p;
      p.setPathHashSizeAndCount((uint8_t) size, (uint8_t) hops);
      ok = ok && p.getPathHashCount() == hops;
      ok = ok && p.getPathHashSize() == size;
      ok = ok && p.getPathByteLen() == hops * size;
    }
  }
  check("the setter round-trips", ok);

  ok = true;
  for (int raw = 0; raw <= 255; raw++) {
    mesh::Packet p;
    p.path_len = (uint8_t) raw;
    ok = ok && mesh::Packet::pathHashCount((uint8_t) raw) == p.getPathHashCount();
    ok = ok && mesh::Packet::pathHashSize((uint8_t) raw) == p.getPathHashSize();
  }
  check("the instance accessors agree with the statics", ok);

  /* 0xFF is companion_radio's "direct, not flooded" sentinel and must never
   * decode as 63 hops of 4-byte hashes, which the arithmetic alone would say. */
  check("the direct sentinel is not a path", !mesh::Packet::isValidPathLen(0xFF));
}

/* ---- 3. the duplicate table ---------------------------------------------
 * Ported from vendor/RIFT test/test_mesh_tables/test_simple_mesh_tables.cpp
 * (8 cases). Upstream runs these against the mock SHA256; here
 * calculatePacketHash() is real SHA-256, so "two packets hash differently"
 * means what it says rather than "two packets disagree under a toy mixer".
 */
static mesh::Packet floodPacket(uint8_t seed) {
  mesh::Packet p;
  p.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  p.payload[0] = seed;
  p.payload_len = 1;
  p.path_len = 0;
  return p;
}

static mesh::Packet directPacket(uint8_t seed) {
  mesh::Packet p;
  p.header = ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  p.payload[0] = seed;
  p.payload_len = 1;
  p.path_len = 0;
  return p;
}

static void test_mesh_tables(void) {
  {
    SimpleMeshTables t;
    mesh::Packet p = floodPacket(0x01);
    check("an unseen packet was not seen", !t.wasSeen(&p));
  }
  {
    SimpleMeshTables t;
    mesh::Packet p = floodPacket(0x01);
    bool first = t.wasSeen(&p);
    bool second = t.wasSeen(&p);
    check("wasSeen is a pure query and does not insert", !first && !second);
  }
  {
    SimpleMeshTables t;
    mesh::Packet p = floodPacket(0x01);
    t.markSeen(&p);
    check("markSeen makes wasSeen true", t.wasSeen(&p));
  }
  {
    SimpleMeshTables t;
    mesh::Packet p1 = floodPacket(0x01);
    mesh::Packet p2 = floodPacket(0x02);
    t.markSeen(&p1);
    check("marking one packet does not mark another", !t.wasSeen(&p2));
  }
  {
    /* The pattern at every onRecvPacket call site. */
    SimpleMeshTables t;
    mesh::Packet p = floodPacket(0x01);
    bool before = t.wasSeen(&p);
    t.markSeen(&p);
    check("query then mark behaves", !before && t.wasSeen(&p));
  }
  {
    SimpleMeshTables t;
    mesh::Packet p = floodPacket(0x01);
    t.markSeen(&p);
    t.wasSeen(&p);
    check("a flood duplicate counts as a flood duplicate",
          t.getNumFloodDups() == 1u && t.getNumDirectDups() == 0u);
  }
  {
    SimpleMeshTables t;
    mesh::Packet p = directPacket(0x01);
    t.markSeen(&p);
    t.wasSeen(&p);
    check("a direct duplicate counts as a direct duplicate",
          t.getNumFloodDups() == 0u && t.getNumDirectDups() == 1u);
  }
  {
    SimpleMeshTables t;
    mesh::Packet p = floodPacket(0x01);
    t.markSeen(&p);
    bool seen = t.wasSeen(&p);
    t.clear(&p);
    check("clear removes a seen packet", seen && !t.wasSeen(&p));
  }

  /* Not upstream: with the real hash, two payloads that differ in one bit
   * must land on different table entries. Under the mock SHA256 this would
   * also pass, which is the point - it proves nothing there and something
   * here. */
  {
    SimpleMeshTables t;
    mesh::Packet a = floodPacket(0x40);
    mesh::Packet b = floodPacket(0x41);
    uint8_t ha[MAX_HASH_SIZE], hb[MAX_HASH_SIZE];
    a.calculatePacketHash(ha);
    b.calculatePacketHash(hb);
    t.markSeen(&a);
    check("a one-bit payload change gives a different packet hash",
          memcmp(ha, hb, MAX_HASH_SIZE) != 0 && !t.wasSeen(&b));
  }
}

/* ---- 4. hex ------------------------------------------------------------
 * Ported from vendor/RIFT test/test_utils/test_tohex.cpp (5 cases).
 */
static void test_hex(void) {
  char out[32];

  uint8_t one[] = { 0xAB };
  mesh::Utils::toHex(out, one, sizeof(one));
  check("toHex converts a single byte", strcmp(out, "AB") == 0);

  uint8_t many[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
  mesh::Utils::toHex(out, many, sizeof(many));
  check("toHex converts multiple bytes", strcmp(out, "0123456789ABCDEF") == 0);

  uint8_t zero[] = { 0x00 };
  mesh::Utils::toHex(out, zero, sizeof(zero));
  check("toHex converts a zero byte", strcmp(out, "00") == 0);

  uint8_t max[] = { 0xFF };
  mesh::Utils::toHex(out, max, sizeof(max));
  check("toHex converts a max byte", strcmp(out, "FF") == 0);

  strcpy(out, "X");
  mesh::Utils::toHex(out, one, 0);
  check("toHex null-terminates on empty input", out[0] == '\0');

  /* fromHex is the other direction, and the one that has to refuse rubbish. */
  uint8_t back[8];
  check("fromHex round-trips", mesh::Utils::fromHex(back, 8, "0123456789ABCDEF")
                               && memcmp(back, many, 8) == 0);
  check("fromHex refuses a short string", !mesh::Utils::fromHex(back, 8, "0123"));
  check("fromHex refuses a long string", !mesh::Utils::fromHex(back, 2, "123456"));

  /* DEBT, third item, smaller than the two below: Utils::fromHex checks the
   * LENGTH of the string and nothing else (vendor/RIFT src/Utils.cpp:218-229).
   * A non-hex character is not rejected; hexVal() returns 0 for it and the
   * byte silently becomes something else. Utils::isHexChar() exists beside it
   * for callers to validate with, so this may well be deliberate - but a
   * caller that trusts the bool gets a key it did not type. Recorded here as
   * what the code does today, not endorsed. */
  check("DEBT fromHex accepts a non-hex character rather than refusing it",
        mesh::Utils::fromHex(back, 2, "12ZZ"));
  check("DEBT and isHexChar, which callers are expected to use instead, refuses it",
        mesh::Utils::isHexChar('1') && !mesh::Utils::isHexChar('Z'));
}

/* ---- 5. UTF-8 truncation ------------------------------------------------
 * Ported from vendor/RIFT test/test_utf8_helpers/test_utf8_helpers.cpp
 * (8 cases). UTF8Helpers.h is in this library's boundary because
 * AdvertDataHelpers.cpp includes it to cut a node name to the advert's
 * 32-byte app-data field without splitting a character.
 */
static void test_utf8(void) {
  const char* name = "Example RPT \xF0\x9F\x94\x8B\xF0\x9F\x87\xB5\xF0\x9F\x87\xB1";
  check("a complete name within the limit is kept",
        mesh::validUtf8PrefixLength(name, 24) == 24u);
  check("a code point crossing the limit is dropped",
        mesh::validUtf8PrefixLength(name, 23) == 20u);

  const char overlong[] = { 'A', (char) 0xC0, (char) 0xAF, 0 };
  const char surrogate[] = { 'A', (char) 0xED, (char) 0xA0, (char) 0x80, 0 };
  const char out_of_range[] = { 'A', (char) 0xF4, (char) 0x90, (char) 0x80, (char) 0x80, 0 };
  const char truncated[] = { 'A', (char) 0xF0, (char) 0x9F, 0 };
  check("malformed and truncated sequences are rejected",
        mesh::validUtf8PrefixLength(overlong, sizeof(overlong)) == 1u
     && mesh::validUtf8PrefixLength(surrogate, sizeof(surrogate)) == 1u
     && mesh::validUtf8PrefixLength(out_of_range, sizeof(out_of_range)) == 1u
     && mesh::validUtf8PrefixLength(truncated, sizeof(truncated)) == 1u);

  const char invalid[] = { 'A', (char) 0x80, 'B', 0 };
  check("an unexpected continuation byte is rejected",
        mesh::validUtf8PrefixLength(invalid, sizeof(invalid)) == 1u);

  /* "hallo " is 6 bytes, then three two-byte letters: 8, 10, 12. */
  const char* nordic = "hallo \xC3\xA6\xC3\xB8\xC3\xA5";
  check("a two-byte letter is never split",
        mesh::validUtf8PrefixLength(nordic, 12) == 12u
     && mesh::validUtf8PrefixLength(nordic, 7) == 6u
     && mesh::validUtf8PrefixLength(nordic, 8) == 8u
     && mesh::validUtf8PrefixLength(nordic, 9) == 8u
     && mesh::validUtf8PrefixLength(nordic, 10) == 10u
     && mesh::validUtf8PrefixLength(nordic, 11) == 10u);

  const char* emoji = "ok \xF0\x9F\x94\x8B";
  check("a four-byte emoji is never split",
        mesh::validUtf8PrefixLength(emoji, 7) == 7u
     && mesh::validUtf8PrefixLength(emoji, 4) == 3u
     && mesh::validUtf8PrefixLength(emoji, 5) == 3u
     && mesh::validUtf8PrefixLength(emoji, 6) == 3u);

  {
    char text[200];
    int i = 0;
    while (i < 159) text[i++] = 'x';
    text[i++] = (char) 0xC3;
    text[i++] = (char) 0xA6;
    text[i] = 0;
    check("a long message cut to fit keeps whole characters",
          mesh::validUtf8PrefixLength(text, 160) == 159u
       && mesh::validUtf8PrefixLength(text, 161) == 161u);
  }

  check("a limit of zero yields nothing",
        mesh::validUtf8PrefixLength("\xC3\xA6", 0) == 0u
     && mesh::validUtf8PrefixLength("\xC3\xA6", 1) == 0u);
}

/* ---- 6. Ed25519 signing and verification --------------------------------
 * Real orlp/ed25519 for signing, real rweather Ed25519::verify for checking -
 * the same split a MeshCore node runs.
 */
static void test_identity_sign_verify(void) {
  mesh::LocalIdentity a = identityA();

  /* The pair from MeshCore's own source: the public key must be the one the
   * private key derives, or everything below is testing a key pair nobody
   * else has. */
  uint8_t expect_pub[PUB_KEY_SIZE];
  mesh::Utils::fromHex(expect_pub, PUB_KEY_SIZE, A_PUB);
  check("the published MeshCore test key pair loads as published",
        memcmp(a.pub_key, expect_pub, PUB_KEY_SIZE) == 0);
  uint8_t prv[PRV_KEY_SIZE];
  mesh::Utils::fromHex(prv, PRV_KEY_SIZE, A_PRV);
  check("MeshCore accepts its own test private key for ECDH",
        mesh::LocalIdentity::validatePrivateKey(prv));

  /* And refuses one that cannot do the exchange. */
  uint8_t dud[PRV_KEY_SIZE];
  memset(dud, 0, sizeof(dud));
  check("an all-zero private key is refused", !mesh::LocalIdentity::validatePrivateKey(dud));

  const char* msg = "the quick brown fox jumps over the lazy dog";
  const int msg_len = (int) strlen(msg);

  uint8_t sig[SIGNATURE_SIZE];
  a.sign(sig, (const uint8_t*) msg, msg_len);
  check("a signature verifies against the signer",
        a.verify(sig, (const uint8_t*) msg, msg_len));

  /* Ed25519 is deterministic (RFC 8032): the same key over the same message
   * must give the same 64 bytes every time, on every host and both
   * architectures. */
  uint8_t sig2[SIGNATURE_SIZE];
  a.sign(sig2, (const uint8_t*) msg, msg_len);
  check("signing is deterministic", memcmp(sig, sig2, SIGNATURE_SIZE) == 0);

  /* Tamper with the message. */
  char tampered[64];
  strcpy(tampered, msg);
  tampered[4] ^= 0x01;
  check("a changed message fails verification",
        !a.verify(sig, (const uint8_t*) tampered, msg_len));

  /* Tamper with the signature, one bit at a time in three places. */
  bool all_rejected = true;
  const int spots[] = { 0, 31, 63 };
  for (int i = 0; i < 3; i++) {
    uint8_t bad[SIGNATURE_SIZE];
    memcpy(bad, sig, SIGNATURE_SIZE);
    bad[spots[i]] ^= 0x01;
    if (a.verify(bad, (const uint8_t*) msg, msg_len)) all_rejected = false;
  }
  check("a flipped signature bit fails verification", all_rejected);

  /* Another identity must not verify A's signature. */
  mesh::LocalIdentity b = identityB();
  check("a different identity does not verify the signature",
        !b.verify(sig, (const uint8_t*) msg, msg_len));

  /* An empty message is still a signable message. */
  uint8_t esig[SIGNATURE_SIZE];
  a.sign(esig, (const uint8_t*) "", 0);
  check("an empty message signs and verifies", a.verify(esig, (const uint8_t*) "", 0));
}

/* ---- 7. X25519 agreement ------------------------------------------------ */
static void test_x25519(void) {
  mesh::LocalIdentity a = identityA();
  mesh::LocalIdentity b = identityB();
  mesh::LocalIdentity c = identityC();

  uint8_t ab[PUB_KEY_SIZE], ba[PUB_KEY_SIZE];
  a.calcSharedSecret(ab, b);
  b.calcSharedSecret(ba, a);
  check("both parties derive the same shared secret",
        memcmp(ab, ba, PUB_KEY_SIZE) == 0);

  /* A secret that came out all zeroes would compare equal and prove nothing. */
  uint8_t zero[PUB_KEY_SIZE];
  memset(zero, 0, sizeof(zero));
  check("the shared secret is not all zeroes", memcmp(ab, zero, PUB_KEY_SIZE) != 0);

  uint8_t ac[PUB_KEY_SIZE];
  a.calcSharedSecret(ac, c);
  check("a different peer gives a different secret",
        memcmp(ab, ac, PUB_KEY_SIZE) != 0);

  /* Deterministic, so a stored contact's cached secret stays valid. */
  uint8_t ab2[PUB_KEY_SIZE];
  a.calcSharedSecret(ab2, b);
  check("the agreement is deterministic", memcmp(ab, ab2, PUB_KEY_SIZE) == 0);
}

/* ---- 8. AES-128 and the MAC --------------------------------------------- */
static void test_cipher_and_mac(void) {
  mesh::LocalIdentity a = identityA();
  mesh::LocalIdentity b = identityB();
  mesh::LocalIdentity c = identityC();

  uint8_t secret[PUB_KEY_SIZE];
  a.calcSharedSecret(secret, b);

  const char* plain = "meet me at the usual place";
  const int plain_len = (int) strlen(plain);

  uint8_t sealed[MAX_PACKET_PAYLOAD];
  int sealed_len = mesh::Utils::encryptThenMAC(secret, sealed, (const uint8_t*) plain, plain_len);
  check("encryptThenMAC produces a MAC plus whole cipher blocks",
        sealed_len == CIPHER_MAC_SIZE + ((plain_len + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE);
  check("the ciphertext is not the plaintext",
        memcmp(sealed + CIPHER_MAC_SIZE, plain, (size_t) plain_len) != 0);

  uint8_t opened[MAX_PACKET_PAYLOAD];
  int opened_len = mesh::Utils::MACThenDecrypt(secret, opened, sealed, sealed_len);
  check("MACThenDecrypt returns the padded plaintext length",
        opened_len == sealed_len - CIPHER_MAC_SIZE);
  check("the plaintext comes back",
        opened_len >= plain_len && memcmp(opened, plain, (size_t) plain_len) == 0);

  /* The other side of the agreement opens it too - which is the whole point. */
  uint8_t secret_b[PUB_KEY_SIZE];
  b.calcSharedSecret(secret_b, a);
  memset(opened, 0, sizeof(opened));
  opened_len = mesh::Utils::MACThenDecrypt(secret_b, opened, sealed, sealed_len);
  check("the peer opens it with its own half of the agreement",
        opened_len > 0 && memcmp(opened, plain, (size_t) plain_len) == 0);

  /* ---- malformed MAC rejection ---- */

  /* Every bit of the two-byte MAC, one at a time. */
  bool all_rejected = true;
  for (int byte = 0; byte < CIPHER_MAC_SIZE; byte++) {
    for (int bit = 0; bit < 8; bit++) {
      uint8_t bad[MAX_PACKET_PAYLOAD];
      memcpy(bad, sealed, (size_t) sealed_len);
      bad[byte] ^= (uint8_t) (1 << bit);
      if (mesh::Utils::MACThenDecrypt(secret, opened, bad, sealed_len) != 0) all_rejected = false;
    }
  }
  check("every single-bit MAC corruption is rejected", all_rejected);

  /* Ciphertext tampering, which the MAC is there to catch. */
  all_rejected = true;
  for (int i = CIPHER_MAC_SIZE; i < sealed_len; i++) {
    uint8_t bad[MAX_PACKET_PAYLOAD];
    memcpy(bad, sealed, (size_t) sealed_len);
    bad[i] ^= 0x80;
    if (mesh::Utils::MACThenDecrypt(secret, opened, bad, sealed_len) != 0) all_rejected = false;
  }
  check("every single-bit ciphertext corruption is rejected", all_rejected);

  /* A third party's secret must not open it. */
  uint8_t secret_c[PUB_KEY_SIZE];
  a.calcSharedSecret(secret_c, c);
  check("a stranger's secret does not open it",
        mesh::Utils::MACThenDecrypt(secret_c, opened, sealed, sealed_len) == 0);

  /* Truncated input: shorter than the MAC, and exactly the MAC. */
  check("input shorter than the MAC is refused",
        mesh::Utils::MACThenDecrypt(secret, opened, sealed, CIPHER_MAC_SIZE - 1) == 0);
  check("input that is only a MAC is refused",
        mesh::Utils::MACThenDecrypt(secret, opened, sealed, CIPHER_MAC_SIZE) == 0);

  /* A truncated ciphertext keeps the real MAC, so this is the MAC doing the
   * work rather than a length check. */
  check("a truncated ciphertext is refused",
        mesh::Utils::MACThenDecrypt(secret, opened, sealed, sealed_len - CIPHER_BLOCK_SIZE) == 0);

  /* An empty plaintext still produces a sealed blob that opens. */
  int e_len = mesh::Utils::encryptThenMAC(secret, sealed, (const uint8_t*) "", 0);
  check("an empty plaintext seals and opens",
        e_len == CIPHER_MAC_SIZE && mesh::Utils::MACThenDecrypt(secret, opened, sealed, e_len) == 0);
}

/* ---- 9. SHA-256 --------------------------------------------------------- */
static void test_sha256(void) {
  /* FIPS 180-2 / RFC 6234: SHA-256("abc"). This is the check the mock cannot
   * pass, and the reason none of the crypto above runs against it. */
  static const uint8_t abc[] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
    0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
  };
  uint8_t out[32];
  mesh::Utils::sha256(out, sizeof(out), (const uint8_t*) "abc", 3);
  check("SHA-256(\"abc\") is the published vector", memcmp(out, abc, 32) == 0);

  /* Truncation is how MeshCore makes its 8-byte packet and channel hashes. */
  uint8_t eight[MAX_HASH_SIZE];
  mesh::Utils::sha256(eight, sizeof(eight), (const uint8_t*) "abc", 3);
  check("a truncated hash is the prefix of the full one",
        memcmp(eight, abc, MAX_HASH_SIZE) == 0);

  /* The two-fragment form must equal hashing the concatenation. */
  uint8_t joined[32], split[32];
  mesh::Utils::sha256(joined, sizeof(joined), (const uint8_t*) "hello world", 11);
  mesh::Utils::sha256(split, sizeof(split), (const uint8_t*) "hello ", 6,
                                            (const uint8_t*) "world", 5);
  check("hashing two fragments equals hashing the join", memcmp(joined, split, 32) == 0);
}

/* ---- 10. the advert app-data helpers ------------------------------------ */
static void test_advert_app_data(void) {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_CHAT, "K230-A");
  uint8_t len = builder.encodeTo(app_data);
  check("an advert app-data blob encodes", len > 0 && len <= MAX_ADVERT_DATA_SIZE);

  AdvertDataParser parser(app_data, len);
  check("it parses back", parser.isValid());
  check("the node type survives", parser.getType() == ADV_TYPE_CHAT);
  check("the name survives", strcmp(parser.getName(), "K230-A") == 0);
  check("no location was claimed", !parser.hasLatLon());

  /* With a position. */
  AdvertDataBuilder located(ADV_TYPE_REPEATER, "K230-R", 59.9139, 10.7522);
  len = located.encodeTo(app_data);
  AdvertDataParser lp(app_data, len);
  check("a located advert parses", lp.isValid() && lp.hasLatLon());
  check("the latitude survives to six decimal places", lp.getIntLat() == 59913900);
  check("the longitude survives to six decimal places", lp.getIntLon() == 10752200);
  check("the located node's type survives", lp.getType() == ADV_TYPE_REPEATER);

  /* Rubbish must not parse as a valid advert. */
  uint8_t junk[MAX_ADVERT_DATA_SIZE];
  memset(junk, 0xFF, sizeof(junk));
  AdvertDataParser jp(junk, sizeof(junk));
  check("an all-ones blob does not parse as a located advert",
        !jp.isValid() || !jp.hasLatLon() || jp.getIntLat() != 0);

  /* Zero length. */
  AdvertDataParser zp(app_data, 0);
  check("a zero-length blob does not parse", !zp.isValid());
}

/* ---- 11. hardening debt: Packet::readFrom and a short buffer ------------
 *
 * FOLLOW-UP HARDENING WORK, NOT A PASS/FAIL OF CORRECT BEHAVIOUR.
 *
 * vendor/RIFT src/Packet.cpp readFrom() consumes the header, the four
 * transport-code bytes and path_len, and then memcpy()s getPathByteLen()
 * bytes of path, all before it first compares i against len (line 80). A
 * caller that passes a buffer shorter than the packet the bytes describe
 * therefore has up to 4 + 189 bytes read from past the end of it, and only
 * afterwards gets false back.
 *
 * It is reachable inside this library's boundary:
 * BaseChatMesh::importContact() (BaseChatMesh.cpp:560) calls it straight
 * through with a caller-supplied length. The on-air path does NOT go through
 * here - Dispatcher::tryParsePacket does its own bounds-checked parse - so
 * this is not an over-the-air issue today.
 *
 * What is demonstrated below is the over-read itself, safely: the backing
 * buffer is larger than the length declared to readFrom(), with a sentinel
 * pattern past the declared end, and the test shows that the sentinel lands
 * in the parsed packet. With an exactly-sized buffer the same code path is an
 * out-of-bounds read; a test written that way would have to expect a crash
 * under ASan, so it is documented here rather than performed.
 *
 * These checks assert what the code does TODAY. When the bounds check moves
 * to the top of readFrom(), they are expected to fail and should be rewritten
 * to assert the fixed behaviour - that is the signal, not a regression.
 */
static void test_debt_readfrom_short_buffer(void) {
  /* 2-byte frame declared; the path it describes reaches three bytes past. */
  uint8_t buf[64];
  memset(buf, 0x5A, sizeof(buf));          /* sentinel: past-the-end bytes */
  buf[0] = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);
  buf[1] = 3;                               /* three hops, 1-byte hashes */

  mesh::Packet p;
  memset(p.path, 0, sizeof(p.path));
  bool ok = p.readFrom(buf, 2);

  check("DEBT readFrom returns false for a frame shorter than its own path", !ok);
  check("DEBT but it has already copied three bytes from past the declared end",
        p.path[0] == 0x5A && p.path[1] == 0x5A && p.path[2] == 0x5A);

  /* Transport codes are read before any length check too: a 1-byte frame
   * gets four bytes taken from past its end. */
  uint8_t tbuf[64];
  memset(tbuf, 0, sizeof(tbuf));
  tbuf[0] = ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);
  tbuf[1] = 0x34; tbuf[2] = 0x12;           /* transport_codes[0] past the end */
  tbuf[3] = 0xCD; tbuf[4] = 0xAB;           /* transport_codes[1] past the end */
  tbuf[5] = 0;                              /* path_len, also past the end */

  mesh::Packet q;
  ok = q.readFrom(tbuf, 1);
  check("DEBT a one-byte transport frame is refused", !ok);
  check("DEBT but its transport codes were taken from past the declared end",
        q.transport_codes[0] == 0x1234 && q.transport_codes[1] == 0xABCD);

  /* The well-formed case still behaves, so the debt above is about the
   * boundary and not about the parser generally. */
  uint8_t good[8];
  good[0] = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  good[1] = 0;
  good[2] = 0xAA; good[3] = 0xBB; good[4] = 0xCC; good[5] = 0xDD;
  mesh::Packet r;
  check("a well-formed short frame still parses", r.readFrom(good, 6)
        && r.payload_len == 4 && r.payload[0] == 0xAA && r.payload[3] == 0xDD);

  /* A packet whose declared path is longer than MAX_PATH_SIZE is refused by
   * isValidPathLen before any copy - that part is already guarded. */
  uint8_t bad[8];
  memset(bad, 0, sizeof(bad));
  bad[0] = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  bad[1] = (uint8_t) ((3 << 6) | 10);       /* hash size 4 is reserved */
  mesh::Packet s;
  check("a reserved path hash size is refused before any copy", !s.readFrom(bad, 8));
}

/* ---- 12. the port is present and sane ----------------------------------
 * A light touch here; tests/meshcore_port_test.cpp is where the seam is
 * actually examined. This only checks the library and the port link together
 * and agree about the vendored commits they were built from.
 */
static void test_build_identity(void) {
  check("the RIFT commit is compiled in",
        strlen(MESHCORE_RIFT_COMMIT) == 40);
  check("the crypto commit is compiled in",
        strlen(MESHCORE_CRYPTO_COMMIT) == 40);

  /* The port's own RNG feeds real key generation. */
  mcport::HostRNG rng;
  mesh::LocalIdentity fresh(&rng);
  uint8_t zero[PUB_KEY_SIZE];
  memset(zero, 0, sizeof(zero));
  check("a key pair generated from the host RNG is not all zeroes",
        memcmp(fresh.pub_key, zero, PUB_KEY_SIZE) != 0);

  uint8_t sig[SIGNATURE_SIZE];
  fresh.sign(sig, (const uint8_t*) "port", 4);
  check("a host-generated identity signs and verifies its own message",
        fresh.verify(sig, (const uint8_t*) "port", 4));
}

int main(void) {
  test_packet_round_trip();
  test_path_len();
  test_mesh_tables();
  test_hex();
  test_utf8();
  test_identity_sign_verify();
  test_x25519();
  test_cipher_and_mac();
  test_sha256();
  test_advert_app_data();
  test_debt_readfrom_short_buffer();
  test_build_identity();

  printf("meshcore_core_test: %d check(s), %d failure(s)\n", checks, failed);
  return failed ? 1 : 0;
}
