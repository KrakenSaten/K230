/*
 * meshcore-frame: the wire format and the crypto under it.
 *
 * Everything here runs against the vendored MeshCore sources and the crypto
 * library MeshCore itself uses - there is no stand-in for either, because a
 * frame that only this tool's idea of MeshCore accepts would prove nothing
 * about a real node.
 *
 * Determinism: the two identities are fixed. One is the key pair MeshCore
 * carries in its own source as a known-good test pair, which lets the Ed25519
 * path be checked against a value the protocol's authors published; the other
 * is generated from a fixed seed. Fixing the input to real key generation is
 * not the same as faking it - every signature, agreement, cipher and MAC below
 * is computed by the shipped implementation.
 *
 * Built and run by tools/meshcore-frame/Makefile:
 *   make meshcore-frame-test        (from the top of the repository)
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mcf.h"

#include <stdio.h>
#include <string.h>

#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

static int failed;
static int checks;

static void check(const char* name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  checks++;
  failed += !ok;
}

/* ---- fixed identities -------------------------------------------------- */

/* vendor/RIFT src/Identity.cpp, LocalIdentity::validatePrivateKey(): the
 * "known good test client keypair" MeshCore ships to check that a private key
 * can do ECDH at all. Using it here means the Ed25519 key derivation is held
 * to a pair the protocol's own source states. */
static const char* const A_PRV =
    "7065e18fd9fabb70c1ed90dca19907de698c88b709ea146eafd93d9b830c7b60"
    "c4681193c79bbc39945ba8064104bb618f8fd7a84a0af6f57033d6e8ddcd6471";
static const char* const A_PUB =
    "1ec77175b0918ed206f9ae04ec136d6d5d4315bb26305427f645b492e9350c10";

/* The second party. Real key generation (ed25519_create_keypair through
 * mesh::LocalIdentity), from a seed this test fixes so the frames below are
 * the same bytes on every run and on every host. */
class FixedSeedRNG : public mesh::RNG {
  uint8_t _next;
public:
  explicit FixedSeedRNG(uint8_t start) : _next(start) { }
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; i++) dest[i] = (uint8_t)(_next + i * 7u);
  }
};

static mesh::LocalIdentity identityA() { return mesh::LocalIdentity(A_PRV, A_PUB); }

static mesh::LocalIdentity identityB() {
  FixedSeedRNG rng(0x5a);
  return mesh::LocalIdentity(&rng);
}

/* The ADVERT this tool builds for identity A, at a fixed timestamp, as a
 * chat node called K230-A. Ed25519 signing is deterministic (RFC 8032) and
 * every other field is fixed, so this frame is a constant. If MeshCore's wire
 * format or its crypto changes under us, this is the line that says so. */
static const char* const ADVERT_VECTOR =
    /* header: flood route, ADVERT payload, version 1 */
    "11"
    /* path_len: one-byte hashes, no hops yet */
    "00"
    /* the advertised public key */
    "1ec77175b0918ed206f9ae04ec136d6d5d4315bb26305427f645b492e9350c10"
    /* the emitted timestamp, little endian */
    "0086ec65"
    /* Ed25519 signature over key + timestamp + app_data */
    "f83d2be91d67a51d1274d34d3cf4b301fcdcacea10197e2fe30819c4009bfd2c"
    "42dcb44a7739f2fcc9cc37ad9268e90c79de701dea39f16f3748cdfe19c0c600"
    /* app_data: flags (name present, chat), then "K230-A" */
    "814b3233302d41";
static const uint32_t ADVERT_VECTOR_TIME = 1709999616u;

/* ---- helpers ----------------------------------------------------------- */

static char err[MCF_ERR_SIZE];

