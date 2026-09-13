/*
 * Wave's modem, in memory, with no audio device: text -> ggwave encode ->
 * PCM -> ggwave decode -> the same bytes. The host acceptance test for the
 * whole acoustic path short of the air.
 *
 * Also: the size formula against ggwave's real output, every profile, the
 * limits (empty, maximum, one over, volume), arbitrary chunking including the
 * capture period that would break ggwave on its own, a misaligned start,
 * truncated PCM and the watchdog that recovers from it, noise on top of a
 * message, noise alone, full-scale garbage, two messages in a row, the peak
 * level against pocketaudio's ceiling, and the heap each instance takes.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_modem.h"

#include "pocketaudio/pocketaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

/* ---- a decode recorder ------------------------------------------------- */

struct got {
    int decoded;
    int missed;
    uint8_t last[256];
    size_t last_len;
};

static void on_decode(void *user, enum wave_decode_kind kind, const uint8_t *data, size_t len)
{
    struct got *g = user;

    if (kind == WAVE_DECODED) {
        g->decoded++;
        g->last_len = len < sizeof(g->last) ? len : sizeof(g->last);
        memcpy(g->last, data, g->last_len);
    } else {
        g->missed++;
    }
}

static uint32_t rng = 0x12345678u;

static uint32_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int16_t noise(int amplitude)
{
    return (int16_t)((int)(next() % (unsigned)(2 * amplitude + 1)) - amplitude);
}

/* Feed n samples of silence (or noise) in capture-sized chunks. */
static void feed_fill(struct wave_decoder *d, struct got *g, size_t n, int amplitude)
{
    int16_t buf[960];

    while (n > 0) {
        size_t k = n < 960 ? n : 960;
        size_t i;

        for (i = 0; i < k; i++) {
            buf[i] = amplitude ? noise(amplitude) : 0;
        }
        wave_decoder_feed(d, buf, k, on_decode, g);
        n -= k;
    }
}

static void feed_chunked(struct wave_decoder *d, struct got *g, const int16_t *s, size_t n, size_t chunk)
{
    size_t off = 0;

    while (off < n) {
        size_t k = n - off < chunk ? n - off : chunk;

        wave_decoder_feed(d, s + off, k, on_decode, g);
        off += k;
    }
}

/* A copy of an encoder's output, since the next encode reuses its buffer. */
static int16_t *encode_copy(struct wave_encoder *e, const char *msg, size_t len, enum wave_profile p,
                            int volume, long *n)
{
    const int16_t *s;
    char err[128];
    int16_t *copy;

    *n = wave_encoder_encode(e, (const uint8_t *)msg, len, p, volume, &s, err, sizeof(err));
    if (*n <= 0) {
        printf("     encode failed: %s\n", err);
        return NULL;
    }
    copy = malloc((size_t)*n * sizeof(int16_t));
    if (copy) {
        memcpy(copy, s, (size_t)*n * sizeof(int16_t));
    }
    return copy;
}

/* ---- tests ------------------------------------------------------------- */

static void test_formula(void)
{
    enum wave_profile p;

    check("formula: DOORS on FAST is 56 frames (1.195 s)", wave_modem_frames_for(WAVE_PROFILE_FAST, 5) == 56);
    check("formula: DOORS on NORMAL is 68 frames", wave_modem_frames_for(WAVE_PROFILE_NORMAL, 5) == 68);
    check("formula: 32 bytes on FAST is 128 frames", wave_modem_frames_for(WAVE_PROFILE_FAST, 32) == 128);
    check("formula: the longest waveform is 64 bytes on NORMAL",
          wave_modem_frames_for(WAVE_PROFILE_NORMAL, WAVE_MAX_MESSAGE_BYTES) == WAVE_MODEM_MAX_FRAMES);
    check("formula: an unknown profile is 0", wave_modem_frames_for((enum wave_profile)9, 5) == 0);
    check("profile names parse", wave_modem_parse_profile("audible_fastest", &p) == 0 &&
                                     p == WAVE_PROFILE_FASTEST);
    check("unknown profile names do not", wave_modem_parse_profile("ultrasound_fast", &p) == -1 &&
                                              wave_modem_parse_profile(NULL, &p) == -1);
    check("the watchdog allows the longest accepted message", WAVE_MODEM_WATCHDOG_FRAMES > WAVE_MODEM_MAX_FRAMES);
}

