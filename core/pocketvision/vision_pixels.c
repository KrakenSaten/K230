/*
 * The pixel modes. See vision_pixels.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_pixels.h"

#include <errno.h>
#include <string.h>

#define GREEN 0x07e0u
#define MAGENTA 0xf81fu
#define YELLOW 0xffe0u
#define CYAN 0x07ffu

struct vision_rgb vision_rgb565_to_rgb(uint16_t v)
{
    struct vision_rgb c;
    unsigned r = (v >> 11) & 31;
    unsigned g = (v >> 5) & 63;
    unsigned b = v & 31;

    c.r = (uint8_t)((r << 3) | (r >> 2));
    c.g = (uint8_t)((g << 2) | (g >> 4));
    c.b = (uint8_t)((b << 3) | (b >> 2));
    return c;
}

uint16_t vision_rgb_to_rgb565(struct vision_rgb c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

uint8_t vision_luma(struct vision_rgb c)
{
    return (uint8_t)((c.r * 77u + c.g * 150u + c.b * 29u) >> 8);
}

static bool picture_ok(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride)
{
    return px && w >= 3 && h >= 3 && w <= VISION_PIXELS_MAX_W && stride >= w;
}

int vision_pixels_sample(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, int32_t x,
                         int32_t y, struct vision_rgb *out)
{
    int32_t x0;
    int32_t y0;
    int32_t i;
    int32_t j;
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t n = 0;

    if (!picture_ok(px, w, h, stride) || !out) {
        return -EINVAL;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= (int32_t)w) {
        x = (int32_t)w - 1;
    }
    if (y >= (int32_t)h) {
        y = (int32_t)h - 1;
    }
    x0 = x - 2 < 0 ? 0 : x - 2;
    y0 = y - 2 < 0 ? 0 : y - 2;
    for (j = y0; j <= y + 2 && j < (int32_t)h; j++) {
        for (i = x0; i <= x + 2 && i < (int32_t)w; i++) {
            struct vision_rgb c = vision_rgb565_to_rgb(px[(uint32_t)j * stride + (uint32_t)i]);

            r += c.r;
            g += c.g;
            b += c.b;
            n++;
        }
    }
    out->r = (uint8_t)(r / n);
    out->g = (uint8_t)(g / n);
    out->b = (uint8_t)(b / n);
    return 0;
}

int vision_pixels_color(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, struct vision_rgb target,
                        uint32_t tol, struct vision_color_result *out)
{
    uint32_t x;
    uint32_t y;
    uint64_t sx = 0;
    uint64_t sy = 0;
    uint32_t matched = 0;
    /* A green target would vanish under a green highlight. */
    uint16_t paint = target.g > target.r && target.g > target.b ? MAGENTA : GREEN;

    if (!picture_ok(px, w, h, stride) || !out) {
        return -EINVAL;
    }
    if (tol > VISION_COLOR_TOL_MAX) {
        tol = VISION_COLOR_TOL_MAX;
    }
    for (y = 0; y < h; y++) {
        uint16_t *row = px + (size_t)y * stride;

        for (x = 0; x < w; x++) {
            struct vision_rgb c = vision_rgb565_to_rgb(row[x]);
            uint32_t d = (uint32_t)(c.r > target.r ? c.r - target.r : target.r - c.r) +
                         (uint32_t)(c.g > target.g ? c.g - target.g : target.g - c.g) +
                         (uint32_t)(c.b > target.b ? c.b - target.b : target.b - c.b);

            if (d <= tol) {
                row[x] = paint;
                matched++;
                sx += x;
                sy += y;
            }
        }
    }
    out->matched = matched;
    out->total = w * h;
    out->cx = matched ? (int32_t)(sx / matched) : -1;
    out->cy = matched ? (int32_t)(sy / matched) : -1;
    return 0;
}

static void luma_row(const uint16_t *row, uint32_t w, uint8_t *out)
{
    uint32_t x;

    for (x = 0; x < w; x++) {
        out[x] = vision_luma(vision_rgb565_to_rgb(row[x]));
    }
}