static bool buildAdvertFrame(const mesh::LocalIdentity& id, uint32_t when,
                             const char* name, uint8_t* raw, size_t* raw_len) {
  mcf::AdvertOpts o;
  o.name = name;
  o.adv_type = ADV_TYPE_CHAT;
  o.has_loc = false;
  o.lat = o.lon = 0.0;
  o.timestamp = when;

  mesh::Packet pkt;
  if (!mcf::buildAdvert(pkt, id, o, err)) return false;

  mcf::Route route;
  mcf::routeInit(route);
  if (!mcf::routeApply(pkt, route, err)) return false;
  return mcf::encode(pkt, raw, raw_len, err);
}

static void hexOf(char* dest, const uint8_t* src, size_t len) {
  mcf::hexEncode(dest, src, len);
}

/* ---- 1, 2: ADVERT round trip and signature ----------------------------- */

static void test_advert_round_trip(void) {
  mesh::LocalIdentity a = identityA();
  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;

  check("ADVERT builds", buildAdvertFrame(a, ADVERT_VECTOR_TIME, "K230-A", raw, &raw_len));

  mesh::Packet pkt;
  check("ADVERT parses", mcf::decode(pkt, raw, raw_len, err));

  check("route type survives the round trip", pkt.getRouteType() == ROUTE_TYPE_FLOOD);
  check("payload type survives the round trip", pkt.getPayloadType() == PAYLOAD_TYPE_ADVERT);
  check("payload version is 1", pkt.getPayloadVer() == PAYLOAD_VER_1);
  check("a fresh flood advert carries no path", pkt.getPathHashCount() == 0);
  check("the path hash size is the MeshCore default", pkt.getPathHashSize() == PATH_HASH_SIZE);
  check("payload is pub_key + timestamp + signature + app_data",
        pkt.payload_len == PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + 7);
  check("the frame is the payload plus a header and a path_len byte",
        raw_len == (size_t)pkt.payload_len + 2);

  /* Re-encoding what was parsed must give the same bytes back. */
  uint8_t again[MAX_TRANS_UNIT];
  size_t again_len = 0;
  check("re-encoding the parsed packet reproduces it",
        mcf::encode(pkt, again, &again_len, err) && again_len == raw_len &&
        memcmp(again, raw, raw_len) == 0);

  mesh::Identity id;
  uint32_t when = 0;
  const uint8_t* app_data = NULL;
  int app_data_len = 0;
  check("the ADVERT signature verifies",
        mcf::advertVerify(pkt, &id, &when, &app_data, &app_data_len));
  check("the advertised key is the one that signed",
        memcmp(id.pub_key, a.pub_key, PUB_KEY_SIZE) == 0);
  check("the timestamp is the one asked for", when == ADVERT_VECTOR_TIME);

  AdvertDataParser parsed(app_data, (uint8_t)app_data_len);
  check("the advert data parses", parsed.isValid());
  check("the advert is a chat node", parsed.getType() == ADV_TYPE_CHAT);
  check("the name survives", parsed.hasName() && strcmp(parsed.getName(), "K230-A") == 0);
  check("the advert data is the flag byte plus the name",
        app_data_len == 1 + 6 && app_data[0] == (ADV_TYPE_CHAT | ADV_NAME_MASK));
  check("no location was claimed", !parsed.hasLatLon());
}

/* ---- 3: a modified signature must fail --------------------------------- */

