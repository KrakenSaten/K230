/*
 * pos-drmtest's pure logic: the K230 DSI pixel-clock quantisation, EDID
 * decoding, the choice of a conservative test mode and the test pattern.
 * No DRM, no I/O, so tests/drmtest_test.c runs it on the build host.
 *
 * The clock rule is the pinned kernel's, not a guess: canaan_dsi.c
 * (canaan_dsi_encoder_mode_fixup) turns every requested pixel clock into
 * 594000 kHz / round(594000 / clock), so only clocks that divide 594 MHz are
 * reproduced exactly. docs/hardware/HDMI_OUTPUT.md explains why that matters
 * for an HDMI monitor.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_DRMTEST_LOGIC_H
#define POCKETOS_DRMTEST_LOGIC_H

#include <stddef.h>
#include <stdint.h>

/* The display PLL the K230 DSI divides from (canaan_dsi.c). */
#define DT_DSI_SOURCE_KHZ 594000
/* Above this error a clock counts as inexact: 0.5 %. HDMI sinks tolerate
 * more in practice, but the first test on a new path should not rely on it. */
#define DT_EXACT_PPM 5000
/* The first test stays at or below 1080p60's pixel clock (CEA 148.5 MHz). */
#define DT_SAFE_MAX_KHZ 148500

/* The clock the DSI host will really produce for a requested one, and the
 * error in parts per million (negative: slower). Returns -1 when the request
 * cannot be divided from 594 MHz at all (0, or above 594 MHz * 2). */
int dt_clock_quantize(int clock_khz, int *actual_khz, int *error_ppm);

struct dt_mode {
    int width;
    int height;
    int refresh;   /* Hz, as the kernel reports it */
    int clock_khz; /* as requested; the DSI host quantises it */
    int interlaced;
    int preferred;
};

/* Choose a mode to test with. want is NULL or "" for the automatic,
 * conservative choice, or "WxH" / "WxH@R" for an explicit one.
 *
 * Automatic: progressive, at least 640x480 in either orientation (so the
 * AMOLED's portrait 568x1232 qualifies), pixel clock at most
 * DT_SAFE_MAX_KHZ; among those the modes the DSI reproduces exactly come
 * first, then the smallest picture, then the smallest clock error, then the
 * refresh closest to 60 Hz. Explicit: the named size (and refresh), the
 * smallest clock error first.
 *
 * Returns the index into modes, or -1 when nothing fits. why receives one
 * line saying what was chosen and whether its clock is exact. */
int dt_choose_mode(const struct dt_mode *modes, int count, const char *want, char *why, size_t why_len);

struct dt_edid {
    int header_ok;
    int checksum_ok;
    char vendor[4];     /* the three-letter PNP id */
    unsigned product;
    uint32_t serial;
    int week;
    int year;
    int version;
    int revision;
    int extensions;
    char name[14];      /* the monitor name descriptor, "" when absent */
    int has_range;      /* range limits descriptor */
    int vmin_hz, vmax_hz, hmin_khz, hmax_khz, max_clock_mhz;
    int has_preferred;  /* first detailed timing */
    int pref_width, pref_height, pref_clock_khz;
};

/* Decode the base block. Returns 0 when len >= 128 (header and checksum are
 * reported, not required), -1 when there is not even one block. */
int dt_edid_parse(const uint8_t *edid, size_t len, struct dt_edid *out);

/* The test frame, XRGB8888: black background, a white border, red, green
 * and blue blocks, and up to three lines of text. stride_px is the pitch in
 * pixels. Lines may be NULL. */
void dt_draw_pattern(uint32_t *px, int width, int height, int stride_px, const char *line1, const char *line2,
                     const char *line3);

#define DT_RGB_BLACK 0x00000000u
#define DT_RGB_WHITE 0x00ffffffu
#define DT_RGB_RED 0x00ff0000u
#define DT_RGB_GREEN 0x0000ff00u
#define DT_RGB_BLUE 0x000000ffu

/* Where the pattern puts things, so the test can look at them. */
int dt_pattern_border(int width, int height);
void dt_pattern_block(int width, int height, int which, int *x, int *y, int *w, int *h);

#endif
