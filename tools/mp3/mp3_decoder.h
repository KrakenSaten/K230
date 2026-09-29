/*
 * pos-mp3's decoder: a file in, 48 kHz mono signed 16-bit out - the one
 * format core/pocketaudio plays - whatever the file's own rate, channels and
 * codec.
 *
 * Two implementations, one per build, chosen by the Makefile:
 *
 *   mp3_decoder_ffmpeg.c  (MP3_FFMPEG=1, the image) FFmpeg's libavformat,
 *                         libavcodec and libswresample, which the image
 *                         already carries (docs/apps/MP3.md, "Backend"): MP3
 *                         and the other formats FFmpeg's demuxers and
 *                         decoders know.
 *   mp3_decoder_wav.c     (host builds and tests) 16-bit PCM WAV only,
 *                         through core/pocketwav, with a linear resampler.
 *                         It lets the real pos-mp3 run end to end on a host
 *                         without FFmpeg's development files.
 *
 * Every call does a bounded amount of work: read() returns after at most one
 * decoded packet (a few thousand samples), so pos-mp3 checks its commands
 * between calls. Nothing is shared between decoders; one process decodes
 * one file.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETMP3_DECODER_H
#define POCKETMP3_DECODER_H

#include <stddef.h>
#include <stdint.h>

#define MP3_DEC_RATE 48000
/* The most samples one read() returns. */
#define MP3_DEC_MAX_READ 4096

enum mp3_dec_result {
    MP3_DEC_OK = 0,
    MP3_DEC_E_MISSING = -1, /* no such file */
    MP3_DEC_E_IO = -2,      /* the file could not be read */
    MP3_DEC_E_FORMAT = -3,  /* not audio this decoder plays, damaged, or empty */
    MP3_DEC_E_DECODE = -4,  /* decoding failed part way */
    MP3_DEC_E_NOMEM = -5
};

/* Text is valid UTF-8 without control characters, at most max - 1 bytes. */
#define MP3_DEC_TEXT_MAX 129

struct mp3_dec_info {
    int64_t total_ms;             /* 0 when not known */
    int seekable;
    unsigned rate;                /* the file's own */
    unsigned channels;
    char codec[24];               /* lower case, [a-z0-9_] */
    char title[MP3_DEC_TEXT_MAX]; /* "" when the file has none */
    char artist[MP3_DEC_TEXT_MAX];
};

struct mp3_decoder;

/* Open and probe path. On failure *out is NULL and err (if not NULL) says
 * why in words. */
int mp3_decoder_open(struct mp3_decoder **out, const char *path, struct mp3_dec_info *info,
                     char *err, size_t errlen);
/* Up to max (<= MP3_DEC_MAX_READ) samples into out. Returns the number (> 0),
 * 0 at the end of the file, or a negative enum mp3_dec_result. A read that
 * returns 0 may be called again and returns 0 again. */
long mp3_decoder_read(struct mp3_decoder *d, int16_t *out, size_t max);
/* Continue from ms (clamped to the file). MP3_DEC_OK, or an error after
 * which the decoder is still usable from where it was, or E_IO. */
int mp3_decoder_seek(struct mp3_decoder *d, int64_t ms);
/* NULL is fine. */
void mp3_decoder_close(struct mp3_decoder *d);

/* Keep the valid UTF-8 of src in dst (at most dstlen - 1 bytes, never a cut
 * character), with control characters and invalid bytes dropped and
 * surrounding spaces trimmed. Shared by both decoders. */
void mp3_dec_clean_text(char *dst, size_t dstlen, const char *src);

#endif
