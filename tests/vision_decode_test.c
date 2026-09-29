/*
 * The detector's raw output into boxes, and the suppression after it: a
 * tensor built the way the head lays it out decodes to the boxes put in
 * with the letterbox undone; the wrong shape, the wrong size, NaN,
 * infinities, absurd coordinates and empty boxes are refused or skipped and
 * counted; the candidate list is bounded and keeps the best; suppression
 * keeps the most confident of overlapping boxes of one class and never
 * merges classes.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketvision/vision_decode.h"
#include "pocketvision/vision_nms.h"

#include <errno.h>
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

enum { IN = 320, CLASSES = 80, FW = 640, FH = 360 };
static uint32_t ROWS;
static float *tensor;

static void clear(void)
{
    memset(tensor, 0, (size_t)(4 + CLASSES) * ROWS * sizeof(float));
}

/* A frame-space box into row r of the tensor, as the head would say it. */
static void put(uint32_t r, int cls, float conf, float x, float y, float w, float h)
{
    float ratio = (float)IN / (float)FW; /* the smaller of the two: 0.5 */

    tensor[0 * ROWS + r] = (x + w / 2) * ratio;
    tensor[1 * ROWS + r] = (y + h / 2) * ratio;
    tensor[2 * ROWS + r] = w * ratio;
    tensor[3 * ROWS + r] = h * ratio;
    tensor[(4 + cls) * ROWS + r] = conf;
}

static struct vision_decode_params params(void)
{
    struct vision_decode_params p = { IN, IN, CLASSES, FW, FH, 350, 0, 0 };

    return p;
}

static void test_rows(void)
{
    check("320 x 320 has 2100 rows (40^2 + 20^2 + 10^2)", vision_decode_rows(320, 320) == 2100);
    check("640 x 640 has 8400", vision_decode_rows(640, 640) == 8400);
    check("an input that is not a multiple of 32 has no head", vision_decode_rows(300, 320) == 0);
    check("a zero input has none", vision_decode_rows(0, 320) == 0);
}

static void test_decode(void)
{
    struct vision_decode_params p = params();
    struct vision_det d[VISION_MAX_CANDIDATES];
    uint32_t dims[3] = { 1, 4 + CLASSES, ROWS };
    uint32_t bad = 99;
    int n;

    clear();
    put(10, 0, 0.9f, 100, 50, 80, 160);   /* a person */
    put(500, 2, 0.6f, 400, 200, 120, 60); /* a car */
    put(1200, 5, 0.2f, 10, 10, 30, 30);   /* below the threshold */
    n = vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
    check("two boxes pass the threshold", n == 2);
    check("nothing was bad", bad == 0);
    check("the person is back in frame pixels",
          n == 2 && d[0].cls == 0 && d[0].conf == 900 && d[0].box.x == 100 && d[0].box.y == 50 &&
              d[0].box.w == 80 && d[0].box.h == 160);
    check("the car too", n == 2 && d[1].cls == 2 && d[1].conf == 600 && d[1].box.x == 400 &&
                             d[1].box.y == 200 && d[1].box.w == 120 && d[1].box.h == 60);

    /* The class with the highest score wins, whatever else scores. */
    clear();
    put(7, 3, 0.4f, 10, 10, 50, 50);
    tensor[(4 + 9) * ROWS + 7] = 0.7f;
    n = vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
    check("the best class of a row is the one reported", n == 1 && d[0].cls == 9 && d[0].conf == 700);

    /* A box over the frame's edge is clipped to it, not refused. */
    clear();
    put(3, 0, 0.8f, -40, -20, 100, 100);
    put(4, 0, 0.8f, 600, 300, 100, 100);
    n = vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
    check("boxes over the edges are clipped to the frame",
          n == 2 && d[0].box.x == 0 && d[0].box.y == 0 && d[0].box.w == 60 && d[0].box.h == 80 &&
              d[1].box.x == 600 && d[1].box.w == 40 && d[1].box.y == 300 && d[1].box.h == 60);
}

/* A crop (TRAFFIC's region of interest) letterboxed alone: the decoder
 * undoes the crop's own ratio, clips to the crop, and moves the boxes back
 * into frame pixels by its origin. */
