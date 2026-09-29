/*
 * Traffic. See vision_traffic.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_traffic.h"

#include <string.h>

#define VISION_SPEED_DISTANCE_DEFAULT_CM 1000

_Static_assert(VISION_TRAFFIC_CLASSES == VISION_PROTO_TRAFFIC_CLASSES,
               "the traffic line carries one count pair per traffic class");

static const char *const names[VISION_TRAFFIC_CLASSES] = {
    "car", "truck", "bus", "moto", "bike", "person",
};

int vision_traffic_class_of(const char *name)
{
    if (!name) {
        return -1;
    }
    if (strcmp(name, "car") == 0) {
        return VISION_TRAFFIC_CAR;
    }
    if (strcmp(name, "truck") == 0) {
        return VISION_TRAFFIC_TRUCK;
    }
    if (strcmp(name, "bus") == 0) {
        return VISION_TRAFFIC_BUS;
    }
    if (strcmp(name, "motorcycle") == 0 || strcmp(name, "motorbike") == 0) {
        return VISION_TRAFFIC_MOTORCYCLE;
    }
    if (strcmp(name, "bicycle") == 0) {
        return VISION_TRAFFIC_BICYCLE;
    }
    if (strcmp(name, "person") == 0) {
        return VISION_TRAFFIC_PERSON;
    }
    return -1;
}

const char *vision_traffic_name(int traffic_cls)
{
    return traffic_cls >= 0 && traffic_cls < VISION_TRAFFIC_CLASSES ? names[traffic_cls] : "?";
}

void vision_traffic_init(struct vision_traffic *tf)
{
    memset(tf, 0, sizeof(*tf));
    memset(tf->map, -1, sizeof(tf->map));
    tf->distance_cm = VISION_SPEED_DISTANCE_DEFAULT_CM;
}

static uint8_t group_of(int traffic_cls)
{
    switch (traffic_cls) {
    case VISION_TRAFFIC_CAR:
    case VISION_TRAFFIC_TRUCK:
    case VISION_TRAFFIC_BUS:
        return 1;
    case VISION_TRAFFIC_MOTORCYCLE:
    case VISION_TRAFFIC_BICYCLE:
        return 2;
    default:
        return 0;
    }
}

int vision_traffic_map(struct vision_traffic *tf, uint32_t cls, int traffic_cls)
{
    if (cls >= VISION_MAX_CLASSES || traffic_cls >= VISION_TRAFFIC_CLASSES) {
        return -1;
    }
    tf->map[cls] = (int8_t)(traffic_cls < 0 ? -1 : traffic_cls);
    tf->group[cls] = group_of(traffic_cls);
    if (cls >= tf->classes) {
        tf->classes = cls + 1;
    }
    return 0;
}

void vision_traffic_map_names(struct vision_traffic *tf, uint32_t classes,
                              const char *(*name)(uint32_t cls))
{
    uint32_t i;

    if (classes > VISION_MAX_CLASSES) {
        classes = VISION_MAX_CLASSES;
    }
    for (i = 0; i < classes; i++) {
        vision_traffic_map(tf, i, vision_traffic_class_of(name(i)));
    }
    tf->classes = classes;
}

bool vision_traffic_wanted(const struct vision_traffic *tf, uint32_t cls)
{
    return cls < VISION_MAX_CLASSES && tf->map[cls] >= 0;
}

int vision_traffic_class(const struct vision_traffic *tf, uint32_t cls)
{
    return cls < VISION_MAX_CLASSES ? tf->map[cls] : -1;
}

bool vision_traffic_vehicle(const struct vision_traffic *tf, uint32_t cls)
{
    switch (vision_traffic_class(tf, cls)) {
    case VISION_TRAFFIC_CAR:
    case VISION_TRAFFIC_TRUCK:
    case VISION_TRAFFIC_BUS:
    case VISION_TRAFFIC_MOTORCYCLE:
        return true;
    default:
        return false;
    }
}

void vision_traffic_reset(struct vision_traffic *tf)
{
    memset(tf->count_ab, 0, sizeof(tf->count_ab));
    memset(tf->count_ba, 0, sizeof(tf->count_ba));
    tf->total_ab = 0;
    tf->total_ba = 0;
    memset(tf->slot, 0, sizeof(tf->slot));
    tf->cur_kmh10 = 0;
    tf->cur_id = 0;
    tf->last_kmh10 = 0;
    tf->max_kmh10 = 0;
    tf->sum_kmh10 = 0;
    tf->n = 0;
    tf->changed = true;
}

int vision_traffic_set_distance(struct vision_traffic *tf, uint32_t cm)
{
    if (cm < VISION_SPEED_DISTANCE_MIN_CM || cm > VISION_SPEED_DISTANCE_MAX_CM) {
        return -1;
    }
    tf->distance_cm = cm;
    /* A crossing timed against the old distance would give a speed over
     * the wrong ground. */
    memset(tf->slot, 0, sizeof(tf->slot));
    tf->cur_kmh10 = 0;
    tf->cur_id = 0;
    tf->changed = true;
    return 0;
}

