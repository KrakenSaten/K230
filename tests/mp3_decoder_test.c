/*
 * pos-mp3's host decoder (tools/mp3/mp3_decoder_wav.c) and the tag cleaner
 * both decoders share (tools/mp3/mp3_dec_text.c): 48 kHz mono out of any
 * rate and one or two channels, the length and position, seeking, the end,
 * and every way a file can be wrong - missing, empty, a directory, not a
 * WAV, a WAV in a format it does not play, cut short.
 *
 * The image's FFmpeg decoder (mp3_decoder_ffmpeg.c) cannot be built on this
 * host; it is checked on the device (docs/hardware/MP3_GATE.md).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mp3_decoder.h"
#include "pocketwav/pocketwav.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;
static int checks;
static char dir[] = "/tmp/mp3_decoder_test.XXXXXX";

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* A WAV of a sine at hz, amplitude amp, frames long; the right channel (if
 * any) carries -value, so a correct mono mix of a stereo file is silence
 * when mirror is set. */
static void write_wav(const char *path, unsigned rate, unsigned channels, unsigned frames, double hz,
                      int amp, int mirror)
{
    uint8_t h[POCKETWAV_HEADER_BYTES];
    FILE *f = fopen(path, "wb");
    unsigned i;

    pocketwav_header(h, rate, channels, frames * channels * 2u);
    fwrite(h, 1, sizeof(h), f);
    for (i = 0; i < frames; i++) {
        int16_t v = (int16_t)lrint(amp * sin(2.0 * M_PI * hz * i / rate));
        int16_t r = mirror ? (int16_t)-v : v;

        fwrite(&v, 2, 1, f);
        if (channels == 2) {
            fwrite(&r, 2, 1, f);
        }
    }
    fclose(f);
}

static char *at(const char *name)
{
    static char buf[4][256];
    static int k;

    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "%s/%s", dir, name);
    return buf[k];
}

/* Read everything; the samples read, the peak, and the number of reads. */
static long drain(struct mp3_decoder *d, int *peak, int *reads, int *err)
{
    int16_t buf[MP3_DEC_MAX_READ];
    long total = 0;

    *peak = 0;
    *reads = 0;
    *err = 0;
    for (;;) {
        long n = mp3_decoder_read(d, buf, sizeof(buf) / sizeof(buf[0]));
        long i;

        if (n < 0) {
            *err = (int)n;
            return total;
        }
        if (n == 0) {
            return total;
        }
        (*reads)++;
        for (i = 0; i < n; i++) {
            int a = abs(buf[i]);

            if (a > *peak) {
                *peak = a;
            }
        }
        total += n;
    }
}

static void rates(void)
{
    static const unsigned r[] = { 8000, 16000, 22050, 44100, 48000, 96000 };
    size_t k;

    for (k = 0; k < sizeof(r) / sizeof(r[0]); k++) {
        struct mp3_decoder *d;
        struct mp3_dec_info info;
        char what[160];
        int peak;
        int reads;
        int err;
        long n;
        long want = 48000; /* one second, whatever the rate */

        write_wav(at("tone.wav"), r[k], 1, r[k], 440.0, 16000, 0);
        check("a mono WAV opens", mp3_decoder_open(&d, at("tone.wav"), &info, NULL, 0) == MP3_DEC_OK);
        snprintf(what, sizeof(what), "%u Hz: it says its rate, one channel, 1000 ms, seekable, the codec", r[k]);
        check(what, info.rate == r[k] && info.channels == 1 && info.total_ms == 1000 && info.seekable &&
                        strcmp(info.codec, "pcm_s16le") == 0 && !info.title[0] && !info.artist[0]);
        n = drain(d, &peak, &reads, &err);
        snprintf(what, sizeof(what), "%u Hz: one second becomes 48000 samples (+-2): %ld, peak %d", r[k], n, peak);
        check(what, err == 0 && labs(n - want) <= 2 && peak > 15000 && peak <= 16001);
        check("  and every read is bounded", reads >= n / MP3_DEC_MAX_READ);
        check("  and the end stays the end", mp3_decoder_read(d, (int16_t[8]){ 0 }, 8) == 0);
        mp3_decoder_close(d);
    }
}

