/*
 * peerd wire format, version 1 (docs/decisions/ADR-017, docs/api/peer.md).
 *
 * Pure parsing and building of frames: no crypto, no sockets, no clock. It
 * is the first code untrusted bytes from the network reach, so it only ever
 * reads inside the buffer it was given and refuses anything whose length is
 * not exactly right for its type (tests/peer_wire_test.c, tests/peer_fuzz.c).
 *
 * Every frame starts with the same 58 bytes, all of them authenticated - by
 * the AEAD's associated data for DATA, by the signature for the rest:
 *
 *   off  size
 *   0    1     wire version (1)
 *   1    1     type
 *   2    32    sender's Ed25519 public key
 *   34   8     recipient key prefix (DATA and the two replies; else zero)
 *   42   8     epoch, big-endian
 *   50   8     sequence, big-endian
 *
 * DATA continues with a 24-byte nonce and the sealed  port(1) | payload.
 *
 * The five signed frames (BEACON, PROBE, PROBE_REPLY, LOOKUP, LOOKUP_REPLY)
 * all have one fixed size, 210 bytes, so a reply is never larger than the
 * request that drew it:
 *
 *   58   4     the sender's own IPv4 address on the interface it sends from
 *   62   2     the UDP port it listens on, big-endian
 *   64   1     flags (bit 0: a table is announced)
 *   65   1     name length, 0..24
 *   66   24    name, UTF-8, zero padded
 *   90   8     table: port, proto_min, proto_max, seats, tag(4)
 *   98   16    challenge (PROBE and LOOKUP: the asker's; replies: echoed)
 *   114  32    target key (LOOKUP: the key sought; else zero)
 *   146  64    Ed25519 signature over "DOORS-PEERD-1" | bytes 0..145
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PEERD_WIRE_H
#define PEERD_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define PW_VERSION 1
#define PW_KEY 32
#define PW_PREFIX 8
#define PW_HEADER 58
#define PW_NONCE 24
#define PW_TAG 16
#define PW_SIG 64
#define PW_NAME_MAX 24
#define PW_CHALLENGE 16
#define PW_PAYLOAD_MAX 1024
#define PW_PORT_MIN 1
#define PW_PORT_MAX 15

#define PW_SIGNED_BODY 146              /* what the signature covers */
#define PW_SIGNED_SIZE (PW_SIGNED_BODY + PW_SIG)                    /* 210 */
#define PW_DATA_MIN (PW_HEADER + PW_NONCE + 1 + 1 + PW_TAG)        /* 100 */
#define PW_DATA_MAX (PW_HEADER + PW_NONCE + 1 + PW_PAYLOAD_MAX + PW_TAG) /* 1123 */
#define PW_FRAME_MAX PW_DATA_MAX

#define PW_SIGN_PREFIX "DOORS-PEERD-1"
#define PW_SIGN_PREFIX_LEN 13

enum pw_type {
    PW_BEACON = 1,
    PW_PROBE = 2,
    PW_DATA = 3,
    PW_PROBE_REPLY = 4,
    PW_LOOKUP = 5,
    PW_LOOKUP_REPLY = 6,
};

#define PW_FLAG_TABLE 0x01

struct pw_header {
    uint8_t type;
    uint8_t sender[PW_KEY];
    uint8_t recipient[PW_PREFIX];
    uint64_t epoch;
    uint64_t seq;
};

struct pw_table {
    uint8_t port;
    uint8_t proto_min;
    uint8_t proto_max;
    uint8_t seats;
    uint32_t tag;
};

struct pw_signed {
    struct pw_header h;
    uint32_t addr;                      /* host byte order */
    uint16_t port;
    uint8_t flags;
    uint8_t name_len;
    char name[PW_NAME_MAX + 1];         /* NUL terminated by the parser */
    struct pw_table table;
    uint8_t challenge[PW_CHALLENGE];
    uint8_t target[PW_KEY];
};

/* Why a frame was refused; also the index of a drop counter. */
enum pw_error {
    PW_OK = 0,
    PW_E_SHORT = -1,        /* under the common header */
    PW_E_VERSION = -2,
    PW_E_TYPE = -3,
    PW_E_LENGTH = -4,       /* not the size its type has */
    PW_E_FIELD = -5,        /* a field out of range: zero epoch or sequence, a bad name, ... */
};

/* The common header. Returns PW_OK and fills *h, or an error. Checks the
 * version, the type, that the length is one its type can have, and that
 * epoch and sequence are not zero. */
int pw_parse_header(const uint8_t *buf, size_t n, struct pw_header *h);

/* A whole signed frame (n must be PW_SIGNED_SIZE and the type a signed one).
 * Checks every field's range and that the name is clean text. The signature
 * is NOT checked here: it is bytes buf[PW_SIGNED_BODY..] over buf[0..]. */
int pw_parse_signed(const uint8_t *buf, size_t n, struct pw_signed *out);

/* Build the first PW_SIGNED_BODY bytes of a signed frame into buf (at least
 * PW_SIGNED_SIZE long); the caller appends the signature. Returns PW_OK, or
 * PW_E_FIELD when *f could not be parsed back. */
int pw_build_signed(const struct pw_signed *f, uint8_t *buf);

/* Build the header of a DATA frame (PW_HEADER bytes). */
void pw_build_header(const struct pw_header *h, uint8_t *buf);

/* 1 when name[0..len) is well-formed UTF-8 with no control character
 * (C0, DEL, C1), no overlong form and no surrogate; len at most PW_NAME_MAX. */
int pw_name_ok(const uint8_t *name, size_t len);

uint64_t pw_get64(const uint8_t *p);
void pw_put64(uint8_t *p, uint64_t v);

#endif
