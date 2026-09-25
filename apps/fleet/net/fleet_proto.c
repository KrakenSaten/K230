/*
 * PocketFleet multiplayer wire format. See fleet_proto.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_proto.h"

#include <string.h>

#define OUTCOME_MISS 1
#define OUTCOME_HIT 2
#define OUTCOME_SUNK 3
#define SHIPS 5
#define CELLS 100

/* Body length per type; DECLINE's longer form is handled on its own. */
static const uint8_t body_len[FLEET_MSG_TYPE_COUNT] = {
    [FLEET_MSG_INVITE] = 1,
    [FLEET_MSG_ACCEPT] = 0,
    [FLEET_MSG_DECLINE] = 1,
    [FLEET_MSG_START] = 0,
    [FLEET_MSG_CANCEL] = 0,
    [FLEET_MSG_COMMIT] = FLEET_COMMIT_BYTES,
    [FLEET_MSG_SHOT] = 2,
    [FLEET_MSG_RESULT] = 2,
    [FLEET_MSG_SYNC] = 6,
    [FLEET_MSG_REVEAL] = FLEET_LAYOUT_BYTES + FLEET_SALT_BYTES,
    [FLEET_MSG_END] = 1,
    [FLEET_MSG_END_ACK] = 1,
};

size_t fleet_proto_length(enum fleet_msg_type type)
{
    if (type <= FLEET_MSG_NONE || type >= FLEET_MSG_TYPE_COUNT) {
        return 0;
    }
    return FLEET_PROTO_HEADER + body_len[type];
}

const char *fleet_proto_type_name(enum fleet_msg_type type)
{
    static const char *names[FLEET_MSG_TYPE_COUNT] = {
        "NONE", "INVITE", "ACCEPT", "DECLINE", "START", "CANCEL", "COMMIT",
        "SHOT", "RESULT", "SYNC", "REVEAL", "END", "END_ACK",
    };

    if ((unsigned)type >= FLEET_MSG_TYPE_COUNT) {
        return "?";
    }
    return names[type];
}

uint8_t fleet_res_make(int outcome, int ship, int destroyed)
{
    uint8_t res;

    if (outcome < OUTCOME_MISS || outcome > OUTCOME_SUNK) {
        return 0;
    }
    if (outcome == OUTCOME_SUNK) {
        if (ship < 0 || ship >= SHIPS) {
            return 0;
        }
    } else {
        if (destroyed) {
            return 0;
        }
        ship = FLEET_RES_SHIP_NONE;
    }
    res = (uint8_t)(outcome | (ship << FLEET_RES_SHIP_SHIFT));
    if (destroyed) {
        res |= FLEET_RES_DESTROYED;
    }
    return res;
}

int fleet_res_outcome(uint8_t res)
{
    return res & FLEET_RES_OUTCOME_MASK;
}

int fleet_res_ship(uint8_t res)
{
    int ship = (res >> FLEET_RES_SHIP_SHIFT) & 7;

    return ship < SHIPS ? ship : -1;
}

int fleet_res_destroyed(uint8_t res)
{
    return (res & FLEET_RES_DESTROYED) != 0;
}

int fleet_res_valid(uint8_t res)
{
    int outcome = fleet_res_outcome(res);
    int ship = (res >> FLEET_RES_SHIP_SHIFT) & 7;

    if (res & 0xC0) {
        return 0;
    }
    if (outcome == OUTCOME_SUNK) {
        return ship < SHIPS;
    }
    if (outcome == OUTCOME_MISS || outcome == OUTCOME_HIT) {
        return ship == FLEET_RES_SHIP_NONE && !fleet_res_destroyed(res);
    }
    return 0;
}

static int sid_ok(uint32_t sid)
{
    return sid != 0 && sid <= 0xFFFFFF;
}

static int layout_byte_ok(uint8_t b)
{
    return (b & 0x7F) < CELLS;
}

/* The checks shared by encode and decode: what a well-formed message of its
 * type may hold. */
