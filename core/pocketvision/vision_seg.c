/*
 * Segmentation after the KPU (vision_seg.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_seg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Cityscapes' nineteen training classes and their usual colours. */
static const char *const seg_names[VISION_SEG_CLASSES] = {
    "road", "sidewalk", "building", "wall", "fence", "pole", "light", "sign", "vegetation", "terrain",
    "sky", "person", "rider", "car", "truck", "bus", "train", "motorcycle", "bicycle",
};
static const uint16_t seg_colors[VISION_SEG_CLASSES] = {
    RGB565(128, 64, 128), RGB565(244, 35, 232), RGB565(70, 70, 70),   RGB565(102, 102, 156), RGB565(190, 153, 153),
    RGB565(153, 153, 153), RGB565(250, 170, 30), RGB565(220, 220, 0),  RGB565(107, 142, 35),  RGB565(152, 251, 152),
    RGB565(70, 130, 180), RGB565(220, 20, 60),  RGB565(255, 0, 0),    RGB565(0, 0, 142),     RGB565(0, 0, 70),
    RGB565(0, 60, 100),   RGB565(0, 80, 100),   RGB565(0, 0, 230),    RGB565(119, 11, 32),
};
static const uint16_t inst_colors[] = {
    RGB565(255, 56, 56),  RGB565(255, 157, 151), RGB565(255, 112, 31), RGB565(255, 178, 29), RGB565(207, 210, 49),
    RGB565(72, 249, 10),  RGB565(146, 204, 23),  RGB565(61, 219, 134), RGB565(26, 147, 52),  RGB565(0, 212, 187),
    RGB565(44, 153, 168), RGB565(0, 194, 255),   RGB565(52, 69, 147),  RGB565(100, 115, 255), RGB565(0, 24, 236),
    RGB565(132, 56, 255), RGB565(82, 0, 133),    RGB565(203, 56, 255), RGB565(255, 149, 200), RGB565(255, 55, 199),
};

const char *vision_seg_label(uint32_t cls)
{
    return cls < VISION_SEG_CLASSES ? seg_names[cls] : "?";
}

uint16_t vision_seg_color(uint32_t cls)
{
    return cls < VISION_SEG_CLASSES ? seg_colors[cls] : 0;
}

uint16_t vision_inst_color(uint32_t cls)
{
    return inst_colors[cls % (sizeof(inst_colors) / sizeof(inst_colors[0]))];
}

/* Separable: a horizontal pass into a small buffer, then a vertical lerp
 * of two of its rows. */
int vision_seg_upsample(const float *src, int c, int h, int w, int s, float *dst)
{
    int oh = h * s;
    int ow = w * s;
    int *x0 = malloc(sizeof(int) * (size_t)ow);
    int *x1 = malloc(sizeof(int) * (size_t)ow);
    float *wx = malloc(sizeof(float) * (size_t)ow);
    float *tmp = malloc(sizeof(float) * (size_t)h * (size_t)ow);
    int ch;
    int x;
    int y;

    if (!x0 || !x1 || !wx || !tmp) {
        free(x0);
        free(x1);
        free(wx);
        free(tmp);
        return -ENOMEM;
    }
    for (x = 0; x < ow; x++) {
        float f = ((float)x + 0.5f) / (float)s - 0.5f;
        int i;

        if (f < 0) {
            f = 0;
        }
        i = (int)f;
        x0[x] = i;
        x1[x] = i + 1 < w ? i + 1 : w - 1;
        wx[x] = f - (float)i;
    }
    for (ch = 0; ch < c; ch++) {
        const float *p = src + (size_t)ch * (size_t)h * (size_t)w;
        float *o = dst + (size_t)ch * (size_t)oh * (size_t)ow;

        for (y = 0; y < h; y++) {
            const float *r = p + (size_t)y * (size_t)w;
            float *t = tmp + (size_t)y * (size_t)ow;

            for (x = 0; x < ow; x++) {
                t[x] = r[x0[x]] + (r[x1[x]] - r[x0[x]]) * wx[x];
            }
        }
        for (y = 0; y < oh; y++) {
            float f = ((float)y + 0.5f) / (float)s - 0.5f;
            const float *a;
            const float *b;
            float *d = o + (size_t)y * (size_t)ow;
            float wy;
            int i;
            int j;

            if (f < 0) {
                f = 0;
            }
            i = (int)f;
            j = i + 1 < h ? i + 1 : h - 1;
            wy = f - (float)i;
            a = tmp + (size_t)i * (size_t)ow;
            b = tmp + (size_t)j * (size_t)ow;
            for (x = 0; x < ow; x++) {
                d[x] = a[x] + (b[x] - a[x]) * wy;
            }
        }
    }
    free(x0);
    free(x1);
    free(wx);
    free(tmp);
    return 0;
}