static void test_encoder(struct wave_encoder *e)
{
    const int16_t *s = (const int16_t *)1;
    char err[128];
    long n;
    char big[WAVE_MAX_MESSAGE_BYTES + 1];

    n = wave_encoder_encode(e, (const uint8_t *)"DOORS", 5, WAVE_PROFILE_FAST, 10, &s, err, sizeof(err));
    check("encode: DOORS on FAST gives 56 frames of samples", n == 56 * WAVE_MODEM_FRAME && s != NULL);
    if (n > 0) {
        int peak = pocketaudio_peak(s, (size_t)n);

        check("encode: volume 10 peaks near -20 dBFS (2500..3500)", peak >= 2500 && peak <= 3500);
        printf("     DOORS FAST volume 10: %ld samples, %.3f s, peak %d\n", n, (double)n / WAVE_MODEM_RATE, peak);
    }
    n = wave_encoder_encode(e, (const uint8_t *)"DOORS", 5, WAVE_PROFILE_FAST, WAVE_MODEM_MAX_VOLUME, &s,
                            err, sizeof(err));
    check("encode: the loudest allowed volume stays under pocketaudio's ceiling",
          n > 0 && pocketaudio_peak(s, (size_t)n) <= POCKETAUDIO_PEAK_CEILING);
    if (n > 0) {
        printf("     DOORS FAST volume %d: peak %d (ceiling %d)\n", WAVE_MODEM_MAX_VOLUME,
               pocketaudio_peak(s, (size_t)n), POCKETAUDIO_PEAK_CEILING);
    }

    n = wave_encoder_encode(e, (const uint8_t *)"", 0, WAVE_PROFILE_FAST, 10, &s, err, sizeof(err));
    check("encode: an empty message is refused", n == -1 && s == NULL && strstr(err, "empty") != NULL);
    memset(big, 'x', sizeof(big));
    n = wave_encoder_encode(e, (const uint8_t *)big, sizeof(big), WAVE_PROFILE_FAST, 10, &s, err, sizeof(err));
    check("encode: one byte over the limit is refused, not truncated", n == -1 && strstr(err, "limit") != NULL);
    n = wave_encoder_encode(e, (const uint8_t *)big, WAVE_MAX_MESSAGE_BYTES, WAVE_PROFILE_NORMAL, 10, &s,
                            err, sizeof(err));
    check("encode: the limit on the slowest profile is the longest waveform",
          n == WAVE_MODEM_MAX_SAMPLES);
    if (n > 0) {
        printf("     64 bytes NORMAL: %.3f s\n", (double)n / WAVE_MODEM_RATE);
    }
    check("encode: volume 0 is refused",
          wave_encoder_encode(e, (const uint8_t *)"a", 1, WAVE_PROFILE_FAST, 0, &s, err, sizeof(err)) == -1);
    check("encode: volume above the cap is refused",
          wave_encoder_encode(e, (const uint8_t *)"a", 1, WAVE_PROFILE_FAST, WAVE_MODEM_MAX_VOLUME + 1, &s,
                              err, sizeof(err)) == -1);
    check("encode: an unknown profile is refused",
          wave_encoder_encode(e, (const uint8_t *)"a", 1, (enum wave_profile)7, 10, &s, err, sizeof(err)) == -1);
    check("encode: NULL payload is refused",
          wave_encoder_encode(e, NULL, 3, WAVE_PROFILE_FAST, 10, &s, err, sizeof(err)) == -1);
    {
        long heap = wave_encoder_heap_bytes(e);

        check("encode: heap is allocated once and bounded (under 12 MB)", heap > 0 && heap < 12L * 1024 * 1024);
        printf("     encoder heap %ld bytes\n", heap);
    }
}

