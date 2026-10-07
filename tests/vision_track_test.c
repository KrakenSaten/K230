/*
 * Tracks and the counting line: an object seen frame after frame keeps its
 * id; one that moves is followed by its motion; a missed frame does not
 * lose it and enough missed frames do; a dropout or a jump the overlap
 * cannot follow is found again by distance; a class the detector cannot
 * make up its mind about stays one track; ids are never reused; the list
 * is bounded; a track counts once when it crosses the line, in the
 * direction it went, left to right and right to left, up and down, never
 * on a touch, never on a jitter, never while unconfirmed, never on a
 * prediction, once through a dropout, and again only when it really comes
 * back; a track that expired and an object that returns are two ids and
 * at most one count each.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketvision/vision_line.h"
#include "pocketvision/vision_track.h"

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

static struct vision_det det(int cls, int x, int y, int w, int h)
{
    struct vision_det d = { { x, y, w, h }, (uint16_t)cls, 800 };

    return d;
}

static const struct vision_track *find(const struct vision_tracker *tr, uint32_t id)
{
    int i;

    for (i = 0; i < tr->count; i++) {
        if (tr->t[i].id == id) {
            return &tr->t[i];
        }
    }
    return NULL;
}

/* One frame with one box, then the count on line l. */
static int step(struct vision_tracker *tr, const struct vision_line *l, struct vision_counts *c,
                const struct vision_det *d, int n)
{
    vision_tracker_update(tr, d, n);
    return vision_line_count(l, 0, tr, c, NULL, 0);
}

static void test_identity(void)
{
    struct vision_tracker tr;
    struct vision_det d[2];
    int i;

    vision_tracker_init(&tr);
    d[0] = det(0, 100, 100, 50, 100);
    check("one detection starts one track", vision_tracker_update(&tr, d, 1) == 1);
    check("with id 1, seen once, not yet confirmed",
          tr.t[0].id == 1 && tr.t[0].hits == 1 && !tr.t[0].confirmed);
    d[0] = det(0, 104, 101, 50, 100);
    vision_tracker_update(&tr, d, 1);
    check("the same object a frame later keeps id 1 and is confirmed",
          tr.count == 1 && tr.t[0].id == 1 && tr.t[0].hits == 2 && tr.t[0].confirmed);
    check("and took the new box", tr.t[0].box.x == 104);
    d[0] = det(0, 108, 102, 50, 100);
    d[1] = det(0, 400, 100, 50, 100);
    vision_tracker_update(&tr, d, 2);
    check("a second object gets id 2, the first keeps 1",
          tr.count == 2 && find(&tr, 1) && find(&tr, 2) && find(&tr, 2)->box.x == 400);
    /* The same box, another class: not the same object. */
    d[0] = det(0, 112, 103, 50, 100);
    d[1] = det(2, 400, 100, 50, 100);
    vision_tracker_update(&tr, d, 2);
    check("a box of another class is a new track, not a match",
          tr.count == 3 && find(&tr, 3) && find(&tr, 3)->cls == 2 && find(&tr, 2)->misses == 1);
    /* Motion: an object moving 10 px a frame is matched where it will be. */
    vision_tracker_init(&tr);
    for (i = 0; i < 10; i++) {
        d[0] = det(0, 100 + i * 10, 100, 40, 40);
        vision_tracker_update(&tr, d, 1);
    }
    check("a moving object keeps one id over ten frames",
          tr.count == 1 && tr.t[0].id == 1 && tr.t[0].hits == 10 &&
              tr.t[0].vx / (1 << VISION_TRACK_V_SHIFT) == 10);
    check("and remembers where it started", tr.t[0].ox == 120 && tr.t[0].oy == 120);
    /* A jump larger than the overlap and the reacquire radius allow is a
     * new object. */
    d[0] = det(0, 500, 300, 40, 40);
    vision_tracker_update(&tr, d, 1);
    check("a jump across the frame is a new track", tr.count == 2 && find(&tr, 2));
}

