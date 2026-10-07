/*
 * Faces (core/pocketvision/vision_face.c): the anchors against the vendor's
 * own table (ai_demo/face_detection/anchors_320.cc, first, last and the
 * first of each stride); the shapes a 320 x 320 model has, and others
 * refused; a face put into the outputs as the model would say it read back
 * with its box and points; the threshold; two anchors on one face kept as
 * one, two faces apart kept as two; the bound; NaN skipped and counted; an
 * absurd size refused rather than overflowing.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketvision/vision_face.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

#define IN 320
static const uint32_t st[3] = { 8, 16, 32 };
static const uint32_t ch[3] = { 8, 4, 20 };
static float buf[9][20 * 40 * 40];
static const float *outs[9];
static size_t counts[9];

static void reset(void)
{
    int i;
    size_t j;

    for (i = 0; i < 9; i++) {
        uint32_t size = (IN / st[i % 3]) * (IN / st[i % 3]);

        counts[i] = (size_t)ch[i / 3] * size;
        outs[i] = buf[i];
        memset(buf[i], 0, sizeof(buf[i]));
        if (i / 3 == 1) {
            /* every anchor: background, sure */
            for (j = 0; j < size; j++) {
                buf[i][0 * size + j] = 4.0f;
                buf[i][2 * size + j] = 4.0f;
            }
        }
    }
}

static int close_to(float a, float b)
{
    return fabsf(a - b) < 1e-5f;
}

/* A face as the model says it: anchor k of cell (cx, cy) at stride s, the
 * face score's logit, the box (input pixels) and a point. */
static void put(int s, uint32_t col, uint32_t row, uint32_t k, float logit, float x, float y, float w, float h)
{
    uint32_t fw = IN / st[s];
    uint32_t size = fw * fw;
    uint32_t cell = row * fw + col;
    float acx = ((float)col + 0.5f) * (float)st[s] / IN;
    float acy = ((float)row + 0.5f) * (float)st[s] / IN;
    static const float mins[3][2] = { { 16, 32 }, { 64, 128 }, { 256, 512 } };
    float aw = mins[s][k] / IN;
    float cx = (x + w / 2) / IN;
    float cy = (y + h / 2) / IN;
    int j;

    buf[3 + s][(k * 2 + 0) * size + cell] = 0.0f;
    buf[3 + s][(k * 2 + 1) * size + cell] = logit;
    buf[s][(k * 4 + 0) * size + cell] = (cx - acx) / (0.1f * aw);
    buf[s][(k * 4 + 1) * size + cell] = (cy - acy) / (0.1f * aw);
    buf[s][(k * 4 + 2) * size + cell] = logf(w / IN / aw) / 0.2f;
    buf[s][(k * 4 + 3) * size + cell] = logf(h / IN / aw) / 0.2f;
    for (j = 0; j < 5; j++) {
        /* every point at the box's middle, the nose a little lower */
        buf[6 + s][(k * 10 + (uint32_t)j * 2) * size + cell] = (cx - acx) / (0.1f * aw);
        buf[6 + s][(k * 10 + (uint32_t)j * 2 + 1) * size + cell] = (cy + (j == 2 ? 0.01f : 0.0f) - acy) / (0.1f * aw);
    }
}

static void test_anchors(void)
{
    struct vision_face_anchor a;

    check("a 320 model has 4200 anchors", vision_face_anchors(IN, IN) == 4200);
    check("a 640 model 16800", vision_face_anchors(640, 640) == 16800);
    check("a size that is not a multiple of 32 has none", vision_face_anchors(300, 320) == 0 &&
                                                               vision_face_anchors(0, 0) == 0);
    check("anchor 0 is the table's", vision_face_anchor(IN, IN, 0, &a) == 0 && close_to(a.cx, 0.0125f) &&
                                         close_to(a.cy, 0.0125f) && close_to(a.w, 0.05f) && close_to(a.h, 0.05f));
    check("anchor 1: the same cell, the larger size", vision_face_anchor(IN, IN, 1, &a) == 0 &&
                                                          close_to(a.cx, 0.0125f) && close_to(a.w, 0.1f));
    check("anchor 2: the next cell along the row", vision_face_anchor(IN, IN, 2, &a) == 0 &&
                                                       close_to(a.cx, 0.0375f) && close_to(a.cy, 0.0125f));
    check("anchor 80: the second row", vision_face_anchor(IN, IN, 80, &a) == 0 && close_to(a.cx, 0.0125f) &&
                                           close_to(a.cy, 0.0375f));
    check("anchor 3200: stride 16's first", vision_face_anchor(IN, IN, 3200, &a) == 0 && close_to(a.cx, 0.025f) &&
                                                close_to(a.w, 0.2f));
    check("anchor 4000: stride 32's first", vision_face_anchor(IN, IN, 4000, &a) == 0 && close_to(a.cx, 0.05f) &&
                                                close_to(a.w, 0.8f));
    check("anchor 4199 is the table's last", vision_face_anchor(IN, IN, 4199, &a) == 0 && close_to(a.cx, 0.95f) &&
                                                 close_to(a.cy, 0.95f) && close_to(a.w, 1.6f));
    check("no anchor 4200", vision_face_anchor(IN, IN, 4200, &a) == -1 && vision_face_anchor(IN, IN, -1, &a) == -1);
}

