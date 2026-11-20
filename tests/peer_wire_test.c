/*
 * peerd wire format (services/peerd/peer_wire.h): every length, every type,
 * every field the parser is meant to refuse, and that what it builds it
 * reads back. Pure: no crypto, no sockets.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "peer_wire.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void sample(struct pw_signed *f, uint8_t type)
{
    int i;

    memset(f, 0, sizeof(*f));
    f->h.type = type;
    for (i = 0; i < PW_KEY; i++) {
        f->h.sender[i] = (uint8_t)(0xA0 + i);
    }
    f->h.epoch = 7;
    f->h.seq = 0x0102030405060708ull;
    f->addr = 0xC0A8012A;       /* 192.168.1.42 */
    f->port = 47230;
    f->name_len = 4;
    memcpy(f->name, "Anna", 4);
    if (type == PW_PROBE || type == PW_PROBE_REPLY || type == PW_LOOKUP || type == PW_LOOKUP_REPLY) {
        for (i = 0; i < PW_CHALLENGE; i++) {
            f->challenge[i] = (uint8_t)(0x30 + i);
        }
    }
    if (type == PW_PROBE_REPLY || type == PW_LOOKUP_REPLY) {
        memset(f->h.recipient, 0x5A, PW_PREFIX);
    }
    if (type == PW_LOOKUP) {
        memset(f->target, 0x77, PW_KEY);
    }
}

static void test_sizes(void)
{
    check("sizes: the header is 58 bytes", PW_HEADER == 58);
    check("sizes: a signed frame is 210 bytes, whatever its type", PW_SIGNED_SIZE == 210 &&
          PW_SIGNED_BODY == 146);
    check("sizes: DATA is 100 to 1123 bytes, so it is never fragmented",
          PW_DATA_MIN == 100 && PW_DATA_MAX == 1123 && PW_DATA_MAX <= 1472);
    check("sizes: the signing prefix is what it says", strlen(PW_SIGN_PREFIX) == PW_SIGN_PREFIX_LEN);
}

/* Every type byte at every length: accepted only at a length that type has. */
static void test_lengths(void)
{
    static uint8_t buf[1300];
    struct pw_header h;
    int type;
    size_t n;
    int bad = 0;
    int accepted = 0;

    memset(buf, 0, sizeof(buf));
    buf[0] = PW_VERSION;
    buf[49] = 1;        /* epoch 1 */
    buf[57] = 1;        /* sequence 1 */
    for (type = 0; type < 256; type++) {
        buf[1] = (uint8_t)type;
        for (n = 0; n <= sizeof(buf); n++) {
            int rc = pw_parse_header(buf, n, &h);
            int want = (type == PW_DATA && n >= PW_DATA_MIN && n <= PW_DATA_MAX) ||
                       ((type == PW_BEACON || type == PW_PROBE || type == PW_PROBE_REPLY ||
                         type == PW_LOOKUP || type == PW_LOOKUP_REPLY) &&
                        n == PW_SIGNED_SIZE);

            accepted += rc == PW_OK;
            bad += (rc == PW_OK) != want;
        }
    }
    check("lengths: every type at every length from 0 to 1300 is accepted only at its own", bad == 0);
    check("lengths: 1024 DATA lengths and five signed ones, and nothing else", accepted == 1024 + 5);
    check("lengths: no buffer at all is refused", pw_parse_header(NULL, 100, &h) == PW_E_SHORT);
    buf[1] = PW_DATA;
    check("lengths: 57 bytes is short", pw_parse_header(buf, 57, &h) == PW_E_SHORT);
    check("lengths: DATA of 99 bytes is refused", pw_parse_header(buf, 99, &h) == PW_E_LENGTH);
    check("lengths: DATA of 1124 bytes is refused", pw_parse_header(buf, 1124, &h) == PW_E_LENGTH);
    buf[1] = PW_BEACON;
    check("lengths: a beacon of 209 or 211 bytes is refused",
          pw_parse_header(buf, 209, &h) == PW_E_LENGTH && pw_parse_header(buf, 211, &h) == PW_E_LENGTH);
}

