/*
 * pos-record's signal processing, measured rather than assumed: the filter's
 * gain at DC, the decimator's passband and its rejection of what would
 * alias, the interpolator's level and images, the DC blocker on the codec's
 * settling offset, the meter on silence, full scale and a known sine, and
 * the playback limiter's promise (never above the ceiling, never louder
 * than the file, never rising again).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_dsp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

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

#define N48 48000

static int16_t in[N48];
static int16_t out[N48 * 3];

static void sine(int16_t *s, size_t n, double hz, double rate, double amp)
{
    size_t i;

    for (i = 0; i < n; i++) {
        s[i] = (int16_t)lrint(amp * sin(2.0 * M_PI * hz * (double)i / rate));
    }
}

/* RMS of s[from..n), as dB relative to a full-scale sine's RMS. */
static double rms_db(const int16_t *s, size_t from, size_t n, double ref_amp)
{
    double sum = 0.0;
    size_t i;

    for (i = from; i < n; i++) {
        sum += (double)s[i] * s[i];
    }
    return 20.0 * log10(sqrt(sum / (double)(n - from)) / (ref_amp / sqrt(2.0)) + 1e-12);
}

static double decimated_db(double hz)
{
    struct rec_decim3 d;
    size_t n;

    sine(in, N48, hz, 48000.0, 16000.0);
    rec_decim3_init(&d);
    n = rec_decim3_run(&d, in, N48, out);
    return n == N48 / 3 ? rms_db(out, 200, n, 16000.0) : -999.0;
}

static void test_taps(void)
{
    const int16_t *t = rec_fir_taps();
    int sum = 0;
    int sym = 1;
    int i;

    for (i = 0; i < REC_FIR_TAPS; i++) {
        sum += t[i];
        sym &= t[i] == t[REC_FIR_TAPS - 1 - i] || i == REC_FIR_TAPS / 2 || i == REC_FIR_TAPS / 2 - 1;
    }
    check("the taps sum to exactly unity gain at DC", sum == 32768);
    check("the filter is symmetric (linear phase)", sym);
}

static void test_decimator(void)
{
    struct rec_decim3 a;
    struct rec_decim3 b;
    int16_t o1[N48 / 3 + 1];
    int16_t o2[N48 / 3 + 1];
    size_t n1;
    size_t n2 = 0;
    size_t off;
    double pass = decimated_db(1000.0);
    double edge = decimated_db(4000.0);
    double alias = decimated_db(12000.0);
    double near = decimated_db(9500.0);

    printf("note decimator: 1 kHz %.2f dB, 4 kHz %.2f dB, 9.5 kHz %.1f dB, 12 kHz %.1f dB\n", pass,
           edge, near, alias);
    check("48 -> 16 kHz: a 1 kHz tone keeps its level (within 0.2 dB)", fabs(pass) < 0.2);
    check("speech up to 4 kHz is flat within 0.5 dB", fabs(edge) < 0.5);
    check("a 12 kHz tone, which would alias to 4 kHz, is down at least 60 dB", alias < -60.0);
    check("a 9.5 kHz tone is down at least 40 dB", near < -40.0);

    /* Any split of the input gives the same output. */
    sine(in, N48, 1234.0, 48000.0, 20000.0);
    rec_decim3_init(&a);
    rec_decim3_init(&b);
    n1 = rec_decim3_run(&a, in, N48, o1);
    for (off = 0; off < N48;) {
        size_t k = (off * 7 + 1) % 977 + 1;

        if (k > N48 - off) {
            k = N48 - off;
        }
        n2 += rec_decim3_run(&b, in + off, k, o2 + n2);
        off += k;
    }
    check("uneven chunks give the same samples as one call", n1 == n2 && memcmp(o1, o2, n1 * 2) == 0);

    /* Full-scale square wave: clamped, never wrapped. */
    {
        size_t i;
        size_t n;
        int wrapped = 0;

        for (i = 0; i < 3000; i++) {
            in[i] = (i / 30) % 2 ? 32767 : -32768;
        }
        rec_decim3_init(&a);
        n = rec_decim3_run(&a, in, 3000, o1);
        for (i = 1; i < n; i++) {
            /* A wrapped sample would jump by most of the range between two
             * neighbours in a 800 Hz square wave's plateau. */
            wrapped |= o1[i] < -30000 && o1[i - 1] > 30000 && i % 10 != 0 && i % 10 != 9;
        }
        check("a full-scale square wave saturates instead of wrapping", !wrapped);
    }
}