int vision_pixels_edge(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t threshold,
                       struct vision_edge_result *out)
{
    /* Three luma rows: above, this, below. Row y is written only after
     * row y + 1's luma has been read, so the picture is edged in place. */
    static uint8_t luma[3][VISION_PIXELS_MAX_W];
    uint32_t y;
    uint32_t strong = 0;

    if (!picture_ok(px, w, h, stride) || !out) {
        return -EINVAL;
    }
    luma_row(px, w, luma[0]);
    luma_row(px + stride, w, luma[1]);
    for (y = 0; y < h; y++) {
        const uint8_t *up = luma[(y + 2) % 3];   /* row y - 1 (row y + 2's slot, not yet filled) */
        const uint8_t *me = luma[y % 3];
        const uint8_t *dn = luma[(y + 1) % 3];
        uint16_t *row = px + (size_t)y * stride;
        uint32_t x;

        if (y + 1 < h && y > 0) {
            luma_row(px + (size_t)(y + 1) * stride, w, luma[(y + 1) % 3]);
        }
        if (y == 0) {
            up = me;
        }
        if (y + 1 >= h) {
            dn = me;
        }
        for (x = 0; x < w; x++) {
            uint32_t xl = x ? x - 1 : x;
            uint32_t xr = x + 1 < w ? x + 1 : x;
            int32_t gx = (int32_t)up[xr] + 2 * (int32_t)me[xr] + (int32_t)dn[xr] - (int32_t)up[xl] -
                         2 * (int32_t)me[xl] - (int32_t)dn[xl];
            int32_t gy = (int32_t)dn[xl] + 2 * (int32_t)dn[x] + (int32_t)dn[xr] - (int32_t)up[xl] -
                         2 * (int32_t)up[x] - (int32_t)up[xr];
            uint32_t mag = (uint32_t)((gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy)) >> 3;
            uint32_t g;

            if (mag > 255) {
                mag = 255;
            }
            if (mag >= VISION_EDGE_STRONG) {
                strong++;
            }
            g = threshold ? (mag >= threshold ? 255u : 0u) : mag;
            row[x] = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        }
    }
    out->strong = strong;
    out->total = w * h;
    return 0;
}

/* ---- TRACE ------------------------------------------------------------------------ */

#define TRACE_MAX_H 4096 /* the tallest picture a slot holds */

/* Per row: the candidate runs (first and last pixel), and the line followed
 * (-1 for none). Static: nothing here allocates. */
static int16_t run_l[TRACE_MAX_H][VISION_TRACE_RUNS];
static int16_t run_r[TRACE_MAX_H][VISION_TRACE_RUNS];
static uint8_t run_n[TRACE_MAX_H];
static uint8_t run_seen[TRACE_MAX_H]; /* per row, a bit per run some chain already passed through */
static int16_t chain[TRACE_MAX_H];

void vision_trace_reset(struct vision_trace_state *st)
{
    memset(st, 0, sizeof(*st));
}

static int32_t iabs(int32_t v)
{
    return v < 0 ? -v : v;
}

/* Row y's candidate runs: pixels clearly darker (lighter) than the row's
 * mean, joined across gaps of up to two pixels, of a plausible width. A row
 * with more runs than VISION_TRACE_RUNS is texture and holds none. */
