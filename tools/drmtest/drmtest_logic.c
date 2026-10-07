/*
 * pos-drmtest's pure logic (drmtest_logic.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "drmtest_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dt_clock_quantize(int clock_khz, int *actual_khz, int *error_ppm)
{
    long div;
    long actual;

    if (clock_khz <= 0) {
        return -1;
    }
    /* DIV64_U64_ROUND_CLOSEST(594000, clock), then 594000 / div. */
    div = ((long)DT_DSI_SOURCE_KHZ + clock_khz / 2) / clock_khz;
    if (div <= 0) {
        return -1;
    }
    actual = DT_DSI_SOURCE_KHZ / div;
    if (actual_khz) {
        *actual_khz = (int)actual;
    }
    if (error_ppm) {
        *error_ppm = (int)((actual - clock_khz) * 1000000L / clock_khz);
    }
    return 0;
}

static int mode_error_ppm(const struct dt_mode *m)
{
    int err = 0;

    if (dt_clock_quantize(m->clock_khz, NULL, &err) < 0) {
        return 1000000;
    }
    return abs(err);
}

/* "WxH" or "WxH@R"; 0 on success. */
static int parse_want(const char *want, int *w, int *h, int *r)
{
    char tail;

    *r = 0;
    if (sscanf(want, "%dx%d@%d%c", w, h, r, &tail) == 3) {
        return (*w > 0 && *h > 0 && *r > 0) ? 0 : -1;
    }
    *r = 0;
    if (sscanf(want, "%dx%d%c", w, h, &tail) == 2) {
        return (*w > 0 && *h > 0) ? 0 : -1;
    }
    return -1;
}

/* Negative when a ranks before b. */
static int rank_auto(const struct dt_mode *a, const struct dt_mode *b)
{
    int ea = mode_error_ppm(a);
    int eb = mode_error_ppm(b);
    int xa = ea > DT_EXACT_PPM;
    int xb = eb > DT_EXACT_PPM;
    long area_a = (long)a->width * a->height;
    long area_b = (long)b->width * b->height;

    if (xa != xb) {
        return xa - xb;
    }
    if (area_a != area_b) {
        return area_a < area_b ? -1 : 1;
    }
    if (ea != eb) {
        return ea < eb ? -1 : 1;
    }
    return abs(a->refresh - 60) - abs(b->refresh - 60);
}

static int rank_explicit(const struct dt_mode *a, const struct dt_mode *b, int want_r)
{
    int ea = mode_error_ppm(a);
    int eb = mode_error_ppm(b);
    int ra = abs(a->refresh - (want_r ? want_r : 60));
    int rb = abs(b->refresh - (want_r ? want_r : 60));

    if (a->interlaced != b->interlaced) {
        return a->interlaced - b->interlaced;
    }
    if (ea != eb) {
        return ea < eb ? -1 : 1;
    }
    return ra - rb;
}