static void stereo(void)
{
    struct mp3_decoder *d;
    struct mp3_dec_info info;
    int peak;
    int reads;
    int err;
    long n;

    write_wav(at("stereo.wav"), 44100, 2, 44100, 1000.0, 20000, 1);
    check("a stereo WAV opens and says two channels",
          mp3_decoder_open(&d, at("stereo.wav"), &info, NULL, 0) == MP3_DEC_OK && info.channels == 2);
    n = drain(d, &peak, &reads, &err);
    check("both channels are mixed: opposite channels cancel to silence", err == 0 && labs(n - 48000) <= 2 && peak <= 1);
    mp3_decoder_close(d);

    write_wav(at("stereo2.wav"), 48000, 2, 4800, 1000.0, 20000, 0);
    mp3_decoder_open(&d, at("stereo2.wav"), &info, NULL, 0);
    n = drain(d, &peak, &reads, &err);
    check("equal channels keep their level", n == 4800 && peak > 19900 && peak <= 20001);
    mp3_decoder_close(d);
}

static void seeking(void)
{
    struct mp3_decoder *d;
    struct mp3_dec_info info;
    int peak;
    int reads;
    int err;
    long n;

    write_wav(at("long.wav"), 22050, 1, 22050 * 4, 440.0, 8000, 0);
    mp3_decoder_open(&d, at("long.wav"), &info, NULL, 0);
    check("four seconds", info.total_ms == 4000);
    check("a seek to 3 s", mp3_decoder_seek(d, 3000) == MP3_DEC_OK);
    n = drain(d, &peak, &reads, &err);
    check("leaves one second to play", labs(n - 48000) <= 2);
    check("a seek back to the start after the end", mp3_decoder_seek(d, 0) == MP3_DEC_OK);
    n = drain(d, &peak, &reads, &err);
    check("plays all four seconds again", labs(n - 4 * 48000) <= 2);
    check("a seek past the end", mp3_decoder_seek(d, 99000) == MP3_DEC_OK);
    check("is the end", mp3_decoder_read(d, (int16_t[8]){ 0 }, 8) == 0);
    mp3_decoder_seek(d, -5);
    n = drain(d, &peak, &reads, &err);
    check("a negative seek is the start", labs(n - 4 * 48000) <= 2);
    mp3_decoder_close(d);
}