static void test_expiry(void)
{
    struct vision_tracker tr;
    struct vision_det d[1];
    int i;
    int alive = 1;

    vision_tracker_init(&tr);
    d[0] = det(0, 100, 100, 50, 50);
    vision_tracker_update(&tr, d, 1);
    vision_tracker_update(&tr, d, 1);
    for (i = 0; i < VISION_TRACK_MAX_MISSES; i++) {
        alive &= vision_tracker_update(&tr, NULL, 0) == 1;
    }
    check("a track survives VISION_TRACK_MAX_MISSES empty frames", alive && tr.t[0].misses == VISION_TRACK_MAX_MISSES);
    check("and expires on the next", vision_tracker_update(&tr, NULL, 0) == 0);
    d[0] = det(0, 100, 100, 50, 50);
    vision_tracker_update(&tr, d, 1);
    check("an object seen again after expiry is a new id, never id 1 again",
          tr.count == 1 && tr.t[0].id == 2);
    /* Seen again within the misses: the same track, misses cleared. */
    vision_tracker_update(&tr, NULL, 0);
    vision_tracker_update(&tr, NULL, 0);
    vision_tracker_update(&tr, d, 1);
    check("seen again after two misses: the same track, misses cleared",
          tr.count == 1 && tr.t[0].id == 2 && tr.t[0].misses == 0);
    vision_tracker_clear(&tr);
    check("clear forgets the tracks", tr.count == 0);
    vision_tracker_update(&tr, d, 1);
    check("but not the ids handed out", tr.t[0].id == 3);
}

static void test_dropout(void)
{
    struct vision_tracker tr;
    struct vision_det d[1];
    int i;

    /* A car at 20 px a frame, unseen for five frames, then seen where it
     * would be: one id. */
    vision_tracker_init(&tr);
    for (i = 0; i < 4; i++) {
        d[0] = det(2, 100 + i * 20, 100, 60, 40);
        vision_tracker_update(&tr, d, 1);
    }
    for (i = 0; i < 5; i++) {
        vision_tracker_update(&tr, NULL, 0);
    }
    check("a coasting track moves with its motion, slowing",
          tr.count == 1 && tr.t[0].box.x > 160 && tr.t[0].box.x < 260 && tr.t[0].misses == 5);
    d[0] = det(2, 100 + 9 * 20, 100, 60, 40);
    vision_tracker_update(&tr, d, 1);
    check("seen again after a five-frame dropout: the same id", tr.count == 1 && tr.t[0].id == 1 &&
                                                                    tr.t[0].misses == 0);
    /* An object that stopped while unseen is found where it was. */
    vision_tracker_init(&tr);
    for (i = 0; i < 4; i++) {
        d[0] = det(0, 100 + i * 10, 100, 40, 80);
        vision_tracker_update(&tr, d, 1);
    }
    for (i = 0; i < 6; i++) {
        vision_tracker_update(&tr, NULL, 0);
    }
    d[0] = det(0, 140, 100, 40, 80);
    vision_tracker_update(&tr, d, 1);
    check("one that stopped while unseen is found where it stopped", tr.count == 1 && tr.t[0].id == 1);
    /* Reacquisition by distance: a box the overlap cannot match (no
     * overlap at all) but of the same size and within reach of the
     * prediction. */
    vision_tracker_init(&tr);
    d[0] = det(0, 100, 100, 100, 100);
    vision_tracker_update(&tr, d, 1);
    vision_tracker_update(&tr, d, 1);
    vision_tracker_update(&tr, NULL, 0);
    vision_tracker_update(&tr, NULL, 0);
    d[0] = det(0, 160, 140, 100, 100); /* 60 px over, 40 down: too little overlap, 72 px away */
    check("the boxes do not overlap", vision_iou_permille(&tr.t[0].box, &d[0].box) < VISION_TRACK_IOU_MIN);
    vision_tracker_update(&tr, d, 1);
    check("a jump within the reacquire radius keeps the id",
          tr.count == 1 && tr.t[0].id == 1 && tr.reacquired == 1);
    d[0] = det(0, 160, 140, 30, 30); /* a tenth of the size */
    vision_tracker_update(&tr, d, 1);
    check("but a box of another size is a new object", tr.count == 2 && find(&tr, 2));
    /* An unconfirmed track is not reacquired: a single spurious box does
     * not grab the next one. */
    vision_tracker_init(&tr);
    d[0] = det(0, 100, 100, 100, 100);
    vision_tracker_update(&tr, d, 1);
    vision_tracker_update(&tr, NULL, 0);
    d[0] = det(0, 160, 140, 100, 100);
    vision_tracker_update(&tr, d, 1);
    check("an unconfirmed track is not reacquired by distance", tr.count == 2);
    /* A new object after an expired track: two ids, the first gone. */
    vision_tracker_init(&tr);
    d[0] = det(0, 100, 100, 50, 50);
    vision_tracker_update(&tr, d, 1);
    vision_tracker_update(&tr, d, 1);
    for (i = 0; i <= VISION_TRACK_MAX_MISSES; i++) {
        vision_tracker_update(&tr, NULL, 0);
    }
    d[0] = det(0, 100, 100, 50, 50);
    vision_tracker_update(&tr, d, 1);
    check("an object after an expired track is id 2 alone", tr.count == 1 && tr.t[0].id == 2 && !find(&tr, 1));
}

