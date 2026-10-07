/*
 * meshcored: hex and validation (services/meshcored/mcd_util.c).
 *
 * Small functions, and the first thing every byte from outside this service
 * goes through. The decoder is the one that matters: MeshCore's own
 * mesh::Utils::fromHex() checks the string's length and nothing else, so a
 * corrupt radio.rx payload would become a frame of plausible bytes rather
 * than a rejected event. These cases are what keeps that from happening
 * here.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mcd_util.h"

#include "mesh_runtime.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    failed += !ok;
}

static void test_hex_decode(void)
{
    uint8_t buf[8];

    memset(buf, 0xAA, sizeof(buf));
    check("an empty string decodes to nothing", mcd_hex_decode("", buf, sizeof(buf)) == 0);
    check("one byte", mcd_hex_decode("41", buf, sizeof(buf)) == 1 && buf[0] == 0x41);
    check("several bytes",
          mcd_hex_decode("00ff10", buf, sizeof(buf)) == 3 &&
          buf[0] == 0x00 && buf[1] == 0xff && buf[2] == 0x10);
    check("upper case is hex too",
          mcd_hex_decode("DEADBEEF", buf, sizeof(buf)) == 4 && buf[0] == 0xDE && buf[3] == 0xEF);
    check("mixed case", mcd_hex_decode("dEaD", buf, sizeof(buf)) == 2 && buf[0] == 0xDE);

    /* The whole point. Every one of these would pass a length-only check. */
    check("a non-hex character is refused, not substituted",
          mcd_hex_decode("zz", buf, sizeof(buf)) == -1);
    check("a non-hex character in the middle is refused",
          mcd_hex_decode("00zz11", buf, sizeof(buf)) == -1);
    check("a space is refused", mcd_hex_decode("00 11", buf, sizeof(buf)) == -1);
    check("a leading 0x is refused", mcd_hex_decode("0x41", buf, sizeof(buf)) == -1);
    check("a high-bit byte is refused", mcd_hex_decode("\x80\x81", buf, sizeof(buf)) == -1);
    check("an odd length is refused", mcd_hex_decode("abc", buf, sizeof(buf)) == -1);
    check("a string longer than the destination is refused",
          mcd_hex_decode("000102030405060708", buf, sizeof(buf)) == -1);
    check("exactly the destination size fits",
          mcd_hex_decode("0001020304050607", buf, sizeof(buf)) == 8);
    check("NULL is refused", mcd_hex_decode(NULL, buf, sizeof(buf)) == -1);

    /* A refused string must leave nothing a caller could mistake for data.
     * It writes as far as the bad character and returns -1; the contract is
     * that the count is the only thing a caller reads, and it is negative. */
    check("a refusal returns a negative count, so no length is usable",
          mcd_hex_decode("41zz", buf, sizeof(buf)) < 0);
}

static void test_hex_encode(void)
{
    uint8_t src[3] = { 0x00, 0xAB, 0xff };
    char out[8];

    check("encodes lower case",
          mcd_hex_encode(src, 3, out, sizeof(out)) != NULL && strcmp(out, "00abff") == 0);
    check("an exact fit works", mcd_hex_encode(src, 3, out, 7) != NULL);
    check("one byte short is refused", mcd_hex_encode(src, 3, out, 6) == NULL);
    check("zero bytes gives an empty string",
          mcd_hex_encode(src, 0, out, sizeof(out)) != NULL && out[0] == '\0');

    /* Round trip, because the two are used on the same bytes: what goes out
     * as a transmit must come back the same from a receive. */
    {
        uint8_t back[3];
        char hex[7];

        mcd_hex_encode(src, 3, hex, sizeof(hex));
        check("encode then decode is the identity",
              mcd_hex_decode(hex, back, sizeof(back)) == 3 && memcmp(src, back, 3) == 0);
    }
}

