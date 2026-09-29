/*
 * Tracks: the same object seen frame after frame keeps one id
 * (pocketvision.h).
 *
 * A tracker holds at most VISION_MAX_TRACKS tracks. Each frame's detections
 * are matched to tracks in three passes, one detection per track:
 *
 *   1. by overlap with where the track is expected to be (its last box
 *      moved by its smoothed motion), best overlaps first, down to
 *      `iou_min`;
 *   2. for a confirmed track still unmatched, by distance: a detection of
 *      a similar size whose centre lies within `reacquire_pm` of the
 *      predicted box's larger side is the object found again after a
 *      dropout or a jump the overlap could not follow. Nearest first. The
 *      radius grows by VISION_TRACK_REACQUIRE_STEP_PM for every frame the
 *      track has gone unseen, up to VISION_TRACK_REACQUIRE_MAX_PM: the
 *      longer an object is hidden, the less its prediction is worth;
 *   3. for a track still unmatched that has been coasting, by overlap with
 *      where it was last SEEN rather than predicted: an object that stood
 *      still or turned while hidden is found where it was, not lost to a
 *      prediction that drifted on (unit B's KPU replay of a street, 2026-09-29:
 *      a car behind cyclists came back beside its coasting track as a new id).
 *
 * A new track is not started from a box that is the same object as a track
 * matched in this frame - IoU at least VISION_TRACK_DUP_IOU, or either
 * VISION_TRACK_DUP_INSIDE inside the other, of a compatible class: the
 * detector's partial box beside its whole one, or two boxes of one person
 * just under suppression's overlap (the same replay: 43 frames of duplicate
 * confirmed tracks). Such boxes are counted in `dup_births`.
 *
 * A match must also be of a size the track could have: within
 * VISION_TRACK_SIZE_RATIO of its area from one frame to the next,
 * VISION_TRACK_SIZE_RATIO_COASTING after a dropout. A box that suddenly
 * spans half the picture is another object (or the detector merging two),
 * not the track grown.
 *
 * A detection matches a track of the same class, or of another class in
 * the same group (`group`, a table the caller sets: car, truck and bus are
 * one object to a detector that cannot make up its mind). The track keeps
 * its class until it has seen the other one VISION_TRACK_CLS_SWITCH times
 * in a row, so a label does not flicker.
 *
 * A matched track takes the detection's box. An unmatched track is kept
 * for up to `max_misses` frames on its prediction, its motion decaying,
 * and then expires. An unmatched detection starts a track when there is
 * room, and is dropped when there is not: the list never grows past its
 * size, whatever the scene does.
 *
 * A track is `confirmed` once it has been seen `min_hits` times, so a single
 * spurious box never gets an id on screen or a place in a count. Ids are
 * handed out once each and never reused within a session.
 *
 * Each track carries VISION_LINES line states (vision_line.h): the side it
 * was last clearly on of each virtual line, with the settling that keeps a
 * jitter from counting.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_TRACK_H
#define POCKETOS_VISION_TRACK_H

#include "pocketvision.h"

#define VISION_TRACK_IOU_MIN 200        /* per-mille: below this a box is not the same object */
#define VISION_TRACK_MAX_MISSES 15      /* frames a track survives unseen (~0.6 s at 25 fps) */
#define VISION_TRACK_MIN_HITS 2         /* sightings before a track is confirmed */
#define VISION_TRACK_REACQUIRE_PM 750   /* pass 2: the centre within 3/4 of the larger side */
#define VISION_TRACK_SIZE_RATIO 2       /* a match's area against the track's, frame to frame */
#define VISION_TRACK_SIZE_RATIO_COASTING 3 /* ... and after a dropout, or by distance */
#define VISION_TRACK_CLS_SWITCH 3       /* sightings of another class before the track takes it */
#define VISION_TRACK_REACQUIRE_STEP_PM 50 /* pass 2's radius grows this much per frame unseen... */
#define VISION_TRACK_REACQUIRE_MAX_PM 1500 /* ... up to twice its start */
#define VISION_TRACK_DUP_IOU 500        /* a new box this much over a track matched this frame is that object again */
#define VISION_TRACK_DUP_INSIDE 850     /* ... as is one this much inside it, or holding it */
#define VISION_TRACK_V_SHIFT 4          /* motion is kept x16 */
#define VISION_LINES 3                  /* line states per track: the count line, speed lines A and B */

struct vision_line_state {
    int8_t side;       /* the side committed to: -1, +1, or 0 while not yet known */
    int8_t pending;    /* the other side, seen but not yet settled */
    uint8_t run;       /* clear sightings of `pending` in a row */
};

struct vision_track {
    uint32_t id;
    struct vision_box box;    /* where it is (or is predicted to be) */
    struct vision_box seen_box; /* where it was last actually seen */
    uint16_t cls;
    uint16_t conf;            /* the last detection's */
    uint16_t hits;            /* frames seen, saturating */
    uint16_t misses;          /* frames unseen in a row */
    uint16_t age;             /* frames since it was born, saturating */
    bool confirmed;
    bool seen;                /* matched in the last update */
    int32_t vx;               /* smoothed motion per frame, x16 */
    int32_t vy;
    int32_t ox;               /* the centre at the first sighting: the displacement since is the direction */
    int32_t oy;
    uint16_t cls_other;       /* another class of the group, seen cls_other_run times in a row */
    uint8_t cls_other_run;
    struct vision_line_state ls[VISION_LINES];
};

struct vision_tracker {
    struct vision_track t[VISION_MAX_TRACKS];
    int count;
    uint32_t next_id;
    uint32_t iou_min;
    uint16_t max_misses;
    uint16_t min_hits;
    uint32_t reacquire_pm;    /* 0: no second pass */
    const uint8_t *group;     /* per class, 0 = only its own class; NULL = none */
    uint32_t group_classes;   /* the table's length */
    uint32_t dropped;         /* detections with no room, for the record */
    uint32_t reacquired;      /* pass 2 matches, for the record */
    uint32_t revived;         /* pass 3 matches, for the record */
    uint32_t dup_births;      /* boxes not made tracks: a second box of a tracked object */
};

void vision_tracker_init(struct vision_tracker *tr);

/* One frame: match, update, expire, start. Returns the number of tracks
 * alive afterwards. */
int vision_tracker_update(struct vision_tracker *tr, const struct vision_det *dets, int n);

/* Forget everything, keeping the id counter. */
void vision_tracker_clear(struct vision_tracker *tr);

/* Whether a detection of class b may continue a track of class a. */
bool vision_tracker_compatible(const struct vision_tracker *tr, uint32_t a, uint32_t b);

/* The centre of a box. */
static inline void vision_box_centre(const struct vision_box *b, int32_t *cx, int32_t *cy)
{
    *cx = b->x + b->w / 2;
    *cy = b->y + b->h / 2;
}

#endif
