/*
 * pos-mp3's WAV decoder, for host builds and tests: 16-bit PCM, mono or
 * stereo (mixed to mono), any rate core/pocketwav accepts, resampled
 * linearly to 48 kHz. See mp3_decoder.h. The image builds
 * mp3_decoder_ffmpeg.c instead, which also plays WAV.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mp3_decoder.h"

#include "pocketwav/pocketwav.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Input frames read per refill. */
#define WAV_CHUNK 2048

struct mp3_decoder {
    int fd;
    struct pocketwav_info info;
    uint64_t frame;       /* the next file frame to read */
    uint64_t step_q32;    /* input frames per output sample, Q32 */
    uint64_t pos_q32;     /* the next output sample's place in buf, Q32 */
    int eof;
    size_t buf_len;
    int16_t buf[WAV_CHUNK + 1];
    uint8_t raw[WAV_CHUNK * 4];
};

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s", what);
    }
}

int mp3_decoder_open(struct mp3_decoder **out, const char *path, struct mp3_dec_info *info,
                     char *err, size_t errlen)
{
    struct mp3_decoder *d;
    struct stat st;
    int r;

    *out = NULL;
    memset(info, 0, sizeof(*info));
    if (stat(path, &st) != 0) {
        say(err, errlen, errno == ENOENT || errno == ENOTDIR ? "the file is not there" : strerror(errno));
        return errno == ENOENT || errno == ENOTDIR ? MP3_DEC_E_MISSING : MP3_DEC_E_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        say(err, errlen, "not a file");
        return MP3_DEC_E_FORMAT;
    }
    if (st.st_size == 0) {
        say(err, errlen, "the file is empty");
        return MP3_DEC_E_FORMAT;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        say(err, errlen, "out of memory");
        return MP3_DEC_E_NOMEM;
    }
    d->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (d->fd < 0) {
        say(err, errlen, strerror(errno));
        free(d);
        return errno == ENOENT ? MP3_DEC_E_MISSING : MP3_DEC_E_IO;
    }
    r = pocketwav_probe_fd(d->fd, &d->info);
    if (r != POCKETWAV_OK || d->info.frames == 0) {
        say(err, errlen, r == POCKETWAV_E_IO           ? "the file could not be read"
                         : r == POCKETWAV_OK           ? "the file holds no audio"
                         : r == POCKETWAV_E_UNSUPPORTED ? "this build plays only 16-bit PCM WAV files"
                                                        : "not an audio file this build plays");
        close(d->fd);
        free(d);
        return r == POCKETWAV_E_IO ? MP3_DEC_E_IO : MP3_DEC_E_FORMAT;
    }
    d->step_q32 = ((uint64_t)d->info.rate << 32) / MP3_DEC_RATE;
    info->total_ms = (int64_t)pocketwav_frames_ms(d->info.frames, d->info.rate);
    info->seekable = 1;
    info->rate = d->info.rate;
    info->channels = d->info.channels;
    snprintf(info->codec, sizeof(info->codec), "pcm_s16le");
    *out = d;
    return MP3_DEC_OK;
}

/* Keep the last sample and add up to WAV_CHUNK more. 0 at the end, < 0 on a
 * read error. */
static int refill(struct mp3_decoder *d)
{
    size_t want = WAV_CHUNK;
    size_t got = 0;
    size_t frames;
    size_t keep = 0;
    size_t i;

    if (d->buf_len > 0) {
        d->buf[0] = d->buf[d->buf_len - 1];
        d->pos_q32 -= (uint64_t)(d->buf_len - 1) << 32;
        keep = 1;
    }
    d->buf_len = keep;
    if (d->frame >= d->info.frames) {
        d->eof = 1;
        return 0;
    }
    if (want > d->info.frames - d->frame) {
        want = (size_t)(d->info.frames - d->frame);
    }
    while (got < want * d->info.block) {
        ssize_t r = pread(d->fd, d->raw + got, want * d->info.block - got,
                          (off_t)(d->info.data_offset + d->frame * d->info.block + got));

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            break; /* shorter than it said: play what is there */
        }
        got += (size_t)r;
    }
    frames = got / d->info.block;
    for (i = 0; i < frames; i++) {
        const uint8_t *b = d->raw + i * d->info.block;
        int v = (int16_t)(b[0] | b[1] << 8);

        if (d->info.channels == 2) {
            v = (v + (int16_t)(b[2] | b[3] << 8)) / 2;
        }
        d->buf[keep + i] = (int16_t)v;
    }
    d->buf_len = keep + frames;
    d->frame += frames;
    if (frames == 0) {
        d->frame = d->info.frames;
        d->eof = 1;
    }
    return (int)frames;
}

long mp3_decoder_read(struct mp3_decoder *d, int16_t *out, size_t max)
{
    size_t n = 0;

    if (max > MP3_DEC_MAX_READ) {
        max = MP3_DEC_MAX_READ;
    }
    while (n < max) {
        size_t idx = (size_t)(d->pos_q32 >> 32);
        int32_t a;
        int32_t b;
        int32_t frac;

        if (idx + 1 >= d->buf_len && !d->eof) {
            if (refill(d) < 0) {
                return MP3_DEC_E_IO;
            }
            continue;
        }
        if (idx >= d->buf_len) {
            break;
        }
        a = d->buf[idx];
        /* The file's last sample has nothing after it: hold it. */
        b = idx + 1 < d->buf_len ? d->buf[idx + 1] : a;
        frac = (int32_t)((d->pos_q32 & 0xFFFFFFFFu) >> 17); /* Q15 */
        out[n++] = (int16_t)(a + (((b - a) * frac) >> 15));
        d->pos_q32 += d->step_q32;
    }
    return (long)n;
}

int mp3_decoder_seek(struct mp3_decoder *d, int64_t ms)
{
    uint64_t f;

    if (ms < 0) {
        ms = 0;
    }
    f = (uint64_t)ms * d->info.rate / 1000u;
    d->frame = f > d->info.frames ? d->info.frames : f;
    d->buf_len = 0;
    d->pos_q32 = 0;
    d->eof = 0;
    return MP3_DEC_OK;
}

void mp3_decoder_close(struct mp3_decoder *d)
{
    if (!d) {
        return;
    }
    if (d->fd >= 0) {
        close(d->fd);
    }
    free(d);
}
