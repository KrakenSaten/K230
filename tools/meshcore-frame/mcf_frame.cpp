/* meshcore-frame: the frames themselves.
 *
 * Every byte that reaches the wire is produced by the vendored MeshCore code.
 * The builders here lay out the payloads exactly as their MeshCore factories
 * do - Mesh::createAdvert, BaseChatMesh::composeMsgPacket through
 * Mesh::createDatagram, and Mesh::createAck - and then hand the result to
 * mesh::Packet for framing. The signature, the shared secret, the cipher and
 * the MAC are mesh::Identity and mesh::Utils; none of them are reimplemented.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mcf.h"

#include <stdio.h>
#include <string.h>

#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>

/* BaseChatMesh's limit on the text of a message. Its header pulls in the
 * whole chat mesh, which this tool has no use for, so the one constant it
 * needs is restated with the reference that defines it. */
#define MCF_MAX_TEXT_LEN (10 * CIPHER_BLOCK_SIZE)  /* helpers/BaseChatMesh.h */

namespace mcf {

/* ---- routing ----------------------------------------------------------- */

void routeInit(Route& r) {
  r.type = ROUTE_TYPE_FLOOD;
  r.path_hash_size = PATH_HASH_SIZE;
  r.path_len = 0;
  r.transport_codes[0] = r.transport_codes[1] = 0;
  memset(r.path, 0, sizeof(r.path));
}

bool routeParsePath(Route& r, const char* hex, char* err) {
  uint8_t bytes[MAX_PATH_SIZE];
  size_t len = 0;
  if (!hexDecode(hex, bytes, sizeof(bytes), &len)) {
    snprintf(err, MCF_ERR_SIZE, "--path must be hex, two digits a byte, at most %d bytes",
             MAX_PATH_SIZE);
    return false;
  }
  if (len % r.path_hash_size != 0) {
    snprintf(err, MCF_ERR_SIZE,
             "--path is %zu bytes, which is not a whole number of %u-byte hashes",
             len, (unsigned)r.path_hash_size);
    return false;
  }
  size_t count = len / r.path_hash_size;
  if (count > 63) {
    snprintf(err, MCF_ERR_SIZE, "--path holds %zu hops; the hop count field holds 63", count);
    return false;
  }
  memcpy(r.path, bytes, len);
  r.path_len = (uint8_t)(((r.path_hash_size - 1) << 6) | (count & 63));
  return true;
}

bool routeApply(mesh::Packet& pkt, const Route& r, char* err) {
  if (r.path_hash_size < 1 || r.path_hash_size > 3) {
    /* Mesh::sendFlood refuses the same range; size 4 is the reserved encoding
     * Packet::isValidPathLen rejects on the way back in. */
    snprintf(err, MCF_ERR_SIZE, "path hash size must be 1, 2 or 3 (4 is reserved)");
    return false;
  }

  pkt.header &= ~PH_ROUTE_MASK;
  pkt.header |= (r.type & PH_ROUTE_MASK);

  switch (r.type) {
    case ROUTE_TYPE_FLOOD:
    case ROUTE_TYPE_TRANSPORT_FLOOD:
      if (r.path_len != 0) {
        snprintf(err, MCF_ERR_SIZE,
                 "a flood route starts with an empty path; --path needs --route direct");
        return false;
      }
      pkt.setPathHashSizeAndCount(r.path_hash_size, 0);
      break;
    case ROUTE_TYPE_DIRECT:
    case ROUTE_TYPE_TRANSPORT_DIRECT:
      /* Mesh::sendDirect for a supplied path, Mesh::sendZeroHop for none. */
      pkt.path_len = mesh::Packet::copyPath(pkt.path, r.path, r.path_len);
      break;
    default:
      snprintf(err, MCF_ERR_SIZE, "unknown route type %u", (unsigned)r.type);
      return false;
  }

  if (pkt.hasTransportCodes()) {
    pkt.transport_codes[0] = r.transport_codes[0];
    pkt.transport_codes[1] = r.transport_codes[1];
  }
  return true;
}

/* ---- ADVERT ------------------------------------------------------------ */

bool buildAdvert(mesh::Packet& pkt, const mesh::LocalIdentity& id,
                 const AdvertOpts& o, char* err) {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  memset(app_data, 0, sizeof(app_data));

  AdvertDataBuilder builder = o.has_loc
      ? AdvertDataBuilder(o.adv_type, o.name, o.lat, o.lon)
      : AdvertDataBuilder(o.adv_type, o.name);
  uint8_t app_data_len = builder.encodeTo(app_data);

  /* AdvertDataBuilder silently drops a name that does not fit what is left of
   * the 32 app-data bytes, and stops at the last whole UTF-8 sequence. Silent
   * truncation is right for a node advertising itself; for a tool asked to
   * produce an exact frame it is not, so the shortfall is reported instead. */
  if (o.name != NULL && o.name[0] != 0) {
    size_t want = strlen(o.name);
    size_t fixed = 1 + (o.has_loc ? 8u : 0u);
    if (want > MAX_ADVERT_DATA_SIZE - fixed) {
      snprintf(err, MCF_ERR_SIZE,
               "--name is %zu bytes; %zu fit beside the other advert data",
               want, MAX_ADVERT_DATA_SIZE - fixed);
      return false;
    }
    if ((size_t)app_data_len != fixed + want) {
      snprintf(err, MCF_ERR_SIZE,
               "--name is not valid UTF-8; MeshCore would send %zu of its %zu bytes",
               (size_t)app_data_len - fixed, want);
      return false;
    }
  }

  if (app_data_len > MAX_ADVERT_DATA_SIZE) {
    snprintf(err, MCF_ERR_SIZE, "advert data is %u bytes; the limit is %d",
             (unsigned)app_data_len, MAX_ADVERT_DATA_SIZE);
    return false;
  }

  /* Mesh::createAdvert, byte for byte. */
  pkt.header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT);

