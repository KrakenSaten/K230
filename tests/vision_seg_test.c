/*
 * Segmentation after the KPU (vision_seg.h) on made-up tensors: the
 * upsampling's corners, its half-pixel weights and its planes kept apart;
 * the argmax across a tile boundary with a tie going to the first plane;
 * the class names and colours; a YOLOv8-seg head decoded to its objects
 * with their coefficients, a duplicate suppressed and another class kept;
 * and the objects' masks cut by their boxes, the surer object winning
 * where two overlap.
 *
 * Synthetic tensors only: this says nothing of what a model sees.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketvision/vision_seg.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
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

static int near(float a, float b)
{
    return fabsf(a - b) < 1e-5f;
}

static void test_upsample(void)
{
    /* Two planes of 2 x 2: a ramp and a constant. */
    static const float src[8] = { 0, 1, 2, 3, 5, 5, 5, 5 };
    float dst[2 * 4 * 4];
    int same = 1;
    int i;

    check("upsampling by 2 runs", vision_seg_upsample(src, 2, 2, 2, 2, dst) == 0);
    check("the corners are the source's corners (edges clamped)",
          near(dst[0], 0) && near(dst[3], 1) && near(dst[12], 2) && near(dst[15], 3));
    check("between them the weights are half-pixel ones", near(dst[1], 0.25f) && near(dst[2], 0.75f) &&
                                                             near(dst[4], 0.5f) && near(dst[5], 0.75f));
    for (i = 0; i < 16; i++) {
        same &= near(dst[16 + i], 5);
    }
    check("a constant plane stays constant, untouched by the one before it", same);
    check("by 1 it is a copy", vision_seg_upsample(src, 2, 2, 2, 1, dst) == 0 && memcmp(dst, src, sizeof(src)) == 0);
}

#define HW 2050 /* two tiles and a bit */

static void test_argmax(void)
{
    static float v[3 * HW];
    static uint8_t m[HW];
    int ok = 1;
    size_t i;

    for (i = 0; i < HW; i++) {
        v[i] = 1;
        v[HW + i] = i % 2 ? 2 : 1;        /* odd pixels: plane 1; even: a tie with plane 0 */
        v[2 * HW + i] = i == 1500 ? 9 : 0; /* one pixel in the second tile: plane 2 */
    }
    memset(m, 0xAA, sizeof(m));
    vision_seg_argmax(v, 3, HW, m);
    for (i = 0; i < HW; i++) {
        ok &= m[i] == (i == 1500 ? 2 : i % 2 ? 1 : 0);
    }
    check("the largest plane per pixel, across the tiles, a tie to the first", ok);
}

static void test_names(void)
{
    uint32_t i;
    int ok = 1;

    for (i = 0; i < VISION_SEG_CLASSES; i++) {
        const char *n = vision_seg_label(i);

        ok &= n[0] != '\0' && n[0] != '?' && strchr(n, ' ') == NULL && strlen(n) <= 20;
    }
    check("nineteen class names, protocol-safe", ok && strcmp(vision_seg_label(0), "road") == 0 &&
                                                     strcmp(vision_seg_label(18), "bicycle") == 0);
    check("a class beyond them is ? and black", strcmp(vision_seg_label(VISION_SEG_CLASSES), "?") == 0 &&
                                                    vision_seg_color(VISION_SEG_CLASSES) == 0);
    check("classes have colours of their own", vision_seg_color(0) != vision_seg_color(13) &&
                                                   vision_inst_color(0) != vision_inst_color(1) &&
                                                   vision_inst_color(0) == vision_inst_color(20));
    {
        uint32_t id;
        uint32_t k;
        int distinct = 1;

        /* Any VISION_INST_PALETTE ids in a row, each a colour of its own. */
        for (id = 1; id <= 40; id++) {
            for (k = 1; k < VISION_INST_PALETTE; k++) {
                distinct &= vision_inst_track_color(id, 0) != vision_inst_track_color(id + k, 0);
            }
        }
        check("an object's colour follows its track id, every twenty in a row apart",
              distinct && vision_inst_track_color(5, 0) == vision_inst_track_color(5, 9) &&
                  vision_inst_track_color(1, 0) == vision_inst_track_color(1 + VISION_INST_PALETTE, 0) &&
                  vision_inst_track_color(UINT32_MAX, 0) != 0);
        check("with no track, its class's colour", vision_inst_track_color(0, 2) == vision_inst_color(2));
    }
}

#define CLASSES 2
#define CH (4 + CLASSES + VISION_INST_COEFFS)
#define ROWS 5

static float head[CH * ROWS];

static void row(int r, float cx, float cy, float w, float h, float s0, float s1, float coeff0)
{
    head[0 * ROWS + r] = cx;
    head[1 * ROWS + r] = cy;
    head[2 * ROWS + r] = w;
    head[3 * ROWS + r] = h;
    head[4 * ROWS + r] = s0;
    head[5 * ROWS + r] = s1;
    head[(4 + CLASSES) * ROWS + r] = coeff0;
}