static void test_crop(void)
{
    struct vision_decode_params p = params();
    struct vision_det d[VISION_MAX_CANDIDATES];
    uint32_t dims[3] = { 1, 4 + CLASSES, ROWS };
    uint32_t bad = 0;
    /* A 200 x 100 road band at (300, 150): ratio min(320/200, 320/100) = 1.6,
     * where the whole frame's is 0.5 - a far car 3.2 times taller. */
    const float ratio = 1.6f;
    int n;

    p.frame_w = 200;
    p.frame_h = 100;
    p.off_x = 300;
    p.off_y = 150;
    clear();
    /* A 40 x 16 car at frame (340, 170): (40, 20) in the crop. */
    tensor[0 * ROWS + 11] = (40.0f + 20.0f) * ratio;
    tensor[1 * ROWS + 11] = (20.0f + 8.0f) * ratio;
    tensor[2 * ROWS + 11] = 40.0f * ratio;
    tensor[3 * ROWS + 11] = 16.0f * ratio;
    tensor[(4 + 2) * ROWS + 11] = 0.5f;
    /* One reaching past the crop's right edge: clipped there, not at the frame's. */
    tensor[0 * ROWS + 12] = (180.0f + 20.0f) * ratio;
    tensor[1 * ROWS + 12] = (50.0f + 8.0f) * ratio;
    tensor[2 * ROWS + 12] = 40.0f * ratio;
    tensor[3 * ROWS + 12] = 16.0f * ratio;
    tensor[(4 + 2) * ROWS + 12] = 0.5f;
    n = vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
    check("a car in the crop comes back in frame pixels",
          n == 2 && d[0].box.x == 340 && d[0].box.y == 170 && d[0].box.w == 40 && d[0].box.h == 16);
    check("and one past the crop's edge is clipped at the crop's edge, in frame pixels",
          n == 2 && d[1].box.x == 480 && d[1].box.w == 20 && d[1].box.y == 200 && d[1].box.h == 16);
    p.off_x = -2;
    check("a crop above or left of the frame is refused",
          vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad) ==
              -EINVAL);
    p.off_x = VISION_MAX_COORD - 100;
    check("and so is one reaching past the largest coordinate",
          vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad) ==
              -EINVAL);
}

static void test_malformed(void)
{
    struct vision_decode_params p = params();
    struct vision_det d[VISION_MAX_CANDIDATES];
    uint32_t dims[3] = { 1, 4 + CLASSES, ROWS };
    uint32_t wrong[3];
    uint32_t bad = 0;
    size_t count = (size_t)(4 + CLASSES) * ROWS;
    int n;

    clear();
    put(10, 0, 0.9f, 100, 50, 80, 160);
    memcpy(wrong, dims, sizeof(wrong));
    wrong[1] = 5 + CLASSES;
    check("a head with one feature too many is refused before a value is read",
          vision_decode(tensor, count, wrong, &p, d, VISION_MAX_CANDIDATES, &bad) == -EPROTO);
    memcpy(wrong, dims, sizeof(wrong));
    wrong[0] = 2;
    check("a batch of two is refused",
          vision_decode(tensor, count, wrong, &p, d, VISION_MAX_CANDIDATES, &bad) == -EPROTO);
    memcpy(wrong, dims, sizeof(wrong));
    wrong[2] = ROWS - 1;
    check("the wrong row count is refused",
          vision_decode(tensor, count, wrong, &p, d, VISION_MAX_CANDIDATES, &bad) == -EPROTO);
    check("a tensor shorter than its shape is refused",
          vision_decode(tensor, count - 1, dims, &p, d, VISION_MAX_CANDIDATES, &bad) == -EPROTO);
    check("a model whose input is not whole cells is refused as parameters",
          (p.in_w = 300, vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad)) ==
              -EINVAL);
    p = params();
    check("too many classes is refused as parameters",
          (p.classes = VISION_MAX_CLASSES + 1,
           vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad)) == -EINVAL);
    p = params();

    /* NaN scores and boxes: skipped and counted, the good row still found. */
    clear();
    put(10, 0, 0.9f, 100, 50, 80, 160);
    tensor[(4 + 1) * ROWS + 20] = NAN;
    tensor[(4 + 0) * ROWS + 21] = 0.9f;
    tensor[0 * ROWS + 21] = INFINITY;
    tensor[(4 + 0) * ROWS + 22] = 0.9f;
    tensor[2 * ROWS + 22] = 0.0f; /* an empty box */
    tensor[(4 + 0) * ROWS + 23] = 0.9f;
    tensor[0 * ROWS + 23] = 1e9f; /* far outside the input */
    tensor[2 * ROWS + 23] = 10.0f;
    tensor[3 * ROWS + 23] = 10.0f;
    tensor[(4 + 0) * ROWS + 24] = 7.0f; /* a score that is not a probability */
    n = vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
    check("NaN, infinity, an empty box, an absurd box and an impossible score are skipped",
          n == 1 && bad == 5 && d[0].box.x == 100);

    /* A quantized model on a dark frame (unit B, 2026-09-28): every score a
     * hair below zero, boxes all over the input. Nothing found, and nothing
     * refused - that is quantization, not a broken tensor. */
    {
        size_t i;
        uint32_t r;

        clear();
        for (i = (size_t)4 * ROWS; i < count; i++) {
            tensor[i] = -0.0091f + (float)(i % 7) * 0.001f; /* -0.0091 .. -0.0031 */
        }
        for (r = 0; r < ROWS; r++) {
            tensor[0 * ROWS + r] = 2.5f + (float)(r % 300);
            tensor[1 * ROWS + r] = 2.5f + (float)(r % 300);
            tensor[2 * ROWS + r] = 2.5f + (float)(r % 50);
            tensor[3 * ROWS + r] = 2.5f + (float)(r % 50);
        }
        n = vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
        check("scores a hair below zero are quantization: nothing found, nothing refused", n == 0 && bad == 0);
        /* A score a hair above one is a certain detection, clamped. */
        put(10, 0, 1.004f, 100, 50, 80, 160);
        n = vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
        check("a score a hair above one is clamped to a certain detection",
              n == 1 && bad == 0 && d[0].conf == 1000 && d[0].box.x == 100);
        /* But no sigmoid gives -0.5. */
        tensor[(4 + 3) * ROWS + 11] = -0.5f;
        for (i = 0; i < CLASSES; i++) {
            tensor[(4 + i) * ROWS + 11] = -0.5f;
        }
        n = vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
        check("a row whose best score is -0.5 is still refused", n == 1 && bad == 1);
    }

    /* Everything NaN: nothing found, every row bad, no crash, no huge
     * coordinate anywhere. */
    {
        size_t i;

        for (i = 0; i < count; i++) {
            tensor[i] = NAN;
        }
        n = vision_decode(tensor, count, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
        check("a tensor of NaN finds nothing and counts every row bad", n == 0 && bad == ROWS);
    }
}

