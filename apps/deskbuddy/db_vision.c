/*
 * DeskBuddy's vision boundary: the event, the queue and the "none"
 * provider. See db_vision.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "db_vision.h"

#include <string.h>

static const char *const kind_names[DB_VISION_KIND_COUNT] = { "none", "person", "owner", "unknown",
                                                              "unavailable" };

bool db_vision_event_valid(const struct db_vision_event *ev)
{
    if (!ev || (int)ev->kind < 0 || ev->kind >= DB_VISION_KIND_COUNT) {
        return false;
    }
    return ev->confidence_pm == DB_CONF_NONE || (ev->confidence_pm >= 0 && ev->confidence_pm <= 1000);
}

const char *db_vision_kind_name(enum db_vision_kind kind)
{
    return ((int)kind >= 0 && kind < DB_VISION_KIND_COUNT) ? kind_names[kind] : "?";
}

int db_vision_kind_parse(const char *name, enum db_vision_kind *out)
{
    int k;

    for (k = 0; name && out && k < DB_VISION_KIND_COUNT; k++) {
        if (strcmp(name, kind_names[k]) == 0) {
            *out = (enum db_vision_kind)k;
            return 0;
        }
    }
    return -1;
}

void db_vision_queue_init(struct db_vision_queue *q)
{
    if (q) {
        memset(q, 0, sizeof(*q));
    }
}

bool db_vision_queue_push(struct db_vision_queue *q, const struct db_vision_event *ev)
{
    if (!q) {
        return false;
    }
    if (!db_vision_event_valid(ev)) {
        q->rejected++;
        return false;
    }
    if (q->count == DB_VISION_QUEUE_CAP) {
        q->head = (q->head + 1) % DB_VISION_QUEUE_CAP;
        q->count--;
        q->dropped++;
    }
    q->ev[(q->head + q->count) % DB_VISION_QUEUE_CAP] = *ev;
    q->count++;
    return true;
}

bool db_vision_queue_pop(struct db_vision_queue *q, struct db_vision_event *out)
{
    if (!q || q->count == 0) {
        return false;
    }
    if (out) {
        *out = q->ev[q->head];
    }
    q->head = (q->head + 1) % DB_VISION_QUEUE_CAP;
    q->count--;
    return true;
}

/* ---- the "none" provider ----------------------------------------------------- */

static int none_start(void *ctx, int64_t now_ms, struct db_vision_queue *q)
{
    struct db_vision_event ev = { .kind = DB_VISION_UNAVAILABLE, .confidence_pm = DB_CONF_NONE,
                                  .mono_ms = now_ms };

    (void)ctx;
    db_vision_queue_push(q, &ev);
    return -1;
}

static int64_t none_poll(void *ctx, int64_t now_ms, struct db_vision_queue *q)
{
    (void)ctx;
    (void)now_ms;
    (void)q;
    return -1;
}

static void none_stop(void *ctx)
{
    (void)ctx;
}

const struct db_vision_provider_ops db_vision_none_ops = {
    .name = "none",
    .start = none_start,
    .poll = none_poll,
    .stop = none_stop,
};
