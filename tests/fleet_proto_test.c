/*
 * PocketFleet multiplayer wire format (docs/apps/FLEET_MULTIPLAYER.md):
 * every type round trips, has exactly the documented length, fits the frame
 * the airtime budget assumes, and refuses every malformed variant - wrong
 * length by one either way, a wrong version, a zero sid, and every field out
 * of range. Then a million random packets, none of which may decode into a
 * message that would not encode back to the same bytes.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static struct fleet_msg base(int type)
{
    struct fleet_msg m;

    memset(&m, 0, sizeof(m));
    m.type = (uint8_t)type;
    m.sid = 0xA1B2C3;
    return m;
}

/* One valid example of every type. */
static int examples(struct fleet_msg *out)
{
    int n = 0;
    int i;

    out[n] = base(FLEET_MSG_INVITE);
    n++;
    out[n] = base(FLEET_MSG_ACCEPT);
    n++;
    out[n] = base(FLEET_MSG_DECLINE);
    out[n].reason = FLEET_DECLINE_BUSY;
    n++;
    out[n] = base(FLEET_MSG_DECLINE);
    out[n].reason = FLEET_DECLINE_BUSY_WITH_YOU;
    out[n].other_sid = 0x010203;
    n++;
    out[n] = base(FLEET_MSG_START);
    n++;
    out[n] = base(FLEET_MSG_CANCEL);
    n++;
    out[n] = base(FLEET_MSG_COMMIT);
    out[n].flags = FLEET_FLAG_HAVE_PEER;
    for (i = 0; i < FLEET_COMMIT_BYTES; i++) {
        out[n].commit[i] = (uint8_t)(i * 17);
    }
    n++;
    out[n] = base(FLEET_MSG_SHOT);
    out[n].ply = 1;
    out[n].cell = 42;
    n++;
    out[n] = base(FLEET_MSG_SHOT);
    out[n].ply = 2;
    out[n].cell = 99;
    out[n].res = fleet_res_make(3, 4, 0);
    n++;
    out[n] = base(FLEET_MSG_RESULT);
    out[n].ply = 200;
    out[n].cell = 0;
    out[n].res = fleet_res_make(3, 0, 1);
    n++;
    out[n] = base(FLEET_MSG_SYNC);
    out[n].ply = 17;
    out[n].flags = FLEET_SYNC_REPLY | FLEET_SYNC_HAVE_COMMIT | (FLEET_SYNC_BATTLE << FLEET_SYNC_PHASE_SHIFT);
    out[n].cell = FLEET_NO_CELL;
    out[n].digest = 0xDEADBEEF;
    n++;
    out[n] = base(FLEET_MSG_REVEAL);
    for (i = 0; i < FLEET_LAYOUT_BYTES; i++) {
        out[n].layout[i] = (uint8_t)(0x80 | (i * 11));
    }
    for (i = 0; i < FLEET_SALT_BYTES; i++) {
        out[n].salt[i] = (uint8_t)(255 - i);
    }
    n++;
    out[n] = base(FLEET_MSG_END);
    out[n].reason = FLEET_END_FORFEIT;
    out[n].ply = 33;
    n++;
    out[n] = base(FLEET_MSG_END_ACK);
    out[n].reason = FLEET_END_FINISHED;
    n++;
    return n;
}

