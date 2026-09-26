/*
 * pos-record's two loops, one step at a time: capture to a file, and a file
 * to the speaker. Each step moves at most one audio period
 * (POCKETAUDIO_PERIOD_FRAMES at 48 kHz, 20 ms) and waits at most one
 * pocketaudio bound (POCKETAUDIO_MAX_WAIT_MS), so the caller checks its stop
 * flag and its commands between steps and never waits longer than that.
 *
 * Memory is fixed: a period of samples in, a period out. Nothing of the
 * recording is kept in RAM; each period goes to the file as it arrives.
 *
 * The stream is the caller's: the helper closes it on pause and opens a new
 * one on resume, and the file, the filters and the position carry on. The
 * tests drive these steps against pocketaudio over a scripted backend (short
 * reads, timeouts, overruns, failures) and a real temporary folder.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_ENGINE_H
#define POCKETREC_ENGINE_H

#include "pocketaudio/pocketaudio.h"
#include "pocketwav/pocketwav.h"
#include "rec_dsp.h"
#include "rec_file.h"

#include <stdint.h>

/* Consecutive device waits that returned nothing before the device is taken
 * to have stopped: 10 x 200 ms. */
#define REC_STALL_WAITS 10

enum rec_step {
    REC_STEP_OK = 0,        /* audio moved */
    REC_STEP_WAIT,          /* the device had nothing within its bound; call again */
    REC_STEP_END,           /* playback: the file is done */
    REC_STEP_LIMIT_LENGTH,  /* recording: the WAV size limit is reached (what fit is written) */
    REC_STEP_STORAGE_FULL,  /* recording: ENOSPC */
    REC_STEP_STORAGE_ERROR, /* recording: another write error (errno kept) */
    REC_STEP_AUDIO_ERROR,   /* the device failed, or stopped delivering */
    REC_STEP_FILE_ERROR     /* playback: the file could not be read */
};

struct rec_recorder {
    struct rec_file file;
    unsigned rate;               /* REC_RATE_VOICE or REC_RATE_STANDARD */
    struct rec_dcblock dc;
    struct rec_decim3 decim;
    struct rec_meter meter;      /* since the caller last reset it */
    unsigned stalls;
    unsigned gaps;               /* capture overruns, kept by the caller across streams */
    int last_errno;
    int16_t in[POCKETAUDIO_PERIOD_FRAMES];
    int16_t out[POCKETAUDIO_PERIOD_FRAMES / REC_RESAMPLE + 1];
};

/* Create the recording's file; no device is touched. 0 or -errno
 * (rec_file_create). */
int rec_recorder_open(struct rec_recorder *r, const char *dir, const char *name, unsigned rate);
/* One period from s into the file. */
enum rec_step rec_recorder_step(struct rec_recorder *r, struct pocketaudio_stream *s);
/* Milliseconds of audio in the file. */
uint64_t rec_recorder_ms(const struct rec_recorder *r);

struct rec_player {
    int fd;
    struct pocketwav_info info;
    uint64_t frame;              /* the next file frame to read */
    struct rec_interp3 interp;
    struct rec_limiter limiter;
    struct rec_meter meter;
    unsigned stalls;
    int16_t out[POCKETAUDIO_PERIOD_FRAMES];
    size_t out_len;
    size_t out_off;
    uint8_t raw[POCKETAUDIO_PERIOD_FRAMES * 4];
};

/* Open a WAV for playback from start_ms. Plays 16-bit PCM, mono or stereo
 * (mixed to mono), at 48 kHz or 16 kHz: what Recorder writes. 0, or a
 * pocketwav_result (POCKETWAV_E_UNSUPPORTED for another rate). */
int rec_player_open(struct rec_player *p, const char *path, uint64_t start_ms);
/* One period of the file into s. */
enum rec_step rec_player_step(struct rec_player *p, struct pocketaudio_stream *s);
uint64_t rec_player_pos_ms(const struct rec_player *p);
uint64_t rec_player_total_ms(const struct rec_player *p);
void rec_player_close(struct rec_player *p);

/* Free bytes for an unprivileged writer on dir's filesystem, or -errno. */
int64_t rec_free_bytes(const char *dir);
/* Test seam, NULL in use: replaces statvfs (pos-record's test-hooks build). */
extern int64_t (*rec_free_hook)(const char *dir);

#endif
