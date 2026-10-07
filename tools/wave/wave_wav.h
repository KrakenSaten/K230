/*
 * The WAV files pos-wave reads and writes: RIFF/WAVE, PCM, signed 16-bit
 * little-endian, 1 or 2 channels. Nothing else is accepted, and the reader
 * says which part of a file it refused rather than converting it: a modem
 * that silently resampled or truncated would turn a format mistake into a
 * mysterious decode failure.
 *
 * Bounded: a file whose data would exceed WAVE_WAV_MAX_SECONDS at its own
 * rate is refused before anything is allocated. A data chunk shorter than its
 * header claims (a recording that was cut off) is read up to the last whole
 * frame, and reported as truncated.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETWAVE_WAV_H
#define POCKETWAVE_WAV_H

#include <stddef.h>
#include <stdint.h>

#define WAVE_WAV_MAX_SECONDS 120

struct wave_wav {
    unsigned rate;
    unsigned channels;
    size_t frames;
    int truncated;      /* the data chunk ended before its declared size */
    int16_t *samples;   /* frames * channels, interleaved, host order */
};

enum wave_wav_result {
    WAVE_WAV_OK = 0,
    WAVE_WAV_IO = -1,          /* cannot open or read */
    WAVE_WAV_INVALID = -2,     /* not a RIFF/WAVE file, or a broken one */
    WAVE_WAV_UNSUPPORTED = -3  /* a valid WAV in a format this does not take */
};

int wave_wav_read(const char *path, struct wave_wav *out, char *err, size_t errlen);
void wave_wav_free(struct wave_wav *w);

/* A mono S16 file. 0, or -1 with a reason. */
int wave_wav_write_mono16(const char *path, const int16_t *samples, size_t n, unsigned rate,
                          char *err, size_t errlen);

#endif
