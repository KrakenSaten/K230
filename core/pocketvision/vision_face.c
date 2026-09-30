/*
 * Faces. See vision_face.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_face.h"

#include <math.h>
#include <string.h>

static const uint32_t steps[3] = { 8, 16, 32 };
static const uint32_t sizes[3][2] = { { 16, 32 }, { 64, 128 }, { 256, 512 } };
static const uint32_t channels[3] = { 8, 4, 20 };   /* loc, conf, landms: two anchors' worth */

int vision_face_anchors(uint32_t in_w, uint32_t in_h)
{
    int n = 0;
    int s;

    if (in_w == 0 || in_h == 0 || in_w % 32 != 0 || in_h % 32 != 0 || in_w > 4096 || in_h > 4096) {
        return 0;
    }
    for (s = 0; s < 3; s++) {
        n += (int)((in_w / steps[s]) * (in_h / steps[s]) * 2);
    }
    return n;
}

int vision_face_anchor(uint32_t in_w, uint32_t in_h, int idx, struct vision_face_anchor *a)
{
    int s;

    if (!a || idx < 0 || idx >= vision_face_anchors(in_w, in_h)) {
        return -1;
    }
    for (s = 0; s < 3; s++) {
        uint32_t fw = in_w / steps[s];
        int here = (int)(fw * (in_h / steps[s]) * 2);

        if (idx < here) {
            uint32_t cell = (uint32_t)idx / 2;
            uint32_t k = (uint32_t)idx % 2;

            a->cx = ((float)(cell % fw) + 0.5f) * (float)steps[s] / (float)in_w;
            a->cy = ((float)(cell / fw) + 0.5f) * (float)steps[s] / (float)in_h;
            a->w = (float)sizes[s][k] / (float)in_w;
            a->h = (float)sizes[s][k] / (float)in_h;
            return 0;
        }
        idx -= here;
    }
    return -1;
}

int vision_face_check(int outputs, const uint32_t *rank, const uint32_t (*dims)[4], uint32_t in_w, uint32_t in_h)
{
    int i;

    if (outputs != VISION_FACE_OUTPUTS || !rank || !dims || vision_face_anchors(in_w, in_h) == 0) {
        return -1;
    }
    for (i = 0; i < VISION_FACE_OUTPUTS; i++) {
        uint32_t s = steps[i % 3];

        if (rank[i] != 4 || dims[i][0] != 1 || dims[i][1] != channels[i / 3] || dims[i][2] != in_h / s ||
            dims[i][3] != in_w / s) {
            return -1;
        }
    }
    return 0;
}

struct cand {
    int idx;          /* the anchor */
    int stride;       /* 0..2 */
    uint32_t cell;
    uint32_t k;
    uint16_t conf;
};

static int32_t clampi(float v, int32_t lo, int32_t hi)
{
    if (!(v > (float)lo)) {
        return lo;
    }
    if (v > (float)hi) {
        return hi;
    }
    return (int32_t)v;
}