static void test_advert_signature_tamper(void) {
  mesh::LocalIdentity a = identityA();
  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  if (!buildAdvertFrame(a, ADVERT_VECTOR_TIME, "K230-A", raw, &raw_len)) {
    check("ADVERT builds for the tamper test", false);
    return;
  }

  const size_t sig_off = 2 + PUB_KEY_SIZE + 4;  /* header, path_len, key, time */

  for (int bit = 0; bit < 8; bit++) {
    uint8_t bad[MAX_TRANS_UNIT];
    memcpy(bad, raw, raw_len);
    bad[sig_off + (size_t)bit * 8] ^= (uint8_t)(1u << bit);

    mesh::Packet pkt;
    bool parsed = mcf::decode(pkt, bad, raw_len, err);
    bool verified = parsed && mcf::advertVerify(pkt, NULL, NULL, NULL, NULL);
    if (bit == 0) {
      check("a frame with a flipped signature bit still parses", parsed);
    }
    if (verified) {
      check("a flipped signature bit is rejected", false);
      return;
    }
  }
  check("a flipped signature bit is rejected", true);

  /* The signature covers the name and the timestamp too, not just the key. */
  uint8_t bad[MAX_TRANS_UNIT];
  memcpy(bad, raw, raw_len);
  bad[raw_len - 1] ^= 0x20;  /* last byte of "K230-A" */
  mesh::Packet pkt;
  check("a changed name breaks the signature",
        mcf::decode(pkt, bad, raw_len, err) && !mcf::advertVerify(pkt, NULL, NULL, NULL, NULL));

  memcpy(bad, raw, raw_len);
  bad[2 + PUB_KEY_SIZE] ^= 0x01;  /* low byte of the timestamp */
  mesh::Packet pkt2;
  check("a changed timestamp breaks the signature",
        mcf::decode(pkt2, bad, raw_len, err) && !mcf::advertVerify(pkt2, NULL, NULL, NULL, NULL));

  /* A signature that is valid for a different key must not pass for this one. */
  mesh::LocalIdentity b = identityB();
  uint8_t other[MAX_TRANS_UNIT];
  size_t other_len = 0;
  if (buildAdvertFrame(b, ADVERT_VECTOR_TIME, "K230-A", other, &other_len) &&
      other_len == raw_len) {
    memcpy(bad, raw, raw_len);
    memcpy(&bad[sig_off], &other[sig_off], SIGNATURE_SIZE);
    mesh::Packet pkt3;
    check("another node's signature does not verify against this key",
          mcf::decode(pkt3, bad, raw_len, err) &&
          !mcf::advertVerify(pkt3, NULL, NULL, NULL, NULL));
  } else {
    check("another node's signature does not verify against this key", false);
  }
}

/* ---- 4: TXT_MSG -------------------------------------------------------- */

