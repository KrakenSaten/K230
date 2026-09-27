/*
 * The little signal processing a recorder needs, in fixed memory and no
 * allocation. Used by pos-record only.
 *
 *   DC blocker   A one-pole high-pass at about 20 Hz. The K230 codec's
 *                capture settles with a DC offset (about -1200 right after
 *                the start-up discard, AUDIO_HARDWARE_MAP §14 item 12) that
 *                would otherwise be written as a thump at the start of every
 *                file and read by the meter as sound in a silent room.
 *   3:1 resampler  48 kHz <-> 16 kHz for the Voice preset: a 96-tap
 *                windowed-sinc low-pass (Blackman, cut-off 7 kHz), polyphase
 *                both ways, Q15 taps and 64-bit sums. The codec runs at
 *                48 kHz only, so this is the one resampling in the path, and
 *                only for Voice; Standard writes the device's samples.
 *   Meter        Peak and RMS over a window of samples, from the samples
 *                actually written or played. Silence reads 0.
 *   Limiter      For playback through pocketaudio, which clamps every sample
 *                to its -12 dBFS ceiling (POCKETAUDIO_PEAK_CEILING): a gain
 *                that starts at unity and is lowered, never raised, exactly
 *                as far as the loudest sample so far needs. A quiet
 *                recording plays at its own level; a loud one is turned down
 *                instead of being clipped. No pumping, no look-ahead.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_DSP_H
#define POCKETREC_DSP_H

#include <stddef.h>
#include <stdint.h>

#define REC_FIR_TAPS 96
#define REC_RESAMPLE 3

struct rec_dcblock {
    float x1;
    float y1;
};

struct rec_decim3 {
    int16_t hist[2 * REC_FIR_TAPS]; /* each sample stored twice: no wrap in the sum */
    unsigned at;
    unsigned phase;
};

struct rec_interp3 {
    int16_t hist[2 * (REC_FIR_TAPS / REC_RESAMPLE)];
    unsigned at;
};

struct rec_meter {
    int peak;
    uint64_t sumsq;
    uint32_t n;
};

struct rec_limiter {
    int ceiling;
    uint32_t gain_q16; /* 65536 is unity; never rises */
};

void rec_dcblock_init(struct rec_dcblock *d);
/* In place. */
void rec_dcblock_run(struct rec_dcblock *d, int16_t *s, size_t n);

void rec_decim3_init(struct rec_decim3 *d);
/* n 48 kHz samples in, up to n / 3 + 1 16 kHz samples out (returned). The
 * phase carries across calls, so any split of the input gives the same
 * output. out may not alias in. */
size_t rec_decim3_run(struct rec_decim3 *d, const int16_t *in, size_t n, int16_t *out);

void rec_interp3_init(struct rec_interp3 *p);
/* n 16 kHz samples in, exactly 3n 48 kHz samples out. */
void rec_interp3_run(struct rec_interp3 *p, const int16_t *in, size_t n, int16_t *out);

void rec_meter_reset(struct rec_meter *m);
void rec_meter_add(struct rec_meter *m, const int16_t *s, size_t n);
/* Peak and RMS, 0..32767, of what was added since the reset; 0 for none. */
void rec_meter_read(const struct rec_meter *m, int *peak, int *rms);

void rec_limiter_init(struct rec_limiter *l, int ceiling);
void rec_limiter_run(struct rec_limiter *l, int16_t *s, size_t n);

/* The filter's taps (Q15, summing to 32768), for the test. */
const int16_t *rec_fir_taps(void);

#endif