static void test_roundtrip(struct wave_encoder *e, struct wave_decoder *d)
{
    static const char *const labels[] = { "NORMAL", "FAST", "FASTEST" };
    char name[96];
    int p;

    for (p = 0; p < WAVE_PROFILE_COUNT; p++) {
        struct got g = { 0 };
        long n;
        int16_t *s = encode_copy(e, "DOORS", 5, (enum wave_profile)p, 10, &n);

        if (!s) {
            check("round trip: encode", 0);
            continue;
        }
        feed_fill(d, &g, 4800, 0);
        feed_chunked(d, &g, s, (size_t)n, 960);
        feed_fill(d, &g, 24000, 0);
        snprintf(name, sizeof(name), "round trip %s: DOORS decodes exactly once", labels[p]);
        check(name, g.decoded == 1 && g.last_len == 5 && memcmp(g.last, "DOORS", 5) == 0);
        snprintf(name, sizeof(name), "round trip %s: and nothing is reported missed", labels[p]);
        check(name, g.missed == 0);
        free(s);
    }

    {
        uint8_t bin[WAVE_MAX_MESSAGE_BYTES];
        size_t i;
        struct got g = { 0 };
        const int16_t *w;
        char err[128];
        long n;

        for (i = 0; i < sizeof(bin); i++) {
            bin[i] = (uint8_t)(i * 37 + 11);
        }
        bin[3] = 0;
        n = wave_encoder_encode(e, bin, sizeof(bin), WAVE_PROFILE_FASTEST, 10, &w, err, sizeof(err));
        if (n > 0) {
            feed_chunked(d, &g, w, (size_t)n, 960);
            feed_fill(d, &g, 24000, 0);
        }
        check("round trip: 64 arbitrary bytes, zero byte included, come back identical",
              g.decoded == 1 && g.last_len == sizeof(bin) && memcmp(g.last, bin, sizeof(bin)) == 0);
    }
    {
        const char *nordic = "Hei p\xC3\xA5 deg, \xC3\xA6\xC3\xB8\xC3\xA5!";
        struct got g = { 0 };
        long n;
        int16_t *s = encode_copy(e, nordic, strlen(nordic), WAVE_PROFILE_FAST, 10, &n);

        if (s) {
            feed_chunked(d, &g, s, (size_t)n, 960);
            feed_fill(d, &g, 24000, 0);
            free(s);
        }
        check("round trip: UTF-8 text comes back byte for byte",
              g.decoded == 1 && g.last_len == strlen(nordic) && memcmp(g.last, nordic, g.last_len) == 0);
    }
}

static void test_chunking(struct wave_encoder *e, struct wave_decoder *d)
{
    static const size_t chunks[] = { 1, 333, 512, 960, 1000, 4096, 60000 };
    char name[96];
    long n;
    int16_t *s = encode_copy(e, "chunks", 6, WAVE_PROFILE_FASTEST, 10, &n);
    size_t i;

    if (!s) {
        check("chunking: encode", 0);
        return;
    }
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        struct got g = { 0 };

        feed_chunked(d, &g, s, (size_t)n, chunks[i]);
        feed_fill(d, &g, 24000, 0);
        snprintf(name, sizeof(name), "chunking: %zu-sample pieces still decode", chunks[i]);
        check(name, g.decoded == 1 && g.last_len == 6 && memcmp(g.last, "chunks", 6) == 0);
    }
    {
        struct got g = { 0 };

        feed_fill(d, &g, 448, 0); /* a start that is not on a frame boundary */
        feed_chunked(d, &g, s, (size_t)n, 960);
        feed_fill(d, &g, 24000, 0);
        check("chunking: a misaligned start still decodes", g.decoded == 1);
    }
    free(s);
}