/* Unit B, 2026-09-29: a walker's box (230 x 436) flipped for a frame to a box
 * pinned at the picture's edge (550 x 436, over the same ground) and back.
 * The flip box is another object, not the walker grown. */
static void test_size_gate(void)
{
    struct vision_tracker tr;
    struct vision_det d[2];
    int i;

    vision_tracker_init(&tr);
    for (i = 0; i < 4; i++) {
        d[0] = det(0, 340 - i * 20, 10, 230, 436);
        vision_tracker_update(&tr, d, 1);
    }
    check("the walker is one confirmed track", tr.count == 1 && tr.t[0].id == 1 && tr.t[0].confirmed);
    d[0] = det(0, 3, 10, 550, 436); /* the flip: 2.4 times the area, covering the walker's place */
    vision_tracker_update(&tr, d, 1);
    check("a box of 2.4 times the area is a new track; the walker coasts", tr.count == 2 && find(&tr, 1) &&
                                                                              find(&tr, 1)->misses == 1 && find(&tr, 2));
    d[0] = det(0, 240, 10, 230, 436); /* the walker's own box again */
    d[1] = det(0, 0, 10, 501, 433);   /* the merge, still there */
    vision_tracker_update(&tr, d, 2);
    check("the walker's own box comes back to the walker's id", tr.count == 2 && find(&tr, 1)->misses == 0 &&
                                                                    find(&tr, 1)->box.w == 230 && find(&tr, 2)->box.w == 501);
    /* A gradual change is followed: 30 % more area each frame. */
    vision_tracker_init(&tr);
    d[0] = det(0, 300, 100, 100, 100);
    vision_tracker_update(&tr, d, 1);
    for (i = 1; i <= 6; i++) {
        int s = 100 + i * 15;

        d[0] = det(0, 350 - s / 2, 150 - s / 2, s, s);
        vision_tracker_update(&tr, d, 1);
    }
    check("an object growing 30 % a frame keeps its id", tr.count == 1 && tr.t[0].id == 1 && tr.t[0].box.w == 190);
    /* After a dropout the allowance is three times. */
    for (i = 0; i < 5; i++) {
        vision_tracker_update(&tr, NULL, 0);
    }
    d[0] = det(0, 200, 0, 300, 300); /* 2.5 x the area, where it was */
    vision_tracker_update(&tr, d, 1);
    check("2.5 times the area after a dropout is still the same object", tr.count == 1 && tr.t[0].id == 1);
}

/* Unit B, 2026-09-29: a track expiring in the same update as another
 * one's first miss handed its box to that other track. */
static void test_expiry_neighbour(void)
{
    struct vision_tracker tr;
    struct vision_det d[2];
    int i;
    const struct vision_track *walker;

    vision_tracker_init(&tr);
    /* Track 1 stands at the picture's edge for two frames, then is gone
     * for good; track 2 is a walker seen throughout, 20 px a frame. */
    for (i = 0; i < 2; i++) {
        d[0] = det(0, 14, 6, 88, 443);
        d[1] = det(0, 300 + i * 20, 9, 200, 436);
        vision_tracker_update(&tr, d, 2);
    }
    for (i = 0; i < VISION_TRACK_MAX_MISSES; i++) {
        d[0] = det(0, 340 + i * 20, 9, 200, 436);
        vision_tracker_update(&tr, d, 1);
    }
    check("the standing track is on its last miss, the walker seen",
          tr.count == 2 && find(&tr, 1)->misses == VISION_TRACK_MAX_MISSES && find(&tr, 2)->misses == 0);
    /* The frame where the standing track expires and the walker is missed
     * once: the walker must coast from its own place. */
    vision_tracker_update(&tr, NULL, 0);
    walker = find(&tr, 2);
    check("the standing track expired, the walker remains", tr.count == 1 && walker && walker->misses == 1);
    check("and coasts on from its own box, not the expired one's",
          walker->box.x > 620 && walker->box.x < 720 && walker->box.w == 200);
}