static void test_bounded(void)
{
    struct vision_decode_params p = params();
    struct vision_det d[VISION_MAX_CANDIDATES];
    uint32_t dims[3] = { 1, 4 + CLASSES, ROWS };
    uint32_t bad;
    uint32_t r;
    int n;
    int i;
    int ok = 1;
    int strong = 0;

    /* Every row passes; the list keeps VISION_MAX_CANDIDATES and among them
     * every one of the strongest. */
    clear();
    for (r = 0; r < ROWS; r++) {
        float conf = r < 100 ? 0.95f : 0.4f;

        put(r, (int)(r % CLASSES), conf, (float)(r % 60) * 10, (float)((r / 60) % 30) * 10, 20, 20);
    }
    n = vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, VISION_MAX_CANDIDATES, &bad);
    check("the candidate list is bounded", n == VISION_MAX_CANDIDATES);
    for (i = 0; i < n; i++) {
        strong += d[i].conf == 950;
        ok &= d[i].box.w == 20 && d[i].box.h == 20;
    }
    check("and the hundred strongest are all in it", strong == 100 && ok);
    check("a smaller list is honoured too",
          vision_decode(tensor, (size_t)(4 + CLASSES) * ROWS, dims, &p, d, 8, &bad) == 8);
}

static void test_nms(void)
{
    struct vision_det c[8];
    struct vision_det out[8];
    int n;

    memset(c, 0, sizeof(c));
    /* Three boxes on one person, one on another, one car on the first. */
    c[0] = (struct vision_det) { { 100, 100, 100, 200 }, 0, 700 };
    c[1] = (struct vision_det) { { 105, 102, 100, 200 }, 0, 900 };
    c[2] = (struct vision_det) { { 95, 98, 104, 204 }, 0, 800 };
    c[3] = (struct vision_det) { { 400, 100, 100, 200 }, 0, 600 };
    c[4] = (struct vision_det) { { 100, 100, 100, 200 }, 2, 650 };
    n = vision_nms(c, 5, 650, out, 8);
    check("three survive: the best person, the car, the other person", n == 3);
    check("most confident first", n == 3 && out[0].conf == 900 && out[1].conf == 650 && out[2].conf == 600);
    check("the three boxes on one person were merged into the best, the car kept",
          n == 3 && out[0].cls == 0 && out[0].box.x == 105 && out[1].cls == 2 && out[2].cls == 0 &&
              out[2].box.x == 400);
    check("the output is bounded", vision_nms(c, 5, 650, out, 2) == 2);
    check("nothing in, nothing out", vision_nms(c, 0, 650, out, 8) == 0);
    /* A stricter threshold keeps nearly-identical boxes apart; a looser one
     * merges everything of a class. */
    c[0] = (struct vision_det) { { 100, 100, 100, 200 }, 0, 700 };
    c[1] = (struct vision_det) { { 150, 100, 100, 200 }, 0, 900 };
    n = vision_nms(c, 2, 900, out, 8);
    check("half-overlapping boxes both survive a loose threshold", n == 2);
    n = vision_nms(c, 2, 100, out, 8);
    check("and only the best a strict one", n == 1 && out[0].conf == 900);
    {
        struct vision_box a = { 0, 0, 10, 10 };
        struct vision_box b = { 5, 5, 10, 10 };
        struct vision_box e = { 0, 0, 0, 10 };

        check("iou of a quarter overlap is 25/175", vision_iou_permille(&a, &b) == 142);
        check("iou of a box with itself is 1000", vision_iou_permille(&a, &a) == 1000);
        check("an empty box overlaps nothing", vision_iou_permille(&a, &e) == 0);
    }
    /* Nested duplicates: a partial box inside the full one is the same
     * object; a box of another class, a box half outside, and two boxes
     * of one size are not. */
    c[0] = (struct vision_det) { { 100, 50, 500, 350 }, 0, 800 }; /* the person, whole */
    c[1] = (struct vision_det) { { 300, 60, 150, 330 }, 0, 600 }; /* the partial box, inside */
    c[2] = (struct vision_det) { { 300, 60, 150, 330 }, 2, 600 }; /* a car in the same place */
    c[3] = (struct vision_det) { { 500, 60, 200, 330 }, 0, 600 }; /* half outside */
    check("the partial box inside the whole one goes, the rest stays",
          vision_nms_nested(c, 4, 850) == 3 && c[0].box.w == 500 && c[1].cls == 2 && c[2].box.x == 500);
    c[0] = (struct vision_det) { { 100, 100, 100, 100 }, 0, 800 };
    c[1] = (struct vision_det) { { 100, 100, 100, 100 }, 0, 700 };
    check("two boxes of one size are left to the IoU test", vision_nms_nested(c, 2, 850) == 2);
    c[0] = (struct vision_det) { { 100, 100, 100, 100 }, 0, 800 };
    c[1] = (struct vision_det) { { 150, 100, 100, 100 }, 0, 700 };
    c[2] = (struct vision_det) { { 0, 0, 640, 360 }, 0, 400 }; /* one box over everything */
    check("a box over the whole frame swallows the smaller ones of its class",
          vision_nms_nested(c, 3, 850) == 1 && c[0].box.w == 640);
    check("nothing in, nothing out", vision_nms_nested(c, 0, 850) == 0);
}