int vision_face_decode(const float *const *out, const size_t *count, uint32_t in_w, uint32_t in_h,
                       uint16_t conf_pm, uint16_t nms_pm, struct vision_face *faces, int max, uint32_t *bad)
{
    struct cand c[VISION_FACE_CANDIDATES];
    struct vision_face f[VISION_FACE_CANDIDATES];
    bool gone[VISION_FACE_CANDIDATES];
    int nc = 0;
    int base = 0;
    int nf = 0;
    int s;
    int i;
    uint32_t skipped = 0;

    if (bad) {
        *bad = 0;
    }
    if (!out || !count || !faces || max <= 0 || vision_face_anchors(in_w, in_h) == 0) {
        return -1;
    }
    if (max > VISION_FACE_MAX) {
        max = VISION_FACE_MAX;
    }
    for (i = 0; i < VISION_FACE_OUTPUTS; i++) {
        uint32_t st = steps[i % 3];

        if (!out[i] || count[i] != (size_t)channels[i / 3] * (in_w / st) * (in_h / st)) {
            return -1;
        }
    }
    /* Scores: the surest anchors over the threshold, kept sorted. */
    for (s = 0; s < 3; s++) {
        uint32_t size = (in_w / steps[s]) * (in_h / steps[s]);
        const float *conf = out[3 + s];
        uint32_t cell;

        for (cell = 0; cell < size; cell++) {
            uint32_t k;

            for (k = 0; k < 2; k++) {
                float bg = conf[(k * 2 + 0) * size + cell];
                float fg = conf[(k * 2 + 1) * size + cell];
                float p;
                uint16_t pm;
                int at;

                if (!isfinite(bg) || !isfinite(fg)) {
                    skipped++;
                    continue;
                }
                /* softmax of two: the face's share */
                p = 1.0f / (1.0f + expf(bg - fg));
                pm = (uint16_t)(p * 1000.0f);
                if (pm < conf_pm) {
                    continue;
                }
                if (nc == VISION_FACE_CANDIDATES && pm <= c[nc - 1].conf) {
                    continue;
                }
                at = nc < VISION_FACE_CANDIDATES ? nc++ : nc - 1;
                while (at > 0 && c[at - 1].conf < pm) {
                    c[at] = c[at - 1];
                    at--;
                }
                c[at].idx = base + (int)(cell * 2 + k);
                c[at].stride = s;
                c[at].cell = cell;
                c[at].k = k;
                c[at].conf = pm;
            }
        }
        base += (int)(size * 2);
    }
    /* Boxes and points for the candidates. */
    for (i = 0; i < nc; i++) {
        uint32_t size = (in_w / steps[c[i].stride]) * (in_h / steps[c[i].stride]);
        const float *loc = out[c[i].stride];
        const float *lm = out[6 + c[i].stride];
        struct vision_face_anchor a;
        float v[4];
        float cx;
        float cy;
        float w;
        float h;
        int j;
        bool ok = true;

        vision_face_anchor(in_w, in_h, c[i].idx, &a);
        for (j = 0; j < 4; j++) {
            v[j] = loc[(c[i].k * 4 + (uint32_t)j) * size + c[i].cell];
            ok = ok && isfinite(v[j]);
        }
        for (j = 0; j < 10 && ok; j++) {
            ok = isfinite(lm[(c[i].k * 10 + (uint32_t)j) * size + c[i].cell]);
        }
        /* An absurd size is no face either: exp() of it would overflow. */
        if (!ok || v[2] > 40.0f || v[3] > 40.0f) {
            skipped++;
            gone[i] = true;
            continue;
        }
        gone[i] = false;
        cx = (a.cx + v[0] * 0.1f * a.w) * (float)in_w;
        cy = (a.cy + v[1] * 0.1f * a.h) * (float)in_h;
        w = a.w * expf(v[2] * 0.2f) * (float)in_w;
        h = a.h * expf(v[3] * 0.2f) * (float)in_h;
        {
            int32_t x0 = clampi(cx - w / 2.0f, 0, (int32_t)in_w);
            int32_t y0 = clampi(cy - h / 2.0f, 0, (int32_t)in_h);
            int32_t x1 = clampi(cx + w / 2.0f, 0, (int32_t)in_w);
            int32_t y1 = clampi(cy + h / 2.0f, 0, (int32_t)in_h);

            f[i].box.x = x0;
            f[i].box.y = y0;
            f[i].box.w = x1 - x0;
            f[i].box.h = y1 - y0;
        }
        if (f[i].box.w <= 0 || f[i].box.h <= 0) {
            gone[i] = true;
            continue;
        }
        f[i].conf = c[i].conf;
        for (j = 0; j < VISION_FACE_POINTS; j++) {
            float px = lm[(c[i].k * 10 + (uint32_t)j * 2) * size + c[i].cell];
            float py = lm[(c[i].k * 10 + (uint32_t)j * 2 + 1) * size + c[i].cell];

            f[i].pt[j][0] = clampi((a.cx + px * 0.1f * a.w) * (float)in_w, 0, (int32_t)in_w);
            f[i].pt[j][1] = clampi((a.cy + py * 0.1f * a.h) * (float)in_h, 0, (int32_t)in_h);
        }
    }
    /* Surest first; anything overlapping a kept face too much goes. */
    for (i = 0; i < nc && nf < max; i++) {
        int j;

        if (gone[i]) {
            continue;
        }
        faces[nf++] = f[i];
        for (j = i + 1; j < nc; j++) {
            if (!gone[j] && vision_iou_permille(&f[i].box, &f[j].box) >= nms_pm) {
                gone[j] = true;
            }
        }
    }
    if (bad) {
        *bad = skipped;
    }
    return nf;
}
