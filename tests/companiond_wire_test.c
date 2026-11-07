/*
 * companiond's companion-protocol framing (services/companiond/cmp_wire.h):
 * frames split across reads, several frames in one read, frames of the
 * wrong direction, oversized and empty frames, and the text and UTF-8 rules
 * the bridge applies before anything reaches meshcored.
 *
 * The expected behaviour is upstream's SerialWifiInterface::checkRecvFrame
 * (meshcore-dev/MeshCore 81dbae59): skip a frame that is not '<' or longer
 * than MAX_FRAME_SIZE by reading past its length; a header with a zero type
 * or length is three bytes dropped.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmp_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

/* Feed a stream in chunks of `step` bytes; collect the frames. */
struct got {
    int n;
    size_t len[16];
    uint8_t data[16][CMP_MAX_FRAME];
};

static void feed(struct cmp_decoder *d, const uint8_t *s, size_t n, size_t step, struct got *g)
{
    size_t at = 0;

    while (at < n) {
        size_t chunk = n - at < step ? n - at : step;
        size_t done = 0;

        while (done < chunk) {
            size_t frame = 0;
            size_t used = cmp_decoder_feed(d, s + at + done, chunk - done, &frame);

            done += used;
            if (frame && g->n < 16) {
                g->len[g->n] = frame;
                memcpy(g->data[g->n], d->buf, frame);
                g->n++;
            }
        }
        at += chunk;
    }
}

static size_t put_frame(uint8_t *out, uint8_t type, const uint8_t *p, uint16_t len)
{
    out[0] = type;
    out[1] = (uint8_t)len;
    out[2] = (uint8_t)(len >> 8);
    if (p) {
        memcpy(out + 3, p, len);
    }
    return 3u + len;
}

static void test_split_and_combined(void)
{
    uint8_t stream[512];
    size_t n = 0;
    const uint8_t a[] = { 0x16, 0x03 };                                   /* DEVICE_QUERY */
    const uint8_t b[] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 'm', 'c', 'c', 'l', 'i' }; /* APP_START */
    const uint8_t c[] = { 0x0a };                                         /* SYNC */
    size_t step;

    n += put_frame(stream + n, CMP_TYPE_FROM_APP, a, sizeof(a));
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, b, sizeof(b));
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, c, sizeof(c));
    /* Every chunk size from one byte at a time to all of it in one read. */
    for (step = 1; step <= n; step++) {
        struct cmp_decoder d;
        struct got g;

        cmp_decoder_init(&d);
        memset(&g, 0, sizeof(g));
        feed(&d, stream, n, step, &g);
        CHECK(g.n == 3, "step %zu: %d frames", step, g.n);
        CHECK(g.len[0] == 2 && memcmp(g.data[0], a, 2) == 0, "step %zu: first frame", step);
        CHECK(g.len[1] == sizeof(b) && memcmp(g.data[1], b, sizeof(b)) == 0,
              "step %zu: second frame", step);
        CHECK(g.len[2] == 1 && g.data[2][0] == 0x0a, "step %zu: third frame", step);
    }
}

static void test_skips(void)
{
    static uint8_t stream[2048];
    static uint8_t big[1000];
    size_t n = 0;
    const uint8_t ok[] = { 0x05 };
    struct cmp_decoder d;
    struct got g;

    memset(big, '<', sizeof(big)); /* a body full of would-be headers */
    /* A radio-to-app frame (the app echoing ours) is read past. */
    n += put_frame(stream + n, CMP_TYPE_TO_APP, ok, 1);
    /* Oversized: 1000 > 176, read past whole, never buffered. */
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, big, sizeof(big));
    /* Exactly the limit is a frame. */
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, big, CMP_MAX_FRAME);
    /* Zero length: three bytes, no frame. Zero type: the same. */
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, NULL, 0);
    stream[n++] = 0;
    stream[n++] = 5;
    stream[n++] = 0;
    /* Junk type with a length: read past. */
    n += put_frame(stream + n, 'X', ok, 1);
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, ok, 1);

    cmp_decoder_init(&d);
    memset(&g, 0, sizeof(g));
    feed(&d, stream, n, 7, &g);
    CHECK(g.n == 2, "%d frames survive", g.n);
    CHECK(g.len[0] == CMP_MAX_FRAME, "the frame at the limit is kept (%zu)", g.len[0]);
    CHECK(g.len[1] == 1 && g.data[1][0] == 0x05, "the last frame is intact");
    CHECK(d.skipped_type == 2 && d.skipped_size == 1 && d.empty == 2,
          "counters type %u size %u empty %u", d.skipped_type, d.skipped_size, d.empty);
}