/* Channel-outer over tiles that stay in the L1 cache; a strict '>' in
 * channel order, so a tie goes to the first plane. Measured on unit B at
 * 19 x 256 x 512: 9.8 ms vectorised, 150 ms as a pixel-outer loop. */
#define SEG_TILE 1024
void vision_seg_argmax(const float *v, int c, size_t hw, uint8_t *m)
{
    float best[SEG_TILE];
    size_t t0;

    for (t0 = 0; t0 < hw; t0 += SEG_TILE) {
        size_t n = hw - t0 < SEG_TILE ? hw - t0 : SEG_TILE;
        uint8_t *mi = m + t0;
        int ch;

        memcpy(best, v + t0, n * sizeof(float));
        memset(mi, 0, n);
        for (ch = 1; ch < c; ch++) {
            const float *p = v + (size_t)ch * hw + t0;
            size_t i;

            for (i = 0; i < n; i++) {
                int gt = p[i] > best[i];

                best[i] = gt ? p[i] : best[i];
                mi[i] = gt ? (uint8_t)ch : mi[i];
            }
        }
    }
}

#define INST_CANDS 256

struct inst_cand {
    struct vision_det det;
    uint32_t row;
};

static int by_conf(const void *a, const void *b)
{
    const struct inst_cand *x = a;
    const struct inst_cand *y = b;

    return (int)y->det.conf - (int)x->det.conf;
}

int vision_inst_decode(const float *out, uint32_t ch, uint32_t rows, uint32_t in_w, uint32_t in_h,
                       uint16_t conf_min, uint32_t iou_max, struct vision_inst *inst, int max)
{
    static struct inst_cand cand[INST_CANDS];
    uint32_t classes;
    float floor_ = (float)conf_min / (float)VISION_CONF_SCALE;
    int n = 0;
    int kept = 0;
    uint32_t r;
    uint32_t c;
    int i;
    int j;

    if (!out || !inst || max <= 0 || ch <= 4 + VISION_INST_COEFFS || rows == 0 || !in_w || !in_h) {
        return -EINVAL;
    }
    classes = ch - 4 - VISION_INST_COEFFS;
    for (r = 0; r < rows && n < INST_CANDS; r++) {
        float best = 0;
        uint32_t cls = 0;
        float cx;
        float cy;
        float w;
        float h;

        /* A cheap first look: most rows have nothing. */
        for (c = 0; c < classes; c++) {
            float v = out[(size_t)(4 + c) * rows + r];

            if (v > best) {
                best = v;
                cls = c;
            }
        }
        if (!(best >= floor_)) {
            continue;
        }
        cx = out[r];
        cy = out[(size_t)rows + r];
        w = out[(size_t)2 * rows + r];
        h = out[(size_t)3 * rows + r];
        if (!(w > 1.0f && h > 1.0f && cx > -(float)in_w && cy > -(float)in_h && cx < 2.0f * (float)in_w &&
              cy < 2.0f * (float)in_h && w < 2.0f * (float)in_w && h < 2.0f * (float)in_h)) {
            continue;
        }
        cand[n].det.box.x = (int32_t)(cx - w / 2);
        cand[n].det.box.y = (int32_t)(cy - h / 2);
        cand[n].det.box.w = (int32_t)w;
        cand[n].det.box.h = (int32_t)h;
        if (cand[n].det.box.x < 0) {
            cand[n].det.box.w += cand[n].det.box.x;
            cand[n].det.box.x = 0;
        }
        if (cand[n].det.box.y < 0) {
            cand[n].det.box.h += cand[n].det.box.y;
            cand[n].det.box.y = 0;
        }
        if (cand[n].det.box.x + cand[n].det.box.w > (int32_t)in_w) {
            cand[n].det.box.w = (int32_t)in_w - cand[n].det.box.x;
        }
        if (cand[n].det.box.y + cand[n].det.box.h > (int32_t)in_h) {
            cand[n].det.box.h = (int32_t)in_h - cand[n].det.box.y;
        }
        if (cand[n].det.box.w < 1 || cand[n].det.box.h < 1) {
            continue;
        }
        cand[n].det.cls = (uint16_t)cls;
        cand[n].det.conf = (uint16_t)(best >= 1.0f ? VISION_CONF_SCALE : best * VISION_CONF_SCALE);
        cand[n].row = r;
        n++;
    }
    qsort(cand, (size_t)n, sizeof(cand[0]), by_conf);
    for (i = 0; i < n && kept < max; i++) {
        for (j = 0; j < kept; j++) {
            if (inst[j].det.cls == cand[i].det.cls &&
                vision_iou_permille(&inst[j].det.box, &cand[i].det.box) > iou_max) {
                break;
            }
        }
        if (j < kept) {
            continue;
        }
        inst[kept].det = cand[i].det;
        for (c = 0; c < VISION_INST_COEFFS; c++) {
            inst[kept].coeff[c] = out[(size_t)(4 + classes + c) * rows + cand[i].row];
        }
        kept++;
    }
    return kept;
}