static void test_classes(void)
{
    /* COCO's traffic classes: person 0, bicycle 1, car 2, motorcycle 3,
     * bus 5, truck 7. */
    static const uint8_t group[8] = { 0, 2, 1, 2, 0, 1, 0, 1 };
    struct vision_tracker tr;
    struct vision_det d[1];
    int i;

    vision_tracker_init(&tr);
    tr.group = group;
    tr.group_classes = 8;
    check("car and truck are one kind of object", vision_tracker_compatible(&tr, 2, 7) &&
                                                       vision_tracker_compatible(&tr, 7, 5));
    check("a person is only a person", !vision_tracker_compatible(&tr, 0, 2) && vision_tracker_compatible(&tr, 0, 0));
    check("a class outside the table matches only itself", !vision_tracker_compatible(&tr, 2, 40));
    d[0] = det(2, 100, 100, 80, 50);
    vision_tracker_update(&tr, d, 1);
    vision_tracker_update(&tr, d, 1);
    d[0] = det(7, 104, 100, 80, 50);
    vision_tracker_update(&tr, d, 1);
    check("a truck box continues the car's track, which stays a car",
          tr.count == 1 && tr.t[0].id == 1 && tr.t[0].cls == 2 && tr.t[0].cls_other_run == 1);
    d[0] = det(2, 108, 100, 80, 50);
    vision_tracker_update(&tr, d, 1);
    check("a car box again forgets the vote", tr.t[0].cls == 2 && tr.t[0].cls_other_run == 0);
    for (i = 0; i < VISION_TRACK_CLS_SWITCH; i++) {
        d[0] = det(7, 112 + i * 4, 100, 80, 50);
        vision_tracker_update(&tr, d, 1);
    }
    check("three trucks in a row and the track is a truck, the same id",
          tr.count == 1 && tr.t[0].id == 1 && tr.t[0].cls == 7);
    d[0] = det(0, 124, 100, 80, 50);
    vision_tracker_update(&tr, d, 1);
    check("a person on the same box is another track", tr.count == 2 && find(&tr, 2)->cls == 0);
    tr.group = NULL;
    d[0] = det(2, 128, 100, 80, 50);
    vision_tracker_update(&tr, d, 1);
    check("without a table a car is not a truck", tr.count == 3);
}

static void test_bounded(void)
{
    struct vision_tracker tr;
    struct vision_det d[VISION_MAX_DETECTIONS];
    int i;

    vision_tracker_init(&tr);
    for (i = 0; i < VISION_MAX_DETECTIONS; i++) {
        d[i] = det(0, i * 60, 0, 40, 40);
    }
    vision_tracker_update(&tr, d, VISION_MAX_DETECTIONS);
    check("a full frame fills the tracker", tr.count == VISION_MAX_TRACKS);
    for (i = 0; i < VISION_MAX_DETECTIONS; i++) {
        d[i] = det(0, i * 60, 500, 40, 40); /* all new places */
    }
    vision_tracker_update(&tr, d, VISION_MAX_DETECTIONS);
    check("a second full frame of new objects adds nothing: the list is bounded",
          tr.count == VISION_MAX_TRACKS);
    check("and the drops are counted", tr.dropped == VISION_MAX_DETECTIONS);
    check("more detections than the limit are cut at the limit",
          vision_tracker_update(&tr, d, 1000) == VISION_MAX_TRACKS);
}

static void test_sides(void)
{
    struct vision_line l = { 0, 100, 200, 100, true, 0 }; /* across, at y = 100 */
    struct vision_line v = { 100, 0, 100, 200, true, 0 }; /* down, at x = 100 */
    struct vision_line z = { 5, 5, 5, 5, true, 0 };
    struct vision_line capped = { 0, 100, 200, 100, true, 22 }; /* a 360 px frame's cap */
    struct vision_box small = { 0, 0, 8, 8 };
    struct vision_box person = { 0, 0, 60, 120 };
    struct vision_box huge = { 0, 0, 550, 349 }; /* a person close to the lens, on a 640 x 360 frame */

    check("above the line is side A", vision_line_side(&l, 50, 40, 4) == -1);
    check("below it is side B", vision_line_side(&l, 50, 160, 4) == 1);
    check("on it is neither", vision_line_side(&l, 50, 100, 4) == 0);
    check("nor within the dead band", vision_line_side(&l, 50, 103, 4) == 0 && vision_line_side(&l, 50, 97, 4) == 0);
    check("just outside the band is a side", vision_line_side(&l, 50, 105, 4) == 1);
    check("a vertical line: left is side B, right is side A",
          vision_line_side(&v, 40, 50, 4) == 1 && vision_line_side(&v, 160, 50, 4) == -1);
    check("a line with no length has no sides", vision_line_side(&z, 1, 1, 4) == 0);
    check("the dead band is a quarter of the box's smaller side, at least 4 px",
          vision_line_dead_px(&l, &small) == VISION_LINE_DEAD_PX && vision_line_dead_px(&l, &person) == 15);
    check("uncapped, a box that fills the frame gets a band nothing could cross", vision_line_dead_px(&l, &huge) == 87);
    check("the cap holds it to the frame's sixteenth; a small box is untouched",
          vision_line_dead_px(&capped, &huge) == 22 && vision_line_dead_px(&capped, &person) == 15);
}