  int len = 0;
  memcpy(&pkt.payload[len], id.pub_key, PUB_KEY_SIZE); len += PUB_KEY_SIZE;

  uint32_t emitted_timestamp = o.timestamp;
  memcpy(&pkt.payload[len], &emitted_timestamp, 4); len += 4;

  uint8_t* signature = &pkt.payload[len]; len += SIGNATURE_SIZE;

  memcpy(&pkt.payload[len], app_data, app_data_len); len += app_data_len;

  pkt.payload_len = len;

  {
    uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
    int msg_len = 0;
    memcpy(&message[msg_len], id.pub_key, PUB_KEY_SIZE); msg_len += PUB_KEY_SIZE;
    memcpy(&message[msg_len], &emitted_timestamp, 4); msg_len += 4;
    memcpy(&message[msg_len], app_data, app_data_len); msg_len += app_data_len;

    id.sign(signature, message, msg_len);
  }
  return true;
}

bool advertVerify(const mesh::Packet& pkt, mesh::Identity* id_out,
                  uint32_t* timestamp_out, const uint8_t** app_data_out,
                  int* app_data_len_out) {
  /* Mesh::onRecvPacket, the PAYLOAD_TYPE_ADVERT case. */
  int i = 0;
  mesh::Identity id;
  if (pkt.payload_len < PUB_KEY_SIZE + 4 + SIGNATURE_SIZE) return false;

  memcpy(id.pub_key, &pkt.payload[i], PUB_KEY_SIZE); i += PUB_KEY_SIZE;

  uint32_t timestamp;
  memcpy(&timestamp, &pkt.payload[i], 4); i += 4;
  const uint8_t* signature = &pkt.payload[i]; i += SIGNATURE_SIZE;

  const uint8_t* app_data = &pkt.payload[i];
  int app_data_len = pkt.payload_len - i;
  if (app_data_len > MAX_ADVERT_DATA_SIZE) app_data_len = MAX_ADVERT_DATA_SIZE;

  uint8_t message[PUB_KEY_SIZE + 4 + MAX_ADVERT_DATA_SIZE];
  int msg_len = 0;
  memcpy(&message[msg_len], id.pub_key, PUB_KEY_SIZE); msg_len += PUB_KEY_SIZE;
  memcpy(&message[msg_len], &timestamp, 4); msg_len += 4;
  memcpy(&message[msg_len], app_data, app_data_len); msg_len += app_data_len;

  if (id_out) *id_out = id;
  if (timestamp_out) *timestamp_out = timestamp;
  if (app_data_out) *app_data_out = app_data;
  if (app_data_len_out) *app_data_len_out = app_data_len;

  return id.verify(signature, message, msg_len);
}