static int fields_ok(const struct fleet_msg *m, size_t len)
{
    int i;

    if (!sid_ok(m->sid)) {
        return 0;
    }
    switch (m->type) {
    case FLEET_MSG_INVITE:
        return m->ply == 0;
    case FLEET_MSG_ACCEPT:
    case FLEET_MSG_START:
    case FLEET_MSG_CANCEL:
        return m->ply == 0;
    case FLEET_MSG_DECLINE:
        if (m->ply != 0 || m->reason >= FLEET_DECLINE_REASON_COUNT) {
            return 0;
        }
        /* The long form exactly when the reason needs it. */
        if (m->reason == FLEET_DECLINE_BUSY_WITH_YOU) {
            return len == fleet_proto_length(FLEET_MSG_DECLINE) + 3 && sid_ok(m->other_sid);
        }
        return len == fleet_proto_length(FLEET_MSG_DECLINE);
    case FLEET_MSG_COMMIT:
        return (m->flags & ~FLEET_FLAG_HAVE_PEER) == 0;
    case FLEET_MSG_SHOT:
        if (m->ply < 1 || m->ply > FLEET_PROTO_PLY_MAX || m->cell >= CELLS) {
            return 0;
        }
        /* The first shot has no ply before it; every later one carries its
         * answer, and that answer cannot end the match (it would be over). */
        if (m->ply == 1) {
            return m->res == 0;
        }
        return fleet_res_valid(m->res) && !fleet_res_destroyed(m->res);
    case FLEET_MSG_RESULT:
        return m->ply >= 1 && m->ply <= FLEET_PROTO_PLY_MAX && m->cell < CELLS &&
               fleet_res_valid(m->res);
    case FLEET_MSG_SYNC: {
        int phase = (m->flags >> FLEET_SYNC_PHASE_SHIFT) & 7;

        if (m->ply > FLEET_PROTO_PLY_MAX || (m->flags & 0xC0) || phase >= FLEET_SYNC_PHASE_COUNT) {
            return 0;
        }
        return m->cell < CELLS || m->cell == FLEET_NO_CELL;
    }
    case FLEET_MSG_REVEAL:
        if ((m->flags & ~FLEET_FLAG_HAVE_PEER) != 0) {
            return 0;
        }
        for (i = 0; i < FLEET_LAYOUT_BYTES; i++) {
            if (!layout_byte_ok(m->layout[i])) {
                return 0;
            }
        }
        return 1;
    case FLEET_MSG_END:
    case FLEET_MSG_END_ACK:
        return m->ply <= FLEET_PROTO_PLY_MAX && m->reason >= FLEET_END_FORFEIT &&
               m->reason < FLEET_END_REASON_COUNT;
    default:
        return 0;
    }
}

static size_t length_for(const struct fleet_msg *m)
{
    size_t len = fleet_proto_length((enum fleet_msg_type)m->type);

    if (m->type == FLEET_MSG_DECLINE && m->reason == FLEET_DECLINE_BUSY_WITH_YOU) {
        len += 3;
    }
    return len;
}

