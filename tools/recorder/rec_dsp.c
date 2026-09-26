/*
 * pos-record's signal processing. See rec_dsp.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rec_dsp.h"

#include <math.h>
#include <string.h>

#define PHASE_TAPS (REC_FIR_TAPS / REC_RESAMPLE)
/* The low-pass: -6 dB at 7 kHz of 48 kHz, Blackman window. */
#define FIR_CUTOFF (7000.0 / 48000.0)
/* The DC blocker's pole: 1 - 2 pi 20 Hz / 48 kHz. */
#define DC_POLE 0.997382f

static int16_t taps[REC_FIR_TAPS];
static int taps_ready;

static void make_taps(void)
{
    double h[REC_FIR_TAPS];
    double sum = 0.0;
    int total = 0;
    int k;

    if (taps_ready) {
        return;
    }
    for (k = 0; k < REC_FIR_TAPS; k++) {
        double m = k - (REC_FIR_TAPS - 1) / 2.0;
        double x = 2.0 * M_PI * FIR_CUTOFF * m;
        double sinc = 2.0 * FIR_CUTOFF * (m == 0.0 ? 1.0 : sin(x) / x);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * k / (REC_FIR_TAPS - 1)) +
                   0.08 * cos(4.0 * M_PI * k / (REC_FIR_TAPS - 1));

        h[k] = sinc * w;
        sum += h[k];
    }
    for (k = 0; k < REC_FIR_TAPS; k++) {
        taps[k] = (int16_t)lrint(h[k] / sum * 32768.0);
        total += taps[k];
    }
    /* Unity gain at DC exactly: the rounding error goes into the middle. */
    taps[REC_FIR_TAPS / 2] = (int16_t)(taps[REC_FIR_TAPS / 2] + (32768 - total));
    taps_ready = 1;
}

const int16_t *rec_fir_taps(void)
{
    make_taps();
    return taps;
}

static int16_t clamp16(int64_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* Q15 product sum back to a sample, rounded half away from zero. */
static int16_t q15(int64_t acc)
{
    return clamp16(acc >= 0 ? (acc + 16384) / 32768 : -((-acc + 16384) / 32768));
}

void rec_dcblock_init(struct rec_dcblock *d)
{
    d->x1 = 0.0f;
    d->y1 = 0.0f;
}

void rec_dcblock_run(struct rec_dcblock *d, int16_t *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        float x = (float)s[i];
        float y = x - d->x1 + DC_POLE * d->y1;

        d->x1 = x;
        d->y1 = y;
        s[i] = clamp16((int64_t)lrintf(y));
    }
}

void rec_decim3_init(struct rec_decim3 *d)
{
    make_taps();
    memset(d, 0, sizeof(*d));
}

size_t rec_decim3_run(struct rec_decim3 *d, const int16_t *in, size_t n, int16_t *out)
{
    size_t produced = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        d->at = (d->at + REC_FIR_TAPS - 1) % REC_FIR_TAPS;
        d->hist[d->at] = in[i];
        d->hist[d->at + REC_FIR_TAPS] = in[i];
        if (++d->phase == REC_RESAMPLE) {
            const int16_t *x = d->hist + d->at; /* x[k] is the sample k steps back */
            int64_t acc = 0;
            int k;

            d->phase = 0;
            for (k = 0; k < REC_FIR_TAPS; k++) {
                acc += (int32_t)taps[k] * x[k];
            }
            out[produced++] = q15(acc);
        }
    }
    return produced;
}

void rec_interp3_init(struct rec_interp3 *p)
{
    make_taps();
    memset(p, 0, sizeof(*p));
}

void rec_interp3_run(struct rec_interp3 *p, const int16_t *in, size_t n, int16_t *out)
{
    size_t i;

    for (i = 0; i < n; i++) {
        const int16_t *x;
        int ph;

        p->at = (p->at + PHASE_TAPS - 1) % PHASE_TAPS;
        p->hist[p->at] = in[i];
        p->hist[p->at + PHASE_TAPS] = in[i];
        x = p->hist + p->at;
        /* Output 3i + ph is the zero-stuffed input filtered: only every
         * third tap meets a sample, and the gain of 3 restores the level
         * the zeros took away. */
        for (ph = 0; ph < REC_RESAMPLE; ph++) {
            int64_t acc = 0;
            int j;

            for (j = 0; j < PHASE_TAPS; j++) {
                acc += (int32_t)taps[REC_RESAMPLE * j + ph] * x[j];
            }
            out[REC_RESAMPLE * i + (size_t)ph] = q15(acc * REC_RESAMPLE);
        }
    }
}

void rec_meter_reset(struct rec_meter *m)
{
    memset(m, 0, sizeof(*m));
}

void rec_meter_add(struct rec_meter *m, const int16_t *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int v = s[i] < 0 ? -(int)s[i] : s[i];

        if (v > 32767) {
            v = 32767;
        }
        if (v > m->peak) {
            m->peak = v;
        }
        m->sumsq += (uint64_t)((int64_t)s[i] * s[i]);
    }
    m->n += (uint32_t)n;
}

void rec_meter_read(const struct rec_meter *m, int *peak, int *rms)
{
    double r = m->n ? sqrt((double)m->sumsq / m->n) : 0.0;

    *peak = m->peak;
    *rms = r > 32767.0 ? 32767 : (int)r;
}

void rec_limiter_init(struct rec_limiter *l, int ceiling)
{
    l->ceiling = ceiling < 1 ? 1 : ceiling > 32767 ? 32767 : ceiling;
    l->gain_q16 = 65536;
}

void rec_limiter_run(struct rec_limiter *l, int16_t *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int64_t x = s[i];
        int64_t a = x < 0 ? -x : x;
        int64_t v = x * (int64_t)l->gain_q16 / 65536;

        if ((v < 0 ? -v : v) > l->ceiling) {
            /* Just enough for this sample, and it stays there. */
            l->gain_q16 = (uint32_t)(((int64_t)l->ceiling * 65536) / a);
            v = x * (int64_t)l->gain_q16 / 65536;
        }
        s[i] = clamp16(v);
    }
}
