/*
 * Text. See vision_text.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_text.h"

#include <math.h>
#include <string.h>

/* The grid and the work stack: fixed, VISION_TEXT_GRID squared. */
static uint16_t label[VISION_TEXT_GRID * VISION_TEXT_GRID];
static uint16_t cellscore[VISION_TEXT_GRID * VISION_TEXT_GRID];
static uint32_t stack[VISION_TEXT_GRID * VISION_TEXT_GRID];

struct region {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint32_t cells;
    uint64_t sum;   /* of the cells' scores, per-mille */
};

static bool before(const struct vision_text_box *a, const struct vision_text_box *b)
{
    /* Reading order: a line that starts clearly higher first; on one line,
     * left first. */
    int32_t half = (a->box.h < b->box.h ? a->box.h : b->box.h) / 2;

    if (a->box.y + half < b->box.y) {
        return true;
    }
    if (b->box.y + half < a->box.y) {
        return false;
    }
    return a->box.x < b->box.x;
}

int vision_text_regions(const float *map, uint32_t mw, uint32_t mh, uint32_t step, uint16_t threshold_pm,
                        uint16_t box_min_pm, struct vision_text_box *out, int max)
{
    struct vision_text_box best[VISION_TEXT_MAX];
    uint32_t s;
    uint32_t gw;
    uint32_t gh;
    uint32_t gx;
    uint32_t gy;
    float thr = (float)threshold_pm / 1000.0f;
    int kept = 0;
    int i;
    uint16_t next = 0;

    if (!map || mw == 0 || mh == 0 || step == 0 || !out || max <= 0 || mw > 8192 || mh > 8192) {
        return -1;
    }
    if (max > VISION_TEXT_MAX) {
        max = VISION_TEXT_MAX;
    }
    /* The grid: a cell per s x s pixels, its score the block's highest. */
    s = ((mw > mh ? mw : mh) + VISION_TEXT_GRID - 1) / VISION_TEXT_GRID;
    s = s == 0 ? 1 : s;
    while ((mw + s - 1) / s > VISION_TEXT_GRID || (mh + s - 1) / s > VISION_TEXT_GRID) {
        s++;
    }
    gw = (mw + s - 1) / s;
    gh = (mh + s - 1) / s;
    for (gy = 0; gy < gh; gy++) {
        for (gx = 0; gx < gw; gx++) {
            float m = 0.0f;
            uint32_t x;
            uint32_t y;

            for (y = gy * s; y < (gy + 1) * s && y < mh; y++) {
                for (x = gx * s; x < (gx + 1) * s && x < mw; x++) {
                    float v = map[((size_t)y * mw + x) * step];

                    /* NaN compares false and is not text. */
                    if (v > m) {
                        m = v;
                    }
                }
            }
            m = m > 1.0f ? 1.0f : m;
            cellscore[gy * gw + gx] = m > thr ? (uint16_t)(m * 1000.0f) : 0;
            label[gy * gw + gx] = 0;
        }
    }
    /* Regions: 4-connected text cells, each filled from its first cell. */
    for (gy = 0; gy < gh; gy++) {
        for (gx = 0; gx < gw; gx++) {
            uint32_t at = gy * gw + gx;
            struct region r;
            uint32_t sp = 0;
            struct vision_text_box b;
            int32_t d;
            int64_t area;
            int64_t perim;

            if (!cellscore[at] || label[at]) {
                continue;
            }
            if (next == 0xffff) {
                break;
            }
            next++;
            memset(&r, 0, sizeof(r));
            r.x0 = r.x1 = (int32_t)gx;
            r.y0 = r.y1 = (int32_t)gy;
            label[at] = next;
            stack[sp++] = at;
            while (sp > 0) {
                uint32_t c = stack[--sp];
                int32_t cx = (int32_t)(c % gw);
                int32_t cy = (int32_t)(c / gw);
                int k;

                r.cells++;
                r.sum += cellscore[c];
                r.x0 = cx < r.x0 ? cx : r.x0;
                r.x1 = cx > r.x1 ? cx : r.x1;
                r.y0 = cy < r.y0 ? cy : r.y0;
                r.y1 = cy > r.y1 ? cy : r.y1;
                for (k = 0; k < 4; k++) {
                    int32_t nx = cx + (k == 0) - (k == 1);
                    int32_t ny = cy + (k == 2) - (k == 3);
                    uint32_t nat;

                    if (nx < 0 || ny < 0 || (uint32_t)nx >= gw || (uint32_t)ny >= gh) {
                        continue;
                    }
                    nat = (uint32_t)ny * gw + (uint32_t)nx;
                    if (cellscore[nat] && !label[nat]) {
                        label[nat] = next;
                        /* Each cell is pushed once: the stack never holds
                         * more than the grid. */
                        stack[sp++] = nat;
                    }
                }
            }
            if (r.cells < VISION_TEXT_MIN_CELLS || r.sum / r.cells < box_min_pm) {
                continue;
            }
            /* Back into the map's pixels, grown by DB's unclip distance. */
            b.box.x = r.x0 * (int32_t)s;
            b.box.y = r.y0 * (int32_t)s;
            b.box.w = (r.x1 - r.x0 + 1) * (int32_t)s;
            b.box.h = (r.y1 - r.y0 + 1) * (int32_t)s;
            area = (int64_t)r.cells * s * s;
            perim = 2 * ((int64_t)b.box.w + b.box.h);
            d = (int32_t)((area * 3) / (2 * perim));
            b.box.x -= d;
            b.box.y -= d;
            b.box.w += 2 * d;
            b.box.h += 2 * d;
            if (b.box.x < 0) {
                b.box.w += b.box.x;
                b.box.x = 0;
            }
            if (b.box.y < 0) {
                b.box.h += b.box.y;
                b.box.y = 0;
            }
            if ((uint32_t)(b.box.x + b.box.w) > mw) {
                b.box.w = (int32_t)mw - b.box.x;
            }
            if ((uint32_t)(b.box.y + b.box.h) > mh) {
                b.box.h = (int32_t)mh - b.box.y;
            }
            b.score = (uint16_t)(r.sum / r.cells);
            /* Keep the surest: replace the weakest kept when full. */
            if (kept < max) {
                best[kept++] = b;
            } else {
                int weakest = 0;

                for (i = 1; i < kept; i++) {
                    weakest = best[i].score < best[weakest].score ? i : weakest;
                }
                if (b.score > best[weakest].score) {
                    best[weakest] = b;
                }
            }
        }
    }
    /* Reading order (insertion: at most VISION_TEXT_MAX). */
    for (i = 1; i < kept; i++) {
        struct vision_text_box t = best[i];
        int j = i - 1;

        while (j >= 0 && before(&t, &best[j])) {
            best[j + 1] = best[j];
            j--;
        }
        best[j + 1] = t;
    }
    memcpy(out, best, (size_t)kept * sizeof(best[0]));
    return kept;
}