int fleet_proto_encode(const struct fleet_msg *m, uint8_t *buf, size_t n)
{
    size_t len;
    uint8_t *b;

    if (!m || !buf) {
        return -1;
    }
    len = length_for(m);
    if (len == 0 || n < len || !fields_ok(m, len)) {
        return -1;
    }
    buf[0] = (uint8_t)((FLEET_PROTO_VERSION << 6) | m->type);
    buf[1] = (uint8_t)(m->sid >> 16);
    buf[2] = (uint8_t)(m->sid >> 8);
    buf[3] = (uint8_t)m->sid;
    buf[4] = (m->type == FLEET_MSG_COMMIT || m->type == FLEET_MSG_REVEAL) ? m->flags : m->ply;
    b = buf + FLEET_PROTO_HEADER;
    switch (m->type) {
    case FLEET_MSG_INVITE:
        b[0] = m->rules;
        break;
    case FLEET_MSG_DECLINE:
        b[0] = m->reason;
        if (m->reason == FLEET_DECLINE_BUSY_WITH_YOU) {
            b[1] = (uint8_t)(m->other_sid >> 16);
            b[2] = (uint8_t)(m->other_sid >> 8);
            b[3] = (uint8_t)m->other_sid;
        }
        break;
    case FLEET_MSG_COMMIT:
        memcpy(b, m->commit, FLEET_COMMIT_BYTES);
        break;
    case FLEET_MSG_SHOT:
    case FLEET_MSG_RESULT:
        b[0] = m->cell;
        b[1] = m->res;
        break;
    case FLEET_MSG_SYNC:
        b[0] = m->flags;
        b[1] = m->cell;
        b[2] = (uint8_t)(m->digest >> 24);
        b[3] = (uint8_t)(m->digest >> 16);
        b[4] = (uint8_t)(m->digest >> 8);
        b[5] = (uint8_t)m->digest;
        break;
    case FLEET_MSG_REVEAL:
        memcpy(b, m->layout, FLEET_LAYOUT_BYTES);
        memcpy(b + FLEET_LAYOUT_BYTES, m->salt, FLEET_SALT_BYTES);
        break;
    case FLEET_MSG_END:
    case FLEET_MSG_END_ACK:
        b[0] = m->reason;
        break;
    default:
        break;
    }
    return (int)len;
}

int fleet_proto_decode(struct fleet_msg *m, const uint8_t *buf, size_t n)
{
    struct fleet_msg d;
    const uint8_t *b;
    size_t want;

    if (!m) {
        return -1;
    }
    memset(m, 0, sizeof(*m));
    if (!buf || n < FLEET_PROTO_HEADER || (buf[0] >> 6) != FLEET_PROTO_VERSION) {
        return -1;
    }
    memset(&d, 0, sizeof(d));
    d.type = buf[0] & 0x3F;
    want = fleet_proto_length((enum fleet_msg_type)d.type);
    if (want == 0) {
        return -1;
    }
    d.sid = (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
    d.ply = buf[4];
    b = buf + FLEET_PROTO_HEADER;
    if (d.type == FLEET_MSG_DECLINE) {
        if (n != want && n != want + 3) {
            return -1;
        }
    } else if (n != want) {
        return -1;
    }
    switch (d.type) {
    case FLEET_MSG_INVITE:
        d.rules = b[0];
        break;
    case FLEET_MSG_DECLINE:
        d.reason = b[0];
        if (n == want + 3) {
            d.other_sid = (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        }
        break;
    case FLEET_MSG_COMMIT:
        d.flags = d.ply;
        d.ply = 0;
        memcpy(d.commit, b, FLEET_COMMIT_BYTES);
        break;
    case FLEET_MSG_SHOT:
    case FLEET_MSG_RESULT:
        d.cell = b[0];
        d.res = b[1];
        break;
    case FLEET_MSG_SYNC:
        d.flags = b[0];
        d.cell = b[1];
        d.digest = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5];
        break;
    case FLEET_MSG_REVEAL:
        d.flags = d.ply;
        d.ply = 0;
        memcpy(d.layout, b, FLEET_LAYOUT_BYTES);
        memcpy(d.salt, b + FLEET_LAYOUT_BYTES, FLEET_SALT_BYTES);
        break;
    case FLEET_MSG_END:
    case FLEET_MSG_END_ACK:
        d.reason = b[0];
        break;
    default:
        break;
    }
    if (!fields_ok(&d, n)) {
        return -1;
    }
    *m = d;
    return 0;
}

uint32_t fleet_proto_airtime_ms(size_t n)
{
    /* services/radiod/airtime.c for SF8, 62.5 kHz, CR 4/5, preamble 32, CRC,
     * explicit header: 22 bytes 304.128 ms, 38 bytes 386.048 ms, 54 bytes
     * 467.968 ms (rounded up). The frame is 2 + 4 + 16 * blocks at zero hops,
     * the plaintext MeshCore's tag(4) + meshcored's port and length (2) + n. */
    if (n <= FLEET_PROTO_ONE_BLOCK) {
        return 305;
    }
    return n <= FLEET_PROTO_ONE_BLOCK + 16 ? 387 : 468;
}