static void test_txt_msg(void) {
  mesh::LocalIdentity a = identityA();
  mesh::LocalIdentity b = identityB();

  mcf::TxtMsgOpts o;
  o.dest_pub_key = b.pub_key;
  o.text = "hello from K230";
  o.timestamp = ADVERT_VECTOR_TIME;
  o.attempt = 0;
  o.txt_type = TXT_TYPE_PLAIN;

  mesh::Packet pkt;
  uint32_t expected_ack = 0;
  check("TXT_MSG builds", mcf::buildTxtMsg(pkt, a, o, &expected_ack, err));

  mcf::Route route;
  mcf::routeInit(route);
  check("TXT_MSG takes a flood route", mcf::routeApply(pkt, route, err));

  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  check("TXT_MSG encodes", mcf::encode(pkt, raw, &raw_len, err));

  mesh::Packet got;
  check("TXT_MSG parses", mcf::decode(got, raw, raw_len, err));
  check("TXT_MSG keeps its payload type", got.getPayloadType() == PAYLOAD_TYPE_TXT_MSG);
  check("the destination hash is the first byte of the recipient key",
        got.payload[0] == b.pub_key[0]);
  check("the source hash is the first byte of the sender key",
        got.payload[1] == a.pub_key[0]);

  /* 5 bytes of header plus 15 of text, zero-padded to the cipher block, with
   * the two hashes and the MAC in front. */
  const int cipher_len = ((5 + 15 + CIPHER_BLOCK_SIZE - 1) / CIPHER_BLOCK_SIZE) * CIPHER_BLOCK_SIZE;
  check("the payload is two hashes, a MAC and whole cipher blocks",
        got.payload_len == 2 * PATH_HASH_SIZE + CIPHER_MAC_SIZE + cipher_len);

  /* The recipient opens it with the same shared secret, from the other side. */
  uint8_t secret_b[PUB_KEY_SIZE], secret_a[PUB_KEY_SIZE];
  b.calcSharedSecret(secret_b, a.pub_key);
  a.calcSharedSecret(secret_a, b.pub_key);
  check("both sides agree on the X25519 shared secret",
        memcmp(secret_a, secret_b, PUB_KEY_SIZE) == 0);

  uint8_t plain[MAX_PACKET_PAYLOAD];
  int plain_len = mesh::Utils::MACThenDecrypt(secret_b, plain, &got.payload[2],
                                              got.payload_len - 2);
  check("the recipient decrypts it", plain_len == cipher_len);

  uint32_t when = 0;
  memcpy(&when, plain, 4);
  check("the timestamp survives", when == ADVERT_VECTOR_TIME);
  check("the attempt number is 0", (plain[4] & 3) == 0);
  check("the text type is plain", (plain[4] >> 2) == TXT_TYPE_PLAIN);
  check("the text survives", strcmp((const char*)&plain[5], "hello from K230") == 0);

  /* The ACK the recipient owes, as BaseChatMesh computes it. */
  uint32_t ack = 0;
  mesh::Utils::sha256((uint8_t*)&ack, 4, plain, 5 + 15, a.pub_key, PUB_KEY_SIZE);
  check("the expected ACK matches what the recipient would compute", ack == expected_ack);

  /* A flipped ciphertext bit must not decrypt: the MAC is checked first. */
  uint8_t bad[MAX_PACKET_PAYLOAD];
  memcpy(bad, got.payload, got.payload_len);
  bad[2 + CIPHER_MAC_SIZE] ^= 0x01;
  check("a flipped ciphertext bit fails the MAC",
        mesh::Utils::MACThenDecrypt(secret_b, plain, &bad[2], got.payload_len - 2) == 0);

  memcpy(bad, got.payload, got.payload_len);
  bad[2] ^= 0x80;  /* the MAC itself */
  check("a corrupted MAC is rejected",
        mesh::Utils::MACThenDecrypt(secret_b, plain, &bad[2], got.payload_len - 2) == 0);

  /* The wrong peer gives the wrong secret, and the MAC catches it. */
  mesh::LocalIdentity c = identityB();
  uint8_t wrong[PUB_KEY_SIZE];
  c.calcSharedSecret(wrong, c.pub_key);  /* b with itself: not the pair used */
  check("the wrong shared secret fails the MAC",
        mesh::Utils::MACThenDecrypt(wrong, plain, &got.payload[2], got.payload_len - 2) == 0);

  /* An attempt above 3 hides the number past the text's null terminator. */
  o.attempt = 5;
  mesh::Packet retry;
  uint32_t retry_ack = 0;
  check("a retry past attempt 3 builds", mcf::buildTxtMsg(retry, a, o, &retry_ack, err));
  /* The attempt bits sit in the byte the ACK hash covers, so each attempt has
   * its own expected ACK - which is how BaseChatMesh keeps the packet hash of
   * a retry distinct from the attempt before it. */
  check("a retry expects a different ACK, because the attempt is signed into it",
        retry_ack != expected_ack);
  int retry_len = mesh::Utils::MACThenDecrypt(secret_b, plain, &retry.payload[2],
                                              retry.payload_len - 2);
  check("the retry decrypts", retry_len > 0);
  check("the retry's low attempt bits are the number modulo 4", (plain[4] & 3) == (5 & 3));
  check("the retry hides the full attempt number after the text",
        plain[5 + 15] == 0 && plain[5 + 15 + 1] == 5);

  /* Too long for a single frame. */
  char long_text[512];
  memset(long_text, 'x', sizeof(long_text) - 1);
  long_text[sizeof(long_text) - 1] = 0;
  o.attempt = 0;
  o.text = long_text;
  mesh::Packet too_big;
  check("a text past the MeshCore limit is refused",
        !mcf::buildTxtMsg(too_big, a, o, &expected_ack, err));
}

/* ---- 5: ACK ------------------------------------------------------------ */

