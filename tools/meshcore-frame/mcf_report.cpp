/* meshcore-frame: what the tool prints about a frame.
 *
 * One "key: value" line per field, stable field names, lower-case hex. It is
 * meant to be read by a person over a bench log and grepped by a script; the
 * frame_hex line is the one that goes to `pos radio send`.
 *
 * Copyright (c) 2026 PocketOS authors.
 * Portions (the ACK hash as BaseChatMesh computes it) are adapted from MeshCore,
 * Copyright (c) 2025 Scott Powell / rippleradios.com, MIT licence
 * (third_party/notices/texts/meshcore.txt).
 * SPDX-License-Identifier: Apache-2.0 AND MIT
 */
#include "mcf.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

namespace mcf {

static const char* routeName(uint8_t t) {
  switch (t) {
    case ROUTE_TYPE_TRANSPORT_FLOOD:  return "transport-flood";
    case ROUTE_TYPE_FLOOD:            return "flood";
    case ROUTE_TYPE_DIRECT:           return "direct";
    case ROUTE_TYPE_TRANSPORT_DIRECT: return "transport-direct";
    default:                          return "?";
  }
}

static const char* payloadName(uint8_t t) {
  switch (t) {
    case PAYLOAD_TYPE_REQ:        return "REQ";
    case PAYLOAD_TYPE_RESPONSE:   return "RESPONSE";
    case PAYLOAD_TYPE_TXT_MSG:    return "TXT_MSG";
    case PAYLOAD_TYPE_ACK:        return "ACK";
    case PAYLOAD_TYPE_ADVERT:     return "ADVERT";
    case PAYLOAD_TYPE_GRP_TXT:    return "GRP_TXT";
    case PAYLOAD_TYPE_GRP_DATA:   return "GRP_DATA";
    case PAYLOAD_TYPE_ANON_REQ:   return "ANON_REQ";
    case PAYLOAD_TYPE_PATH:       return "PATH";
    case PAYLOAD_TYPE_TRACE:      return "TRACE";
    case PAYLOAD_TYPE_MULTIPART:  return "MULTIPART";
    case PAYLOAD_TYPE_CONTROL:    return "CONTROL";
    case PAYLOAD_TYPE_RAW_CUSTOM: return "RAW_CUSTOM";
    default:                      return "unassigned";
  }
}

static const char* advertTypeName(uint8_t t) {
  switch (t) {
    case ADV_TYPE_NONE:     return "none";
    case ADV_TYPE_CHAT:     return "chat";
    case ADV_TYPE_REPEATER: return "repeater";
    case ADV_TYPE_ROOM:     return "room";
    case ADV_TYPE_SENSOR:   return "sensor";
    default:                return "reserved";
  }
}

static const char* txtTypeName(uint8_t t) {
  switch (t) {
    case TXT_TYPE_PLAIN:        return "plain";
    case TXT_TYPE_CLI_DATA:     return "cli-data";
    case TXT_TYPE_SIGNED_PLAIN: return "signed-plain";
    default:                    return "reserved";
  }
}

static void printHexField(const char* key, const uint8_t* src, size_t len) {
  /* The longest field is a 184-byte payload; the frame line is printed from
   * its own buffer below. */
  char hex[2 * MAX_PACKET_PAYLOAD + 1];
  if (len > MAX_PACKET_PAYLOAD) len = MAX_PACKET_PAYLOAD;
  hexEncode(hex, src, len);
  printf("%s: %s\n", key, hex);
}

/* Text that came off the air or out of a ciphertext, printed so that it
 * cannot drive the terminal it is printed to. Control bytes become \xNN;
 * anything above ASCII is passed through, so a UTF-8 node name still reads
 * as itself. */
static void printTextField(const char* key, const uint8_t* src, size_t len) {
  printf("%s: ", key);
  for (size_t i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c < 0x20 || c == 0x7F) {
      printf("\\x%02x", (unsigned)c);
    } else {
      putchar((char)c);
    }
  }
  putchar('\n');
}

static void printTimestamp(const char* key, uint32_t t) {
  time_t tt = (time_t)t;
  struct tm tm_utc;
  char when[32] = "?";
  if (gmtime_r(&tt, &tm_utc) != NULL) {
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  }
  printf("%s: %u (%s)\n", key, (unsigned)t, when);
}