static void test_key_prefix(void)
{
    uint8_t buf[32];

    check("one byte is the shortest prefix", mcd_key_prefix_parse("e1", buf, sizeof(buf)) == 1);
    check("a whole key is the longest",
          mcd_key_prefix_parse("e1710aa062e6aefc04ad35118ba1e81f"
                               "07cb0bec983276af3a0082015daeb274", buf, sizeof(buf)) == 32);
    check("an empty prefix is refused", mcd_key_prefix_parse("", buf, sizeof(buf)) == -1);
    check("a single character is refused", mcd_key_prefix_parse("e", buf, sizeof(buf)) == -1);
    check("longer than a key is refused",
          mcd_key_prefix_parse("e1710aa062e6aefc04ad35118ba1e81f"
                               "07cb0bec983276af3a0082015daeb27400", buf, sizeof(buf)) == -1);
    check("a non-hex prefix is refused", mcd_key_prefix_parse("gg", buf, sizeof(buf)) == -1);
    check("an odd-length prefix is refused",
          mcd_key_prefix_parse("e17", buf, sizeof(buf)) == -1);
    check("NULL is refused", mcd_key_prefix_parse(NULL, buf, sizeof(buf)) == -1);
}

static void test_text(void)
{
    char long_text[200];

    check("ordinary text is acceptable", mcd_text_acceptable("hello from the K230", 160));
    check("an empty message is not", !mcd_text_acceptable("", 160));
    check("NULL is not", !mcd_text_acceptable(NULL, 160));
    check("a newline is allowed", mcd_text_acceptable("one\ntwo", 160));
    check("a tab is allowed", mcd_text_acceptable("one\ttwo", 160));
    check("an escape character is not", !mcd_text_acceptable("\033[2J", 160));
    check("a carriage return is not", !mcd_text_acceptable("one\rtwo", 160));
    check("a delete character is not", !mcd_text_acceptable("one\177two", 160));
    check("UTF-8 passes through",
          mcd_text_acceptable("\xc3\xa6\xc3\xb8\xc3\xa5 hytta", 160));

    memset(long_text, 'x', sizeof(long_text));
    long_text[160] = '\0';
    check("exactly the limit is acceptable", mcd_text_acceptable(long_text, 160));
    long_text[160] = 'x';
    long_text[161] = '\0';
    check("one over the limit is not", !mcd_text_acceptable(long_text, 160));
}

/* ---- remote text on the way out ----------------------------------------
 *
 * An advert name and a message body are chosen by whoever is on the air.
 * These are the cases that decide whether one hostile node can break every
 * client's read of every event, or put an escape sequence on somebody's
 * screen.
 */
static void expect_sanitized(const char *name, const char *src, const char *want)
{
    char got[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];

    mcd_text_sanitize(src, got, sizeof(got));
    if (strcmp(got, want) == 0) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s: got \"%s\", wanted \"%s\"\n", name, got, want);
        failed++;
    }
    checks++;
}

#define FFFD "\xEF\xBF\xBD"

