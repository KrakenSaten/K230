/*
 * Traffic: the detector's classes mapped by name and everything else
 * filtered out; every crossing of the count line counted per class and in
 * total, IN and OUT; a speed from the time between the two lines and the
 * ground distance, either way round, and refused for the wrong order, a
 * turn between the lines, a stale first crossing, a replaced id, a time
 * too short, one line only, or an impossible result; the current speed
 * retired with its track; last, max and mean; reset; the distance; many
 * tracks at once. The clock is fed, never read.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketvision/vision_traffic.h"

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

/* A few of COCO's names, the way the vendor model lists them. */
static const char *coco(uint32_t cls)
{
    switch (cls) {
    case 0: return "person";
    case 1: return "bicycle";
    case 2: return "car";
    case 3: return "motorcycle";
    case 5: return "bus";
    case 7: return "truck";
    case 56: return "chair";
    default: return "thing";
    }
}

static struct vision_crossing crossing(uint32_t id, uint16_t cls, int dir)
{
    struct vision_crossing x = { id, cls, (int8_t)dir, 0 };

    return x;
}

/* A tracker holding these ids, confirmed, so the slots stay alive. */
static void hold(struct vision_tracker *tr, const uint32_t *ids, int n)
{
    int i;

    vision_tracker_init(tr);
    for (i = 0; i < n; i++) {
        memset(&tr->t[i], 0, sizeof(tr->t[i]));
        tr->t[i].id = ids[i];
        tr->t[i].confirmed = true;
    }
    tr->count = n;
}

static void test_classes(void)
{
    struct vision_traffic tf;

    check("names map to the six traffic classes",
          vision_traffic_class_of("car") == VISION_TRAFFIC_CAR && vision_traffic_class_of("truck") == VISION_TRAFFIC_TRUCK &&
              vision_traffic_class_of("bus") == VISION_TRAFFIC_BUS &&
              vision_traffic_class_of("motorcycle") == VISION_TRAFFIC_MOTORCYCLE &&
              vision_traffic_class_of("bicycle") == VISION_TRAFFIC_BICYCLE &&
              vision_traffic_class_of("person") == VISION_TRAFFIC_PERSON);
    check("anything else is not traffic", vision_traffic_class_of("chair") == -1 && vision_traffic_class_of(NULL) == -1);
    check("the short names", strcmp(vision_traffic_name(VISION_TRAFFIC_MOTORCYCLE), "moto") == 0 &&
                                 strcmp(vision_traffic_name(VISION_TRAFFIC_PERSON), "person") == 0 &&
                                 strcmp(vision_traffic_name(9), "?") == 0);
    vision_traffic_init(&tf);
    check("nothing is wanted before a map", !vision_traffic_wanted(&tf, 2) && tf.distance_cm == 1000);
    vision_traffic_map_names(&tf, 80, coco);
    check("the detector's classes are mapped by name",
          vision_traffic_wanted(&tf, 2) && vision_traffic_class(&tf, 7) == VISION_TRAFFIC_TRUCK &&
              vision_traffic_class(&tf, 0) == VISION_TRAFFIC_PERSON && !vision_traffic_wanted(&tf, 56) &&
              !vision_traffic_wanted(&tf, 79) && tf.classes == 80);
    check("out of range is not wanted", !vision_traffic_wanted(&tf, 1000) && vision_traffic_class(&tf, 1000) == -1);
    check("motor vehicles are one group to the tracker, two-wheelers another, people neither",
          tf.group[2] == 1 && tf.group[7] == 1 && tf.group[5] == 1 && tf.group[1] == 2 && tf.group[3] == 2 &&
              tf.group[0] == 0 && tf.group[56] == 0);
    check("the motor vehicles may pass at TRAFFIC's lower floor; bicycles, people and the rest may not",
          vision_traffic_vehicle(&tf, 2) && vision_traffic_vehicle(&tf, 7) && vision_traffic_vehicle(&tf, 5) &&
              vision_traffic_vehicle(&tf, 3) && !vision_traffic_vehicle(&tf, 1) && !vision_traffic_vehicle(&tf, 0) &&
              !vision_traffic_vehicle(&tf, 56) && !vision_traffic_vehicle(&tf, 1000));
    check("and that floor is below the general one", VISION_TRAFFIC_VEHICLE_CONF_MIN == 250);
    check("a class out of range cannot be mapped", vision_traffic_map(&tf, VISION_MAX_CLASSES, 0) == -1 &&
                                                       vision_traffic_map(&tf, 2, VISION_TRAFFIC_CLASSES) == -1);
    check("a class can be unmapped", vision_traffic_map(&tf, 2, -1) == 0 && !vision_traffic_wanted(&tf, 2));
}