static void test_header_fields(void)
{
    uint8_t buf[PW_DATA_MIN];
    struct pw_header h;
    struct pw_header out;
    int i;

    memset(&h, 0, sizeof(h));
    h.type = PW_DATA;
    for (i = 0; i < PW_KEY; i++) {
        h.sender[i] = (uint8_t)(i + 1);
    }
    memset(h.recipient, 0xEE, PW_PREFIX);
    h.epoch = 0x1122334455667788ull;
    h.seq = 0x99AABBCCDDEEFF01ull;
    memset(buf, 0, sizeof(buf));
    pw_build_header(&h, buf);
    check("header: built and read back, field for field",
          pw_parse_header(buf, sizeof(buf), &out) == PW_OK && out.type == PW_DATA &&
          memcmp(out.sender, h.sender, PW_KEY) == 0 && memcmp(out.recipient, h.recipient, PW_PREFIX) == 0 &&
          out.epoch == h.epoch && out.seq == h.seq);
    check("header: the numbers are big-endian on the wire", buf[42] == 0x11 && buf[49] == 0x88 &&
          buf[50] == 0x99 && buf[57] == 0x01);
    buf[0] = 0;
    check("header: version 0 is refused", pw_parse_header(buf, sizeof(buf), &out) == PW_E_VERSION);
    buf[0] = 2;
    check("header: version 2 is refused", pw_parse_header(buf, sizeof(buf), &out) == PW_E_VERSION);
    buf[0] = PW_VERSION;
    memset(buf + 42, 0, 8);
    check("header: epoch 0 is refused", pw_parse_header(buf, sizeof(buf), &out) == PW_E_FIELD);
    pw_put64(buf + 42, 5);
    memset(buf + 50, 0, 8);
    check("header: sequence 0 is refused", pw_parse_header(buf, sizeof(buf), &out) == PW_E_FIELD);
    pw_put64(buf + 50, UINT64_MAX);
    pw_put64(buf + 42, UINT64_MAX);
    check("header: the largest epoch and sequence are read whole",
          pw_parse_header(buf, sizeof(buf), &out) == PW_OK && out.epoch == UINT64_MAX &&
          out.seq == UINT64_MAX);
}

