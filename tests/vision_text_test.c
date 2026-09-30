/*
 * Text (core/pocketvision/vision_text.c): regions of a probability map -
 * found, in reading order, grown back by the unclip, the weak and the tiny
 * dropped, the surest kept when there are too many, a map of NaN or nothing
 * read as no text - and the CTC read of one line from probabilities and
 * from logits: the blank dropped, repeats merged, a repeat across a blank
 * kept, a step of NaN read as nothing, the bound held.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketvision/vision_text.h"

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

#define MW 512
#define MH 512
static float map[MH * MW * 2];

static void clear(void)
{
    memset(map, 0, sizeof(map));
}

static void fill(int x, int y, int w, int h, float v)
{
    int i;
    int j;

    for (j = y; j < y + h; j++) {
        for (i = x; i < x + w; i++) {
            map[((size_t)j * MW + i) * 2] = v;
        }
    }
}

static int contains(const struct vision_box *b, int x, int y, int w, int h)
{
    return b->x <= x && b->y <= y && b->x + b->w >= x + w && b->y + b->h >= y + h;
}

static void test_regions(void)
{
    struct vision_text_box tb[VISION_TEXT_MAX];
    int n;
    int i;

    clear();
    check("an empty map has no text", vision_text_regions(map, MW, MH, 2, 300, 500, tb, VISION_TEXT_MAX) == 0);
    fill(300, 40, 120, 20, 0.9f);  /* the second line's right part... */
    fill(40, 200, 200, 24, 0.95f); /* a lower line */
    fill(40, 42, 200, 18, 0.9f);   /* the first line, left */
    n = vision_text_regions(map, MW, MH, 2, 300, 500, tb, VISION_TEXT_MAX);
    check("three lines found", n == 3);
    check("in reading order: the top line left to right, then the lower one",
          n == 3 && tb[0].box.x < 60 && tb[1].box.x > 280 && tb[2].box.y > 180);
    check("each box holds its line, grown back by the unclip",
          n == 3 && contains(&tb[0].box, 40, 42, 200, 18) && contains(&tb[1].box, 300, 40, 120, 20) &&
              contains(&tb[2].box, 40, 200, 200, 24) && tb[2].box.x < 40 && tb[2].box.h > 24);
    /* DB's unclip: area x 1.5 / perimeter, 16 px for a 200 x 24 line. */
    check("but not by much: under the line's height at each end", n == 3 && tb[2].box.x >= 40 - 24 &&
                                                                    tb[2].box.w <= 200 + 48 && tb[2].box.x == 24);
    check("the scores are the lines' own", n == 3 && tb[2].score >= 940 && tb[0].score >= 890 && tb[0].score <= 910);

    clear();
    fill(10, 10, 2, 2, 0.99f);
    check("a speck is noise", vision_text_regions(map, MW, MH, 2, 300, 500, tb, VISION_TEXT_MAX) == 0);
    clear();
    fill(10, 10, 100, 20, 0.4f);
    check("a region over the threshold but weak on the whole is dropped",
          vision_text_regions(map, MW, MH, 2, 300, 500, tb, VISION_TEXT_MAX) == 0);
    check("and kept at a lower bar", vision_text_regions(map, MW, MH, 2, 300, 350, tb, VISION_TEXT_MAX) == 1);
    clear();
    fill(10, 10, 100, 20, 0.3f);
    check("exactly the threshold is not text", vision_text_regions(map, MW, MH, 2, 300, 200, tb, VISION_TEXT_MAX) == 0);

    clear();
    for (i = 0; i < 20; i++) {
        /* 20 lines, each a little surer than the one above. */
        fill(20, 10 + i * 25, 100, 12, 0.6f + 0.015f * (float)i);
    }
    n = vision_text_regions(map, MW, MH, 2, 300, 500, tb, VISION_TEXT_MAX);
    check("more lines than it keeps: the surest VISION_TEXT_MAX", n == VISION_TEXT_MAX && tb[0].box.y > 10 + 7 * 25 - 8);
    {
        int ok = 1;

        for (i = 1; i < n; i++) {
            ok &= tb[i].box.y > tb[i - 1].box.y;
        }
        check("still in reading order", ok);
    }
    check("asked for fewer, fewer", vision_text_regions(map, MW, MH, 2, 300, 500, tb, 3) == 3);

    clear();
    for (i = 0; i < MW * MH; i++) {
        map[(size_t)i * 2] = NAN;
    }
    check("a map of NaN has no text", vision_text_regions(map, MW, MH, 2, 300, 500, tb, VISION_TEXT_MAX) == 0);
    check("nonsense is refused", vision_text_regions(NULL, MW, MH, 2, 300, 500, tb, 4) == -1 &&
                                     vision_text_regions(map, 0, MH, 2, 300, 500, tb, 4) == -1 &&
                                     vision_text_regions(map, MW, MH, 0, 300, 500, tb, 4) == -1 &&
                                     vision_text_regions(map, MW, MH, 2, 300, 500, tb, 0) == -1);
    clear();
    fill(0, 0, 600 > MW ? MW : 600, 30, 0.9f);
    n = vision_text_regions(map, MW, MH, 2, 300, 500, tb, 4);
    check("a line at the map's edge is clipped to the map", n == 1 && tb[0].box.x == 0 && tb[0].box.y == 0 &&
                                                              tb[0].box.w == MW);
}

