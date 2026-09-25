/*
 * PocketFleet multiplayer wire format, protocol v1
 * (docs/apps/FLEET_MULTIPLAYER.md, "Packet format").
 *
 * A packet is what Fleet hands to the transport and gets back from it:
 *
 *   vt(1) | sid(3, big-endian) | ply(1) | body
 *
 * The ply byte carries the flags in COMMIT and REVEAL, which have no ply:
 * that keeps REVEAL to two AES blocks in the MeshCore frame.
 *
 * vt is the version in bits 7-6 and the type in bits 5-0. Every type has
 * exactly one length (DECLINE has two), and decoding refuses anything else:
 * a packet that is one byte short or one byte long is not "nearly" a
 * packet, it is somebody else's bytes. Every enum, cell and flag is checked
 * here, so the state machine only ever sees values it can act on.
 *
 * Pure C, no I/O.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETFLEET_PROTO_H
#define POCKETFLEET_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define FLEET_PROTO_VERSION 1
#define FLEET_PROTO_HEADER 5
/* The longest packet (REVEAL). meshcored takes up to 160. */
#define FLEET_PROTO_MAX 26
/* One AES block in the MeshCore frame, after its 4-byte tag and meshcored's
 * two (port and length): a packet this long or shorter is a 22-byte frame,
 * and up to FLEET_PROTO_MAX a 38-byte one. */
#define FLEET_PROTO_ONE_BLOCK 10
#define FLEET_PROTO_PLY_MAX 200
#define FLEET_COMMIT_BYTES 16
#define FLEET_SALT_BYTES 16
#define FLEET_LAYOUT_BYTES 5
#define FLEET_KEY_BYTES 32
#define FLEET_NO_CELL 0xFF

enum fleet_msg_type {
    FLEET_MSG_NONE = 0,
    FLEET_MSG_INVITE = 1,
    FLEET_MSG_ACCEPT = 2,
    FLEET_MSG_DECLINE = 3,
    FLEET_MSG_START = 4,
    FLEET_MSG_CANCEL = 5,
    FLEET_MSG_COMMIT = 6,
    FLEET_MSG_SHOT = 7,
    FLEET_MSG_RESULT = 8,
    FLEET_MSG_SYNC = 9,
    FLEET_MSG_REVEAL = 10,
    FLEET_MSG_END = 11,
    FLEET_MSG_END_ACK = 12,
    FLEET_MSG_TYPE_COUNT
};

enum fleet_decline_reason {
    FLEET_DECLINE_USER = 0,
    FLEET_DECLINE_BUSY = 1,
    FLEET_DECLINE_VERSION = 2,
    FLEET_DECLINE_RULES = 3,
    FLEET_DECLINE_BUSY_WITH_YOU = 4,
    FLEET_DECLINE_REASON_COUNT
};

enum fleet_end_reason {
    FLEET_END_NONE = 0,
    FLEET_END_FORFEIT = 1,
    FLEET_END_VOID = 2,
    FLEET_END_ABANDON = 3,
    FLEET_END_CANCELLED = 4,
    FLEET_END_UNKNOWN = 5,
    FLEET_END_VIOLATION = 6,
    FLEET_END_FINISHED = 7,
    FLEET_END_REASON_COUNT
};

/* SYNC phase codes. */
enum fleet_sync_phase {
    FLEET_SYNC_NONE = 0,
    FLEET_SYNC_PRESTART = 1,
    FLEET_SYNC_DEPLOY = 2,
    FLEET_SYNC_COMMITTED = 3,
    FLEET_SYNC_BATTLE = 4,
    FLEET_SYNC_REVEAL = 5,
    FLEET_SYNC_DONE = 6,
    FLEET_SYNC_PHASE_COUNT
};

#define FLEET_RULES_CLASSIC 0

/* Flag bits. */
#define FLEET_FLAG_HAVE_PEER 0x01          /* COMMIT, REVEAL (in the ply byte): I hold yours */
#define FLEET_SYNC_REPLY 0x01
#define FLEET_SYNC_HAVE_COMMIT 0x02
#define FLEET_SYNC_HAVE_REVEAL 0x04
#define FLEET_SYNC_PHASE_SHIFT 3

/* res byte */
#define FLEET_RES_OUTCOME_MASK 0x03
#define FLEET_RES_SHIP_SHIFT 2
#define FLEET_RES_SHIP_NONE 7
#define FLEET_RES_DESTROYED 0x20

/* A decoded packet. Only the fields of its type are meaningful. */
struct fleet_msg {
    uint8_t type;                         /* enum fleet_msg_type */
    uint32_t sid;                         /* 24 bits, non-zero */
    uint8_t ply;
    uint8_t rules;                        /* INVITE */
    uint8_t reason;                       /* DECLINE, END, END_ACK */
    uint32_t other_sid;                   /* DECLINE busy-with-you */
    uint8_t flags;                        /* COMMIT, SYNC, REVEAL (COMMIT, REVEAL: the ply byte) */
    uint8_t commit[FLEET_COMMIT_BYTES];   /* COMMIT */
    uint8_t cell;                         /* SHOT, RESULT; SYNC pending */
    uint8_t res;                          /* RESULT; SHOT prev */
    uint32_t digest;                      /* SYNC */
    uint8_t layout[FLEET_LAYOUT_BYTES];   /* REVEAL */
    uint8_t salt[FLEET_SALT_BYTES];       /* REVEAL */
};

/* Encode. Returns the length written, or -1 when the message is not one this
 * codec would decode again (bad type, field or length) or n is too small. */
int fleet_proto_encode(const struct fleet_msg *m, uint8_t *buf, size_t n);
/* Decode. Returns 0, or -1 and leaves *m zeroed for anything malformed. */
int fleet_proto_decode(struct fleet_msg *m, const uint8_t *buf, size_t n);
/* The exact length of a packet of this type (DECLINE: without the extra
 * sid), or 0 for an unknown type. */
size_t fleet_proto_length(enum fleet_msg_type type);
const char *fleet_proto_type_name(enum fleet_msg_type type);

/* ---- field helpers ------------------------------------------------------ */

/* res = outcome | ship | destroyed. outcome is enum fleet_shot_result's
 * MISS/HIT/SUNK (1..3). Returns 0 for a combination that is not valid. */
uint8_t fleet_res_make(int outcome, int ship, int destroyed);
int fleet_res_outcome(uint8_t res);        /* 0..3 */
int fleet_res_ship(uint8_t res);           /* 0..4, or -1 */
int fleet_res_destroyed(uint8_t res);
/* 1 when res is a valid answer (outcome 1..3 and consistent fields). */
int fleet_res_valid(uint8_t res);

/* Airtime of the MeshCore frame a packet of n bytes becomes, at zero hops, on
 * the MeshCore profile: 304 ms for one block, 386 ms for two. */
uint32_t fleet_proto_airtime_ms(size_t n);

#endif