static void test_max_announced(void)
{
    /* A 65535-byte announcement followed by its bytes and a real frame:
     * memory stays one frame, and the stream stays in step. */
    static uint8_t stream[3 + 65535 + 4];
    size_t n = 0;
    struct cmp_decoder d;
    struct got g;

    stream[n++] = CMP_TYPE_FROM_APP;
    stream[n++] = 0xFF;
    stream[n++] = 0xFF;
    memset(stream + n, 0x3c, 65535);
    n += 65535;
    n += put_frame(stream + n, CMP_TYPE_FROM_APP, (const uint8_t *)"\x0a", 1);
    cmp_decoder_init(&d);
    memset(&g, 0, sizeof(g));
    feed(&d, stream, n, 1500, &g);
    CHECK(g.n == 1 && g.len[0] == 1 && g.data[0][0] == 0x0a, "in step after 64 KiB (%d)", g.n);
}

static void test_encode(void)
{
    uint8_t out[CMP_MAX_FRAME + 8];
    uint8_t p[CMP_MAX_FRAME + 1];

    memset(p, 7, sizeof(p));
    CHECK(cmp_frame_encode(p, 3, out, sizeof(out)) == 6 && out[0] == '>' && out[1] == 3 &&
              out[2] == 0 && out[3] == 7,
          "encode header");
    CHECK(cmp_frame_encode(p, CMP_MAX_FRAME, out, sizeof(out)) == CMP_MAX_FRAME + 3,
          "encode at the limit");
    CHECK(cmp_frame_encode(p, CMP_MAX_FRAME + 1, out, sizeof(out)) == 0, "refuse over the limit");
    CHECK(cmp_frame_encode(p, 0, out, sizeof(out)) == 0, "refuse empty");
    CHECK(cmp_frame_encode(p, 10, out, 12) == 0, "refuse what does not fit");
}

static void test_text(void)
{
    const char *emoji = "ok \xF0\x9F\x91\x8D"; /* thumbs up, 4 bytes */

    CHECK(cmp_text_ok((const uint8_t *)"hello\nthere\t!", 13), "plain text, newline, tab");
    CHECK(cmp_text_ok((const uint8_t *)emoji, strlen(emoji)), "emoji");
    CHECK(!cmp_text_ok((const uint8_t *)"a\0b", 3), "NUL refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\x1b[31m", 5), "ESC refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\x7f", 1), "DEL refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\xC2\x85", 2), "C1 refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\xC3", 1), "truncated sequence refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\xC0\xAF", 2), "overlong refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\xED\xA0\x80", 3), "surrogate refused");
    CHECK(!cmp_text_ok((const uint8_t *)"\xF4\x90\x80\x80", 4), "above U+10FFFF refused");

    CHECK(cmp_utf8_prefix(emoji, strlen(emoji), 6) == 3, "cut before the emoji, not in it");
    CHECK(cmp_utf8_prefix(emoji, strlen(emoji), 7) == 7, "whole emoji fits");
    CHECK(cmp_utf8_prefix("abc\xC3", 4, 10) == 3, "a broken tail is not counted");
}

static void test_hex(void)
{
    uint8_t b[3];
    char s[7];

    CHECK(cmp_unhex("0aFf10", b, 3) == 0 && b[0] == 0x0a && b[1] == 0xff && b[2] == 0x10, "unhex");
    CHECK(cmp_unhex("0aFf1", b, 3) == -1, "odd length refused");
    CHECK(cmp_unhex("0aFg10", b, 3) == -1, "non-hex refused");
    cmp_hex(b, 3, s);
    CHECK(strcmp(s, "0aff10") == 0, "hex %s", s);
}

int main(void)
{
    test_split_and_combined();
    test_skips();
    test_max_announced();
    test_encode();
    test_text();
    test_hex();
    printf("companiond_wire_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