/* Prints an ADVERT payload, and says whether its signature holds. */
static bool reportAdvert(const mesh::Packet& pkt) {
  mesh::Identity id;
  uint32_t timestamp = 0;
  const uint8_t* app_data = NULL;
  int app_data_len = 0;

  if (pkt.payload_len < PUB_KEY_SIZE + 4 + SIGNATURE_SIZE) {
    printf("advert.error: payload is %u bytes; an advert needs at least %d\n",
           (unsigned)pkt.payload_len, PUB_KEY_SIZE + 4 + SIGNATURE_SIZE);
    return false;
  }

  bool sig_ok = advertVerify(pkt, &id, &timestamp, &app_data, &app_data_len);

  printHexField("advert.public_key", id.pub_key, PUB_KEY_SIZE);
  printHexField("advert.node_hash", id.pub_key, PATH_HASH_SIZE);
  printTimestamp("advert.timestamp", timestamp);
  printHexField("advert.signature", &pkt.payload[PUB_KEY_SIZE + 4], SIGNATURE_SIZE);
  printf("advert.app_data_bytes: %d\n", app_data_len);
  printHexField("advert.app_data", app_data, (size_t)app_data_len);

  if (app_data_len > 0) {
    /* AdvertDataParser reads every field its flag byte announces before it
     * looks at the length it was given, so app_data too short for its own
     * flags has it reading bytes that were never there. They come from the
     * rest of the packet's payload array rather than from past its end, so
     * this is a meaningless value rather than a crash - but it is still not
     * something to print as if it were in the frame. Work out what the flags
     * ask for first. */
    int need = 1;
    if (app_data[0] & ADV_LATLON_MASK) need += 8;
    if (app_data[0] & ADV_FEAT1_MASK) need += 2;
    if (app_data[0] & ADV_FEAT2_MASK) need += 2;

    if (app_data_len < need) {
      printf("advert.app_data_valid: no (flags 0x%02x need %d bytes, %d present)\n",
             (unsigned)app_data[0], need, app_data_len);
    } else {
      AdvertDataParser parsed(app_data, (uint8_t)app_data_len);
      printf("advert.app_data_valid: %s\n", parsed.isValid() ? "yes" : "no");
      if (parsed.isValid()) {
        printf("advert.type: %u (%s)\n", (unsigned)parsed.getType(),
               advertTypeName(parsed.getType()));
        const char* name = parsed.hasName() ? parsed.getName() : "";
        printTextField("advert.name", (const uint8_t*)name, strlen(name));
        if (parsed.hasLatLon()) {
          printf("advert.lat: %.6f\n", parsed.getLat());
          printf("advert.lon: %.6f\n", parsed.getLon());
        } else {
          printf("advert.location: none\n");
        }
      }
    }
  }

  printf("advert.signature_valid: %s\n", sig_ok ? "yes" : "no");
  return sig_ok;
}

/* Prints a TXT_MSG payload. Without a key pair the ciphertext is described
 * but not opened; with one the MAC is checked and the plaintext printed. */
static bool reportTxtMsg(const mesh::Packet& pkt, const mesh::LocalIdentity* id,
                         const uint8_t* peer_pub_key) {
  const int prefix = 2 * PATH_HASH_SIZE;
  if (pkt.payload_len < (uint16_t)(prefix + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE)) {
    printf("txt.error: payload is %u bytes; a text message needs at least %d\n",
           (unsigned)pkt.payload_len, prefix + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE);
    return false;
  }

  printHexField("txt.dest_hash", &pkt.payload[0], PATH_HASH_SIZE);
  printHexField("txt.src_hash", &pkt.payload[PATH_HASH_SIZE], PATH_HASH_SIZE);
  printHexField("txt.mac", &pkt.payload[prefix], CIPHER_MAC_SIZE);
  printf("txt.cipher_bytes: %u\n", (unsigned)(pkt.payload_len - prefix - CIPHER_MAC_SIZE));

  if (id == NULL || peer_pub_key == NULL) {
    printf("txt.decrypted: not attempted (needs --key and --peer)\n");
    return true;
  }

  uint8_t secret[PUB_KEY_SIZE];
  id->calcSharedSecret(secret, peer_pub_key);

  uint8_t plain[MAX_PACKET_PAYLOAD];
  int plain_len = mesh::Utils::MACThenDecrypt(secret, plain, &pkt.payload[prefix],
                                              pkt.payload_len - prefix);
  if (plain_len <= 0) {
    printf("txt.decrypted: no (the MAC does not match this key pair)\n");
    return false;
  }
  printf("txt.decrypted: yes\n");

  if (plain_len < 5) {
    printf("txt.error: plaintext is %d bytes; a text message needs 5 before the text\n", plain_len);
    return false;
  }

  uint32_t timestamp;
  memcpy(&timestamp, plain, 4);
  uint8_t attempt = plain[4] & 3;
  uint8_t txt_type = plain[4] >> 2;

  printTimestamp("txt.timestamp", timestamp);
  printf("txt.attempt: %u\n", (unsigned)attempt);
  printf("txt.txt_type: %u (%s)\n", (unsigned)txt_type, txtTypeName(txt_type));

  /* The plaintext is zero-padded to the cipher block, so the text ends at the
   * first NUL or at the end of the block, whichever comes first. */
  size_t text_len = 0;
  while (5 + text_len < (size_t)plain_len && plain[5 + text_len] != 0) text_len++;
  printf("txt.text_bytes: %zu\n", text_len);
  printTextField("txt.text", &plain[5], text_len);

  /* The ACK the recipient owes the sender. BaseChatMesh computes it over the
   * timestamp, the flags and the text, salted with the SENDER's public key -
   * so which of the two keys to use depends on which way this message went.
   * The one-byte source hash says which, when it is not ambiguous. */
  bool from_key = (pkt.payload[PATH_HASH_SIZE] == id->pub_key[0]);
  bool from_peer = (pkt.payload[PATH_HASH_SIZE] == peer_pub_key[0]);
  printf("txt.sender: %s\n",
         from_key && from_peer ? "ambiguous (both keys share the source hash)"
                               : from_key  ? "--key"
                               : from_peer ? "--peer"
                                           : "neither key matches the source hash");

  uint32_t ack_key = 0, ack_peer = 0;
  mesh::Utils::sha256((uint8_t*)&ack_key, 4, plain, (int)(5 + text_len),
                      id->pub_key, PUB_KEY_SIZE);
  mesh::Utils::sha256((uint8_t*)&ack_peer, 4, plain, (int)(5 + text_len),
                      peer_pub_key, PUB_KEY_SIZE);
  printHexField("txt.ack_if_sent_by_key", (const uint8_t*)&ack_key, 4);
  printHexField("txt.ack_if_sent_by_peer", (const uint8_t*)&ack_peer, 4);
  return true;
}