/* Unit B, 2026-09-29: the owner walked left to right past the lens; the
 * detector's box grew to 550 of 640 frame pixels and its centre ended 38 px
 * past the line, where it sat. With the box-scaled band alone that was
 * never a crossing. */
static void test_huge_box(void)
{
    struct vision_tracker tr;
    struct vision_line down = { 320, 0, 320, 360, true, 0 };
    struct vision_counts c = { 0, 0 };
    struct vision_det d[1];
    int i;
    /* Centres 84 .. 358 as logged (view px scaled to the frame), the box
     * widening as it goes; 30 frames, then still. */
    static const int cx[] = { 84, 134, 142, 143, 153, 182, 182, 192, 212, 243, 262, 292, 302, 313, 313, 322,
                              333, 342, 350, 354, 358, 353, 355, 356, 356, 356, 356, 356, 356, 356 };
    static const int w[] = { 168, 267, 280, 287, 250, 210, 200, 210, 260, 320, 370, 410, 450, 470, 480, 470,
                             519, 520, 520, 520, 520, 550, 550, 550, 550, 550, 550, 550, 550, 550 };

    vision_tracker_init(&tr);
    for (i = 0; i < 30; i++) {
        d[0] = det(0, cx[i] - w[i] / 2, 5, w[i], 349);
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&down, 0, &tr, &c, NULL, 0);
    }
    check("without the cap the pass is lost (the finding)", c.ab == 0 && c.ba == 0 && tr.count == 1);
    down.dead_max = 360 / VISION_LINE_DEAD_DIV;
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 30; i++) {
        d[0] = det(0, cx[i] - w[i] / 2, 5, w[i], 349);
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&down, 0, &tr, &c, NULL, 0);
    }
    check("with the frame cap it counts once, to the right (B to A), on one id",
          c.ab == 0 && c.ba == 1 && tr.count == 1 && tr.t[0].id == 1);
    /* And a jitter of a capped box still counts nothing: the band is 22 px. */
    for (i = 0; i < 20; i++) {
        d[0] = det(0, 320 + (i % 2 ? 15 : -15) - 275, 5, 550, 349);
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&down, 0, &tr, &c, NULL, 0);
    }
    check("a 15 px jitter of the huge box stays inside the capped band", c.ab == 0 && c.ba == 1);
}