/* TRAFFIC's weak vehicles (vision_nms_add_weak): a distant car below the
 * general threshold is added, but never at the cost of a stronger box. */
static void test_add_weak(void)
{
    struct vision_det d[8];
    struct vision_det w[8];
    int n;

    memset(d, 0, sizeof(d));
    memset(w, 0, sizeof(w));
    d[0] = (struct vision_det) { { 100, 200, 60, 30 }, 2, 700 };  /* a near car */
    d[1] = (struct vision_det) { { 300, 100, 40, 80 }, 0, 600 };  /* a person */
    w[0] = (struct vision_det) { { 500, 150, 24, 10 }, 2, 300 };  /* a far car, alone */
    w[1] = (struct vision_det) { { 102, 201, 60, 30 }, 2, 280 };  /* the near car again */
    w[2] = (struct vision_det) { { 0, 150, 640, 120 }, 2, 270 };  /* a band over the near car */
    w[3] = (struct vision_det) { { 110, 205, 20, 12 }, 2, 260 };  /* a part of the near car */
    w[4] = (struct vision_det) { { 300, 100, 40, 80 }, 2, 260 };  /* a car where the person is */
    n = vision_nms_add_weak(d, 2, w, 5, 650, 850, 8);
    check("a far car nothing else saw is added after the strong boxes",
          n == 4 && d[2].box.x == 500 && d[2].conf == 300);
    check("the strong boxes are untouched, in their order",
          d[0].box.x == 100 && d[0].conf == 700 && d[1].cls == 0 && d[1].conf == 600);
    check("a weak box overlapping, containing or inside a strong one of its class is left out",
          n == 4 && d[3].box.x == 300 && d[3].cls == 2);
    check("but a weak car where a person is still counts (another class)", n == 4 && d[3].conf == 260);
    n = vision_nms_add_weak(d, 2, w, 5, 650, 850, 3);
    check("the total is bounded", n == 3 && d[2].box.x == 500);
    check("no weak boxes, nothing changes", vision_nms_add_weak(d, 2, w, 0, 650, 850, 8) == 2);
    check("full already, nothing is added", vision_nms_add_weak(d, 2, w, 5, 650, 850, 2) == 2);
}

int main(void)
{
    ROWS = vision_decode_rows(IN, IN);
    tensor = calloc((size_t)(4 + CLASSES) * ROWS, sizeof(float));
    if (!tensor) {
        return 2;
    }
    test_rows();
    test_decode();
    test_crop();
    test_malformed();
    test_bounded();
    test_nms();
    test_add_weak();
    free(tensor);
    printf("vision_decode_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
