/*
 * DeskBuddy's boundary with vision: what DeskBuddy is told, and by whom.
 *
 * DeskBuddy never opens a camera, never runs a model and never includes a
 * header of the Vision app or of core/pocketvision. What it knows about the
 * room arrives as a db_vision_event from a db_vision_provider, and that is
 * the whole contract (tests/deskbuddy_lint.sh holds it). A provider is the
 * only thing that changes when the real pipeline arrives; the state machine
 * (db_brain.h), the log and the screen stay as they are.
 *
 * The events are conclusions, not detections: "somebody is there", "it is
 * the owner", "it is somebody else", "nobody", "I cannot see". Turning boxes,
 * faces and embeddings into those - and deciding how sure is sure enough -
 * is the provider's job (db_identity.h describes the owner-matching half).
 *
 * Pure C, no LVGL, no I/O, clock passed in: tested on the host
 * (tests/db_vision_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef DB_VISION_H
#define DB_VISION_H

#include <stdbool.h>
#include <stdint.h>

enum db_vision_kind {
    DB_VISION_NO_PERSON = 0,     /* nobody in view */
    DB_VISION_PERSON_DETECTED,   /* somebody, identity not (yet) known */
    DB_VISION_OWNER_RECOGNIZED,  /* somebody, and it is the owner */
    DB_VISION_UNKNOWN_PERSON,    /* somebody, and it is not the owner */
    DB_VISION_UNAVAILABLE,       /* no camera, no model, or the source failed */
    DB_VISION_KIND_COUNT
};

/* confidence_pm when the source gives none. */
#define DB_CONF_NONE (-1)

struct db_vision_event {
    enum db_vision_kind kind;
    bool face;              /* a face was found (identity events imply one) */
    int16_t confidence_pm;  /* 0..1000, or DB_CONF_NONE */
    int64_t mono_ms;        /* when it was seen, on the app's monotonic clock */
    int64_t wall_s;         /* wall clock (epoch s), 0 when the clock is not set */
};

/* A kind in range and a confidence that is DB_CONF_NONE or 0..1000. */
bool db_vision_event_valid(const struct db_vision_event *ev);
/* "none", "person", "owner", "unknown", "unavailable"; "?" out of range. */
const char *db_vision_kind_name(enum db_vision_kind kind);
/* The inverse of db_vision_kind_name. 0, or -1 leaving *out alone. */
int db_vision_kind_parse(const char *name, enum db_vision_kind *out);

/* ---- the queue between a provider and the brain ------------------------ *
 *
 * Fixed size. A provider that produces faster than the UI drains never grows
 * it: when it is full the oldest event goes (the newest is the truth about
 * the room) and is counted. An invalid event is refused and counted. */
#define DB_VISION_QUEUE_CAP 16

struct db_vision_queue {
    struct db_vision_event ev[DB_VISION_QUEUE_CAP];
    unsigned head;      /* index of the oldest */
    unsigned count;
    unsigned dropped;   /* pushed out by a newer one */
    unsigned rejected;  /* refused as invalid */
};

void db_vision_queue_init(struct db_vision_queue *q);
/* false when refused as invalid; a full queue still takes it (see above). */
bool db_vision_queue_push(struct db_vision_queue *q, const struct db_vision_event *ev);
/* The oldest; false when empty. */
bool db_vision_queue_pop(struct db_vision_queue *q, struct db_vision_event *out);

/* ---- providers ------------------------------------------------------------ *
 *
 * Every call runs on the LVGL thread and must return at once: a provider
 * that does real work does it in its own process or thread (the Vision
 * helper's shape, ADR-006) and hands over what is ready. */
struct db_vision_provider_ops {
    const char *name;
    /* Begin. 0, or -1 when the source cannot run - the provider then pushes
     * DB_VISION_UNAVAILABLE itself, so the caller has nothing to special-case. */
    int (*start)(void *ctx, int64_t now_ms, struct db_vision_queue *q);
    /* Move whatever is ready into q. Returns the monotonic ms at which it
     * next wants to be polled, or -1 when it has nothing scheduled. */
    int64_t (*poll)(void *ctx, int64_t now_ms, struct db_vision_queue *q);
    /* End; no call follows except start(). Safe to call twice. */
    void (*stop)(void *ctx);
};

struct db_vision_provider {
    const struct db_vision_provider_ops *ops;
    void *ctx;
};

/* The provider for a build or a board with no vision: it says so once. */
extern const struct db_vision_provider_ops db_vision_none_ops;

#endif