static void test_line(void)
{
    struct vision_tracker tr;
    struct vision_line l = { 0, 100, 200, 100, true, 0 }; /* across, at y = 100 */
    struct vision_counts c = { 0, 0 };
    struct vision_det d[3];
    struct vision_crossing x[4];
    int i;
    int n;

    /* One object walks down through the line: 40 x 40, so a 10 px band. */
    vision_tracker_init(&tr);
    for (i = 0; i < 12; i++) {
        d[0] = det(0, 80, i * 20, 40, 40); /* centre y = i * 20 + 20 */
        step(&tr, &l, &c, d, 1);
    }
    check("one crossing downward counts once as A to B", c.ab == 1 && c.ba == 0);
    /* Back up: counted the other way. */
    for (i = 11; i >= 0; i--) {
        d[0] = det(0, 80, i * 20, 40, 40);
        step(&tr, &l, &c, d, 1);
    }
    check("coming back counts once as B to A", c.ab == 1 && c.ba == 1);

    /* Up to the line and away again: a touch is not a crossing. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    {
        static const int ys[] = { 40, 60, 75, 80, 78, 75, 60, 40 }; /* centres: A, A, band, band, band, band, A, A */

        for (i = 0; i < 8; i++) {
            d[0] = det(0, 80, ys[i], 40, 40);
            step(&tr, &l, &c, d, 1);
        }
    }
    check("touching the line and turning back counts nothing", c.ab == 0 && c.ba == 0 && tr.t[0].ls[0].side == -1);

    /* A wobble on the line: never a count. A 40 x 60 box walking up to the
     * line in steps its overlap can follow, then dithering on it. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        d[0] = det(0, 80, 30 + i * 15, 40, 60); /* centres 60, 75, 90: side A, A, band */
        step(&tr, &l, &c, d, 1);
    }
    check("walking up to the line counts nothing", c.ab == 0 && c.ba == 0 && tr.t[0].ls[0].side == -1);
    for (i = 0; i < 20; i++) {
        d[0] = det(0, 80, 68 + (i % 2) * 4, 40, 60); /* centre 98 or 102: within the band */
        step(&tr, &l, &c, d, 1);
    }
    check("wobbling on the line counts nothing", c.ab == 0 && c.ba == 0 && tr.count == 1);
    d[0] = det(0, 80, 82, 40, 60); /* centre 112: side B, 12 px past a 10 px band - not far */
    step(&tr, &l, &c, d, 1);
    check("one sighting on the far side is not yet a crossing", c.ab == 0 && tr.t[0].ls[0].pending == 1);
    d[0] = det(0, 80, 84, 40, 60); /* centre 114 */
    n = vision_tracker_update(&tr, d, 1);
    n = vision_line_count(&l, 0, &tr, &c, x, 4);
    check("the second sighting settles it: one crossing, reported with its id and direction",
          n == 1 && c.ab == 1 && c.ba == 0 && x[0].id == 1 && x[0].cls == 0 && x[0].dir == 1 && x[0].index == 0);

    /* One sighting far beyond the band settles at once: the object that
     * leaves the frame right after crossing. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        d[0] = det(0, 80, 30 + i * 15, 40, 60); /* centres 60, 75, 90: A */
        step(&tr, &l, &c, d, 1);
    }
    d[0] = det(0, 80, 100, 40, 60); /* centre 130: 30 px past, twice the far mark */
    step(&tr, &l, &c, d, 1);
    check("a single sighting far past the line counts at once", c.ab == 1 && c.ba == 0);
    for (i = 0; i < 6; i++) {
        step(&tr, &l, &c, NULL, 0); /* then gone */
    }
    check("and the loss that follows adds nothing", c.ab == 1 && c.ba == 0);

    /* A jitter across the line, one frame each side: never a count. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 20; i++) {
        d[0] = det(0, 80, 48 + (i % 2) * 24, 40, 80); /* centres 88 and 112: A, B, A, B... (a 10 px band) */
        step(&tr, &l, &c, d, 1);
    }
    check("jumping across and back every frame counts nothing", c.ab == 0 && c.ba == 0 && tr.count == 1);
    for (i = 0; i < 3; i++) {
        d[0] = det(0, 80, 72, 40, 80); /* centre 112: B, and stays */
        step(&tr, &l, &c, d, 1);
    }
    check("staying on the far side then counts once", c.ab == 1 && c.ba == 0);

    /* An unconfirmed track (seen once) that crosses is not counted; a track
     * born on the far side is not counted either. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    d[0] = det(0, 80, 140, 40, 40);
    step(&tr, &l, &c, d, 1);
    d[0] = det(0, 80, 144, 40, 40);
    step(&tr, &l, &c, d, 1);
    check("a track born below the line counts nothing", c.ab == 0 && c.ba == 0);

    /* Two objects at once, one each way. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 12; i++) {
        d[0] = det(0, 20, i * 20, 40, 40);
        d[1] = det(0, 140, 220 - i * 20, 40, 40);
        step(&tr, &l, &c, d, 2);
    }
    check("two objects crossing opposite ways: one each", c.ab == 1 && c.ba == 1);
    /* Three at once the same way, at different speeds. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 16; i++) {
        d[0] = det(0, 0, i * 20, 40, 40);
        d[1] = det(2, 60, i * 15, 40, 40);
        d[2] = det(0, 140, i * 10, 40, 40);
        step(&tr, &l, &c, d, 3);
    }
    check("three objects down at once: three counts, three ids", c.ab == 3 && c.ba == 0 && tr.count == 3);

    /* A coasting (unseen) track never crosses by itself. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        d[0] = det(0, 80, 20 + i * 20, 40, 40); /* moving down 20 a frame, still above */
        step(&tr, &l, &c, d, 1);
    }
    for (i = 0; i < 6; i++) {
        step(&tr, &l, &c, NULL, 0); /* its prediction carries it past the line */
    }
    check("a prediction across the line is not a crossing", c.ab == 0 && c.ba == 0 && tr.t[0].ls[0].side == -1);
    /* ... and the object seen again beyond the line is the crossing, once,
     * on the same id: a dropout on the line loses nothing. */
    d[0] = det(0, 80, 160, 40, 40);
    step(&tr, &l, &c, d, 1);
    d[0] = det(0, 80, 170, 40, 40);
    step(&tr, &l, &c, d, 1);
    check("seen again past the line after the dropout: one crossing, the same id",
          c.ab == 1 && tr.count == 1 && tr.t[0].id == 1);

    /* An object that crosses, is lost for good, and a new one that appears
     * beyond the line: one count, not two. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 8; i++) {
        d[0] = det(0, 80, i * 20, 40, 40);
        step(&tr, &l, &c, d, 1);
    }
    for (i = 0; i <= VISION_TRACK_MAX_MISSES; i++) {
        step(&tr, &l, &c, NULL, 0);
    }
    for (i = 0; i < 4; i++) {
        d[0] = det(0, 80, 160 + i * 4, 40, 40);
        step(&tr, &l, &c, d, 1);
    }
    check("an expired track and a newcomer beyond the line: one count, two ids",
          c.ab == 1 && c.ba == 0 && tr.count == 1 && tr.t[0].id == 2);

    /* Forgetting: the line moved, every track learns its side afresh. */
    vision_line_forget(&tr, 0);
    check("a forgotten side is unknown", tr.t[0].ls[0].side == 0);
    d[0] = det(0, 80, 176, 40, 40);
    step(&tr, &l, &c, d, 1);
    check("and learnt again without a count", tr.t[0].ls[0].side == 1 && c.ab == 1);

    l.enabled = false;
    d[0] = det(0, 80, 20, 40, 40);
    vision_tracker_update(&tr, d, 1);
    check("a disabled line counts nothing", vision_line_count(&l, 0, &tr, &c, NULL, 0) == 0 && c.ab == 1);
    l.enabled = true;
    check("a line index out of range counts nothing", vision_line_count(&l, VISION_LINES, &tr, &c, NULL, 0) == 0);
}