static void test_ack(void) {
  const uint8_t ack[4] = { 0xde, 0xad, 0xbe, 0xef };
  mesh::Packet pkt;
  check("ACK builds", mcf::buildAck(pkt, ack, sizeof(ack), err));

  mcf::Route route;
  mcf::routeInit(route);
  route.type = ROUTE_TYPE_DIRECT;
  check("ACK takes a zero-hop direct route", mcf::routeApply(pkt, route, err));

  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  check("ACK encodes", mcf::encode(pkt, raw, &raw_len, err));
  check("an ACK is six bytes on the wire", raw_len == 6);

  mesh::Packet got;
  check("ACK parses", mcf::decode(got, raw, raw_len, err));
  check("ACK keeps its payload type", got.getPayloadType() == PAYLOAD_TYPE_ACK);
  check("ACK keeps its route type", got.getRouteType() == ROUTE_TYPE_DIRECT);
  check("a zero-hop route has an empty path", got.path_len == 0);
  check("the ACK payload survives",
        got.payload_len == sizeof(ack) && memcmp(got.payload, ack, sizeof(ack)) == 0);

  /* The six-byte multi-ack payload is a payload like any other. */
  const uint8_t ack6[6] = { 1, 2, 3, 4, 5, 6 };
  mesh::Packet six;
  check("a six-byte ACK builds", mcf::buildAck(six, ack6, sizeof(ack6), err));
  check("an empty ACK is refused", !mcf::buildAck(six, ack6, 0, err));
}

/* ---- 6: hex ------------------------------------------------------------ */

static void test_hex(void) {
  uint8_t out[8];
  size_t len = 0;

  check("hex decodes", mcf::hexDecode("00ff10", out, sizeof(out), &len) &&
                       len == 3 && out[0] == 0x00 && out[1] == 0xff && out[2] == 0x10);
  check("upper case decodes the same", mcf::hexDecode("00FF10", out, sizeof(out), &len) &&
                                       len == 3 && out[1] == 0xff);
  check("an odd number of digits is refused", !mcf::hexDecode("abc", out, sizeof(out), &len));
  check("an empty string is refused", !mcf::hexDecode("", out, sizeof(out), &len));
  check("a non-hex digit is refused", !mcf::hexDecode("00zz", out, sizeof(out), &len));
  check("leading whitespace is refused", !mcf::hexDecode(" 00", out, sizeof(out), &len));
  check("a sign is refused", !mcf::hexDecode("-1", out, sizeof(out), &len));
  check("an 0x prefix is refused", !mcf::hexDecode("0x00", out, sizeof(out), &len));
  check("a separator is refused", !mcf::hexDecode("00:ff", out, sizeof(out), &len));
  check("more bytes than will fit is refused",
        !mcf::hexDecode("000102030405060708", out, sizeof(out), &len));

  char hex[2 * 3 + 1];
  const uint8_t bytes[3] = { 0x00, 0xff, 0x10 };
  hexOf(hex, bytes, sizeof(bytes));
  check("hex encodes in lower case, as radiod prints it", strcmp(hex, "00ff10") == 0);
}

/* ---- 7, 8: frames the parser must refuse ------------------------------- */

