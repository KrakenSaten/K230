/* meshcore-frame: build and parse MeshCore wire frames on the host.
 *
 * Everything that decides what goes on the air comes from the vendored
 * MeshCore sources (vendor/RIFT at the pinned commit) and the crypto library
 * MeshCore itself uses (vendor/Crypto): mesh::Packet does the framing,
 * mesh::Utils does the cipher and the MAC, mesh::Identity does Ed25519 and
 * X25519, AdvertDataBuilder/Parser do the advert app-data. This header is the
 * seam between that code and a command line - it adds argument checking, host
 * randomness and hex, and nothing else.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Identity.h>
#include <MeshCore.h>
#include <Packet.h>
#include <Utils.h>

namespace mcf {

/* The vendored wire code copies multi-byte fields with memcpy() and never
 * byte-swaps: transport codes, the advert timestamp, the message timestamp
 * and the advert lat/lon are all host order on the wire, which on every
 * MeshCore target (ESP32, nRF52, RP2040) and on the K230 means little endian.
 * A big-endian host would silently produce frames no node can read, so it is
 * refused at compile time rather than at the far end of a radio link. */
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "MeshCore's wire format is little endian; this tool needs a little-endian host");

/* Longest error text any call below writes, terminator included. */
#define MCF_ERR_SIZE 160

/* ---- hex ---------------------------------------------------------------
 * Strict on input: hex digits only, two per byte, nothing else - no 0x, no
 * separators, no whitespace, no sign. radiod's radio.send is equally strict
 * (services/radiod/main.c, hex_decode), so hex this refuses would not have
 * been transmitted either. Both cases are accepted; output is lower case,
 * which is what radiod prints in radio.rx events. */
bool hexDecode(const char* hex, uint8_t* out, size_t max, size_t* len);
void hexEncode(char* dest, const uint8_t* src, size_t len);  /* needs 2*len+1 */

/* ---- host randomness ---------------------------------------------------
 * mesh::RNG over the operating system's CSPRNG. Used for identity generation
 * and for the padding blobs MeshCore fills with random bytes. It fails hard
 * rather than returning predictable bytes. */
class HostRNG : public mesh::RNG {
public:
  void random(uint8_t* dest, size_t sz) override;
};

/* ---- identity ----------------------------------------------------------
 * The file is a MeshCore `.id`: pub_key(32) then prv_key(64), 96 raw bytes,
 * written by mesh::LocalIdentity itself (Identity.cpp, writeTo(Stream&)). */
#define MCF_IDENTITY_FILE_SIZE (PUB_KEY_SIZE + PRV_KEY_SIZE)

bool identityLoad(mesh::LocalIdentity& id, const char* path, char* err);
bool identitySave(const mesh::LocalIdentity& id, const char* path, char* err);
bool identityCreate(mesh::LocalIdentity& id, char* err);

/* ---- routing -----------------------------------------------------------
 * How the packet is addressed, applied after the payload is built, exactly as
 * Mesh::sendFlood()/sendDirect()/sendZeroHop() apply it. */
struct Route {
  uint8_t type;             /* ROUTE_TYPE_* */
  uint8_t path_hash_size;   /* 1..3 for a flood; 4 is reserved and refused */
  uint8_t path[MAX_PATH_SIZE];
  uint8_t path_len;         /* packed: hash size in bits 6-7, hop count in 0-5 */
  uint16_t transport_codes[2];
};

void routeInit(Route& r);
bool routeParsePath(Route& r, const char* hex, char* err);
bool routeApply(mesh::Packet& pkt, const Route& r, char* err);

/* ---- builders ----------------------------------------------------------
 * Each mirrors its MeshCore factory (Mesh::createAdvert, Mesh::createDatagram
 * via BaseChatMesh::composeMsgPacket, Mesh::createAck) and signs, encrypts
 * and MACs with the vendored code. */
struct AdvertOpts {
  const char* name;      /* NULL or "" for an advert with no name */
  uint8_t adv_type;      /* ADV_TYPE_* */
  bool has_loc;
  double lat, lon;
  uint32_t timestamp;    /* UNIX seconds, the value the signature covers */
};

bool buildAdvert(mesh::Packet& pkt, const mesh::LocalIdentity& id,
                 const AdvertOpts& o, char* err);

/* Splits an ADVERT payload the way Mesh::onRecvPacket does and checks the
 * Ed25519 signature over pub_key + timestamp + app_data. Returns false for a
 * bad signature and for a payload too short to hold one; the out parameters
 * are filled in either way when there is enough payload to fill them. */
bool advertVerify(const mesh::Packet& pkt, mesh::Identity* id_out,
                  uint32_t* timestamp_out, const uint8_t** app_data_out,
                  int* app_data_len_out);

struct TxtMsgOpts {
  const uint8_t* dest_pub_key;  /* PUB_KEY_SIZE bytes */
  const char* text;
  uint32_t timestamp;
  uint8_t attempt;
  uint8_t txt_type;             /* TXT_TYPE_* */
};

/* expected_ack receives the 4-byte ACK the recipient is expected to send
 * back, computed the way BaseChatMesh does it. */
bool buildTxtMsg(mesh::Packet& pkt, const mesh::LocalIdentity& id,
                 const TxtMsgOpts& o, uint32_t* expected_ack, char* err);

bool buildAck(mesh::Packet& pkt, const uint8_t* ack, size_t ack_len, char* err);

/* ---- frame in and out --------------------------------------------------
 * encode() refuses anything that would not fit the 255-byte MTU; decode()
 * bounds-checks the wire layout before handing the bytes to
 * mesh::Packet::readFrom(), which trusts its input. */
bool encode(const mesh::Packet& pkt, uint8_t* out, size_t* out_len, char* err);
bool decode(mesh::Packet& pkt, const uint8_t* raw, size_t len, char* err);

/* ---- reporting ---------------------------------------------------------
 * Writes the "key: value" block both commands print, and returns what its
 * last line says: false when a signature did not verify or a MAC did not
 * match, so `parse` can exit non-zero on a frame that is well formed but not
 * authentic. `id` and `peer_pub_key` may be NULL; with both, a TXT_MSG
 * between them is decrypted. */
bool report(const mesh::Packet& pkt, const uint8_t* raw, size_t raw_len,
            const mesh::LocalIdentity* id, const uint8_t* peer_pub_key);

}  /* namespace mcf */
