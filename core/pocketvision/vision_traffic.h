/*
 * Traffic: what the count line and two speed lines say about the vehicles
 * and people that cross them (pocketvision.h, docs/apps/VISION.md).
 *
 * The detector's classes are mapped onto six traffic classes by name
 * (vision_traffic_class_of), so another detector with the same names
 * drops in; a class with no traffic name is not traffic and is filtered
 * out before tracking (vision_traffic_wanted).
 *
 * COUNTS. Every crossing of the count line (vision_line.h) is counted per
 * traffic class and in total, in the direction it went: A to B is IN, B to
 * A is OUT.
 *
 * SPEED. Two parallel lines, A and B, a known distance apart on the
 * ground. A track that crosses A and later B (or B and later A) in the
 * same direction has a speed: the distance over the time between the two
 * crossings. The time is the caller's clock, passed in; nothing here reads
 * one. A measurement is refused, and said so, when the track crosses the
 * same line again before the other (it turned round), when the two
 * crossings are in opposite directions, when the time between them is
 * under VISION_SPEED_MIN_MS (not a vehicle: two boxes swapped ids) or over
 * VISION_SPEED_MAX_MS (it stopped in between, or the id was replaced by a
 * new track's), or when the track is gone before the second line. A new
 * id starts from nothing: it can never inherit the first crossing of the
 * track it replaced. Pixel motion is never turned into a speed; only the
 * two timestamps and the configured distance are.
 *
 * km/h x10 = distance_cm * 360 / elapsed_ms. Integer throughout.
 *
 * The current speed is the newest measurement while its track is alive;
 * the last is kept until the next; the maximum and the mean (over a
 * saturating count) are there because they cost nothing.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_TRAFFIC_H
#define POCKETOS_VISION_TRAFFIC_H

#include "pocketvision_proto.h"
#include "vision_line.h"

enum vision_traffic_class {
    VISION_TRAFFIC_CAR = 0,
    VISION_TRAFFIC_TRUCK,
    VISION_TRAFFIC_BUS,
    VISION_TRAFFIC_MOTORCYCLE,
    VISION_TRAFFIC_BICYCLE,
    VISION_TRAFFIC_PERSON,
    VISION_TRAFFIC_CLASSES
};

/* The tracker's line-state slots used for the three lines. */
#define VISION_LINE_COUNT 0
#define VISION_LINE_SPEED_A 1
#define VISION_LINE_SPEED_B 2

/* The confidence (per-mille) a motor vehicle needs in TRAFFIC, below the
 * general threshold: a car on a road 35 m away is a few pixels of the
 * model's input (the 640 x 360 frame is letterboxed to half size) and
 * scores under 0.35. 0.25 is the detector's own default (Ultralytics'
 * predict conf). Such a detection only adds an object nothing stronger saw
 * (vision_nms_add_weak); people and bicycles keep the general threshold. */
#define VISION_TRAFFIC_VEHICLE_CONF_MIN 250

#define VISION_SPEED_MIN_MS 100
#define VISION_SPEED_MAX_MS 30000
#define VISION_SPEED_DISTANCE_MIN_CM 50
#define VISION_SPEED_DISTANCE_MAX_CM 100000
#define VISION_SPEED_KMH10_MAX 3000    /* 300 km/h: above it, not a measurement */
#define VISION_SPEED_MEAN_N_MAX 10000  /* the mean's count saturates here */

struct vision_speed_slot {
    uint32_t id;        /* the track; 0: free */
    int8_t first;       /* the line crossed first: 0 none, 1 A, 2 B */
    int8_t dir;         /* the direction of that crossing */
    int64_t t_first;    /* when */
    uint32_t kmh10;     /* the speed this track measured, 0 while none */
};

struct vision_traffic {
    int8_t map[VISION_MAX_CLASSES];    /* detector class -> traffic class, or -1 */
    uint8_t group[VISION_MAX_CLASSES]; /* for the tracker: 1 the motor vehicles, 2 the two-wheelers */
    uint32_t classes;                  /* detector classes mapped */
    uint32_t count_ab[VISION_TRAFFIC_CLASSES];
    uint32_t count_ba[VISION_TRAFFIC_CLASSES];
    uint32_t total_ab;
    uint32_t total_ba;
    uint32_t distance_cm;
    struct vision_speed_slot slot[VISION_MAX_TRACKS];
    uint32_t cur_kmh10;     /* the newest measurement while its track lives */
    uint32_t cur_id;
    uint32_t last_kmh10;
    uint32_t max_kmh10;
    uint64_t sum_kmh10;
    uint32_t n;             /* measurements in the mean, saturating */
    uint32_t measured;      /* measurements ever, for the record */
    uint32_t rejected;      /* refusals ever */
    bool changed;           /* something above changed since the caller last cleared it */
};

/* The traffic class a detector class name means, or -1. */
int vision_traffic_class_of(const char *name);
/* A short name for a traffic class: car truck bus moto bike person. */
const char *vision_traffic_name(int traffic_cls);

/* Set up with no classes mapped and the default distance. */
void vision_traffic_init(struct vision_traffic *tf);
/* Map detector class `cls` onto traffic class `traffic_cls` (-1 to
 * unmap). Returns 0, or -1 for a class out of range. */
int vision_traffic_map(struct vision_traffic *tf, uint32_t cls, int traffic_cls);
/* Map the detector's classes by their names. */
void vision_traffic_map_names(struct vision_traffic *tf, uint32_t classes,
                              const char *(*name)(uint32_t cls));
/* Whether a detection of this class is traffic. */
bool vision_traffic_wanted(const struct vision_traffic *tf, uint32_t cls);
/* The traffic class of a detector class, or -1. */
int vision_traffic_class(const struct vision_traffic *tf, uint32_t cls);
/* Whether a detector class is a motor vehicle (car, truck, bus,
 * motorcycle): what may pass at VISION_TRAFFIC_VEHICLE_CONF_MIN. */
bool vision_traffic_vehicle(const struct vision_traffic *tf, uint32_t cls);

/* Counts and speeds to zero; the slots forgotten. The map and the
 * distance stay. */
void vision_traffic_reset(struct vision_traffic *tf);
/* Set the distance between the speed lines; out of range is refused
 * (-1) and nothing changes. Measurements in progress are dropped. */
int vision_traffic_set_distance(struct vision_traffic *tf, uint32_t cm);

/* Crossings of the count line, as vision_line_count reported them. */
void vision_traffic_counted(struct vision_traffic *tf, const struct vision_crossing *x, int n);
/* Crossings of speed line A (line = 1) or B (line = 2) at now_ms. Returns
 * how many speeds were measured. */
int vision_traffic_crossed(struct vision_traffic *tf, int line, const struct vision_crossing *x,
                           int n, int64_t now_ms);
/* After every frame: slots of tracks that are gone are freed, a first
 * crossing older than VISION_SPEED_MAX_MS is dropped, and the current
 * speed is retired when its track is gone. */
void vision_traffic_settle(struct vision_traffic *tf, const struct vision_tracker *tr, int64_t now_ms);

/* The speed measured on this track, km/h x10, or 0. */
uint32_t vision_traffic_track_speed(const struct vision_traffic *tf, uint32_t id);
/* The mean, km/h x10, or 0 with no measurement. */
uint32_t vision_traffic_mean_kmh10(const struct vision_traffic *tf);

#endif