static void test_sanitize(void)
{
    /* Ordinary text is not touched at all. */
    expect_sanitized("plain ASCII passes through", "HYTTA", "HYTTA");
    expect_sanitized("so does a space and punctuation", "RPT-NORD (2)", "RPT-NORD (2)");
    expect_sanitized("an empty string stays empty", "", "");

    /* Valid Unicode survives: a node name in Norwegian must not come out as
     * three replacement characters. */
    expect_sanitized("two-byte UTF-8 survives", "\xc3\xa6\xc3\xb8\xc3\xa5",
                     "\xc3\xa6\xc3\xb8\xc3\xa5");
    expect_sanitized("three-byte UTF-8 survives", "\xe2\x82\xac", "\xe2\x82\xac");
    expect_sanitized("four-byte UTF-8 survives", "\xf0\x9f\x93\xa1", "\xf0\x9f\x93\xa1");
    expect_sanitized("a combining mark survives", "e\xcc\x81", "e\xcc\x81");

    /* The escape sequence. JSON would encode the ESC and a client would
     * decode it straight back out again, which is how a remote advert name
     * ends up clearing somebody's terminal. */
    expect_sanitized("an ESC is replaced", "\x1b[2J", FFFD "[2J");
    expect_sanitized("a colour sequence loses its ESC", "\x1b[31mred", FFFD "[31mred");
    expect_sanitized("an OSC title sequence loses its ESC",
                     "\x1b]0;pwned\x07", FFFD "]0;pwned" FFFD);

    /* Other control characters. */
    expect_sanitized("a carriage return is replaced", "over\rwrite", "over" FFFD "write");
    expect_sanitized("a bell is replaced", "ding\x07", "ding" FFFD);
    expect_sanitized("a backspace is replaced", "a\x08" "b", "a" FFFD "b");
    expect_sanitized("DEL is replaced", "a\x7f" "b", "a" FFFD "b");
    expect_sanitized("a C1 control is replaced", "a\xc2\x9b" "b", "a" FFFD "b");
    /* Newline and tab are kept: they are safe, and the outbound side already
     * allows them, so a message can come back looking like it was sent. */
    expect_sanitized("a newline is kept", "one\ntwo", "one\ntwo");
    expect_sanitized("a tab is kept", "one\ttwo", "one\ttwo");

    /* Malformed UTF-8, which is what makes an IPC frame unparsable. */
    expect_sanitized("a lone continuation byte is replaced", "a\x80" "b", "a" FFFD "b");
    expect_sanitized("a truncated two-byte sequence is replaced", "a\xc3", "a" FFFD);
    expect_sanitized("a truncated three-byte sequence is replaced", "a\xe2\x82", "a" FFFD FFFD);
    expect_sanitized("an overlong encoding is replaced", "\xc0\xaf", FFFD FFFD);
    expect_sanitized("a surrogate half is replaced", "\xed\xa0\x80", FFFD FFFD FFFD);
    expect_sanitized("a codepoint above U+10FFFF is replaced",
                     "\xf5\x80\x80\x80", FFFD FFFD FFFD FFFD);
    expect_sanitized("0xFF is replaced", "\xff", FFFD);
    /* A bad lead byte must not decide how much of the rest to swallow: the
     * decoder resyncs one byte on, so the text after it survives. */
    expect_sanitized("a bad lead byte does not eat what follows",
                     "\xf0" "HYTTA", FFFD "HYTTA");

    /* Output is truncated on a character boundary, never mid-sequence: half a
     * sequence would be the very thing this exists to prevent. */
    {
        char small[8];

        mcd_text_sanitize("\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", small, sizeof(small));
        check("truncation keeps whole characters",
              strcmp(small, "\xe2\x82\xac\xe2\x82\xac") == 0);
        check("and the result is still terminated", small[strlen(small)] == '\0');
    }
    {
        char tiny[2];

        mcd_text_sanitize("\xe2\x82\xac", tiny, sizeof(tiny));
        check("a buffer too small for one character gives an empty string",
              tiny[0] == '\0');
    }

    /* The longest a name and a message can legally be, unchanged. */
    {
        char name[MCD_NODE_NAME_LEN];
        char out[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];

        memset(name, 'N', sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        mcd_text_sanitize(name, out, sizeof(out));
        check("a maximum-length node name survives whole", strcmp(out, name) == 0);
    }
    {
        char text[MCD_MAX_TEXT + 1];
        char out[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];

        memset(text, 'M', MCD_MAX_TEXT);
        text[MCD_MAX_TEXT] = '\0';
        mcd_text_sanitize(text, out, sizeof(out));
        check("a maximum-length message survives whole", strcmp(out, text) == 0);
        check("and is still the length it was", strlen(out) == MCD_MAX_TEXT);
    }
    /* The worst case for growth: every byte replaced, three bytes each. The
     * buffer size the header promises has to hold it. */
    {
        char text[MCD_MAX_TEXT + 1];
        char out[MCD_SANITIZED_SIZE(MCD_MAX_TEXT)];

        memset(text, (char)0xFF, MCD_MAX_TEXT);
        text[MCD_MAX_TEXT] = '\0';
        mcd_text_sanitize(text, out, sizeof(out));
        check("a message of nothing but bad bytes fits the promised buffer",
              strlen(out) == (size_t)MCD_MAX_TEXT * 3);
    }

    check("NULL is handled", (mcd_text_sanitize(NULL, (char[4]){ 'x' }, 4), true));
}

int main(void)
{
    test_hex_decode();
    test_hex_encode();
    test_key_prefix();
    test_text();
    test_sanitize();
    printf("meshcored_util_test: %d check(s), %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
