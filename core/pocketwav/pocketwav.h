/*
 * pocketwav: the WAV container, and nothing else - no audio device, no
 * allocation, no file names.
 *
 * What Doors writes is the canonical 44-byte header: RIFF/WAVE, one "fmt "
 * chunk (PCM, 16 bits), then "data". What it reads is any RIFF/WAVE whose
 * data is signed 16-bit PCM in one or two channels, with the chunks before
 * "data" inside the first POCKETWAV_PROBE_BYTES of the file.
 *
 * SIZES. Both size fields of a WAV are 32-bit, and several readers treat
 * them as signed. So the data of a file written here never exceeds
 * POCKETWAV_MAX_DATA_BYTES (just under 2 GiB, whole 4-byte frames): the RIFF
 * size is then at most 2^31 - 1 and no reader sees a negative or wrapped
 * length. At 48 kHz mono that is 6 h 12 min, at 16 kHz mono 18 h 38 min.
 *
 * WHAT A FILE CLAIMS AND WHAT IT HOLDS. pocketwav_probe_fd() reports both:
 * the data size the header declares, and the whole frames the file really
 * holds after the data chunk's start. They differ for a file whose writer
 * was interrupted (a header written earlier, or a placeholder); the reader
 * uses what is present and says which way they differ. pocketwav_fix_fd()
 * makes the header tell the truth about what is present, and cuts a trailing
 * partial frame, which is how an interrupted recording is repaired.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETWAV_H
#define POCKETOS_POCKETWAV_H

#include <stdint.h>

#define POCKETWAV_HEADER_BYTES 44u
/* RIFF size 36 + data <= 2^31 - 1, rounded down to whole 4-byte frames. */
#define POCKETWAV_MAX_DATA_BYTES 0x7FFFFFD8u
/* The chunks before "data" must start within this many bytes. */
#define POCKETWAV_PROBE_BYTES 4096u
#define POCKETWAV_MIN_RATE 8000u
#define POCKETWAV_MAX_RATE 192000u

enum pocketwav_result {
    POCKETWAV_OK = 0,
    POCKETWAV_E_IO = -1,          /* cannot read or write the file */
    POCKETWAV_E_INVALID = -2,     /* not a RIFF/WAVE file, or a broken one */
    POCKETWAV_E_UNSUPPORTED = -3  /* a valid WAV in a format Doors does not play */
};

struct pocketwav_info {
    unsigned rate;
    unsigned channels;
    unsigned block;          /* bytes per frame */
    uint64_t file_bytes;
    uint64_t data_offset;    /* where the samples start */
    uint64_t data_declared;  /* what the data chunk's header says */
    uint64_t data_present;   /* whole frames really in the file, capped at declared */
    uint64_t frames;         /* data_present / block */
    int truncated;           /* the file ends before the declared data does */
    int trailing;            /* whole frames beyond the declared size (an unfinished header) */
};

/* The canonical header for data_bytes of 16-bit PCM. */
void pocketwav_header(uint8_t out[POCKETWAV_HEADER_BYTES], unsigned rate, unsigned channels,
                      uint32_t data_bytes);

/* Read and check fd's header (pread; the file offset is not moved). */
int pocketwav_probe_fd(int fd, struct pocketwav_info *info);

/* Make the header's two sizes match the whole frames present after
 * data_offset (capped at POCKETWAV_MAX_DATA_BYTES) and cut anything after
 * them. Accepts a data size that is short, long or a placeholder. Does not
 * sync. 0 with *info describing the result, or a pocketwav_result. */
int pocketwav_fix_fd(int fd, struct pocketwav_info *info);

/* Milliseconds of audio in frames at rate, rounded down. */
uint64_t pocketwav_frames_ms(uint64_t frames, unsigned rate);

const char *pocketwav_strerror(int result);

#endif