static void test_interpolator(void)
{
    struct rec_interp3 p;
    int16_t s16[16000];
    double pass;
    double image;
    size_t i;
    double sum_img = 0.0;

    sine(s16, 16000, 1000.0, 16000.0, 16000.0);
    rec_interp3_init(&p);
    rec_interp3_run(&p, s16, 16000, out);
    pass = rms_db(out, 600, 48000, 16000.0);
    /* The first image of 1 kHz at 16 kHz is 15 kHz: correlate against it. */
    {
        double re = 0.0;
        double im = 0.0;

        for (i = 600; i < 48000; i++) {
            re += out[i] * cos(2.0 * M_PI * 15000.0 * (double)i / 48000.0);
            im += out[i] * sin(2.0 * M_PI * 15000.0 * (double)i / 48000.0);
        }
        sum_img = 2.0 * sqrt(re * re + im * im) / (48000.0 - 600.0);
    }
    image = 20.0 * log10(sum_img / 16000.0 + 1e-12);
    printf("note interpolator: 1 kHz %.2f dB, 15 kHz image %.1f dB\n", pass, image);
    check("16 -> 48 kHz: a 1 kHz tone keeps its level (within 0.2 dB)", fabs(pass) < 0.2);
    check("its 15 kHz image is down at least 60 dB", image < -60.0);
}

static void test_dcblock(void)
{
    struct rec_dcblock d;
    size_t i;
    int late = 0;

    /* The codec's settling offset: about -1200, decaying. */
    for (i = 0; i < N48; i++) {
        in[i] = (int16_t)(-1200.0 * exp(-(double)i / 20000.0) - 300.0);
    }
    rec_dcblock_init(&d);
    rec_dcblock_run(&d, in, N48);
    for (i = N48 - 4800; i < N48; i++) {
        late = in[i] < 0 ? (-in[i] > late ? -in[i] : late) : (in[i] > late ? in[i] : late);
    }
    check("a settling DC offset is gone after a second (below 20 of 32767)", late < 20);

    sine(in, N48, 1000.0, 48000.0, 10000.0);
    rec_dcblock_init(&d);
    rec_dcblock_run(&d, in, N48);
    check("a 1 kHz tone passes the DC blocker unchanged (within 0.1 dB)",
          fabs(rms_db(in, 4800, N48, 10000.0)) < 0.1);
}

static void test_meter(void)
{
    struct rec_meter m;
    int peak;
    int rms;
    int16_t z[960];

    memset(z, 0, sizeof(z));
    rec_meter_reset(&m);
    rec_meter_read(&m, &peak, &rms);
    check("a meter with nothing added reads 0", peak == 0 && rms == 0);
    rec_meter_add(&m, z, 960);
    rec_meter_read(&m, &peak, &rms);
    check("silence reads 0", peak == 0 && rms == 0);
    z[5] = -32768;
    rec_meter_add(&m, z, 960);
    rec_meter_read(&m, &peak, &rms);
    check("-32768 reads as 32767, never overflowing", peak == 32767 && rms > 0);
    sine(in, 4800, 1000.0, 48000.0, 16384.0);
    rec_meter_reset(&m);
    rec_meter_add(&m, in, 4800);
    rec_meter_read(&m, &peak, &rms);
    check("a half-scale sine reads its peak and RMS", peak >= 16380 && peak <= 16384 &&
                                                            rms >= 11580 && rms <= 11590);
}

static void test_limiter(void)
{
    struct rec_limiter l;
    int16_t s[4];
    size_t i;
    int over = 0;
    uint32_t last;
    int rose = 0;

    rec_limiter_init(&l, 8192);
    sine(in, 4800, 440.0, 48000.0, 4000.0);
    memcpy(out, in, 4800 * 2);
    rec_limiter_run(&l, out, 4800);
    check("a quiet recording plays at its own level", memcmp(in, out, 4800 * 2) == 0 && l.gain_q16 == 65536);

    sine(in, N48, 440.0, 48000.0, 32767.0);
    rec_limiter_init(&l, 8192);
    last = l.gain_q16;
    for (i = 0; i < N48; i += 960) {
        memcpy(out, in + i, 960 * 2);
        rec_limiter_run(&l, out, 960);
        rose |= l.gain_q16 > last;
        last = l.gain_q16;
        for (size_t j = 0; j < 960; j++) {
            over |= out[j] > 8192 || out[j] < -8192;
        }
    }
    check("a full-scale recording never exceeds the -12 dBFS ceiling", !over);
    check("the gain only ever falls", !rose);
    check("and falls no further than the loudest sample needs (to about -12 dB)",
          l.gain_q16 >= 16380 && l.gain_q16 <= 16390);
    s[0] = -32768;
    s[1] = 32767;
    s[2] = 0;
    s[3] = 100;
    rec_limiter_init(&l, 8192);
    rec_limiter_run(&l, s, 4);
    check("-32768 is limited too", s[0] >= -8192 && s[1] <= 8192 && s[2] == 0);
}

int main(void)
{
    test_taps();
    test_decimator();
    test_interpolator();
    test_dcblock();
    test_meter();
    test_limiter();
    printf("rec_dsp_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