static void test_decode(void)
{
    struct vision_inst inst[VISION_INST_MAX];
    int n;

    memset(head, 0, sizeof(head));
    row(0, 100, 100, 80, 80, 0.9f, 0.1f, 1.5f);  /* the object */
    row(1, 102, 100, 80, 80, 0.8f, 0.0f, 7.0f);  /* the same again, less sure */
    row(2, 100, 100, 80, 80, 0.0f, 0.7f, -2.0f); /* another class in the same place */
    row(3, 250, 250, 40, 40, 0.2f, 0.1f, 0.0f);  /* below the floor */
    row(4, 250, 250, 0, 40, 0.9f, 0.0f, 0.0f);   /* no box at all */
    n = vision_inst_decode(head, CH, ROWS, 320, 320, 350, 650, inst, VISION_INST_MAX);
    check("two objects: the duplicate, the weak row and the empty box are gone", n == 2);
    check("the surest first, with its class, box and coefficients",
          n == 2 && inst[0].det.cls == 0 && inst[0].det.conf == 900 && inst[0].det.box.x == 60 &&
              inst[0].det.box.y == 60 && inst[0].det.box.w == 80 && inst[0].det.box.h == 80 &&
              near(inst[0].coeff[0], 1.5f) && near(inst[0].coeff[1], 0));
    check("another class in the same place is another object", n == 2 && inst[1].det.cls == 1 &&
                                                                   inst[1].det.conf == 700 &&
                                                                   near(inst[1].coeff[0], -2.0f));
    check("no more than asked for", vision_inst_decode(head, CH, ROWS, 320, 320, 350, 650, inst, 1) == 1);
    row(0, 310, 100, 80, 80, 0.9f, 0.1f, 1.5f);
    n = vision_inst_decode(head, CH, ROWS, 320, 320, 350, 650, inst, VISION_INST_MAX);
    check("a box over the edge is cut to the input", n >= 1 && inst[0].det.box.x == 270 && inst[0].det.box.w == 50);
    check("a head too narrow for coefficients, no rows, no room: refused",
          vision_inst_decode(head, 4 + VISION_INST_COEFFS, ROWS, 320, 320, 350, 650, inst, 4) == -EINVAL &&
              vision_inst_decode(head, CH, 0, 320, 320, 350, 650, inst, 4) == -EINVAL &&
              vision_inst_decode(head, CH, ROWS, 320, 320, 350, 650, inst, 0) == -EINVAL);
}

#define PW 8
#define PH 8
#define MW (PW * 2)
#define MH (PH * 2)

static void test_map(void)
{
    static float proto[VISION_INST_COEFFS * PH * PW];
    static float tmp[PH * PW];
    static uint8_t map[MW * MH];
    struct vision_inst inst[2];
    int left = 1;
    int right = 1;
    int none = 1;
    int x;
    int y;

    /* Prototype 0: positive on the left half, negative on the right. */
    memset(proto, 0, sizeof(proto));
    for (y = 0; y < PH; y++) {
        for (x = 0; x < PW; x++) {
            proto[y * PW + x] = x < PW / 2 ? 1.0f : -1.0f;
        }
    }
    memset(inst, 0, sizeof(inst));
    inst[0].det.box = (struct vision_box) { 0, 0, 32, 32 };
    inst[0].coeff[0] = 1.0f;
    vision_inst_map(inst, 1, proto, PW, PH, 32, 32, tmp, map);
    for (y = 0; y < MH; y++) {
        for (x = 0; x < MW; x++) {
            left &= map[y * MW + x] == (x < MW / 2 ? 0 : VISION_SEG_NONE);
        }
    }
    check("a mask is where coefficients times prototypes are positive", left);

    /* The same mask in a box of the top-left quarter only. */
    inst[0].det.box = (struct vision_box) { 0, 0, 16, 16 };
    vision_inst_map(inst, 1, proto, PW, PH, 32, 32, tmp, map);
    for (y = 0; y < MH; y++) {
        for (x = 0; x < MW; x++) {
            none &= map[y * MW + x] == (x < MW / 2 && y < MH / 2 ? 0 : VISION_SEG_NONE);
        }
    }
    check("and only inside the object's box", none);

    /* Two objects over the whole input: the first takes the left, the
     * second (the prototype negated) the right; where the second also
     * claims everything, the first keeps what it has. */
    inst[0].det.box = (struct vision_box) { 0, 0, 32, 32 };
    inst[1].det.box = (struct vision_box) { 0, 0, 32, 32 };
    inst[1].coeff[0] = -1.0f;
    vision_inst_map(inst, 2, proto, PW, PH, 32, 32, tmp, map);
    for (y = 0; y < MH; y++) {
        for (x = 0; x < MW; x++) {
            right &= map[y * MW + x] == (x < MW / 2 ? 0 : 1);
        }
    }
    check("two objects share the map, each cell one object's", right);
    inst[1].coeff[0] = 1.0f;
    vision_inst_map(inst, 2, proto, PW, PH, 32, 32, tmp, map);
    check("where two overlap the surer one keeps the cell", map[0] == 0 && map[MW / 2 - 1] == 0);
    vision_inst_map(inst, 0, proto, PW, PH, 32, 32, tmp, map);
    check("no objects: an empty map", map[0] == VISION_SEG_NONE && map[MW * MH - 1] == VISION_SEG_NONE);
}

int main(void)
{
    test_upsample();
    test_argmax();
    test_names();
    test_decode();
    test_map();
    printf("vision_seg_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