int dt_choose_mode(const struct dt_mode *modes, int count, const char *want, char *why, size_t why_len)
{
    int best = -1;
    int i;
    int explicit_mode = want && *want;
    int ww = 0;
    int wh = 0;
    int wr = 0;
    int actual = 0;
    int err = 0;

    if (why && why_len) {
        why[0] = '\0';
    }
    if (explicit_mode && parse_want(want, &ww, &wh, &wr) < 0) {
        if (why && why_len) {
            snprintf(why, why_len, "'%s' is not WxH or WxH@R", want);
        }
        return -1;
    }
    for (i = 0; i < count; i++) {
        const struct dt_mode *m = &modes[i];

        if (dt_clock_quantize(m->clock_khz, NULL, NULL) < 0) {
            continue;
        }
        if (explicit_mode) {
            if (m->width != ww || m->height != wh || (wr && m->refresh != wr)) {
                continue;
            }
            if (best < 0 || rank_explicit(m, &modes[best], wr) < 0) {
                best = i;
            }
        } else {
            /* 640x480 in either orientation: the AMOLED's only mode is the
             * portrait 568x1232 (unit A, 2026-09-27). */
            int shorter = m->width < m->height ? m->width : m->height;
            int longer = m->width < m->height ? m->height : m->width;

            if (m->interlaced || shorter < 480 || longer < 640 || m->clock_khz > DT_SAFE_MAX_KHZ) {
                continue;
            }
            if (best < 0 || rank_auto(m, &modes[best]) < 0) {
                best = i;
            }
        }
    }
    if (!why || !why_len) {
        return best;
    }
    if (best < 0) {
        if (explicit_mode) {
            snprintf(why, why_len, "the connector does not offer %s", want);
        } else {
            snprintf(why, why_len, "no progressive mode of at least 640x480 (either orientation) at or below %d kHz",
                     DT_SAFE_MAX_KHZ);
        }
        return -1;
    }
    dt_clock_quantize(modes[best].clock_khz, &actual, &err);
    snprintf(why, why_len, "%s %dx%d@%d%s: requested %d kHz, the DSI produces %d kHz (%+.2f %%)%s",
             explicit_mode ? "requested" : "safest", modes[best].width, modes[best].height, modes[best].refresh,
             modes[best].interlaced ? "i" : "", modes[best].clock_khz, actual, err / 10000.0,
             abs(err) > DT_EXACT_PPM ? "; INEXACT, the monitor may reject it" : "; exact");
    return best;
}

int dt_edid_parse(const uint8_t *e, size_t len, struct dt_edid *out)
{
    static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    unsigned sum = 0;
    unsigned v;
    int i;

    memset(out, 0, sizeof(*out));
    if (!e || len < 128) {
        return -1;
    }
    out->header_ok = memcmp(e, header, sizeof(header)) == 0;
    for (i = 0; i < 128; i++) {
        sum += e[i];
    }
    out->checksum_ok = (sum & 0xff) == 0;
    v = ((unsigned)e[8] << 8) | e[9];
    out->vendor[0] = (char)('@' + ((v >> 10) & 0x1f));
    out->vendor[1] = (char)('@' + ((v >> 5) & 0x1f));
    out->vendor[2] = (char)('@' + (v & 0x1f));
    out->vendor[3] = '\0';
    out->product = (unsigned)e[10] | ((unsigned)e[11] << 8);
    out->serial = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
    out->week = e[16];
    out->year = 1990 + e[17];
    out->version = e[18];
    out->revision = e[19];
    out->extensions = e[126];

    for (i = 0; i < 4; i++) {
        const uint8_t *d = e + 54 + 18 * i;

        if (d[0] || d[1]) {
            if (!out->has_preferred) {
                out->has_preferred = 1;
                out->pref_clock_khz = ((int)d[0] | ((int)d[1] << 8)) * 10;
                out->pref_width = (int)d[2] | ((int)(d[4] & 0xf0) << 4);
                out->pref_height = (int)d[5] | ((int)(d[7] & 0xf0) << 4);
            }
            continue;
        }
        if (d[3] == 0xfc) {
            int n;

            for (n = 0; n < 13 && d[5 + n] != 0x0a && d[5 + n] != 0x00; n++) {
                char c = (char)d[5 + n];

                out->name[n] = (c >= 0x20 && c < 0x7f) ? c : '?';
            }
            out->name[n] = '\0';
            while (n > 0 && out->name[n - 1] == ' ') {
                out->name[--n] = '\0';
            }
        } else if (d[3] == 0xfd) {
            out->has_range = 1;
            out->vmin_hz = d[5];
            out->vmax_hz = d[6];
            out->hmin_khz = d[7];
            out->hmax_khz = d[8];
            out->max_clock_mhz = d[9] * 10;
        }
    }
    return 0;
}

/* ---- the pattern ------------------------------------------------------------ */

/* 5x7 glyphs, one byte per row, bit 4 leftmost. */
struct glyph {
    char c;
    uint8_t rows[7];
};