int vision_text_ctc(const float *scores, uint32_t steps, uint32_t classes, uint32_t blank, uint32_t *out, int max,
                    uint16_t *conf_pm)
{
    return vision_text_ctc_pos(scores, steps, classes, blank, out, NULL, max, conf_pm);
}

int vision_text_ctc_pos(const float *scores, uint32_t steps, uint32_t classes, uint32_t blank, uint32_t *out,
                        uint16_t *pos, int max, uint16_t *conf_pm)
{
    bool probs = true;
    uint32_t t;
    uint32_t c;
    uint32_t prev = blank;
    int n = 0;
    double sum_conf = 0.0;
    int kept = 0;

    if (!scores || steps == 0 || classes == 0 || blank >= classes || !out || max <= 0) {
        return -1;
    }
    /* Probabilities or logits? A softmax output sums to 1 at every step. */
    for (t = 0; t < steps && probs; t++) {
        double s = 0.0;

        for (c = 0; c < classes; c++) {
            float v = scores[(size_t)t * classes + c];

            if (!(v >= -0.05f && v <= 1.05f)) {
                probs = false;
                break;
            }
            s += v;
        }
        if (s < 0.95 || s > 1.05) {
            probs = false;
        }
    }
    for (t = 0; t < steps; t++) {
        const float *row = &scores[(size_t)t * classes];
        uint32_t best = 0;
        float bv = row[0];

        for (c = 1; c < classes; c++) {
            if (row[c] > bv) {
                bv = row[c];
                best = c;
            }
        }
        if (!(bv == bv)) {
            prev = blank; /* a step of NaN reads nothing */
            continue;
        }
        if (best != blank && best != prev) {
            float p = bv;

            if (!probs) {
                /* The best class's probability: 1 / sum(exp(v - best)). */
                double s = 0.0;

                for (c = 0; c < classes; c++) {
                    s += exp((double)(row[c] - bv));
                }
                p = (float)(1.0 / s);
            }
            if (n < max) {
                if (pos) {
                    pos[n] = (uint16_t)(t > 0xffff ? 0xffff : t);
                }
                out[n++] = best;
            }
            sum_conf += p;
            kept++;
        }
        prev = best;
    }
    if (conf_pm) {
        double m = kept ? sum_conf / kept : 0.0;

        m = m < 0.0 ? 0.0 : (m > 1.0 ? 1.0 : m);
        *conf_pm = (uint16_t)(m * 1000.0 + 0.5);
    }
    return n;
}
