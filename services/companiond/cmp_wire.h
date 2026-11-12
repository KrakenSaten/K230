/*
 * The MeshCore companion protocol on a TCP stream: its framing, and the
 * byte layouts companiond answers with.
 *
 * Reference, pinned: meshcore-dev/MeshCore at
 * 81dbae592ffad40b4212bc0b37e82b38da08f4bc (companion firmware v1.18.0,
 * FIRMWARE_VER_CODE 14):
 *   src/helpers/wifi/SerialWifiInterface.cpp   the TCP framing
 *   src/helpers/BaseSerialInterface.h          MAX_FRAME_SIZE 176
 *   examples/companion_radio/MyMesh.cpp        commands, replies, pushes
 * docs/services/COMPANIOND.md says what is and is not implemented.
 *
 * Framing (SerialWifiInterface.cpp, identical to the USB serial one): every
 * frame is one type byte, a 16-bit little-endian length and that many bytes.
 * The app writes '<' (0x3c) frames, the radio writes '>' (0x3e) frames. The
 * firmware skips a frame of another type and a frame longer than
 * MAX_FRAME_SIZE by reading past its length; a length of zero is not a frame
 * there at all. This decoder does the same and never buffers more than one
 * frame of MAX_FRAME_SIZE bytes, however long the length a peer announces.
 *
 * Pure: no I/O, no allocation. tests/companiond_wire_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CMP_WIRE_H
#define CMP_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define CMP_MAX_FRAME 176      /* upstream MAX_FRAME_SIZE */
#define CMP_HDR_LEN 3
#define CMP_TYPE_FROM_APP 0x3c /* '<' */
#define CMP_TYPE_TO_APP 0x3e   /* '>' */

/* What upstream's firmware says it is (MyMesh.h at the pinned commit). Every
 * reply layout below is that commit's, so the version code is too. */
#define CMP_FIRMWARE_VER_CODE 14
#define CMP_PUB_KEY_SIZE 32
#define CMP_MAX_PATH_SIZE 64
#define CMP_OUT_PATH_UNKNOWN 0xFF

/* Commands (app to radio), MyMesh.cpp. Only the ones companiond names. */
enum {
    CMP_CMD_APP_START = 1,
    CMP_CMD_SEND_TXT_MSG = 2,
    CMP_CMD_SEND_CHANNEL_TXT_MSG = 3,
    CMP_CMD_GET_CONTACTS = 4,
    CMP_CMD_GET_DEVICE_TIME = 5,
    CMP_CMD_SET_DEVICE_TIME = 6,
    CMP_CMD_SYNC_NEXT_MESSAGE = 10,
    CMP_CMD_DEVICE_QUERY = 22,
    CMP_CMD_EXPORT_PRIVATE_KEY = 23,
    CMP_CMD_IMPORT_PRIVATE_KEY = 24,
    CMP_CMD_GET_CONTACT_BY_KEY = 30,
    CMP_CMD_GET_CHANNEL = 31,
    CMP_CMD_GET_DEFAULT_FLOOD_SCOPE = 64,
};

/* Replies and pushes (radio to app). */
enum {
    CMP_RESP_OK = 0,
    CMP_RESP_ERR = 1,
    CMP_RESP_CONTACTS_START = 2,
    CMP_RESP_CONTACT = 3,
    CMP_RESP_END_OF_CONTACTS = 4,
    CMP_RESP_SELF_INFO = 5,
    CMP_RESP_SENT = 6,
    CMP_RESP_CONTACT_MSG_RECV = 7,
    CMP_RESP_CHANNEL_MSG_RECV = 8,
    CMP_RESP_CURR_TIME = 9,
    CMP_RESP_NO_MORE_MESSAGES = 10,
    CMP_RESP_DEVICE_INFO = 13,
    CMP_RESP_DISABLED = 15,
    CMP_RESP_CONTACT_MSG_RECV_V3 = 16,
    CMP_RESP_CHANNEL_MSG_RECV_V3 = 17,
    CMP_RESP_CHANNEL_INFO = 18,
    CMP_RESP_DEFAULT_FLOOD_SCOPE = 28,

    CMP_PUSH_ADVERT = 0x80,
    CMP_PUSH_PATH_UPDATED = 0x81,
    CMP_PUSH_SEND_CONFIRMED = 0x82,
    CMP_PUSH_MSG_WAITING = 0x83,
    CMP_PUSH_CONTACT_DELETED = 0x8F,
};

enum {
    CMP_ERR_UNSUPPORTED_CMD = 1,
    CMP_ERR_NOT_FOUND = 2,
    CMP_ERR_TABLE_FULL = 3,
    CMP_ERR_BAD_STATE = 4,
    CMP_ERR_ILLEGAL_ARG = 6,
};

#define CMP_TXT_TYPE_PLAIN 0

/* ---- inbound framing ---------------------------------------------------- */

struct cmp_decoder {
    uint8_t hdr[CMP_HDR_LEN];
    size_t hdr_len;
    size_t want;       /* bytes of the current frame still to come */
    int skipping;      /* current frame is being read past, not kept */
    uint8_t buf[CMP_MAX_FRAME];
    size_t len;        /* bytes of the current frame kept so far */
    /* Counters, for the log. */
    uint32_t frames;
    uint32_t skipped_type;
    uint32_t skipped_size;
    uint32_t empty;
};

void cmp_decoder_init(struct cmp_decoder *d);

/* Consume bytes from data[0..n). Stops as soon as one whole '<' frame is
 * complete and returns how many bytes it consumed; *frame_len is then the
 * frame's length and the frame is in d->buf until the next call. When no
 * frame completed, every byte was consumed and *frame_len is 0. A caller
 * loops until it has handed over all of its bytes. */
size_t cmp_decoder_feed(struct cmp_decoder *d, const uint8_t *data, size_t n,
                        size_t *frame_len);

/* ---- outbound framing --------------------------------------------------- */

/* Writes '>' + LE16 length + payload into out (capacity cap). Returns the
 * bytes written, or 0 when the payload is empty, longer than CMP_MAX_FRAME or
 * would not fit - the firmware refuses such a frame too (writeFrame). */
size_t cmp_frame_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t cap);

/* ---- little helpers shared by the bridge --------------------------------- */

static inline void cmp_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void cmp_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t cmp_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Length of the longest prefix of s[0..n) that is whole, well-formed UTF-8
 * and at most max bytes: a cut never lands inside a character. */
size_t cmp_utf8_prefix(const char *s, size_t n, size_t max);

/* 1 when s[0..n) is well-formed UTF-8 with no NUL and no control character
 * other than newline and tab: what meshcored's mesh.send will take, and what
 * can go into a JSON string without changing. */
int cmp_text_ok(const uint8_t *s, size_t n);

/* Lower-case hex of n bytes into out (2n+1). */
void cmp_hex(const uint8_t *in, size_t n, char *out);
/* Parse exactly 2n hex characters. Returns 0 or -1. */
int cmp_unhex(const char *in, uint8_t *out, size_t n);

#endif
