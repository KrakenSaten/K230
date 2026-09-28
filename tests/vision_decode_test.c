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
    struct vision_decode_params p = { IN, IN, CLASSES, FW, FH, 350 };

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
    test_malformed();
    test_bounded();
    test_nms();
    free(tensor);
    printf("vision_decode_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