static void test_malformed_frames(void) {
  mesh::Packet pkt;
  uint8_t raw[MAX_TRANS_UNIT + 8];
  memset(raw, 0, sizeof(raw));

  check("an empty frame is refused", !mcf::decode(pkt, raw, 0, err));

  /* Past the 255-byte MTU. */
  memset(raw, 0x41, sizeof(raw));
  raw[0] = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  raw[1] = 0;
  check("a frame past the MTU is refused", !mcf::decode(pkt, raw, MAX_TRANS_UNIT + 1, err));
  check("a frame at the MTU but past the payload limit is refused",
        !mcf::decode(pkt, raw, MAX_TRANS_UNIT, err));

  /* A payload the builder would never produce. */
  mesh::Packet over;
  over.header = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  over.path_len = 0;
  over.payload_len = MAX_PACKET_PAYLOAD + 1;
  uint8_t out[MAX_TRANS_UNIT];
  size_t out_len = 0;
  check("encoding past the payload limit is refused", !mcf::encode(over, out, &out_len, err));

  /* path_len with hash size 4, the reserved encoding. */
  uint8_t bad[8];
  bad[0] = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_DIRECT;
  bad[1] = 0xC1;  /* size 4, one hop */
  bad[2] = 0x11;
  bad[3] = 0x22;
  check("the reserved path hash size is refused", !mcf::decode(pkt, bad, 4, err));

  /* A hop count the frame cannot hold: 63 one-byte hashes in a 4-byte frame.
   * This is the case mesh::Packet::readFrom would read past the end for. */
  bad[1] = 63;
  check("a path longer than the frame is refused", !mcf::decode(pkt, bad, 4, err));

  /* 32 three-byte hashes is 96 bytes, past the 64-byte path field. */
  bad[1] = (uint8_t)((2 << 6) | 32);
  check("a path past MAX_PATH_SIZE is refused", !mcf::decode(pkt, bad, 4, err));

  /* Header and path_len only, no payload. */
  bad[0] = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  bad[1] = 0;
  check("a frame with no payload is refused", !mcf::decode(pkt, bad, 2, err));
  check("a frame cut before path_len is refused", !mcf::decode(pkt, bad, 1, err));

  /* A transport route needs four more bytes before path_len. */
  bad[0] = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_TRANSPORT_FLOOD;
  check("a transport frame cut before its codes is refused", !mcf::decode(pkt, bad, 4, err));

  uint8_t transport[8] = { 0, 0x02, 0x01, 0x04, 0x03, 0x00, 0xaa, 0xbb };
  transport[0] = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT) | ROUTE_TYPE_TRANSPORT_FLOOD;
  check("a whole transport frame parses", mcf::decode(pkt, transport, 8, err));
  check("its transport codes are little endian",
        pkt.transport_codes[0] == 0x0102 && pkt.transport_codes[1] == 0x0304);
}

/* ---- 9, 10: the fixed vector and a byte-for-byte round trip ------------ */

static void test_known_vector(void) {
  mesh::LocalIdentity a = identityA();

  char pub_hex[2 * PUB_KEY_SIZE + 1];
  hexOf(pub_hex, a.pub_key, PUB_KEY_SIZE);
  check("the fixed private key derives the public key MeshCore records",
        strcmp(pub_hex, A_PUB) == 0);

  uint8_t prv[PRV_KEY_SIZE];
  size_t prv_len = 0;
  check("MeshCore accepts the fixed private key for ECDH",
        mcf::hexDecode(A_PRV, prv, sizeof(prv), &prv_len) && prv_len == PRV_KEY_SIZE &&
        mesh::LocalIdentity::validatePrivateKey(prv));

  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  if (!buildAdvertFrame(a, ADVERT_VECTOR_TIME, "K230-A", raw, &raw_len)) {
    check("the fixed ADVERT builds", false);
    return;
  }

  char hex[2 * MAX_TRANS_UNIT + 1];
  hexOf(hex, raw, raw_len);
  bool same = (strcmp(hex, ADVERT_VECTOR) == 0);
  check("the ADVERT is the recorded vector, byte for byte", same);
  if (!same) {
    printf("     expected %s\n", ADVERT_VECTOR);
    printf("     got      %s\n", hex);
  }

  /* Hex out, hex in, hex out again. */
  uint8_t back[MAX_TRANS_UNIT];
  size_t back_len = 0;
  check("the printed hex decodes to the same bytes",
        mcf::hexDecode(hex, back, sizeof(back), &back_len) &&
        back_len == raw_len && memcmp(back, raw, raw_len) == 0);

  mesh::Packet pkt;
  uint8_t again[MAX_TRANS_UNIT];
  size_t again_len = 0;
  char hex2[2 * MAX_TRANS_UNIT + 1];
  check("parsing and re-encoding the vector reproduces the hex",
        mcf::decode(pkt, back, back_len, err) &&
        mcf::encode(pkt, again, &again_len, err) &&
        (hexOf(hex2, again, again_len), strcmp(hex, hex2) == 0));

  check("the recorded vector still verifies",
        mcf::decode(pkt, back, back_len, err) &&
        mcf::advertVerify(pkt, NULL, NULL, NULL, NULL));
}