/* ---- TXT_MSG ----------------------------------------------------------- */

bool buildTxtMsg(mesh::Packet& pkt, const mesh::LocalIdentity& id,
                 const TxtMsgOpts& o, uint32_t* expected_ack, char* err) {
  size_t text_len = strlen(o.text);
  if (text_len > MCF_MAX_TEXT_LEN) {
    snprintf(err, MCF_ERR_SIZE, "--text is %zu bytes; the MeshCore limit is %d",
             text_len, MCF_MAX_TEXT_LEN);
    return false;
  }
  if (o.attempt > 3 && text_len > MCF_MAX_TEXT_LEN - 2) {
    snprintf(err, MCF_ERR_SIZE, "--text is %zu bytes; an attempt above 3 spends two of the %d",
             text_len, MCF_MAX_TEXT_LEN);
    return false;
  }
  if (o.txt_type > 63) {
    snprintf(err, MCF_ERR_SIZE, "--txt-type must fit the six bits above the attempt number");
    return false;
  }

  /* BaseChatMesh::composeMsgPacket. */
  uint8_t temp[5 + MCF_MAX_TEXT_LEN + 2];
  memcpy(temp, &o.timestamp, 4);
  temp[4] = (uint8_t)((o.attempt & 3) | (o.txt_type << 2));
  memcpy(&temp[5], o.text, text_len);

  mesh::Utils::sha256((uint8_t*)expected_ack, 4, temp, (int)(5 + text_len),
                      id.pub_key, PUB_KEY_SIZE);

  size_t len = 5 + text_len;
  if (o.attempt > 3) {
    temp[len++] = 0;            /* the null terminator the reader stops at */
    temp[len++] = o.attempt;    /* attempt number hidden past it */
  }

  /* Mesh::createDatagram, the PAYLOAD_TYPE_TXT_MSG case. */
  if (len + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE - 1 > MAX_PACKET_PAYLOAD) {
    snprintf(err, MCF_ERR_SIZE, "the encrypted message would not fit %d payload bytes",
             MAX_PACKET_PAYLOAD);
    return false;
  }

  mesh::Identity dest(o.dest_pub_key);
  uint8_t secret[PUB_KEY_SIZE];
  id.calcSharedSecret(secret, dest);

  pkt.header = (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT);

  int plen = 0;
  plen += dest.copyHashTo(&pkt.payload[plen]);
  plen += id.copyHashTo(&pkt.payload[plen]);
  plen += mesh::Utils::encryptThenMAC(secret, &pkt.payload[plen], temp, (int)len);
  pkt.payload_len = plen;
  return true;
}

/* ---- ACK --------------------------------------------------------------- */

