/*
 * Tracks and the counting line: an object seen frame after frame keeps its
 * id; one that moves is followed by its motion; a missed frame does not
 * lose it and enough missed frames do; ids are never reused; the list is
 * bounded; a track counts once when it crosses the line, in the direction
 * it went, never on a wobble, never while unconfirmed, and again only when
 * it really comes back.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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
          tr.count == 1 && tr.t[0].id == 1 && tr.t[0].hits == 10 && tr.t[0].vx == 10);
    /* A jump larger than the overlap allows is a new object. */
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

static void test_line(void)
{
    struct vision_tracker tr;
    struct vision_line l = { 0, 100, 200, 100, true }; /* across, at y = 100 */
    struct vision_counts c = { 0, 0 };
    struct vision_det d[2];
    int i;

    check("above the line is side A", vision_line_side(&l, 50, 40, 4) == -1);
    check("below it is side B", vision_line_side(&l, 50, 160, 4) == 1);
    check("on it is neither", vision_line_side(&l, 50, 100, 4) == 0);
    check("nor within the dead band", vision_line_side(&l, 50, 103, 4) == 0 && vision_line_side(&l, 50, 97, 4) == 0);
    check("just outside the band is a side", vision_line_side(&l, 50, 105, 4) == 1);
    {
        struct vision_line v = { 100, 0, 100, 200, true }; /* down, at x = 100 */
        struct vision_line z = { 5, 5, 5, 5, true };

        check("a vertical line: left is side B, right is side A",
              vision_line_side(&v, 40, 50, 4) == 1 && vision_line_side(&v, 160, 50, 4) == -1);
        check("a line with no length has no sides", vision_line_side(&z, 1, 1, 4) == 0);
    }

    /* One object walks down through the line. */
    vision_tracker_init(&tr);
    for (i = 0; i < 12; i++) {
        d[0] = det(0, 80, i * 20, 40, 40); /* centre y = i * 20 + 20 */
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&l, &tr, &c);
    }
    check("one crossing downward counts once as A to B", c.ab == 1 && c.ba == 0);
    /* Back up: counted the other way. */
    for (i = 11; i >= 0; i--) {
        d[0] = det(0, 80, i * 20, 40, 40);
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&l, &tr, &c);
    }
    check("coming back counts once as B to A", c.ab == 1 && c.ba == 1);

    /* A wobble on the line: never a count. A 40 x 60 box walking up to the
     * line in steps its overlap can follow, then dithering on it. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        d[0] = det(0, 80, 30 + i * 15, 40, 60); /* centres 60, 75, 90: side A */
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&l, &tr, &c);
    }
    check("walking up to the line counts nothing", c.ab == 0 && c.ba == 0 && tr.t[0].side == -1);
    for (i = 0; i < 20; i++) {
        d[0] = det(0, 80, 68 + (i % 2) * 4, 40, 60); /* centre 98 or 102: within the band */
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&l, &tr, &c);
    }
    check("wobbling on the line counts nothing", c.ab == 0 && c.ba == 0 && tr.count == 1);
    d[0] = det(0, 80, 100, 40, 60); /* centre 130: side B */
    vision_tracker_update(&tr, d, 1);
    vision_line_count(&l, &tr, &c);
    check("committing to the other side counts once", c.ab == 1 && c.ba == 0 && tr.t[0].id == 1);

    /* An unconfirmed track (seen once) that crosses is not counted; a track
     * born on the far side is not counted either. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    d[0] = det(0, 80, 140, 40, 40);
    vision_tracker_update(&tr, d, 1);
    vision_line_count(&l, &tr, &c);
    d[0] = det(0, 80, 144, 40, 40);
    vision_tracker_update(&tr, d, 1);
    vision_line_count(&l, &tr, &c);
    check("a track born below the line counts nothing", c.ab == 0 && c.ba == 0);
    /* Two objects at once, one each way. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 12; i++) {
        d[0] = det(0, 20, i * 20, 40, 40);
        d[1] = det(0, 140, 220 - i * 20, 40, 40);
        vision_tracker_update(&tr, d, 2);
        vision_line_count(&l, &tr, &c);
    }
    check("two objects crossing opposite ways: one each", c.ab == 1 && c.ba == 1);
    /* A coasting (unseen) track never crosses by itself. */
    vision_tracker_init(&tr);
    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        d[0] = det(0, 80, 20 + i * 20, 40, 40); /* moving down 20 a frame, still above */
        vision_tracker_update(&tr, d, 1);
        vision_line_count(&l, &tr, &c);
    }
    for (i = 0; i < 6; i++) {
        vision_tracker_update(&tr, NULL, 0); /* its prediction carries it past the line */
        vision_line_count(&l, &tr, &c);
    }
    check("a prediction across the line is not a crossing", c.ab == 0 && c.ba == 0);
    l.enabled = false;
    d[0] = det(0, 80, 160, 40, 40);
    vision_tracker_update(&tr, d, 1);
    check("a disabled line counts nothing", vision_line_count(&l, &tr, &c) == 0 && c.ab == 0);
}

int main(void)
{
    test_identity();
    test_expiry();
    test_bounded();
    test_line();
    printf("vision_track_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