static void test_directions(void)
{
    struct vision_tracker tr;
    struct vision_line across = { 0, 100, 200, 100, true, 0 };
    struct vision_line down = { 100, 0, 100, 200, true, 0 };
    struct vision_counts c;
    struct vision_det d[1];
    int i;

    /* Left to right across a vertical line: side B (left) to side A. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 12; i++) {
        d[0] = det(2, i * 20 - 20, 80, 40, 40);
        step(&tr, &down, &c, d, 1);
    }
    check("left to right is B to A on a line drawn downward", c.ab == 0 && c.ba == 1);
    /* Right to left. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 11; i >= 0; i--) {
        d[0] = det(2, i * 20 - 20, 80, 40, 40);
        step(&tr, &down, &c, d, 1);
    }
    check("right to left is A to B", c.ab == 1 && c.ba == 0);
    /* Up to down across a horizontal line. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 12; i++) {
        d[0] = det(0, 80, i * 20 - 20, 40, 40);
        step(&tr, &across, &c, d, 1);
    }
    check("top to bottom is A to B on a line drawn left to right", c.ab == 1 && c.ba == 0);
    /* Down to up. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 11; i >= 0; i--) {
        d[0] = det(0, 80, i * 20 - 20, 40, 40);
        step(&tr, &across, &c, d, 1);
    }
    check("bottom to top is B to A", c.ab == 0 && c.ba == 1);
    /* The two lines at once, on their own states: a diagonal walk crosses
     * both, each counted on its own. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    {
        struct vision_counts c2 = { 0, 0 };

        for (i = 0; i < 12; i++) {
            d[0] = det(0, i * 20 - 30, i * 20 - 30, 60, 60);
            vision_tracker_update(&tr, d, 1);
            vision_line_count(&across, 0, &tr, &c, NULL, 0);
            vision_line_count(&down, 1, &tr, &c2, NULL, 0);
        }
        check("two lines with their own track states count independently",
              c.ab == 1 && c.ba == 0 && c2.ab == 0 && c2.ba == 1);
    }
}

/* Tracking 2.0, each rule from unit B's KPU replay of a real street
 * (docs/apps/VISION.md, "Tracking"): a second box of a tracked object never
 * becomes a second track; two people close together still get one each; a
 * track is found again where it was last seen when its prediction drifted
 * on; and the longer it was hidden, the wider it is looked for. */
static void test_tracking2(void)
{
    struct vision_tracker tr;
    struct vision_det d[2];
    uint32_t id;
    int i;

    /* A partial box beside the whole one. */
    vision_tracker_init(&tr);
    for (i = 0; i < 3; i++) {
        d[0] = det(2, 100 + 2 * i, 100, 80, 40);
        vision_tracker_update(&tr, d, 1);
    }
    id = tr.t[0].id;
    d[0] = det(2, 106, 100, 80, 40);
    d[1] = det(2, 108, 100, 40, 40);
    vision_tracker_update(&tr, d, 2);
    check("a partial box inside a tracked car is no second track", tr.count == 1 && tr.t[0].id == id &&
                                                                       tr.dup_births == 1);
    d[1] = det(2, 106, 102, 80, 38);
    d[0] = det(2, 108, 100, 80, 40);
    vision_tracker_update(&tr, d, 2);
    check("nor a second box of it just under suppression's overlap", tr.count == 1 && tr.dup_births == 2);
    d[1] = det(0, 108, 100, 40, 40);
    vision_tracker_update(&tr, d, 2);
    check("a person on the car is another object, and gets a track", tr.count == 2 && tr.dup_births == 2);

    /* Two people side by side, overlapping under the duplicate's bound. */
    vision_tracker_init(&tr);
    d[0] = det(0, 100, 100, 50, 100);
    d[1] = det(0, 118, 100, 50, 100); /* IoU 0.47, 64 % inside */
    vision_tracker_update(&tr, d, 2);
    d[0] = det(0, 101, 100, 50, 100);
    d[1] = det(0, 119, 100, 50, 100);
    vision_tracker_update(&tr, d, 2);
    check("two people close together are two confirmed tracks", tr.count == 2 && tr.t[0].confirmed &&
                                                                    tr.t[1].confirmed && tr.dup_births == 0);

    /* Found where it was last seen: it stopped while hidden, and its
     * prediction drifted on (78 px after five frames at 20 px a frame,
     * beyond pass 2's reach). */
    vision_tracker_init(&tr);
    for (i = 0; i < 4; i++) {
        d[0] = det(2, 100 + 20 * i, 200, 40, 40);
        vision_tracker_update(&tr, d, 1);
    }
    id = tr.t[0].id;
    for (i = 0; i < 5; i++) {
        vision_tracker_update(&tr, d, 0);
    }
    d[0] = det(2, 160, 200, 40, 40);
    vision_tracker_update(&tr, d, 1);
    check("a car that stopped while hidden is found where it was last seen, same id",
          tr.count == 1 && tr.t[0].id == id && tr.t[0].seen && tr.revived == 1);

    /* Found by a radius that grew: it kept going while hidden eight frames,
     * further than its slowing prediction (34 px off, pass 2's first reach
     * is 30). */
    vision_tracker_init(&tr);
    for (i = 0; i < 6; i++) {
        d[0] = det(2, 100 + 10 * i, 200, 40, 40);
        vision_tracker_update(&tr, d, 1);
    }
    id = tr.t[0].id;
    for (i = 0; i < 8; i++) {
        vision_tracker_update(&tr, d, 0);
    }
    d[0] = det(2, 150 + 90, 200, 40, 40);
    vision_tracker_update(&tr, d, 1);
    check("a car hidden eight frames is found by a wider reach, same id", tr.count == 1 && tr.t[0].id == id &&
                                                                              tr.t[0].seen && tr.reacquired == 1);
    /* But not without bound: a track kept long (FAR keeps them longer)
     * reaches twice its start at most. Hidden 30 frames its prediction has
     * slowed to x 212 (the motion decays in integer steps); a car 78 px
     * beyond is further than 60 px (the cap) and nearer than an uncapped
     * 90. */
    vision_tracker_init(&tr);
    tr.max_misses = 40;
    for (i = 0; i < 6; i++) {
        d[0] = det(2, 100 + 10 * i, 200, 40, 40);
        vision_tracker_update(&tr, d, 1);
    }
    for (i = 0; i < 30; i++) {
        vision_tracker_update(&tr, d, 0);
    }
    d[0] = det(2, 290, 200, 40, 40);
    vision_tracker_update(&tr, d, 1);
    check("the reach is bounded: twice its start at most, a car beyond it is another", tr.count == 2 &&
                                                                                          tr.t[1].id != tr.t[0].id &&
                                                                                          tr.reacquired == 0);
}

int main(void)
{
    test_identity();
    test_expiry();
    test_dropout();
    test_size_gate();
    test_expiry_neighbour();
    test_classes();
    test_bounded();
    test_sides();
    test_huge_box();
    test_line();
    test_directions();
    test_tracking2();
    printf("vision_track_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
