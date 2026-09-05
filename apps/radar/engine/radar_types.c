/*
 * PocketRadar vocabulary. See radar_types.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_types.h"

#include <stdio.h>

/* Base lifetimes are quoted in seconds so the tuning stays readable when
 * RADAR_TICK_MS is the only thing that knows about milliseconds. */
static const struct {
    const char *name;
    uint16_t base_ttl;      /* ticks at level 0 */
    int32_t base_points;
    uint8_t target;
} classes[RADAR_CLASS_COUNT] = {
    { "NORMAL",     140,  100, 1 },   /* 7.0 s */
    { "FAST",        80,  150, 1 },   /* 4.0 s */
    { "DECOY",      120, -150, 0 },   /* 6.0 s */
    { "HIGH VALUE", 100,  300, 1 }    /* 5.0 s */
};

static const char *const contact_state_names[RADAR_CONTACT_STATE_COUNT] = {
    "NEW", "SELECTED", "ACQUIRED"
};

static const char *const run_state_names[RADAR_RUN_STATE_COUNT] = {
    "READY", "ACTIVE", "OVER"
};

static const char *const engage_names[RADAR_ENGAGE_COUNT] = {
    "INVALID", "HIT", "FOUL"
};

static const char *const event_names[RADAR_EVENT_COUNT] = {
    "NONE", "SPAWN", "ACQUIRED", "FADED", "HIT", "FOUL", "LEVEL", "OVER"
};

static int in_range(int value, int count)
{
    return value >= 0 && value < count;
}

const char *radar_class_name(enum radar_class cls)
{
    return in_range((int)cls, RADAR_CLASS_COUNT) ? classes[cls].name : "?";
}

uint16_t radar_class_base_ttl(enum radar_class cls)
{
    return in_range((int)cls, RADAR_CLASS_COUNT) ? classes[cls].base_ttl : 0;
}

int32_t radar_class_base_points(enum radar_class cls)
{
    return in_range((int)cls, RADAR_CLASS_COUNT) ? classes[cls].base_points : 0;
}

int radar_class_is_target(enum radar_class cls)
{
    return in_range((int)cls, RADAR_CLASS_COUNT) ? classes[cls].target : 0;
}

const char *radar_contact_state_name(enum radar_contact_state state)
{
    return in_range((int)state, RADAR_CONTACT_STATE_COUNT)
           ? contact_state_names[state] : "?";
}

const char *radar_run_state_name(enum radar_run_state state)
{
    return in_range((int)state, RADAR_RUN_STATE_COUNT) ? run_state_names[state] : "?";
}

const char *radar_engage_name(enum radar_engage result)
{
    return in_range((int)result, RADAR_ENGAGE_COUNT) ? engage_names[result] : "?";
}

const char *radar_event_name(enum radar_event_type type)
{
    return in_range((int)type, RADAR_EVENT_COUNT) ? event_names[type] : "?";
}

/* ---- geometry --------------------------------------------------------- */

int radar_bearing_wrap(int bearing)
{
    bearing %= RADAR_BEARING_MAX;
    if (bearing < 0) {
        bearing += RADAR_BEARING_MAX;
    }
    return bearing;
}

int radar_bearing_delta(int a, int b)
{
    int d = radar_bearing_wrap(a) - radar_bearing_wrap(b);

    if (d > RADAR_BEARING_MAX / 2) {
        d -= RADAR_BEARING_MAX;
    } else if (d <= -RADAR_BEARING_MAX / 2) {
        d += RADAR_BEARING_MAX;
    }
    return d;
}

int radar_bearing_name(int bearing, char *buf, size_t n)
{
    if (!buf || n == 0) {
        return -1;
    }
    if (n < RADAR_BEARING_NAME_MAX) {
        buf[0] = '?';
        buf[n > 1 ? 1 : 0] = '\0';
        return -1;
    }
    snprintf(buf, n, "%03d", radar_bearing_wrap(bearing) / 10);
    return 0;
}

static int32_t clamp_range(int range)
{
    if (range < 0) {
        return 0;
    }
    if (range > RADAR_RANGE_MAX) {
        return RADAR_RANGE_MAX;
    }
    return range;
}

int32_t radar_polar_dist2(int bearing_a, int range_a, int bearing_b, int range_b)
{
    int32_t ra = clamp_range(range_a);
    int32_t rb = clamp_range(range_b);
    int32_t dr = ra - rb;
    int32_t dd = radar_bearing_delta(bearing_a, bearing_b);
    int32_t arc;

    if (dd < 0) {
        dd = -dd;
    }
    /* The mean of the two radii is the right lever arm for a small angle,
     * and it keeps the metric symmetric in its arguments. */
    arc = ((ra + rb) / 2 * dd) / RADAR_DECIDEG_PER_RADIAN;
    return dr * dr + arc * arc;
}
