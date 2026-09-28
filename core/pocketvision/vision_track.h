/*
 * Tracks: the same object seen frame after frame keeps one id
 * (pocketvision.h).
 *
 * A tracker holds at most VISION_MAX_TRACKS tracks. Each frame's detections
 * are matched to tracks of the same class by overlap with where the track
 * is expected to be (its last box moved by its last motion), best overlaps
 * first, one detection per track. A matched track takes the detection's box.
 * An unmatched track is kept for up to `max_misses` frames on its prediction
 * and then expires. An unmatched detection starts a track when there is
 * room, and is dropped when there is not: the list never grows past its
 * size, whatever the scene does.
 *
 * A track is `confirmed` once it has been seen `min_hits` times, so a single
 * spurious box never gets an id on screen or a place in a count. Ids are
 * handed out once each and never reused within a session.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_TRACK_H
#define POCKETOS_VISION_TRACK_H

#include "pocketvision.h"

#define VISION_TRACK_IOU_MIN 200    /* per-mille: below this a box is not the same object */
#define VISION_TRACK_MAX_MISSES 8   /* frames a track survives unseen */
#define VISION_TRACK_MIN_HITS 2     /* sightings before a track is confirmed */

struct vision_track {
    uint32_t id;
    struct vision_box box;    /* where it is (or is predicted to be) */
    uint16_t cls;
    uint16_t conf;            /* the last detection's */
    uint16_t hits;            /* frames seen, saturating */
    uint16_t misses;          /* frames unseen in a row */
    bool confirmed;
    bool seen;                /* matched in the last update */
    int32_t vx;               /* motion per frame, from the last two sightings */
    int32_t vy;
    /* For the line counter (vision_line.h): which side the centre was last
     * known on, -1, +1, or 0 while not yet known. */
    int8_t side;
};

struct vision_tracker {
    struct vision_track t[VISION_MAX_TRACKS];
    int count;
    uint32_t next_id;
    uint32_t iou_min;
    uint16_t max_misses;
    uint16_t min_hits;
    uint32_t dropped;         /* detections with no room, for the record */
};

void vision_tracker_init(struct vision_tracker *tr);

/* One frame: match, update, expire, start. Returns the number of tracks
 * alive afterwards. */
int vision_tracker_update(struct vision_tracker *tr, const struct vision_det *dets, int n);

/* Forget everything, keeping the id counter. */
void vision_tracker_clear(struct vision_tracker *tr);

/* The centre of a box. */
static inline void vision_box_centre(const struct vision_box *b, int32_t *cx, int32_t *cy)
{
    *cx = b->x + b->w / 2;
    *cy = b->y + b->h / 2;
}

#endif