static void test_shapes(void)
{
    uint32_t rank[9];
    uint32_t dims[9][4];
    int i;

    for (i = 0; i < 9; i++) {
        rank[i] = 4;
        dims[i][0] = 1;
        dims[i][1] = ch[i / 3];
        dims[i][2] = IN / st[i % 3];
        dims[i][3] = IN / st[i % 3];
    }
    check("the 320 model's nine outputs fit", vision_face_check(9, rank, (const uint32_t (*)[4])dims, IN, IN) == 0);
    check("eight do not", vision_face_check(8, rank, (const uint32_t (*)[4])dims, IN, IN) == -1);
    dims[4][1] = 8;
    check("conf and loc swapped do not", vision_face_check(9, rank, (const uint32_t (*)[4])dims, IN, IN) == -1);
    dims[4][1] = 4;
    check("nor for a 640 input", vision_face_check(9, rank, (const uint32_t (*)[4])dims, 640, 640) == -1);
    rank[8] = 3;
    check("nor a rank-3 output", vision_face_check(9, rank, (const uint32_t (*)[4])dims, IN, IN) == -1);
}

static void test_decode(void)
{
    struct vision_face f[VISION_FACE_MAX];
    uint32_t bad = 99;
    int n;
    int i;

    reset();
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("no face in a picture of background", n == 0 && bad == 0);

    reset();
    put(1, 5, 8, 0, 3.0f, 70, 120, 60, 72);
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("one face", n == 1);
    check("its box, as said", n == 1 && abs(f[0].box.x - 70) <= 1 && abs(f[0].box.y - 120) <= 1 &&
                                  abs(f[0].box.w - 60) <= 1 && abs(f[0].box.h - 72) <= 1);
    check("its score, the softmax", n == 1 && f[0].conf >= 950 && f[0].conf <= 953);
    check("its points", n == 1 && abs(f[0].pt[0][0] - 100) <= 1 && abs(f[0].pt[0][1] - 156) <= 1 &&
                            f[0].pt[2][1] > f[0].pt[0][1]);

    reset();
    put(1, 5, 8, 0, 0.2f, 70, 120, 60, 72);
    check("a doubtful face (55 %) is not one",
          vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad) == 0);
    check("unless the threshold is lower",
          vision_face_decode(outs, counts, IN, IN, 500, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad) == 1);

    reset();
    put(1, 5, 8, 0, 3.0f, 70, 120, 60, 72);
    put(1, 6, 8, 0, 2.0f, 74, 122, 58, 70);
    put(1, 5, 9, 1, 2.5f, 68, 118, 62, 74);
    put(2, 7, 1, 0, 4.0f, 200, 20, 90, 100);
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("three anchors on one face and one on another: two faces", n == 2);
    check("the surest first", n == 2 && f[0].conf > f[1].conf && abs(f[0].box.x - 200) <= 1);
    check("the first face by its surest anchor", n == 2 && abs(f[1].box.x - 70) <= 1 && abs(f[1].box.w - 60) <= 1);

    reset();
    for (i = 0; i < 30; i++) {
        put(0, (uint32_t)(i % 10) * 4, (uint32_t)(i / 10) * 12, 0, 2.0f + (float)i * 0.01f,
            (float)(i % 10) * 32, (float)(i / 10) * 96, 16, 16);
    }
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("thirty faces apart: the bound, the surest", n == VISION_FACE_MAX && f[0].conf >= f[VISION_FACE_MAX - 1].conf);
    check("fewer when asked", vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, 3,
                                                 &bad) == 3);

    reset();
    put(1, 5, 8, 0, 3.0f, 70, 120, 60, 72);
    buf[3][0] = NAN;
    buf[4][(0 * 2 + 1) * 400 + 7] = INFINITY;
    buf[5][(1 * 2 + 1) * 100 + 3] = NAN;
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("NaN scores skipped and counted, the face kept", n == 1 && bad == 3);

    reset();
    put(1, 5, 8, 0, 3.0f, 70, 120, 60, 72);
    buf[1][(0 * 4 + 2) * 400 + 8 * 20 + 5] = 1e9f;
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("an absurd size is no face", n == 0 && bad == 1);

    reset();
    put(2, 9, 9, 1, 3.0f, 250, 250, 200, 200);
    n = vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad);
    check("a face past the edge is clipped to the picture",
          n == 1 && f[0].box.x + f[0].box.w <= IN && f[0].box.y + f[0].box.h <= IN && f[0].box.x == 250);

    reset();
    counts[4] = 12;
    check("outputs of the wrong size are refused",
          vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad) == -1);
    reset();
    outs[7] = NULL;
    check("a missing output too",
          vision_face_decode(outs, counts, IN, IN, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, f, VISION_FACE_MAX, &bad) == -1);
}

int main(void)
{
    test_anchors();
    test_shapes();
    test_decode();
    printf("vision_face_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
