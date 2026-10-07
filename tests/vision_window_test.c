/*
 * Traffic's recent statistics (core/pocketvision/vision_window.c): crossings
 * by direction and class, speeds and their mean, the window's edge to the
 * millisecond, a ring that fills and says so, and nothing that grows.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pocketvision/vision_labels.h"
#include "pocketvision/vision_window.h"

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

static struct vision_crossing cross(uint16_t cls, int8_t dir)
{
    struct vision_crossing x;

    memset(&x, 0, sizeof(x));
    x.id = 1;
    x.cls = cls;
    x.dir = dir;
    return x;
}

int main(void)
{
    static struct vision_window w;
    struct vision_traffic tf;
    struct vision_window_summary s;
    struct vision_crossing x[3];
    int i;

    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, VISION_COCO_CLASSES, vision_label);
    vision_window_init(&w);
    vision_window_summary(&w, 0, &s);
    check("empty: nothing, five minutes, whole", s.crossed == 0 && s.speeds == 0 && s.mean_kmh10 == 0 &&
                                                   s.window_s == 300 && !s.saturated);

    /* COCO: 0 person, 2 car, 7 truck, 56 chair. */
    x[0] = cross(2, 1);
    x[1] = cross(2, -1);
    x[2] = cross(7, 1);
    vision_window_crossed(&w, &tf, x, 3, 1000);
    x[0] = cross(0, -1);
    x[1] = cross(56, 1);
    vision_window_crossed(&w, &tf, x, 2, 2000);
    vision_window_speed(&w, 400, 2500);
    vision_window_speed(&w, 501, 2600);
    vision_window_speed(&w, 0, 2700);
    vision_window_summary(&w, 3000, &s);
    check("five crossings, three IN and two OUT", s.crossed == 5 && s.ab == 3 && s.ba == 2);
    check("by class: two cars, a truck, a person; a chair counted in the total, in no class",
          s.cls[VISION_TRAFFIC_CAR] == 2 && s.cls[VISION_TRAFFIC_TRUCK] == 1 && s.cls[VISION_TRAFFIC_PERSON] == 1 &&
              s.cls[VISION_TRAFFIC_BUS] == 0);
    check("two speeds, their mean; a zero speed is no measurement", s.speeds == 2 && s.mean_kmh10 == 450);

    check("at exactly five minutes after the first events they still count",
          !vision_window_summary(&w, 1000 + VISION_WINDOW_MS, &s) && s.crossed == 5);
    check("a millisecond later the first three go, and the summary says it changed",
          vision_window_summary(&w, 1001 + VISION_WINDOW_MS, &s) && s.crossed == 2 && s.ab == 1 && s.ba == 1 &&
              s.cls[VISION_TRAFFIC_CAR] == 0);
    check("then the rest", vision_window_summary(&w, 2601 + VISION_WINDOW_MS, &s) && s.crossed == 0 && s.speeds == 0 &&
                               s.mean_kmh10 == 0);
    check("an empty window stays unchanged", !vision_window_summary(&w, 999999, &s));

    /* A ring that fills inside the window. */
    vision_window_init(&w);
    x[0] = cross(2, 1);
    for (i = 0; i < VISION_WINDOW_EVENTS; i++) {
        vision_window_crossed(&w, &tf, x, 1, 10000 + i);
    }
    vision_window_summary(&w, 20000, &s);
    check("a full ring is still whole", s.crossed == VISION_WINDOW_EVENTS && !s.saturated);
    vision_window_crossed(&w, &tf, x, 1, 20000);
    vision_window_summary(&w, 20001, &s);
    check("one more: the oldest goes, the count holds at the ring, and it says it is short",
          s.crossed == VISION_WINDOW_EVENTS && s.saturated);
    check("still short while what went would have been inside the window",
          !vision_window_summary(&w, 10000 + VISION_WINDOW_MS, &s) && s.saturated);
    vision_window_summary(&w, 10001 + VISION_WINDOW_MS, &s);
    check("whole again once it would have aged out anyway (nothing else has yet)", !s.saturated && s.crossed == VISION_WINDOW_EVENTS);
    check("the ring is the bound, whatever the traffic", sizeof(w.ev) / sizeof(w.ev[0]) == VISION_WINDOW_EVENTS);
    printf("vision_window_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