bool buildAck(mesh::Packet& pkt, const uint8_t* ack, size_t ack_len, char* err) {
  if (ack_len == 0 || ack_len > MAX_PACKET_PAYLOAD) {
    snprintf(err, MCF_ERR_SIZE,
             "an ACK payload is 1 to %d bytes (MeshCore sends 4, or 6 in a multi-ack)",
             MAX_PACKET_PAYLOAD);
    return false;
  }
  /* Mesh::createAck. */
  pkt.header = (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  memcpy(pkt.payload, ack, ack_len);
  pkt.payload_len = ack_len;
  return true;
}

/* ---- frame in and out -------------------------------------------------- */

bool encode(const mesh::Packet& pkt, uint8_t* out, size_t* out_len, char* err) {
  if (pkt.payload_len > MAX_PACKET_PAYLOAD) {
    snprintf(err, MCF_ERR_SIZE, "payload is %u bytes; the limit is %d",
             (unsigned)pkt.payload_len, MAX_PACKET_PAYLOAD);
    return false;
  }
  if (pkt.path_len > 255 || !mesh::Packet::isValidPathLen((uint8_t)pkt.path_len)) {
    snprintf(err, MCF_ERR_SIZE, "path_len 0x%02x does not encode a path MeshCore accepts",
             (unsigned)pkt.path_len);
    return false;
  }
  int raw = pkt.getRawLength();
  if (raw > MAX_TRANS_UNIT) {
    /* Packet::writeTo returns the length in a uint8_t, so past the MTU it
     * does not merely overrun the air-time budget: it reports a length that
     * wrapped. Refuse before that. */
    snprintf(err, MCF_ERR_SIZE, "frame is %d bytes; the MTU is %d", raw, MAX_TRANS_UNIT);
    return false;
  }
  uint8_t written = pkt.writeTo(out);
  if ((int)written != raw) {
    snprintf(err, MCF_ERR_SIZE, "frame encoded to %u bytes, expected %d",
             (unsigned)written, raw);
    return false;
  }
  *out_len = written;
  return true;
}

bool decode(mesh::Packet& pkt, const uint8_t* raw, size_t len, char* err) {
  /* mesh::Packet::readFrom trusts its input: it copies the path bytes without
   * checking them against the frame length, and it takes that length in a
   * uint8_t. This tool is fed hex from a command line and from the air, so
   * the layout is bounds-checked here first, in the order readFrom walks it,
   * and nothing is handed over that readFrom could read past. */
  if (len < 1) {
    snprintf(err, MCF_ERR_SIZE, "empty frame");
    return false;
  }
  if (len > MAX_TRANS_UNIT) {
    snprintf(err, MCF_ERR_SIZE, "frame is %zu bytes; the MTU is %d", len, MAX_TRANS_UNIT);
    return false;
  }

  uint8_t header = raw[0];
  uint8_t route = header & PH_ROUTE_MASK;
  bool transport = (route == ROUTE_TYPE_TRANSPORT_FLOOD || route == ROUTE_TYPE_TRANSPORT_DIRECT);

  size_t i = 1 + (transport ? 4u : 0u);
  if (len < i + 1) {
    snprintf(err, MCF_ERR_SIZE,
             "frame is %zu bytes; %zu are needed for the header%s and path_len",
             len, i + 1, transport ? ", transport codes" : "");
    return false;
  }

  uint8_t path_len = raw[i];
  i += 1;
  if (!mesh::Packet::isValidPathLen(path_len)) {
    snprintf(err, MCF_ERR_SIZE,
             "path_len 0x%02x is %u hashes of %u bytes, which MeshCore refuses",
             (unsigned)path_len, (unsigned)mesh::Packet::pathHashCount(path_len),
             (unsigned)mesh::Packet::pathHashSize(path_len));
    return false;
  }

  size_t path_bytes = (size_t)mesh::Packet::pathHashCount(path_len) *
                      (size_t)mesh::Packet::pathHashSize(path_len);
  if (len < i + path_bytes) {
    snprintf(err, MCF_ERR_SIZE,
             "frame is %zu bytes; the path alone needs %zu from offset %zu",
             len, path_bytes, i);
    return false;
  }
  i += path_bytes;

  if (len <= i) {
    snprintf(err, MCF_ERR_SIZE, "frame has no payload after its %zu-byte header and path", i);
    return false;
  }
  if (len - i > MAX_PACKET_PAYLOAD) {
    snprintf(err, MCF_ERR_SIZE, "payload is %zu bytes; the limit is %d",
             len - i, MAX_PACKET_PAYLOAD);
    return false;
  }

  if (!pkt.readFrom(raw, (uint8_t)len)) {
    snprintf(err, MCF_ERR_SIZE, "MeshCore refused the frame");
    return false;
  }
  return true;
}

}  /* namespace mcf */