static void reportAck(const mesh::Packet& pkt) {
  printf("ack.bytes: %u\n", (unsigned)pkt.payload_len);
  printHexField("ack.hex", pkt.payload, pkt.payload_len);
  if (pkt.payload_len != 4 && pkt.payload_len != 6) {
    printf("ack.note: MeshCore sends 4 bytes, or 6 for the multi-ack path\n");
  }
}

bool report(const mesh::Packet& pkt, const uint8_t* raw, size_t raw_len,
            const mesh::LocalIdentity* id, const uint8_t* peer_pub_key) {
  char frame_hex[2 * MAX_TRANS_UNIT + 1];
  hexEncode(frame_hex, raw, raw_len);

  printf("frame_bytes: %zu\n", raw_len);
  printf("frame_hex: %s\n", frame_hex);
  printf("header: 0x%02x\n", (unsigned)pkt.header);
  printf("route_type: %u (%s)\n", (unsigned)pkt.getRouteType(), routeName(pkt.getRouteType()));
  printf("payload_type: %u (%s)\n", (unsigned)pkt.getPayloadType(), payloadName(pkt.getPayloadType()));
  printf("payload_ver: %u\n", (unsigned)pkt.getPayloadVer());

  if (pkt.hasTransportCodes()) {
    printf("transport_codes: %u %u\n",
           (unsigned)pkt.transport_codes[0], (unsigned)pkt.transport_codes[1]);
  }

  printf("path_len: 0x%02x\n", (unsigned)pkt.path_len);
  printf("path_hash_size: %u\n", (unsigned)pkt.getPathHashSize());
  printf("path_hash_count: %u\n", (unsigned)pkt.getPathHashCount());
  printf("path_bytes: %u\n", (unsigned)pkt.getPathByteLen());
  printHexField("path_hex", pkt.path, pkt.getPathByteLen());
  printf("payload_bytes: %u\n", (unsigned)pkt.payload_len);

  uint8_t hash[MAX_HASH_SIZE];
  pkt.calculatePacketHash(hash);
  printHexField("packet_hash", hash, sizeof(hash));

  bool ok = true;
  switch (pkt.getPayloadType()) {
    case PAYLOAD_TYPE_ADVERT:  ok = reportAdvert(pkt); break;
    case PAYLOAD_TYPE_TXT_MSG: ok = reportTxtMsg(pkt, id, peer_pub_key); break;
    case PAYLOAD_TYPE_ACK:     reportAck(pkt); break;
    default:
      printf("note: this tool describes the frame but does not decode %s payloads\n",
             payloadName(pkt.getPayloadType()));
      break;
  }

  printf("validation: %s\n", ok ? "ok" : "failed");
  return ok;
}

}  /* namespace mcf */