static void test_signed(void)
{
    static const uint8_t types[] = { PW_BEACON, PW_PROBE, PW_PROBE_REPLY, PW_LOOKUP, PW_LOOKUP_REPLY };
    uint8_t buf[PW_SIGNED_SIZE];
    uint8_t copy[PW_SIGNED_SIZE];
    struct pw_signed f;
    struct pw_signed out;
    size_t t;
    int ok = 1;

    for (t = 0; t < sizeof(types); t++) {
        sample(&f, types[t]);
        ok &= pw_build_signed(&f, buf) == PW_OK && pw_parse_signed(buf, sizeof(buf), &out) == PW_OK &&
              out.h.type == types[t] && out.addr == f.addr && out.port == f.port &&
              out.name_len == 4 && strcmp(out.name, "Anna") == 0 &&
              memcmp(out.challenge, f.challenge, PW_CHALLENGE) == 0 &&
              memcmp(out.target, f.target, PW_KEY) == 0 && out.h.epoch == 7;
    }
    check("signed: each of the five types builds and reads back", ok);

    sample(&f, PW_BEACON);
    f.flags = PW_FLAG_TABLE;
    f.table.port = 1;
    f.table.proto_min = 1;
    f.table.proto_max = 2;
    f.table.seats = 2;
    f.table.tag = 0xDEADBEEF;
    check("signed: a beacon with a table reads back",
          pw_build_signed(&f, buf) == PW_OK && pw_parse_signed(buf, sizeof(buf), &out) == PW_OK &&
          (out.flags & PW_FLAG_TABLE) && out.table.port == 1 && out.table.proto_max == 2 &&
          out.table.seats == 2 && out.table.tag == 0xDEADBEEF);
    check("signed: DATA is not a signed frame", ({
              uint8_t d[PW_SIGNED_SIZE];

              memcpy(d, buf, sizeof(d));
              d[1] = PW_DATA;
              pw_parse_signed(d, sizeof(d), &out) != PW_OK;
          }));

#define REFUSED(label, edit)                                                       \
    do {                                                                           \
        memcpy(copy, buf, sizeof(copy));                                           \
        edit;                                                                      \
        check(label, pw_parse_signed(copy, sizeof(copy), &out) == PW_E_FIELD);     \
    } while (0)

    REFUSED("signed: port 0 is refused", (copy[62] = 0, copy[63] = 0));
    REFUSED("signed: an unknown flag is refused", copy[64] |= 0x02);
    REFUSED("signed: a name length over 24 is refused", copy[65] = 25);
    REFUSED("signed: bytes after the name's end are refused", copy[66 + 10] = 'x');
    REFUSED("signed: a control character in the name is refused", copy[66] = 0x07);
    REFUSED("signed: a table port of 0 is refused", copy[90] = 0);
    REFUSED("signed: a table port of 16 is refused", copy[90] = 16);
    REFUSED("signed: proto_min above proto_max is refused", copy[91] = 3);
    REFUSED("signed: a table with no seats is refused", copy[93] = 0);
    REFUSED("signed: table bytes without the flag are refused", copy[64] = 0);
    REFUSED("signed: a beacon with a recipient is refused", copy[34] = 1);
    REFUSED("signed: a beacon with a challenge is refused", copy[98] = 1);
    REFUSED("signed: a beacon with a target is refused", copy[114] = 1);
    sample(&f, PW_PROBE);
    pw_build_signed(&f, buf);
    REFUSED("signed: a probe with a target is refused", copy[120] = 1);
    REFUSED("signed: a probe with a recipient is refused", copy[40] = 1);
    sample(&f, PW_PROBE_REPLY);
    pw_build_signed(&f, buf);
    REFUSED("signed: a reply with a target is refused", copy[145] = 1);
    sample(&f, PW_LOOKUP);
    pw_build_signed(&f, buf);
    REFUSED("signed: a lookup with a recipient is refused", copy[41] = 9);
#undef REFUSED

    sample(&f, PW_BEACON);
    f.name_len = 25;
    check("signed: a name too long is not built", pw_build_signed(&f, buf) == PW_E_FIELD);
    sample(&f, PW_BEACON);
    f.port = 0;
    check("signed: a frame that would be refused is not built", pw_build_signed(&f, buf) == PW_E_FIELD);
    sample(&f, PW_BEACON);
    f.name_len = 0;
    memset(f.name, 0, sizeof(f.name));
    check("signed: no name at all is fine",
          pw_build_signed(&f, buf) == PW_OK && pw_parse_signed(buf, sizeof(buf), &out) == PW_OK &&
          out.name[0] == '\0');
    sample(&f, PW_BEACON);
    f.name_len = 24;
    memcpy(f.name, "123456789012345678901234", 24);
    check("signed: a name of exactly 24 bytes is whole and ended",
          pw_build_signed(&f, buf) == PW_OK && pw_parse_signed(buf, sizeof(buf), &out) == PW_OK &&
          strlen(out.name) == 24);
}

static int name(const char *s)
{
    return pw_name_ok((const uint8_t *)s, strlen(s));
}

static void test_names(void)
{
    check("name: plain text", name("Anna") && name("K230-B") && name(""));
    check("name: Norwegian letters", name("Bj\xc3\xb8rn \xc3\x85se"));
    check("name: three- and four-byte characters", name("\xe2\x82\xac") && name("\xf0\x9f\x9a\xa2"));
    check("name: a control character is refused", !name("a\x01" "b") && !name("tab\there") && !name("\x7f"));
    check("name: a C1 control is refused", !name("\xc2\x85") && !name("\xc2\x9f"));
    check("name: U+00A0 just above them is fine", name("\xc2\xa0"));
    check("name: a lone continuation byte is refused", !name("\x80") && !name("a\xbf"));
    check("name: an overlong form is refused", !name("\xc0\xaf") && !name("\xc1\x81") &&
          !name("\xe0\x80\xaf") && !name("\xf0\x80\x80\xaf"));
    check("name: a surrogate is refused", !name("\xed\xa0\x80") && !name("\xed\xbf\xbf"));
    check("name: beyond U+10FFFF is refused", !name("\xf4\x90\x80\x80") && !name("\xf5\x80\x80\x80"));
    check("name: a character cut short is refused", !name("\xc3") && !name("\xe2\x82") && !name("\xf0\x9f\x9a"));
    check("name: 25 bytes is too long, 24 is not",
          !pw_name_ok((const uint8_t *)"1234567890123456789012345", 25) &&
          pw_name_ok((const uint8_t *)"123456789012345678901234", 24));
    check("name: an embedded zero byte is refused", !pw_name_ok((const uint8_t *)"a\0b", 3));
}

int main(void)
{
    test_sizes();
    test_lengths();
    test_header_fields();
    test_signed();
    test_names();
    printf("peer_wire_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
