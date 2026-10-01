/*
 * pos-record's record and play steps. See rec_engine.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_engine.h"

#include "rec_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

int64_t (*rec_free_hook)(const char *dir);

int64_t rec_free_bytes(const char *dir)
{
    struct statvfs v;

    if (rec_free_hook) {
        return rec_free_hook(dir);
    }
    if (statvfs(dir, &v) != 0) {
        return -errno;
    }
    return (int64_t)v.f_bavail * (int64_t)v.f_frsize;
}

/* ---- recording ---------------------------------------------------------- */

int rec_recorder_open(struct rec_recorder *r, const char *dir, const char *name, unsigned rate)
{
    memset(r, 0, sizeof(*r));
    if (rate != REC_RATE_VOICE && rate != REC_RATE_STANDARD) {
        return -EINVAL;
    }
    r->rate = rate;
    rec_dcblock_init(&r->dc);
    rec_decim3_init(&r->decim);
    rec_meter_reset(&r->meter);
    return rec_file_create(&r->file, dir, name, rate);
}

uint64_t rec_recorder_ms(const struct rec_recorder *r)
{
    return pocketwav_frames_ms(r->file.data_bytes / 2u, r->rate);
}

enum rec_step rec_recorder_step(struct rec_recorder *r, struct pocketaudio_stream *s)
{
    const int16_t *samples;
    size_t n;
    size_t want;
    long got = pocketaudio_read(s, r->in, POCKETAUDIO_PERIOD_FRAMES);
    uint64_t room;
    int e;

    if (got < 0) {
        return REC_STEP_AUDIO_ERROR;
    }
    if (got == 0) {
        return ++r->stalls >= REC_STALL_WAITS ? REC_STEP_AUDIO_ERROR : REC_STEP_WAIT;
    }
    r->stalls = 0;
    rec_dcblock_run(&r->dc, r->in, (size_t)got);
    if (r->rate == REC_RATE_VOICE) {
        n = rec_decim3_run(&r->decim, r->in, (size_t)got, r->out);
        samples = r->out;
    } else {
        n = (size_t)got;
        samples = r->in;
    }
    /* The limit is checked before the write: a recording never grows past
     * what a WAV header can describe, and what still fits is kept. */
    want = n;
    room = rec_file_room(&r->file) / 2u;
    if (n > room) {
        n = (size_t)room;
    }
    rec_meter_add(&r->meter, samples, n);
    e = n ? rec_file_append(&r->file, samples, n) : 0;
    if (e == -ENOSPC || e == -EDQUOT) {
        r->last_errno = -e;
        return REC_STEP_STORAGE_FULL;
    }
    if (e != 0) {
        r->last_errno = -e;
        return e == -EFBIG ? REC_STEP_LIMIT_LENGTH : REC_STEP_STORAGE_ERROR;
    }
    return n < want || rec_file_room(&r->file) < 2u ? REC_STEP_LIMIT_LENGTH : REC_STEP_OK;
}

/* ---- playback ----------------------------------------------------------- */

int rec_player_open(struct rec_player *p, const char *path, uint64_t start_ms)
{
    int rc;

    memset(p, 0, sizeof(*p));
    p->fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (p->fd < 0) {
        return POCKETWAV_E_IO;
    }
    rc = pocketwav_probe_fd(p->fd, &p->info);
    if (rc == POCKETWAV_OK && p->info.rate != REC_RATE_VOICE && p->info.rate != REC_RATE_STANDARD) {
        rc = POCKETWAV_E_UNSUPPORTED;
    }
    if (rc != POCKETWAV_OK) {
        close(p->fd);
        p->fd = -1;
        return rc;
    }
    p->frame = start_ms * p->info.rate / 1000u;
    if (p->frame > p->info.frames) {
        p->frame = p->info.frames;
    }
    rec_interp3_init(&p->interp);
    rec_limiter_init(&p->limiter, POCKETAUDIO_PEAK_CEILING);
    rec_meter_reset(&p->meter);
    return POCKETWAV_OK;
}

uint64_t rec_player_pos_ms(const struct rec_player *p)
{
    return pocketwav_frames_ms(p->frame, p->info.rate);
}

uint64_t rec_player_total_ms(const struct rec_player *p)
{
    return pocketwav_frames_ms(p->info.frames, p->info.rate);
}

/* The next period of the file, as 48 kHz mono in p->out. 0 frames at the
 * end, -1 when the file cannot be read. */
static long fill(struct rec_player *p)
{
    size_t want = p->info.rate == REC_RATE_VOICE ? POCKETAUDIO_PERIOD_FRAMES / REC_RESAMPLE
                                                 : POCKETAUDIO_PERIOD_FRAMES;
    size_t bytes;
    size_t got = 0;
    size_t frames;
    size_t i;
    int16_t mono[POCKETAUDIO_PERIOD_FRAMES];

    if (p->frame >= p->info.frames) {
        return 0;
    }
    if (want > p->info.frames - p->frame) {
        want = (size_t)(p->info.frames - p->frame);
    }
    bytes = want * p->info.block;
    while (got < bytes) {
        ssize_t r = pread(p->fd, p->raw + got, bytes - got,
                          (off_t)(p->info.data_offset + p->frame * p->info.block + got));

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            break; /* the file got shorter under us: play what is there */
        }
        got += (size_t)r;
    }
    frames = got / p->info.block;
    for (i = 0; i < frames; i++) {
        const uint8_t *b = p->raw + i * p->info.block;
        int v = (int16_t)(b[0] | b[1] << 8);

        if (p->info.channels == 2) {
            v = (v + (int16_t)(b[2] | b[3] << 8)) / 2;
        }
        mono[i] = (int16_t)v;
    }
    p->frame += frames;
    if (frames == 0) {
        p->frame = p->info.frames;
        return 0;
    }
    if (p->info.rate == REC_RATE_VOICE) {
        rec_interp3_run(&p->interp, mono, frames, p->out);
        frames *= REC_RESAMPLE;
    } else {
        memcpy(p->out, mono, frames * sizeof(int16_t));
    }
    rec_limiter_run(&p->limiter, p->out, frames);
    rec_meter_add(&p->meter, p->out, frames);
    p->out_len = frames;
    p->out_off = 0;
    return (long)frames;
}

enum rec_step rec_player_step(struct rec_player *p, struct pocketaudio_stream *s)
{
    long w;

    if (p->out_off >= p->out_len) {
        long n = fill(p);

        if (n < 0) {
            return REC_STEP_FILE_ERROR;
        }
        if (n == 0) {
            return REC_STEP_END;
        }
    }
    w = pocketaudio_write(s, p->out + p->out_off, p->out_len - p->out_off);
    if (w < 0) {
        return REC_STEP_AUDIO_ERROR;
    }
    if (w == 0) {
        return ++p->stalls >= REC_STALL_WAITS ? REC_STEP_AUDIO_ERROR : REC_STEP_WAIT;
    }
    p->stalls = 0;
    p->out_off += (size_t)w;
    return REC_STEP_OK;
}

void rec_player_close(struct rec_player *p)
{
    if (p->fd >= 0) {
        close(p->fd);
    }
    p->fd = -1;
}