/* ---- the crypto, on its own -------------------------------------------- */

static void test_crypto(void) {
  mesh::LocalIdentity a = identityA();
  mesh::LocalIdentity b = identityB();

  const char* msg = "MeshCore interop, K230 side";
  uint8_t sig[SIGNATURE_SIZE];
  a.sign(sig, (const uint8_t*)msg, (int)strlen(msg));
  check("Ed25519 sign then verify",
        a.verify(sig, (const uint8_t*)msg, (int)strlen(msg)));

  for (int i = 0; i < SIGNATURE_SIZE; i += 7) {
    uint8_t bad[SIGNATURE_SIZE];
    memcpy(bad, sig, sizeof(bad));
    bad[i] ^= 0x40;
    if (a.verify(bad, (const uint8_t*)msg, (int)strlen(msg))) {
      check("Ed25519 verify rejects a modified signature", false);
      return;
    }
  }
  check("Ed25519 verify rejects a modified signature", true);

  const char* other = "MeshCore interop, K230 sidE";
  check("Ed25519 verify rejects a modified message",
        !a.verify(sig, (const uint8_t*)other, (int)strlen(other)));
  check("Ed25519 verify rejects another node's key",
        !b.verify(sig, (const uint8_t*)msg, (int)strlen(msg)));

  /* X25519, through the Ed25519 keys MeshCore transposes. */
  uint8_t sa[PUB_KEY_SIZE], sb[PUB_KEY_SIZE];
  a.calcSharedSecret(sa, b.pub_key);
  b.calcSharedSecret(sb, a.pub_key);
  check("X25519 agreement is symmetric", memcmp(sa, sb, PUB_KEY_SIZE) == 0);

  bool all_zero = true;
  for (int i = 0; i < PUB_KEY_SIZE; i++) {
    if (sa[i] != 0) { all_zero = false; break; }
  }
  check("the shared secret is not degenerate", !all_zero);

  uint8_t sc_[PUB_KEY_SIZE];
  a.calcSharedSecret(sc_, a.pub_key);
  check("a different pair gives a different secret", memcmp(sa, sc_, PUB_KEY_SIZE) != 0);

  /* AES-128 with the encrypt-then-MAC wrapper MeshCore puts round it. */
  const char plain_text[] = "twenty bytes of tex";
  const uint8_t* plain = reinterpret_cast<const uint8_t*>(plain_text);
  const size_t plain_size = sizeof(plain_text);  /* 19 characters and the null */
  uint8_t sealed[64];
  int sealed_len = mesh::Utils::encryptThenMAC(sa, sealed, plain, (int)plain_size);
  check("encryptThenMAC pads to the cipher block and prefixes a MAC",
        sealed_len == CIPHER_MAC_SIZE + 2 * CIPHER_BLOCK_SIZE);

  uint8_t opened[64];
  int opened_len = mesh::Utils::MACThenDecrypt(sa, opened, sealed, sealed_len);
  check("MACThenDecrypt returns the plaintext",
        opened_len == 2 * CIPHER_BLOCK_SIZE && memcmp(opened, plain, plain_size) == 0);

  for (int i = 0; i < sealed_len; i++) {
    uint8_t bad[64];
    memcpy(bad, sealed, (size_t)sealed_len);
    bad[i] ^= 0x01;
    if (mesh::Utils::MACThenDecrypt(sa, opened, bad, sealed_len) != 0) {
      check("a corrupted MAC or ciphertext is rejected", false);
      return;
    }
  }
  check("a corrupted MAC or ciphertext is rejected", true);

  uint8_t wrong_key[PUB_KEY_SIZE];
  memcpy(wrong_key, sa, sizeof(wrong_key));
  wrong_key[0] ^= 0x01;
  check("the wrong key is rejected by the MAC",
        mesh::Utils::MACThenDecrypt(wrong_key, opened, sealed, sealed_len) == 0);
  check("a MAC-only input is rejected",
        mesh::Utils::MACThenDecrypt(sa, opened, sealed, CIPHER_MAC_SIZE) == 0);

  /* SHA-256, as the packet hash and the ACK use it. */
  uint8_t h1[MAX_HASH_SIZE], h2[MAX_HASH_SIZE];
  mesh::Utils::sha256(h1, sizeof(h1), (const uint8_t*)"abc", 3);
  mesh::Utils::sha256(h2, sizeof(h2), (const uint8_t*)"ab", 2, (const uint8_t*)"c", 1);
  check("a two-fragment hash is the hash of the join", memcmp(h1, h2, sizeof(h1)) == 0);
  /* FIPS 180-4: SHA-256("abc") starts ba7816bf 8f01cfea. */
  static const uint8_t sha_abc[MAX_HASH_SIZE] =
      { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea };
  check("SHA-256 matches the published test vector",
        memcmp(h1, sha_abc, sizeof(sha_abc)) == 0);
}