static void row_runs(const uint16_t *row, uint32_t w, uint32_t y, bool dark, uint32_t min_run, uint32_t max_run)
{
    static uint8_t lum[VISION_PIXELS_MAX_W];
    uint32_t sum = 0;
    uint32_t mean;
    int32_t thr;
    int32_t start = -1;
    int32_t last = -1;
    uint32_t x;
    int n = 0;
    bool texture = false;

    for (x = 0; x < w; x++) {
        lum[x] = vision_luma(vision_rgb565_to_rgb(row[x]));
        sum += lum[x];
    }
    mean = sum / w;
    if (dark) {
        uint32_t c = mean / 4 > VISION_TRACE_CONTRAST ? mean / 4 : VISION_TRACE_CONTRAST;

        thr = (int32_t)mean - (int32_t)c;
    } else {
        uint32_t c = (255 - mean) / 4 > VISION_TRACE_CONTRAST ? (255 - mean) / 4 : VISION_TRACE_CONTRAST;

        thr = (int32_t)mean + (int32_t)c;
    }
    for (x = 0; x <= w; x++) {
        bool hit = x < w && (dark ? (int32_t)lum[x] < thr : (int32_t)lum[x] > thr);

        if (hit) {
            if (start < 0) {
                start = (int32_t)x;
            }
            last = (int32_t)x;
            continue;
        }
        if (start >= 0 && (x == w || (int32_t)x - last > 2)) {
            uint32_t width = (uint32_t)(last - start + 1);

            if (width >= min_run && width <= max_run) {
                if (n < VISION_TRACE_RUNS) {
                    run_l[y][n] = (int16_t)start;
                    run_r[y][n] = (int16_t)last;
                    n++;
                } else {
                    texture = true;
                }
            }
            start = -1;
        }
    }
    run_n[y] = texture ? 0 : (uint8_t)n;
}

/* Follow a line upwards from the run centred at xb on row yb: on each row
 * the run nearest where it was heading, within reach; a gap of more than a
 * sixteenth of the picture's height ends it. Returns the rows on it, and
 * the sum of their runs' widths in *wsum; with mark, they are written into
 * chain[]. */
static uint32_t follow(uint32_t w, uint32_t h, int32_t yb, int32_t xb, int32_t wb, bool mark, uint32_t *wsum)
{
    int32_t gap_max = (int32_t)h / 16 < 3 ? 3 : (int32_t)h / 16;
    int32_t px = xb;
    int32_t py = yb;
    int32_t step16 = 0; /* dx per row going up, x16 */
    uint32_t n = 1;
    int32_t y;

    *wsum = (uint32_t)wb;
    if (mark) {
        chain[yb] = (int16_t)xb;
    }
    for (y = yb - 1; y >= 0; y--) {
        int32_t dy = py - y;
        int32_t expect = px + (step16 * dy) / 16;
        int32_t reach = (int32_t)w / 20 + 2 * dy;
        int32_t best = -1;
        int32_t best_w = 0;
        int32_t best_k = 0;
        int32_t best_d;
        int k;

        if (reach < 3) {
            reach = 3;
        }
        best_d = reach + 1;
        for (k = 0; k < run_n[y]; k++) {
            int32_t c = (run_l[y][k] + run_r[y][k]) / 2;
            int32_t d = iabs(c - expect);

            if (d <= reach && d < best_d) {
                best = c;
                best_w = run_r[y][k] - run_l[y][k] + 1;
                best_k = k;
                best_d = d;
            }
        }
        if (best < 0) {
            if (dy > gap_max) {
                break;
            }
            continue;
        }
        {
            int32_t s16 = ((best - px) * 16) / dy;

            step16 = n == 1 ? s16 : (3 * step16 + s16) / 4;
        }
        px = best;
        py = y;
        n++;
        *wsum += (uint32_t)best_w;
        /* A run a chain has passed through starts no chain of its own: it
         * would only be the same line again, shorter. */
        run_seen[y] |= (uint8_t)(1u << best_k);
        if (mark) {
            chain[y] = (int16_t)best;
        }
    }
    return n;
}

int vision_pixels_trace(uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, bool dark,
                        struct vision_trace_state *st, struct vision_trace_result *out)
{
    uint32_t min_run = w / 80 < 2 ? 2 : w / 80;
    uint32_t max_run = (w * 6) / 10;
    uint32_t need = h / 8 < 3 ? 3 : h / 8;
    bool prior = st && st->ever && st->lost <= VISION_TRACE_REACQUIRE;
    int32_t best_y = -1;
    int32_t best_x = -1;
    uint32_t best_n = 0;
    int32_t near_y = -1;
    int32_t near_x = -1;
    uint32_t near_n = 0;

    int32_t y;

