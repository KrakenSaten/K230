/*
 * pos-wave's WAV reader and writer. See wave_wav.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_wav.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORMAT_PCM 1
#define FORMAT_EXTENSIBLE 0xFFFE

static void say(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || !errlen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

void wave_wav_free(struct wave_wav *w)
{
    if (w) {
        free(w->samples);
        memset(w, 0, sizeof(*w));
    }
}

int wave_wav_read(const char *path, struct wave_wav *out, char *err, size_t errlen)
{
    unsigned char hdr[12];
    unsigned char ch[8];
    unsigned char fmt[40];
    int have_fmt = 0;
    unsigned format = 0;
    unsigned bits = 0;
    unsigned block = 0;
    FILE *f;
    int rc = WAVE_WAV_INVALID;

    memset(out, 0, sizeof(*out));
    f = fopen(path, "rb");
    if (!f) {
        say(err, errlen, "cannot open %s: %s", path, strerror(errno));
        return WAVE_WAV_IO;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        say(err, errlen, "not a RIFF/WAVE file");
        goto done;
    }
    for (;;) {
        uint32_t size;

        if (fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
            say(err, errlen, have_fmt ? "no data chunk" : "no fmt chunk");
            goto done;
        }
        size = le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            size_t want = size < sizeof(fmt) ? size : sizeof(fmt);

            if (size < 16 || fread(fmt, 1, want, f) != want) {
                say(err, errlen, "broken fmt chunk");
                goto done;
            }
            if (size > want && fseek(f, (long)(size - want), SEEK_CUR) != 0) {
                say(err, errlen, "broken fmt chunk");
                goto done;
            }
            format = le16(fmt);
            out->channels = le16(fmt + 2);
            out->rate = le32(fmt + 4);
            block = le16(fmt + 12);
            bits = le16(fmt + 14);
            if (format == FORMAT_EXTENSIBLE && want >= 26) {
                format = le16(fmt + 24); /* the sub-format GUID starts with the code */
            }
            have_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            size_t frames;
            size_t got;

            if (!have_fmt) {
                say(err, errlen, "data before fmt");
                goto done;
            }
            if (format != FORMAT_PCM) {
                say(err, errlen, "unsupported sample format (format code %u, need PCM)", format);
                rc = WAVE_WAV_UNSUPPORTED;
                goto done;
            }
            if (bits != 16) {
                say(err, errlen, "unsupported sample format (%u-bit, need 16-bit)", bits);
                rc = WAVE_WAV_UNSUPPORTED;
                goto done;
            }
            if (out->channels < 1 || out->channels > 2 || block != out->channels * 2) {
                say(err, errlen, "unsupported channel layout (%u channels)", out->channels);
                rc = WAVE_WAV_UNSUPPORTED;
                goto done;
            }
            if (out->rate < 8000 || out->rate > 192000) {
                say(err, errlen, "invalid sample rate %u", out->rate);
                goto done;
            }
            frames = size / block;
            if (frames > (size_t)WAVE_WAV_MAX_SECONDS * out->rate) {
                /* arecord writes a placeholder size when it cannot seek back;
                 * cap rather than refuse, and read what is there. */
                frames = (size_t)WAVE_WAV_MAX_SECONDS * out->rate;
                out->truncated = 1;
            }
            out->samples = malloc((frames ? frames : 1) * block);
            if (!out->samples) {
                say(err, errlen, "out of memory");
                rc = WAVE_WAV_IO;
                goto done;
            }
            got = fread(out->samples, block, frames, f);
            if (got < frames) {
                out->truncated = 1;
            }
            out->frames = got;
            {
                /* Little-endian on disk; make it host order whatever the host. */
                unsigned char *b = (unsigned char *)out->samples;
                size_t i;

                for (i = 0; i < got * out->channels; i++) {
                    out->samples[i] = (int16_t)(b[2 * i] | b[2 * i + 1] << 8);
                }
            }
            rc = WAVE_WAV_OK;
            goto done;
        } else {
            if (fseek(f, (long)size + (long)(size & 1), SEEK_CUR) != 0) {
                say(err, errlen, "broken chunk");
                goto done;
            }
        }
    }

done:
    fclose(f);
    if (rc != WAVE_WAV_OK) {
        wave_wav_free(out);
    }
    return rc;
}

int wave_wav_write_mono16(const char *path, const int16_t *samples, size_t n, unsigned rate,
                          char *err, size_t errlen)
{
    unsigned char h[44];
    uint32_t data = (uint32_t)(n * 2);
    FILE *f;
    size_t i;
    int ok;

#define PUT32(p, v) ((p)[0] = (unsigned char)(v), (p)[1] = (unsigned char)((v) >> 8), \
                     (p)[2] = (unsigned char)((v) >> 16), (p)[3] = (unsigned char)((v) >> 24))
#define PUT16(p, v) ((p)[0] = (unsigned char)(v), (p)[1] = (unsigned char)((v) >> 8))
    if (n > (size_t)WAVE_WAV_MAX_SECONDS * rate) {
        say(err, errlen, "too long for a WAV here");
        return -1;
    }
    memcpy(h, "RIFF", 4);
    PUT32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    PUT32(h + 16, 16);
    PUT16(h + 20, FORMAT_PCM);
    PUT16(h + 22, 1);
    PUT32(h + 24, rate);
    PUT32(h + 28, rate * 2);
    PUT16(h + 32, 2);
    PUT16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    PUT32(h + 40, data);
    f = fopen(path, "wb");
    if (!f) {
        say(err, errlen, "cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    for (i = 0; ok && i < n; i++) {
        unsigned char s[2];

        PUT16(s, (uint16_t)samples[i]);
        ok = fwrite(s, 1, 2, f) == 2;
    }
#undef PUT32
#undef PUT16
    if (fclose(f) != 0 || !ok) {
        say(err, errlen, "cannot write %s", path);
        return -1;
    }
    return 0;
}