/* ---- routing ----------------------------------------------------------- */

static void test_routing(void) {
  mcf::Route r;
  mcf::routeInit(r);

  check("a one-byte three-hop path packs into path_len",
        mcf::routeParsePath(r, "aabbcc", err) && r.path_len == 3);

  mcf::routeInit(r);
  r.path_hash_size = 2;
  check("a two-byte hash sets the size bits",
        mcf::routeParsePath(r, "aabbccdd", err) && r.path_len == ((1 << 6) | 2));

  mcf::routeInit(r);
  r.path_hash_size = 3;
  check("a path that is not a whole number of hashes is refused",
        !mcf::routeParsePath(r, "aabbccdd", err));

  mcf::routeInit(r);
  check("a path longer than MAX_PATH_SIZE is refused",
        !mcf::routeParsePath(r, "00000000000000000000000000000000"
                                "00000000000000000000000000000000"
                                "00000000000000000000000000000000"
                                "00000000000000000000000000000000"
                                "0000000000000000000000000000000000", err));

  mcf::routeInit(r);
  r.path_hash_size = 4;
  mesh::Packet pkt;
  pkt.header = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  check("the reserved path hash size is refused when routing", !mcf::routeApply(pkt, r, err));

  mcf::routeInit(r);
  if (!mcf::routeParsePath(r, "aabbcc", err)) {
    check("a path on a flood route is refused", false);
  } else {
    mesh::Packet flood;
    flood.header = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
    check("a path on a flood route is refused", !mcf::routeApply(flood, r, err));
  }

  /* A direct route carries its path through the wire and back. */
  mcf::routeInit(r);
  r.type = ROUTE_TYPE_DIRECT;
  if (!mcf::routeParsePath(r, "aabbcc", err)) {
    check("a direct route carries its path", false);
    return;
  }
  const uint8_t ack[4] = { 1, 2, 3, 4 };
  mesh::Packet direct;
  uint8_t raw[MAX_TRANS_UNIT];
  size_t raw_len = 0;
  mesh::Packet got;
  check("a direct route carries its path",
        mcf::buildAck(direct, ack, sizeof(ack), err) &&
        mcf::routeApply(direct, r, err) &&
        mcf::encode(direct, raw, &raw_len, err) &&
        mcf::decode(got, raw, raw_len, err) &&
        got.getPathHashCount() == 3 && got.getPathHashSize() == 1 &&
        got.path[0] == 0xaa && got.path[1] == 0xbb && got.path[2] == 0xcc);
}

int main(void) {
  test_advert_round_trip();
  test_advert_signature_tamper();
  test_txt_msg();
  test_ack();
  test_hex();
  test_malformed_frames();
  test_known_vector();
  test_crypto();
  test_routing();

  printf("meshcore_frame_test: %d check(s), %d failure(s)\n", checks, failed);
  return failed > 0;
}