    if (!picture_ok(px, w, h, stride) || !out || h > TRACE_MAX_H) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    for (y = 0; y < (int32_t)h; y++) {
        row_runs(px + (size_t)y * stride, w, (uint32_t)y, dark, min_run, max_run);
        chain[y] = -1;
        run_seen[y] = 0;
        if (run_n[y]) {
            out->candidates++;
        }
    }
    /* The starts: every run of the lower half that no chain has passed
     * through yet, from the bottom up. Not only the lowest rows that hold
     * any: on unit A specks in the dark corners below the paper filled the
     * lowest 50 such rows, and a line in plain view was never tried. A chain
     * from a speck ends within a gap, and a run already on a chain starts
     * none, so a line is followed once, not once per row. The longest chain
     * wins; one that starts where the line was last seen wins if it is at
     * least half as long. */
    for (y = (int32_t)h - 1; y >= (int32_t)h / 2; y--) {
        int32_t ref = prior ? st->last_x : (int32_t)w / 2;
        int k;

        for (k = 0; k < run_n[y]; k++) {
            int32_t c = (run_l[y][k] + run_r[y][k]) / 2;
            uint32_t wsum;
            uint32_t n;

            if (run_seen[y] & (1u << k)) {
                continue;
            }
            run_seen[y] |= (uint8_t)(1u << k);
            n = follow(w, h, y, c, run_r[y][k] - run_l[y][k] + 1, false, &wsum);

            /* A line is longer than it is wide: a chain no longer than twice
             * its mean width is a blob (a shadow, a mat), not a line. */
            if ((uint64_t)n * n < 2 * (uint64_t)wsum) {
                continue;
            }
            if (n > best_n || (n == best_n && iabs(c - ref) < iabs(best_x - ref))) {
                best_n = n;
                best_y = y;
                best_x = c;
            }
            if (prior && iabs(c - st->last_x) <= (int32_t)w / 8 && n > near_n) {
                near_n = n;
                near_y = y;
                near_x = c;
            }
        }
    }
    if (prior && near_n >= need && 2 * near_n >= best_n) {
        best_n = near_n;
        best_y = near_y;
        best_x = near_x;
    }
    out->rows = best_n;
    if (best_n >= need) {
        uint32_t wsum;

        follow(w, h, best_y, best_x, 0, true, &wsum);
        out->found = true;
        /* A short fragment at the chain's bottom, cut from the rest by a gap,
         * is a speck the chain bridged from (unit A: specks at the picture's
         * lower edge became the line's "bottom"): it goes, while enough of
         * the line is left. */
        for (;;) {
            int32_t frag_max = (int32_t)h / 20 < 4 ? 4 : (int32_t)h / 20;
            int32_t seg = 0;
            int32_t last = -1;
            int32_t gap_at = -1;

            for (y = (int32_t)h - 1; y >= 0; y--) {
                if (chain[y] < 0) {
                    continue;
                }
                if (last >= 0 && last - y > 2) {
                    gap_at = y;
                    break;
                }
                seg++;
                last = y;
            }
            if (gap_at < 0 || seg > frag_max || best_n - (uint32_t)seg < need) {
                break;
            }
            for (y = (int32_t)h - 1; y > gap_at; y--) {
                chain[y] = -1;
            }
            best_n -= (uint32_t)seg;
            out->rows = best_n;
        }
    }
    /* Mark the candidates' edges in cyan, then the line followed in yellow
     * over them. */
    for (y = 0; y < (int32_t)h; y++) {
        uint16_t *row = px + (size_t)y * stride;
        int k;

        for (k = 0; k < run_n[y]; k++) {
            row[run_l[y][k]] = CYAN;
            row[run_r[y][k]] = CYAN;
        }
        if (out->found && chain[y] >= 0) {
            int32_t i;

            for (i = chain[y] - 1; i <= chain[y] + 1; i++) {
                if (i >= 0 && i < (int32_t)w) {
                    row[i] = YELLOW;
                }
            }
        }
    }
    if (!out->found) {
        /* Said at once: a line seen a picture ago is not reported as here. */
        if (st) {
            st->streak = 0;
            if (st->lost < UINT32_MAX) {
                st->lost++;
            }
            out->lost = st->lost;
            out->ever = st->ever;
        } else {
            out->lost = 1;
        }
        return 0;
    }
    {
        /* Least squares over the line followed: x = mx + b (y - my), b in dx
         * per 1000 rows going down; its x at the bottom row, x16. */
        int64_t n = 0;
        int64_t sx = 0;
        int64_t sy = 0;
        int64_t syy = 0;
        int64_t sxy = 0;
        int64_t den;
        int64_t b_pm;
        int32_t top = -1;
        int32_t bottom = -1;
        int32_t x16;
        int32_t s_x16;
        int32_t s_slope;
        int32_t centre16 = 8 * (int32_t)w;
        int64_t off;

        for (y = 0; y < (int32_t)h; y++) {
            if (chain[y] < 0) {
                continue;
            }
            if (top < 0) {
                top = y;
            }
            bottom = y;
            n++;
            sx += chain[y];
            sy += y;
            syy += (int64_t)y * y;
            sxy += (int64_t)y * chain[y];
        }
        den = n * syy - sy * sy;
        b_pm = den ? ((n * sxy - sy * sx) * 1000) / den : 0;
        if (b_pm > 100000) {
            b_pm = 100000;
        } else if (b_pm < -100000) {
            b_pm = -100000;
        }
        {
            /* Its bottom is where the line itself is on its lowest rows (an
             * eighth of them, at least three), not the straight fit carried
             * down: on unit A an S-shaped line read LEFT 99% from the fit
             * while its end was at about -74%. The fit gives the lean. */
            int64_t want = n / 8 < 3 ? 3 : n / 8;
            int64_t got = 0;
            int64_t bx = 0;
            int64_t by = 0;
            int64_t byy = 0;
            int64_t bxy = 0;
            int64_t bden;
            int64_t lean_pm;

            for (y = bottom; y >= top && got < want; y--) {
                if (chain[y] >= 0) {
                    bx += chain[y];
                    by += y;
                    byy += (int64_t)y * y;
                    bxy += (int64_t)y * chain[y];
                    got++;
                }
            }
            /* Their mean, carried down to the bottom row itself by their own
             * lean (not the whole line's: a curve's differs). */
            bden = got * byy - by * by;
            lean_pm = bden ? ((got * bxy - by * bx) * 1000) / bden : 0;
            lean_pm = lean_pm > 100000 ? 100000 : lean_pm < -100000 ? -100000 : lean_pm;
            x16 = (int32_t)((16 * bx) / got + (lean_pm * 16 * (got * bottom - by)) / (1000 * got));
        }
        s_x16 = x16;
        s_slope = (int32_t)b_pm;
        if (st) {
            /* Smoothed over consecutive pictures; a jump of more than a sixth
             * of the width is another line, taken as it is. */
            if (st->streak > 0 && iabs(x16 - st->s_x16) <= 16 * (int32_t)w / 6) {
                s_x16 = (st->s_x16 + x16) / 2;
                s_slope = (st->s_slope_pm + (int32_t)b_pm) / 2;
            }
            st->s_x16 = s_x16;
            st->s_slope_pm = s_slope;
            st->streak++;
            st->lost = 0;
            st->ever = true;
            st->last_x = s_x16 / 16;
        }
        off = ((int64_t)(s_x16 - centre16) * 1000) / centre16;
        out->offset_pm = (int32_t)(off > 1000 ? 1000 : off < -1000 ? -1000 : off);
        out->slope_pm = s_slope;
        out->y1 = bottom;
        out->x1 = s_x16 / 16;
        out->y0 = top;
        out->x0 = (s_x16 - (int32_t)(((int64_t)s_slope * 16 * (bottom - top)) / 1000)) / 16;
        out->x0 = out->x0 < 0 ? 0 : out->x0 >= (int32_t)w ? (int32_t)w - 1 : out->x0;
        out->x1 = out->x1 < 0 ? 0 : out->x1 >= (int32_t)w ? (int32_t)w - 1 : out->x1;
        out->ever = true;
    }
    return 0;
}