static void test_counts(void)
{
    struct vision_traffic tf;
    struct vision_crossing x[4];

    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, 80, coco);
    x[0] = crossing(1, 2, 1);  /* a car IN */
    x[1] = crossing(2, 7, 1);  /* a truck IN */
    x[2] = crossing(3, 0, -1); /* a person OUT */
    x[3] = crossing(4, 2, -1); /* a car OUT */
    vision_traffic_counted(&tf, x, 4);
    check("crossings are counted per class and in total, by direction",
          tf.total_ab == 2 && tf.total_ba == 2 && tf.count_ab[VISION_TRAFFIC_CAR] == 1 &&
              tf.count_ba[VISION_TRAFFIC_CAR] == 1 && tf.count_ab[VISION_TRAFFIC_TRUCK] == 1 &&
              tf.count_ba[VISION_TRAFFIC_PERSON] == 1 && tf.count_ab[VISION_TRAFFIC_BUS] == 0 && tf.changed);
    x[0] = crossing(5, 56, 1); /* a chair, somehow */
    vision_traffic_counted(&tf, x, 1);
    check("a crossing of no traffic class is in the total only", tf.total_ab == 3 &&
                                                                    tf.count_ab[VISION_TRAFFIC_CAR] == 1);
    vision_traffic_reset(&tf);
    check("reset zeroes every count", tf.total_ab == 0 && tf.total_ba == 0 && tf.count_ab[VISION_TRAFFIC_CAR] == 0 &&
                                          tf.count_ba[VISION_TRAFFIC_PERSON] == 0);
    check("but keeps the map and the distance", vision_traffic_wanted(&tf, 2) && tf.distance_cm == 1000);
}

static void test_speed(void)
{
    struct vision_traffic tf;
    struct vision_tracker tr;
    struct vision_crossing x[2];
    uint32_t ids[4] = { 1, 2, 3, 4 };

    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, 80, coco);
    hold(&tr, ids, 1);
    /* 10 m between the lines, 1 s apart: 36 km/h. */
    x[0] = crossing(1, 2, 1);
    check("the first line alone measures nothing", vision_traffic_crossed(&tf, 1, x, 1, 1000) == 0 &&
                                                       tf.cur_kmh10 == 0 && tf.n == 0);
    vision_traffic_settle(&tf, &tr, 1500);
    check("nor does time alone", tf.n == 0 && tf.rejected == 0);
    check("then the second: 36.0 km/h, the current speed, on that track",
          vision_traffic_crossed(&tf, 2, x, 1, 2000) == 1 && tf.cur_kmh10 == 360 && tf.cur_id == 1 &&
              tf.last_kmh10 == 360 && tf.max_kmh10 == 360 && tf.n == 1 && vision_traffic_mean_kmh10(&tf) == 360 &&
              vision_traffic_track_speed(&tf, 1) == 360 && tf.measured == 1 && tf.rejected == 0 && tf.changed);
    /* The other way round, B then A, the same direction on both. */
    hold(&tr, ids + 1, 1);
    x[0] = crossing(2, 7, -1);
    vision_traffic_crossed(&tf, 2, x, 1, 5000);
    check("B then A works too: 20 m/s is 72.0 km/h", vision_traffic_crossed(&tf, 1, x, 1, 5500) == 1 &&
                                                          tf.cur_kmh10 == 720 && tf.cur_id == 2 && tf.last_kmh10 == 720 &&
                                                          tf.max_kmh10 == 720 && tf.n == 2 &&
                                                          vision_traffic_mean_kmh10(&tf) == 540);
    check("the earlier track keeps its own speed", vision_traffic_track_speed(&tf, 1) == 360 ||
                                                       vision_traffic_track_speed(&tf, 1) == 0);
    vision_traffic_settle(&tf, &tr, 5600);
    check("a measured track that is gone leaves its speed behind", vision_traffic_track_speed(&tf, 1) == 0 &&
                                                                       vision_traffic_track_speed(&tf, 2) == 720);
    hold(&tr, ids + 2, 1);
    vision_traffic_settle(&tf, &tr, 5700);
    check("the current speed retires with its track; the last stays", tf.cur_kmh10 == 0 && tf.cur_id == 0 &&
                                                                          tf.last_kmh10 == 720 && tf.max_kmh10 == 720);

    /* Refusals. */
    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, 80, coco);
    hold(&tr, ids, 4);
    x[0] = crossing(1, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 1000);
    vision_traffic_crossed(&tf, 1, x, 1, 1400);
    check("the same line twice is a turn: refused, the second crossing starts over",
          tf.n == 0 && tf.rejected == 1 && tf.slot[0].first == 1 && tf.slot[0].t_first == 1400);
    vision_traffic_crossed(&tf, 2, x, 1, 2400);
    check("and the measurement then runs from the second crossing", tf.n == 1 && tf.cur_kmh10 == 360);
    x[0] = crossing(2, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 3000);
    x[0] = crossing(2, 2, -1);
    vision_traffic_crossed(&tf, 2, x, 1, 4000);
    check("opposite directions on the two lines: refused", tf.n == 1 && tf.rejected == 2 &&
                                                              vision_traffic_track_speed(&tf, 2) == 0);
    x[0] = crossing(3, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 5000);
    vision_traffic_crossed(&tf, 2, x, 1, 5000 + VISION_SPEED_MIN_MS - 1);
    check("too short a time (two boxes that swapped ids): refused", tf.n == 1 && tf.rejected == 3);
    x[0] = crossing(4, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 6000);
    vision_traffic_settle(&tf, &tr, 6000 + VISION_SPEED_MAX_MS + 1);
    check("a first crossing gone stale is dropped", tf.rejected == 4 && tf.slot[3].first == 0);
    vision_traffic_crossed(&tf, 2, x, 1, 6000 + VISION_SPEED_MAX_MS + 500);
    check("so the second line then starts a new measurement, not a speed",
          tf.n == 1 && tf.slot[3].first == 2 && tf.rejected == 4);
    /* A track that leaves between the lines. */
    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, 80, coco);
    hold(&tr, ids, 1);
    x[0] = crossing(1, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 1000);
    hold(&tr, ids + 1, 1); /* id 1 gone, id 2 in its place */
    vision_traffic_settle(&tf, &tr, 1200);
    check("a track gone before the second line: refused, its slot freed", tf.rejected == 1 && tf.slot[0].id == 0);
    x[0] = crossing(2, 2, 1);
    vision_traffic_crossed(&tf, 2, x, 1, 2000);
    check("the id that replaced it cannot inherit the first crossing", tf.n == 0 && tf.slot[0].id == 2 &&
                                                                          tf.slot[0].first == 2);
    /* An impossible result. */
    vision_traffic_init(&tf);
    hold(&tr, ids, 1);
    vision_traffic_set_distance(&tf, 100000); /* 1 km */
    x[0] = crossing(1, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 1000);
    vision_traffic_crossed(&tf, 2, x, 1, 1200); /* 1 km in 200 ms */
    check("above 300 km/h is not a measurement", tf.n == 0 && tf.rejected == 1);
    check("a line that is neither A nor B measures nothing", vision_traffic_crossed(&tf, 3, x, 1, 1300) == 0);
    /* The distance. */
    check("the distance has bounds", vision_traffic_set_distance(&tf, 10) == -1 &&
                                         vision_traffic_set_distance(&tf, 200000) == -1 && tf.distance_cm == 100000);
    vision_traffic_crossed(&tf, 1, x, 1, 2000);
    check("changing it drops a measurement in progress", vision_traffic_set_distance(&tf, 500) == 0 &&
                                                            tf.slot[0].first == 0 && tf.changed);
    vision_traffic_crossed(&tf, 1, x, 1, 3000);
    vision_traffic_crossed(&tf, 2, x, 1, 3500);
    check("5 m in half a second is 36.0 km/h", tf.cur_kmh10 == 360);
    /* Reset. */
    vision_traffic_reset(&tf);
    check("reset forgets every speed and slot", tf.cur_kmh10 == 0 && tf.last_kmh10 == 0 && tf.max_kmh10 == 0 &&
                                                    tf.n == 0 && tf.slot[0].id == 0 && vision_traffic_mean_kmh10(&tf) == 0);
}