void vision_inst_map(const struct vision_inst *inst, int n, const float *proto, uint32_t pw, uint32_t ph,
                     uint32_t in_w, uint32_t in_h, float *tmp, uint8_t *map)
{
    uint32_t mw = pw * 2;
    uint32_t mh = ph * 2;
    int i;

    memset(map, VISION_SEG_NONE, (size_t)mw * mh);
    for (i = 0; i < n && i < VISION_SEG_NONE; i++) {
        const struct vision_box *b = &inst[i].det.box;
        /* The box in map cells, and in prototype cells with one to spare
         * each side for the interpolation. */
        int32_t mx0 = b->x * (int32_t)mw / (int32_t)in_w;
        int32_t my0 = b->y * (int32_t)mh / (int32_t)in_h;
        int32_t mx1 = ((b->x + b->w) * (int32_t)mw + (int32_t)in_w - 1) / (int32_t)in_w;
        int32_t my1 = ((b->y + b->h) * (int32_t)mh + (int32_t)in_h - 1) / (int32_t)in_h;
        int32_t px0;
        int32_t py0;
        int32_t px1;
        int32_t py1;
        int32_t x;
        int32_t y;
        int k;

        mx1 = mx1 > (int32_t)mw ? (int32_t)mw : mx1;
        my1 = my1 > (int32_t)mh ? (int32_t)mh : my1;
        if (mx0 >= mx1 || my0 >= my1) {
            continue;
        }
        px0 = mx0 / 2 > 0 ? mx0 / 2 - 1 : 0;
        py0 = my0 / 2 > 0 ? my0 / 2 - 1 : 0;
        px1 = mx1 / 2 + 2 < (int32_t)pw ? mx1 / 2 + 2 : (int32_t)pw;
        py1 = my1 / 2 + 2 < (int32_t)ph ? my1 / 2 + 2 : (int32_t)ph;
        for (y = py0; y < py1; y++) {
            memset(tmp + (size_t)y * pw + px0, 0, sizeof(float) * (size_t)(px1 - px0));
        }
        for (k = 0; k < VISION_INST_COEFFS; k++) {
            const float *p = proto + (size_t)k * ph * pw;
            float co = inst[i].coeff[k];

            for (y = py0; y < py1; y++) {
                const float *pr = p + (size_t)y * pw;
                float *t = tmp + (size_t)y * pw;

                for (x = px0; x < px1; x++) {
                    t[x] += co * pr[x];
                }
            }
        }
        for (y = my0; y < my1; y++) {
            float fy = ((float)y + 0.5f) / 2 - 0.5f;
            int32_t y0;
            int32_t y1;
            float wy;

            fy = fy < (float)py0 ? (float)py0 : fy;
            y0 = (int32_t)fy;
            y1 = y0 + 1 < py1 ? y0 + 1 : py1 - 1;
            wy = fy - (float)y0;
            for (x = mx0; x < mx1; x++) {
                uint8_t *cell = &map[(size_t)y * mw + (size_t)x];
                float fx = ((float)x + 0.5f) / 2 - 0.5f;
                int32_t x0;
                int32_t x1;
                float wxx;
                float top;
                float bot;

                if (*cell != VISION_SEG_NONE) {
                    continue;
                }
                fx = fx < (float)px0 ? (float)px0 : fx;
                x0 = (int32_t)fx;
                x1 = x0 + 1 < px1 ? x0 + 1 : px1 - 1;
                wxx = fx - (float)x0;
                top = tmp[(size_t)y0 * pw + x0] + (tmp[(size_t)y0 * pw + x1] - tmp[(size_t)y0 * pw + x0]) * wxx;
                bot = tmp[(size_t)y1 * pw + x0] + (tmp[(size_t)y1 * pw + x1] - tmp[(size_t)y1 * pw + x0]) * wxx;
                /* A sigmoid above a half is a logit above zero. */
                if (top + (bot - top) * wy > 0) {
                    *cell = (uint8_t)i;
                }
            }
        }
    }
}