#define STEPS 16
#define CLASSES 5 /* 'a' 'b' 'c' 'd', then the blank */
static float sc[STEPS * CLASSES];

static void seq(const int *cls)
{
    int t;

    memset(sc, 0, sizeof(sc));
    for (t = 0; t < STEPS; t++) {
        sc[t * CLASSES + cls[t]] = 1.0f;
    }
}

static void test_ctc(void)
{
    const int blank = CLASSES - 1;
    const int s1[STEPS] = { 4, 0, 0, 4, 1, 4, 4, 1, 2, 2, 2, 4, 3, 4, 4, 4 };
    uint32_t out[8];
    uint16_t pos[8];
    uint16_t conf = 0;
    int n;
    int t;

    seq(s1);
    n = vision_text_ctc_pos(sc, STEPS, CLASSES, blank, out, pos, 8, &conf);
    check("probabilities: a b b c d - repeats merged, a repeat across a blank kept, the blank dropped",
          n == 5 && out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 2 && out[4] == 3 && conf == 1000);
    check("with the step each was read at", n == 5 && pos[0] == 1 && pos[1] == 4 && pos[2] == 7 && pos[3] == 8 &&
                                                 pos[4] == 12);
    check("no more than asked for", vision_text_ctc(sc, STEPS, CLASSES, blank, out, 2, NULL) == 2 && out[1] == 1);
    for (t = 0; t < STEPS * CLASSES; t++) {
        sc[t] = sc[t] > 0.5f ? 6.0f : -2.0f; /* logits */
    }
    n = vision_text_ctc(sc, STEPS, CLASSES, blank, out, 8, &conf);
    check("logits read the same, their confidence from a softmax", n == 5 && out[3] == 2 && conf > 990 && conf < 1000);
    seq(s1);
    for (t = 0; t < CLASSES; t++) {
        sc[4 * CLASSES + t] = NAN;
    }
    n = vision_text_ctc(sc, STEPS, CLASSES, blank, out, 8, &conf);
    check("a step of NaN reads nothing (the first b), and the reading goes on past it", n == 4 && out[0] == 0 &&
                                                                                           out[1] == 1 && out[2] == 2);
    {
        const int s2[STEPS] = { 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4 };

        seq(s2);
        check("all blank reads nothing", vision_text_ctc(sc, STEPS, CLASSES, blank, out, 8, &conf) == 0 && conf == 0);
    }
    check("a blank outside the classes is refused", vision_text_ctc(sc, STEPS, CLASSES, CLASSES, out, 8, NULL) == -1);
    check("nonsense is refused", vision_text_ctc(NULL, STEPS, CLASSES, blank, out, 8, NULL) == -1 &&
                                     vision_text_ctc(sc, 0, CLASSES, blank, out, 8, NULL) == -1 &&
                                     vision_text_ctc(sc, STEPS, CLASSES, blank, NULL, 8, NULL) == -1);
}

int main(void)
{
    test_regions();
    test_ctc();
    printf("vision_text_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