void vision_traffic_counted(struct vision_traffic *tf, const struct vision_crossing *x, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int tc = vision_traffic_class(tf, x[i].cls);

        if (x[i].dir > 0) {
            tf->total_ab++;
            if (tc >= 0) {
                tf->count_ab[tc]++;
            }
        } else {
            tf->total_ba++;
            if (tc >= 0) {
                tf->count_ba[tc]++;
            }
        }
        tf->changed = true;
    }
}

static struct vision_speed_slot *slot_of(struct vision_traffic *tf, uint32_t id)
{
    int i;
    struct vision_speed_slot *free_slot = NULL;

    for (i = 0; i < VISION_MAX_TRACKS; i++) {
        if (tf->slot[i].id == id) {
            return &tf->slot[i];
        }
        if (!free_slot && tf->slot[i].id == 0) {
            free_slot = &tf->slot[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->id = id;
    }
    return free_slot;
}

static void reject(struct vision_traffic *tf, struct vision_speed_slot *s)
{
    s->first = 0;
    s->dir = 0;
    s->t_first = 0;
    tf->rejected++;
}

int vision_traffic_crossed(struct vision_traffic *tf, int line, const struct vision_crossing *x,
                           int n, int64_t now_ms)
{
    return vision_traffic_crossed_speeds(tf, line, x, n, now_ms, NULL, 0);
}

int vision_traffic_crossed_speeds(struct vision_traffic *tf, int line, const struct vision_crossing *x, int n,
                                  int64_t now_ms, uint32_t *speeds, int max)
{
    int i;
    int measured = 0;

    if (line != 1 && line != 2) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        struct vision_speed_slot *s = slot_of(tf, x[i].id);
        int64_t elapsed;
        uint64_t kmh10;

        if (!s) {
            /* Every slot holds a live track: the tracker is full and so
             * is this. Counted as a refusal, never as a speed. */
            tf->rejected++;
            continue;
        }
        if (s->first == 0) {
            s->first = (int8_t)line;
            s->dir = x[i].dir;
            s->t_first = now_ms;
            continue;
        }
        if (s->first == line) {
            /* The same line again before the other: it turned round, or
             * jittered through. Start over from this crossing. */
            reject(tf, s);
            s->first = (int8_t)line;
            s->dir = x[i].dir;
            s->t_first = now_ms;
            continue;
        }
        elapsed = now_ms - s->t_first;
        if (s->dir != x[i].dir || elapsed < VISION_SPEED_MIN_MS || elapsed > VISION_SPEED_MAX_MS) {
            reject(tf, s);
            continue;
        }
        kmh10 = ((uint64_t)tf->distance_cm * 360u) / (uint64_t)elapsed;
        if (kmh10 == 0 || kmh10 > VISION_SPEED_KMH10_MAX) {
            reject(tf, s);
            continue;
        }
        s->first = 0;
        s->dir = 0;
        s->t_first = 0;
        s->kmh10 = (uint32_t)kmh10;
        tf->cur_kmh10 = (uint32_t)kmh10;
        tf->cur_id = x[i].id;
        tf->last_kmh10 = (uint32_t)kmh10;
        if (kmh10 > tf->max_kmh10) {
            tf->max_kmh10 = (uint32_t)kmh10;
        }
        if (tf->n < VISION_SPEED_MEAN_N_MAX) {
            tf->sum_kmh10 += kmh10;
            tf->n++;
        }
        tf->measured++;
        tf->changed = true;
        if (speeds && measured < max) {
            speeds[measured] = (uint32_t)kmh10;
        }
        measured++;
    }
    return measured;
}

static bool alive(const struct vision_tracker *tr, uint32_t id)
{
    int i;

    for (i = 0; i < tr->count; i++) {
        if (tr->t[i].id == id) {
            return true;
        }
    }
    return false;
}

void vision_traffic_settle(struct vision_traffic *tf, const struct vision_tracker *tr, int64_t now_ms)
{
    int i;

    for (i = 0; i < VISION_MAX_TRACKS; i++) {
        struct vision_speed_slot *s = &tf->slot[i];

        if (s->id == 0) {
            continue;
        }
        if (!alive(tr, s->id)) {
            /* Gone before the second line: no measurement. A track that
             * measured one just leaves. */
            if (s->first != 0) {
                tf->rejected++;
            }
            memset(s, 0, sizeof(*s));
            continue;
        }
        if (s->first != 0 && now_ms - s->t_first > VISION_SPEED_MAX_MS) {
            reject(tf, s);
        }
    }
    if (tf->cur_id != 0 && !alive(tr, tf->cur_id)) {
        tf->cur_id = 0;
        tf->cur_kmh10 = 0;
        tf->changed = true;
    }
}

uint32_t vision_traffic_track_speed(const struct vision_traffic *tf, uint32_t id)
{
    int i;

    if (id == 0) {
        return 0;
    }
    for (i = 0; i < VISION_MAX_TRACKS; i++) {
        if (tf->slot[i].id == id) {
            return tf->slot[i].kmh10;
        }
    }
    return 0;
}

uint32_t vision_traffic_mean_kmh10(const struct vision_traffic *tf)
{
    return tf->n ? (uint32_t)(tf->sum_kmh10 / tf->n) : 0;
}