static const struct glyph font5x7[] = {
    { 'A', { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } }, { 'B', { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e } },
    { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } }, { 'D', { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e } },
    { 'E', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f } }, { 'F', { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f } }, { 'H', { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e } }, { 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c } },
    { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } }, { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
    { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } }, { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'O', { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } }, { 'P', { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 } },
    { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } }, { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e } }, { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e } }, { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a } }, { 'X', { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04 } }, { 'Z', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f } },
    { '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } }, { '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
    { '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } }, { '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
    { '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } }, { '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
    { '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } }, { '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } }, { '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
    { 'x', { 0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11 } }, { '@', { 0x0e, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0e } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } }, { '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
    { ':', { 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00 } }, { '/', { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 } },
    { '+', { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 } }, { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
    { '?', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04 } },
};

static const uint8_t *glyph_rows(char c)
{
    size_t i;

    if (c == ' ') {
        return NULL;
    }
    if (c >= 'a' && c <= 'z' && c != 'x') {
        c = (char)(c - 'a' + 'A');
    }
    for (i = 0; i < sizeof(font5x7) / sizeof(font5x7[0]); i++) {
        if (font5x7[i].c == c) {
            return font5x7[i].rows;
        }
    }
    return glyph_rows('?');
}

static void fill(uint32_t *px, int stride, int width, int height, int x, int y, int w, int h, uint32_t rgb)
{
    int r;
    int c;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > width) {
        w = width - x;
    }
    if (y + h > height) {
        h = height - y;
    }
    for (r = 0; r < h; r++) {
        uint32_t *row = px + (size_t)(y + r) * stride + x;

        for (c = 0; c < w; c++) {
            row[c] = rgb;
        }
    }
}

static void text(uint32_t *px, int stride, int width, int height, int x, int y, int scale, const char *s)
{
    for (; s && *s; s++, x += 6 * scale) {
        const uint8_t *rows = glyph_rows(*s);
        int r;
        int c;

        if (!rows) {
            continue;
        }
        for (r = 0; r < 7; r++) {
            for (c = 0; c < 5; c++) {
                if (rows[r] & (0x10 >> c)) {
                    fill(px, stride, width, height, x + c * scale, y + r * scale, scale, scale, DT_RGB_WHITE);
                }
            }
        }
    }
}

int dt_pattern_border(int width, int height)
{
    int m = width < height ? width : height;
    int b = m / 120;

    return b < 2 ? 2 : b;
}

void dt_pattern_block(int width, int height, int which, int *x, int *y, int *w, int *h)
{
    int bw = width / 5;
    int bh = height / 4;

    *w = bw;
    *h = bh;
    *x = bw / 2 + which * (bw + bw / 4);
    *y = height / 2;
}

void dt_draw_pattern(uint32_t *px, int width, int height, int stride_px, const char *line1, const char *line2,
                     const char *line3)
{
    static const uint32_t blocks[3] = { DT_RGB_RED, DT_RGB_GREEN, DT_RGB_BLUE };
    int b = dt_pattern_border(width, height);
    int scale = height / 90;
    int i;

    /* Lines of up to 30 characters fit across, portrait panels included. */
    if (scale > width / (6 * 30)) {
        scale = width / (6 * 30);
    }
    if (scale < 1) {
        scale = 1;
    }
    fill(px, stride_px, width, height, 0, 0, width, height, DT_RGB_BLACK);
    fill(px, stride_px, width, height, 0, 0, width, b, DT_RGB_WHITE);
    fill(px, stride_px, width, height, 0, height - b, width, b, DT_RGB_WHITE);
    fill(px, stride_px, width, height, 0, 0, b, height, DT_RGB_WHITE);
    fill(px, stride_px, width, height, width - b, 0, b, height, DT_RGB_WHITE);
    for (i = 0; i < 3; i++) {
        int x;
        int y;
        int w;
        int h;

        dt_pattern_block(width, height, i, &x, &y, &w, &h);
        fill(px, stride_px, width, height, x, y, w, h, blocks[i]);
    }
    text(px, stride_px, width, height, 4 * b, 4 * b, scale, line1);
    text(px, stride_px, width, height, 4 * b, 4 * b + 10 * scale, scale, line2);
    text(px, stride_px, width, height, 4 * b, 4 * b + 20 * scale, scale, line3);
}