static void test_round_trips(void)
{
    struct fleet_msg ex[32];
    int n = examples(ex);
    int i;
    int round = 1;
    int lengths = 1;
    int frames = 1;
    int shorter = 1;
    int longer = 1;
    int version = 1;
    int zero_sid = 1;

    for (i = 0; i < n; i++) {
        uint8_t buf[64];
        struct fleet_msg back;
        int len = fleet_proto_encode(&ex[i], buf, sizeof(buf));
        size_t want = fleet_proto_length((enum fleet_msg_type)ex[i].type) +
                      (ex[i].type == FLEET_MSG_DECLINE && ex[i].reason == FLEET_DECLINE_BUSY_WITH_YOU ? 3 : 0);

        uint8_t again[64];

        /* Compared by re-encoding, not memcmp of structs: padding is not a field. */
        if (len < 0 || fleet_proto_decode(&back, buf, (size_t)len) != 0 ||
            fleet_proto_encode(&back, again, sizeof(again)) != len || memcmp(again, buf, (size_t)len) != 0 ||
            back.ply != ex[i].ply || back.cell != ex[i].cell || back.res != ex[i].res ||
            back.digest != ex[i].digest || back.other_sid != ex[i].other_sid) {
            printf("     round trip failed for %s\n", fleet_proto_type_name(ex[i].type));
            round = 0;
            continue;
        }
        lengths &= (size_t)len == want;
        frames &= len <= FLEET_PROTO_MAX;
        /* The hot path has to stay in one AES block, and nothing may
         * need three. */
        if (ex[i].type == FLEET_MSG_SHOT || ex[i].type == FLEET_MSG_RESULT ||
            ex[i].type == FLEET_MSG_INVITE || ex[i].type == FLEET_MSG_ACCEPT ||
            ex[i].type == FLEET_MSG_START || ex[i].type == FLEET_MSG_END ||
            ex[i].type == FLEET_MSG_END_ACK) {
            frames &= len <= FLEET_PROTO_ONE_BLOCK;
        }
        frames &= fleet_proto_airtime_ms((size_t)len) <= 387;
        shorter &= fleet_proto_decode(&back, buf, (size_t)len - 1) != 0;
        if (ex[i].type != FLEET_MSG_DECLINE || ex[i].reason == FLEET_DECLINE_BUSY_WITH_YOU) {
            buf[len] = 0;
            longer &= fleet_proto_decode(&back, buf, (size_t)len + 1) != 0;
        }
        buf[0] = (uint8_t)((2 << 6) | ex[i].type);
        version &= fleet_proto_decode(&back, buf, (size_t)len) != 0;
        buf[0] = (uint8_t)((1 << 6) | ex[i].type);
        buf[1] = buf[2] = buf[3] = 0;
        zero_sid &= fleet_proto_decode(&back, buf, (size_t)len) != 0;
    }
    check("every type round trips exactly", round);
    check("every type has its documented length", lengths);
    check("SHOT, RESULT and the session messages fit one AES block; nothing needs three", frames);
    check("one byte short is refused", shorter);
    check("one byte long is refused", longer);
    check("another version is refused", version);
    check("a zero sid is refused", zero_sid);
    check("the lengths: SHOT 7, RESULT 7, SYNC 11, COMMIT 21, REVEAL 26",
          fleet_proto_length(FLEET_MSG_SHOT) == 7 && fleet_proto_length(FLEET_MSG_RESULT) == 7 &&
          fleet_proto_length(FLEET_MSG_SYNC) == 11 && fleet_proto_length(FLEET_MSG_COMMIT) == 21 &&
          fleet_proto_length(FLEET_MSG_REVEAL) == 26);
    check("airtime: one block 305 ms, two blocks 387 ms",
          fleet_proto_airtime_ms(7) == 305 && fleet_proto_airtime_ms(10) == 305 &&
          fleet_proto_airtime_ms(11) == 387 && fleet_proto_airtime_ms(26) == 387);
}

static int refused(struct fleet_msg m)
{
    uint8_t buf[64];

    return fleet_proto_encode(&m, buf, sizeof(buf)) < 0;
}

/* Encode a message field by field regardless of validity, so decode's own
 * checks are what is tested. */
static int decodes_raw(int type, int ply, const uint8_t *body, size_t blen)
{
    uint8_t buf[64];
    struct fleet_msg m;

    buf[0] = (uint8_t)((1 << 6) | type);
    buf[1] = 0x12;
    buf[2] = 0x34;
    buf[3] = 0x56;
    buf[4] = (uint8_t)ply;
    memcpy(buf + 5, body, blen);
    return fleet_proto_decode(&m, buf, 5 + blen) == 0;
}

