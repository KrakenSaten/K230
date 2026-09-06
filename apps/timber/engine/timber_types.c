/*
 * PocketTimber vocabulary. See timber_types.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_types.h"

static const char *const class_names[TIMBER_CLASS_COUNT] = {
    "FREE", "EASY", "FIRM", "STUCK"
};

static const char *const run_state_names[TIMBER_RUN_STATE_COUNT] = {
    "READY", "ACTIVE", "COLLAPSING", "OVER"
};

static const char *const turn_names[TIMBER_TURN_COUNT] = {
    "SELECT", "PULLING", "PLACING"
};

static const char *const cause_names[TIMBER_CAUSE_COUNT] = {
    "NONE", "TIP", "JOLT", "PLACEMENT", "SWAY"
};

static const char *const event_names[TIMBER_EVENT_COUNT] = {
    "NONE", "SELECT", "DESELECT", "TEST", "STICK", "JOLT", "SHIFT", "SLIP",
    "PLACE", "LAYER", "CREAK", "COLLAPSE", "LAND", "OVER"
};

const char *timber_class_name(enum timber_class cls)
{
    return (unsigned)cls < TIMBER_CLASS_COUNT ? class_names[cls] : "?";
}

const char *timber_run_state_name(enum timber_run_state state)
{
    return (unsigned)state < TIMBER_RUN_STATE_COUNT ? run_state_names[state] : "?";
}

const char *timber_turn_name(enum timber_turn turn)
{
    return (unsigned)turn < TIMBER_TURN_COUNT ? turn_names[turn] : "?";
}

const char *timber_cause_name(enum timber_cause cause)
{
    return (unsigned)cause < TIMBER_CAUSE_COUNT ? cause_names[cause] : "?";
}

const char *timber_event_name(enum timber_event_type type)
{
    return (unsigned)type < TIMBER_EVENT_COUNT ? event_names[type] : "?";
}

/* ---- geometry --------------------------------------------------------- */

int timber_layer_axis(int layer)
{
    return (layer & 1) ? TIMBER_AXIS_Y : TIMBER_AXIS_X;
}

int timber_block_rect(int layer, int slot, int32_t extraction, struct timber_rect *out)
{
    int32_t across;

    if (!out || layer < 0 || layer >= TIMBER_LAYERS_MAX || slot < 0 || slot >= TIMBER_SLOTS ||
        extraction < -TIMBER_EXTRACTION_MAX || extraction > TIMBER_EXTRACTION_MAX) {
        return -1;
    }
    across = slot * TIMBER_UNIT;
    if (timber_layer_axis(layer) == TIMBER_AXIS_X) {
        out->x0 = extraction;
        out->x1 = extraction + TIMBER_BLOCK_LENGTH;
        out->y0 = across;
        out->y1 = across + TIMBER_UNIT;
    } else {
        out->x0 = across;
        out->x1 = across + TIMBER_UNIT;
        out->y0 = extraction;
        out->y1 = extraction + TIMBER_BLOCK_LENGTH;
    }
    return 0;
}

int64_t timber_rect_area(const struct timber_rect *r)
{
    if (!r || r->x1 <= r->x0 || r->y1 <= r->y0) {
        return 0;
    }
    return (int64_t)(r->x1 - r->x0) * (int64_t)(r->y1 - r->y0);
}

int timber_rect_intersect(const struct timber_rect *a, const struct timber_rect *b,
                          struct timber_rect *out)
{
    struct timber_rect r;

    if (!a || !b || !out) {
        return 0;
    }
    r.x0 = a->x0 > b->x0 ? a->x0 : b->x0;
    r.y0 = a->y0 > b->y0 ? a->y0 : b->y0;
    r.x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    r.y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    if (r.x1 <= r.x0 || r.y1 <= r.y0) {
        return 0;
    }
    *out = r;
    return 1;
}

int64_t timber_rect_overlap(const struct timber_rect *a, const struct timber_rect *b)
{
    struct timber_rect r;

    if (!timber_rect_intersect(a, b, &r)) {
        return 0;
    }
    return timber_rect_area(&r);
}

void timber_rect_centre(const struct timber_rect *r, int32_t *cx, int32_t *cy)
{
    if (!r) {
        return;
    }
    /* Arithmetic shift, so a negative extraction rounds the same way as a
     * positive one rounds down: toward negative infinity, consistently. */
    if (cx) {
        *cx = (int32_t)(((int64_t)r->x0 + r->x1) >> 1);
    }
    if (cy) {
        *cy = (int32_t)(((int64_t)r->y0 + r->y1) >> 1);
    }
}