static void test_many(void)
{
    struct vision_traffic tf;
    struct vision_tracker tr;
    struct vision_crossing x[VISION_MAX_TRACKS];
    uint32_t ids[VISION_MAX_TRACKS];
    int i;
    int ok = 1;

    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, 80, coco);
    for (i = 0; i < VISION_MAX_TRACKS; i++) {
        ids[i] = (uint32_t)(i + 1);
        x[i] = crossing(ids[i], 2, 1);
    }
    hold(&tr, ids, VISION_MAX_TRACKS);
    vision_traffic_crossed(&tf, 1, x, VISION_MAX_TRACKS, 1000);
    check("every track has a slot", vision_traffic_crossed(&tf, 2, x, VISION_MAX_TRACKS, 2000) == VISION_MAX_TRACKS &&
                                        tf.n == VISION_MAX_TRACKS && tf.rejected == 0);
    for (i = 0; i < VISION_MAX_TRACKS; i++) {
        ok &= vision_traffic_track_speed(&tf, ids[i]) == 360;
    }
    check("each with its own speed", ok);
    x[0] = crossing(1000, 2, 1);
    vision_traffic_crossed(&tf, 1, x, 1, 3000);
    check("a track beyond the slots is refused, never a speed", tf.rejected == 1 && vision_traffic_track_speed(&tf, 1000) == 0);
    hold(&tr, ids, 0);
    vision_traffic_settle(&tf, &tr, 4000);
    check("gone, they free their slots", tf.slot[0].id == 0 && tf.slot[VISION_MAX_TRACKS - 1].id == 0 && tf.cur_kmh10 == 0);
    /* The mean's count saturates. */
    hold(&tr, ids, 1);
    for (i = 0; i < VISION_SPEED_MEAN_N_MAX + 5; i++) {
        x[0] = crossing(1, 2, 1);
        vision_traffic_crossed(&tf, 1, x, 1, 10000 + i * 2000);
        vision_traffic_crossed(&tf, 2, x, 1, 11000 + i * 2000);
    }
    check("the mean's count saturates", tf.n == VISION_SPEED_MEAN_N_MAX && tf.measured == VISION_MAX_TRACKS + VISION_SPEED_MEAN_N_MAX + 5 &&
                                            vision_traffic_mean_kmh10(&tf) == 360);
}

int main(void)
{
    test_classes();
    test_counts();
    test_speed();
    test_many();
    printf("vision_traffic_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