static void broken(void)
{
    struct mp3_decoder *d = (struct mp3_decoder *)1;
    struct mp3_dec_info info;
    char err[160];
    FILE *f;
    int fd;
    uint8_t h[POCKETWAV_HEADER_BYTES];

    check("a missing file is MISSING, and says so",
          mp3_decoder_open(&d, at("nothing.mp3"), &info, err, sizeof(err)) == MP3_DEC_E_MISSING && !d &&
              strstr(err, "not there"));
    check("a path through a file is MISSING too",
          mp3_decoder_open(&d, "/etc/hostname/x.mp3", &info, err, sizeof(err)) == MP3_DEC_E_MISSING);
    fd = open(at("empty.mp3"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    close(fd);
    check("a zero-length file is FORMAT: empty",
          mp3_decoder_open(&d, at("empty.mp3"), &info, err, sizeof(err)) == MP3_DEC_E_FORMAT && !d &&
              strstr(err, "empty"));
    mkdir(at("adir.mp3"), 0755);
    check("a directory is FORMAT", mp3_decoder_open(&d, at("adir.mp3"), &info, err, sizeof(err)) == MP3_DEC_E_FORMAT);
    f = fopen(at("fake.mp3"), "wb");
    fputs("ID3 this is not really audio at all, just text pretending to be an MP3 file\n", f);
    fclose(f);
    check("text named .mp3 is FORMAT (this decoder plays WAV only)",
          mp3_decoder_open(&d, at("fake.mp3"), &info, err, sizeof(err)) == MP3_DEC_E_FORMAT && !d);
    /* 24-bit PCM: a valid WAV in a format it does not play. */
    f = fopen(at("wide.wav"), "wb");
    pocketwav_header(h, 48000, 1, 3000);
    h[34] = 24;
    h[32] = 3;
    h[28] = (uint8_t)(48000 * 3);
    h[29] = (uint8_t)((48000 * 3) >> 8);
    h[30] = (uint8_t)((48000 * 3) >> 16);
    fwrite(h, 1, sizeof(h), f);
    for (fd = 0; fd < 3000; fd++) {
        fputc(0, f);
    }
    fclose(f);
    check("a 24-bit WAV is FORMAT, not a crash",
          mp3_decoder_open(&d, at("wide.wav"), &info, err, sizeof(err)) == MP3_DEC_E_FORMAT && !d);
    /* A header that promises data that is not there. */
    f = fopen(at("hollow.wav"), "wb");
    pocketwav_header(h, 48000, 1, 96000);
    fwrite(h, 1, sizeof(h), f);
    fclose(f);
    check("a header with no samples is FORMAT: nothing to play",
          mp3_decoder_open(&d, at("hollow.wav"), &info, err, sizeof(err)) == MP3_DEC_E_FORMAT && !d);
    /* Cut short: plays what is there. */
    write_wav(at("cut.wav"), 48000, 1, 48000, 440.0, 8000, 0);
    if (truncate(at("cut.wav"), POCKETWAV_HEADER_BYTES + 2 * 12000) == 0) {
        int peak;
        int reads;
        int e;
        long n;

        check("a file cut short opens", mp3_decoder_open(&d, at("cut.wav"), &info, NULL, 0) == MP3_DEC_OK);
        n = drain(d, &peak, &reads, &e);
        check("and plays what is there, then ends", e == 0 && labs(n - 12000) <= 2);
        mp3_decoder_close(d);
    }
    mp3_decoder_close(NULL);
    check("closing nothing is fine", 1);
}

static void text(void)
{
    char out[16];
    char big[MP3_DEC_TEXT_MAX];
    char in[400];

    mp3_dec_clean_text(out, sizeof(out), "  Hello  ");
    check("spaces around are trimmed", strcmp(out, "Hello") == 0);
    mp3_dec_clean_text(out, sizeof(out), "a\tb\nc\x01" "d");
    check("tabs and line breaks become spaces, other controls go", strcmp(out, "a b cd") == 0);
    mp3_dec_clean_text(out, sizeof(out), "Bj\xc3\xb6rk");
    check("UTF-8 is kept", strcmp(out, "Bj\xc3\xb6rk") == 0);
    mp3_dec_clean_text(out, sizeof(out), "Bj\xf6rk \xff\xfe");
    check("Latin-1 bytes and junk are dropped, never passed on", strcmp(out, "Bjrk") == 0);
    mp3_dec_clean_text(out, sizeof(out), "\xc0\xaf \xed\xa0\x80 ok");
    check("overlong forms and surrogates are dropped", strcmp(out, "ok") == 0);
    mp3_dec_clean_text(out, 6, "abcd\xc3\xa9");
    check("a cut never splits a character", strcmp(out, "abcd") == 0);
    memset(in, 'x', sizeof(in) - 1);
    in[sizeof(in) - 1] = '\0';
    mp3_dec_clean_text(big, sizeof(big), in);
    check("long text is cut to the limit", strlen(big) == MP3_DEC_TEXT_MAX - 1);
    mp3_dec_clean_text(out, sizeof(out), NULL);
    check("no text is empty text", out[0] == '\0');
}

int main(void)
{
    char cmd[128];

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    rates();
    stereo();
    seeking();
    broken();
    text();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", dir);
    }
    printf("mp3_decoder_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