static void test_damage(struct wave_encoder *e, struct wave_decoder *d)
{
    long n;
    int16_t *s = encode_copy(e, "DOORS", 5, WAVE_PROFILE_FAST, 25, &n);
    struct got g = { 0 };
    unsigned stops0 = wave_decoder_watchdog_stops(d);

    if (!s) {
        check("damage: encode", 0);
        return;
    }

    /* Truncated: the start marker and part of the data, then nothing. */
    feed_chunked(d, &g, s, (size_t)n * 6 / 10, 960);
    check("truncated: the receiver is recording", wave_decoder_receiving(d));
    feed_fill(d, &g, (size_t)(WAVE_MODEM_WATCHDOG_FRAMES + 8) * WAVE_MODEM_FRAME, 0);
    check("truncated: no message is invented", g.decoded == 0);
    check("truncated: the watchdog stopped the reception", wave_decoder_watchdog_stops(d) == stops0 + 1);
    check("truncated: and reported it as missed", g.missed >= 1);
    check("truncated: the receiver is listening again", !wave_decoder_receiving(d));
    feed_chunked(d, &g, s, (size_t)n, 960);
    feed_fill(d, &g, 24000, 0);
    check("truncated: the next whole message decodes at once", g.decoded == 1);
    check("truncated: the recovery took seconds, not ggwave's 38.5",
          (WAVE_MODEM_WATCHDOG_FRAMES * WAVE_MODEM_FRAME) / WAVE_MODEM_RATE < 8);

    /* Noise on top of a message. */
    {
        int16_t *noisy = malloc((size_t)n * sizeof(int16_t));
        long i;

        memset(&g, 0, sizeof(g));
        if (noisy) {
            for (i = 0; i < n; i++) {
                int v = s[i] + noise(800);

                noisy[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
            feed_fill(d, &g, 9600, 800);
            feed_chunked(d, &g, noisy, (size_t)n, 960);
            feed_fill(d, &g, 24000, 800);
            free(noisy);
        }
        check("noise: DOORS under white noise (peak 800 vs 7980) still decodes", g.decoded == 1 &&
                                                                                    memcmp(g.last, "DOORS", 5) == 0);
    }

    /* Noise alone, and full-scale garbage. */
    memset(&g, 0, sizeof(g));
    feed_fill(d, &g, 10 * WAVE_MODEM_RATE, 3000);
    check("noise alone: 10 s produce no message", g.decoded == 0);
    feed_fill(d, &g, 5 * WAVE_MODEM_RATE, 32767);
    check("garbage: 5 s of full-scale random samples produce no message, and no crash", g.decoded == 0);
    feed_fill(d, &g, 24000, 0);

    /* Two messages in a row. */
    memset(&g, 0, sizeof(g));
    {
        long n2;
        int16_t *s2 = encode_copy(e, "second", 6, WAVE_PROFILE_FAST, 25, &n2);

        feed_fill(d, &g, 48000, 0);
        feed_chunked(d, &g, s, (size_t)n, 960);
        feed_fill(d, &g, 24000, 0);
        if (s2) {
            feed_chunked(d, &g, s2, (size_t)n2, 960);
        }
        feed_fill(d, &g, 24000, 0);
        check("two messages half a second apart both decode, in order",
              g.decoded == 2 && g.last_len == 6 && memcmp(g.last, "second", 6) == 0);
        free(s2);
    }
    free(s);
}

int main(void)
{
    char err[128];
    struct wave_encoder *e;
    struct wave_decoder *d;
    struct got g = { 0 };

    test_formula();
    e = wave_encoder_new(err, sizeof(err));
    check("an encoder can be made", e != NULL);
    d = wave_decoder_new(err, sizeof(err));
    check("a decoder can be made", d != NULL);
    if (!e || !d) {
        printf("wave_modem_test: %d checks, %d failure(s)\n", checks, failed);
        return 1;
    }
    {
        long heap = wave_decoder_heap_bytes(d);

        check("decoder heap is bounded (under 12 MB)", heap > 0 && heap < 12L * 1024 * 1024);
        printf("     decoder heap %ld bytes\n", heap);
    }
    check("feeding without a callback is refused", wave_decoder_feed(d, (int16_t[4]){ 0 }, 4, NULL, NULL) == -1);
    check("feeding NULL samples is refused", wave_decoder_feed(d, NULL, 4, on_decode, &g) == -1);
    check("feeding nothing is fine", wave_decoder_feed(d, NULL, 0, on_decode, &g) == 0);

    test_encoder(e);
    test_roundtrip(e, d);
    test_chunking(e, d);
    test_damage(e, d);

    wave_encoder_free(e);
    wave_decoder_free(d);
    wave_encoder_free(NULL);
    wave_decoder_free(NULL);
    printf("wave_modem_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