static void test_fields(void)
{
    struct fleet_msg m;
    uint8_t body[32];
    int i;

    m = base(FLEET_MSG_SHOT);
    m.ply = 1;
    m.cell = 100;
    check("a cell of 100 is refused", refused(m));
    m.cell = 5;
    m.ply = 0;
    check("ply 0 is refused for a shot", refused(m));
    m.ply = 201;
    check("ply 201 is refused", refused(m));
    m.ply = 1;
    m.res = fleet_res_make(1, 0, 0);
    check("the first shot carries no previous answer", refused(m));
    m.ply = 2;
    m.res = 0;
    check("every later shot carries one", refused(m));
    m.res = fleet_res_make(3, 2, 1);
    check("and it cannot be the one that ended the match", refused(m));

    m = base(FLEET_MSG_RESULT);
    m.ply = 3;
    m.cell = 3;
    check("an answer of 'none' is refused", refused(m));
    m.res = 1 | (2 << FLEET_RES_SHIP_SHIFT);
    check("a miss naming a ship is refused", refused(m));
    m.res = 3 | (5 << FLEET_RES_SHIP_SHIFT);
    check("a sinking of ship 5 is refused", refused(m));
    m.res = 2 | (7 << FLEET_RES_SHIP_SHIFT) | FLEET_RES_DESTROYED;
    check("'fleet destroyed' on a plain hit is refused", refused(m));
    m.res = fleet_res_make(2, 0, 0) | 0x40;
    check("the reserved bits must be zero", refused(m));
    check("fleet_res_make refuses a sunk ship out of range", fleet_res_make(3, 5, 0) == 0);
    check("fleet_res_make ignores the ship on a miss", fleet_res_make(1, 3, 0) == (1 | (7 << 2)));

    m = base(FLEET_MSG_END);
    check("an END with no reason is refused", refused(m));
    m.reason = FLEET_END_REASON_COUNT;
    check("an END with an unknown reason is refused", refused(m));

    m = base(FLEET_MSG_DECLINE);
    m.reason = FLEET_DECLINE_REASON_COUNT;
    check("an unknown decline reason is refused", refused(m));
    m.reason = FLEET_DECLINE_BUSY_WITH_YOU;
    m.other_sid = 0;
    check("busy-with-you needs the other sid", refused(m));
    body[0] = FLEET_DECLINE_USER;
    body[1] = body[2] = body[3] = 1;
    check("the long DECLINE form is only for busy-with-you",
          !decodes_raw(FLEET_MSG_DECLINE, 0, body, 4));
    body[0] = FLEET_DECLINE_BUSY_WITH_YOU;
    check("and busy-with-you is only in the long form",
          !decodes_raw(FLEET_MSG_DECLINE, 0, body, 1));

    memset(body, 0, sizeof(body));
    check("unknown COMMIT flags (in the ply byte) are refused",
          !decodes_raw(FLEET_MSG_COMMIT, 0x80, body, 16));
    check("a COMMIT's known flag is accepted", decodes_raw(FLEET_MSG_COMMIT, 1, body, 16));

    memset(body, 0, sizeof(body));
    body[0] = (uint8_t)(7 << FLEET_SYNC_PHASE_SHIFT);
    body[1] = FLEET_NO_CELL;
    check("an unknown SYNC phase is refused", !decodes_raw(FLEET_MSG_SYNC, 0, body, 6));
    body[0] = 0;
    body[1] = 100;
    check("a SYNC pending cell of 100 is refused", !decodes_raw(FLEET_MSG_SYNC, 0, body, 6));

    memset(body, 0, sizeof(body));
    body[2] = 100;
    check("a layout cell of 100 is refused", !decodes_raw(FLEET_MSG_REVEAL, 0, body, 21));
    body[2] = 0x80 | 99;
    check("a vertical layout byte in range is accepted", decodes_raw(FLEET_MSG_REVEAL, 0, body, 21));
    check("unknown REVEAL flags are refused", !decodes_raw(FLEET_MSG_REVEAL, 0x02, body, 21));

    for (i = 0; i < 64; i++) {
        uint8_t hdr[5] = { (uint8_t)((1 << 6) | i), 1, 2, 3, 0 };
        struct fleet_msg d;

        if (i >= FLEET_MSG_INVITE && i < FLEET_MSG_TYPE_COUNT) {
            continue;
        }
        if (fleet_proto_decode(&d, hdr, sizeof(hdr)) == 0) {
            break;
        }
    }
    check("every unknown type is refused", i == 64);
}

static void test_random(void)
{
    unsigned seed = 12345;
    long accepted = 0;
    int stable = 1;
    long i;

    for (i = 0; i < 1000000; i++) {
        uint8_t buf[32];
        uint8_t again[64];
        struct fleet_msg m;
        size_t n;
        size_t j;
        int len;

        seed = seed * 1103515245u + 12345u;
        n = (seed >> 16) % 30;
        for (j = 0; j < n; j++) {
            seed = seed * 1103515245u + 12345u;
            buf[j] = (uint8_t)(seed >> 16);
        }
        /* Bias towards the right version and length so the field checks,
         * not the length check, are what refuse most of them. */
        if (n > 0 && (i & 1)) {
            int type = 1 + buf[0] % (FLEET_MSG_TYPE_COUNT - 1);

            buf[0] = (uint8_t)((1 << 6) | type);
            if (i & 2) {
                n = fleet_proto_length((enum fleet_msg_type)type);
                for (j = 1; j < n; j++) {
                    seed = seed * 1103515245u + 12345u;
                    buf[j] = (uint8_t)(seed >> 16);
                }
            }
        }
        if (fleet_proto_decode(&m, buf, n) != 0) {
            continue;
        }
        accepted++;
        len = fleet_proto_encode(&m, again, sizeof(again));
        if (len != (int)n || memcmp(again, buf, n) != 0) {
            stable = 0;
        }
    }
    printf("     %ld of 1000000 random packets decoded\n", accepted);
    check("a million random packets: whatever decodes encodes back to the same bytes", stable);
    check("and some did decode, so the checks were reached", accepted > 0);
}

int main(void)
{
    test_round_trips();
    test_fields();
    test_random();
    printf("fleet_proto_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
