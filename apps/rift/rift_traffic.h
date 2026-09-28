/*
 * What was heard on the air, minute by minute, for the last twenty.
 *
 * ACTIVITY draws a bar per minute from these (ui/rift_graph.c): how many
 * frames the service reported hearing in that minute, split into what
 * they were - a message, an advert, or anything else. It is a count of
 * frames MeshCore parsed, from the mesh.activity feed and nothing else:
 * not a measure of the link, not the service's cumulative counters, and
 * not anything RIFT infers. Frames this device sent are not in it - they
 * were not heard, they were said - and their outcomes are on the feed.
 *
 * The T-Deck's RIFT keeps the same twenty bins (RiftActivity in
 * vendor/RIFT); this is the same idea for the same reason, kept in the
 * model so the screen only draws.
 *
 * No LVGL: host-tested by tests/rift_model_test.c.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_TRAFFIC_H
#define RIFT_TRAFFIC_H

#include <stdint.h>

#define RIFT_TRAFFIC_MINUTES 20
#define RIFT_TRAFFIC_MINUTE_MS 60000LL

enum rift_traffic_class {
    RIFT_TRAFFIC_MSG = 0, /* text, group_text, group_data: somebody said something */
    RIFT_TRAFFIC_ADV,     /* advert: somebody said they are here */
    RIFT_TRAFFIC_OTHER,   /* ack, path, req, trace, control, ...: the mesh at work */
    RIFT_TRAFFIC_CLASSES
};

/* A ring of minutes. bin[i] holds the minute (base_minute + i) modulo the
 * ring; the newest minute is the one `minute` names. Counts saturate. */
struct rift_traffic {
    uint16_t count[RIFT_TRAFFIC_MINUTES][RIFT_TRAFFIC_CLASSES];
    int64_t minute;   /* the newest minute held, mono_ms / RIFT_TRAFFIC_MINUTE_MS */
    int started;      /* something has been noted */
};

/* The twenty minutes as a screen reads them: oldest first, the current
 * minute last. Minutes nothing was heard in are zero. */
struct rift_traffic_bins {
    uint16_t count[RIFT_TRAFFIC_MINUTES][RIFT_TRAFFIC_CLASSES];
    int started;
};

void rift_traffic_init(struct rift_traffic *t);
/* Which class a mesh.activity rx payload_type word is. */
enum rift_traffic_class rift_traffic_class_of(const char *payload_type);
/* Count one frame heard at mono_ms. A frame older than the window is
 * dropped; a frame from the future moves the window on. */
void rift_traffic_note(struct rift_traffic *t, int64_t mono_ms, enum rift_traffic_class c);
/* Read the ring as of now_ms: minutes that have passed since the newest
 * note are empty. */
void rift_traffic_read(const struct rift_traffic *t, int64_t now_ms, struct rift_traffic_bins *out);
/* The busiest minute in the window (all classes), for the caption. */
unsigned rift_traffic_peak(const struct rift_traffic_bins *b);

#endif
