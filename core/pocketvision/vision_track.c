/*
 * Tracks. See vision_track.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_track.h"

#include <string.h>

void vision_tracker_init(struct vision_tracker *tr)
{
    memset(tr, 0, sizeof(*tr));
    tr->next_id = 1;
    tr->iou_min = VISION_TRACK_IOU_MIN;
    tr->max_misses = VISION_TRACK_MAX_MISSES;
    tr->min_hits = VISION_TRACK_MIN_HITS;
    tr->reacquire_pm = VISION_TRACK_REACQUIRE_PM;
}

void vision_tracker_clear(struct vision_tracker *tr)
{
    memset(tr->t, 0, sizeof(tr->t));
    tr->count = 0;
}

bool vision_tracker_compatible(const struct vision_tracker *tr, uint32_t a, uint32_t b)
{
    if (a == b) {
        return true;
    }
    if (!tr->group || a >= tr->group_classes || b >= tr->group_classes) {
        return false;
    }
    return tr->group[a] != 0 && tr->group[a] == tr->group[b];
}

static void predicted(const struct vision_track *t, struct vision_box *b)
{
    *b = t->box;
    b->x += t->vx / (1 << VISION_TRACK_V_SHIFT);
    b->y += t->vy / (1 << VISION_TRACK_V_SHIFT);
}

static void remove_at(struct vision_tracker *tr, int i)
{
    if (i < tr->count - 1) {
        memmove(&tr->t[i], &tr->t[i + 1], (size_t)(tr->count - 1 - i) * sizeof(tr->t[0]));
    }
    tr->count--;
}

/* A track takes a detection: the box, the confidence, the motion, the
 * class vote. */
static void take(struct vision_tracker *tr, struct vision_track *t, const struct vision_det *d)
{
    int32_t ocx;
    int32_t ocy;
    int32_t ncx;
    int32_t ncy;

    vision_box_centre(&t->box, &ocx, &ocy);
    vision_box_centre(&d->box, &ncx, &ncy);
    if (t->misses == 0 && t->hits == 1) {
        /* The first motion there is, taken as it is. */
        t->vx = (ncx - ocx) * (1 << VISION_TRACK_V_SHIFT);
        t->vy = (ncy - ocy) * (1 << VISION_TRACK_V_SHIFT);
    } else if (t->misses == 0 && t->hits > 1) {
        /* Motion from the last actual sighting, smoothed: three parts the
         * old, one part the new, so one jittery box does not throw the
         * prediction. */
        t->vx = (t->vx * 3 + (ncx - ocx) * (1 << VISION_TRACK_V_SHIFT)) / 4;
        t->vy = (t->vy * 3 + (ncy - ocy) * (1 << VISION_TRACK_V_SHIFT)) / 4;
    }
    /* A track that was coasting keeps the motion it had: the new box is
     * the truth about where it is, not about how fast it went. */
    t->box = d->box;
    t->conf = d->conf;
    if (d->cls != t->cls) {
        if (t->cls_other == d->cls && t->cls_other_run < 0xff) {
            t->cls_other_run++;
        } else {
            t->cls_other = d->cls;
            t->cls_other_run = 1;
        }
        if (t->cls_other_run >= VISION_TRACK_CLS_SWITCH) {
            t->cls = d->cls;
            t->cls_other_run = 0;
        }
    } else {
        t->cls_other_run = 0;
    }
    if (t->hits < 0xffff) {
        t->hits++;
    }
    t->misses = 0;
    t->seen = true;
    if (t->hits >= tr->min_hits) {
        t->confirmed = true;
    }
}

static int32_t larger_side(const struct vision_box *b)
{
    return b->w > b->h ? b->w : b->h;
}

/* Whether a detection is of a size a track could have: areas within a
 * factor of three of each other. */
static bool similar_size(const struct vision_box *a, const struct vision_box *b)
{
    int64_t aa = (int64_t)a->w * a->h;
    int64_t bb = (int64_t)b->w * b->h;

    if (aa <= 0 || bb <= 0) {
        return false;
    }
    return aa * 3 >= bb && bb * 3 >= aa;
}

int vision_tracker_update(struct vision_tracker *tr, const struct vision_det *dets, int n)
{
    /* Overlap of every track's prediction with every detection, computed
     * once. Both dimensions are bounded, so this is a fixed 32 x 32 table. */
    static uint32_t iou[VISION_MAX_TRACKS][VISION_MAX_DETECTIONS];
    static struct vision_box pred[VISION_MAX_TRACKS];
    bool det_used[VISION_MAX_DETECTIONS];
    int i;
    int j;

    if (n < 0) {
        n = 0;
    }
    if (n > VISION_MAX_DETECTIONS) {
        n = VISION_MAX_DETECTIONS;
    }
    memset(det_used, 0, sizeof(det_used));
    for (i = 0; i < tr->count; i++) {
        tr->t[i].seen = false;
        if (tr->t[i].age < 0xffff) {
            tr->t[i].age++;
        }
        predicted(&tr->t[i], &pred[i]);
        for (j = 0; j < n; j++) {
            iou[i][j] = vision_tracker_compatible(tr, tr->t[i].cls, dets[j].cls)
                            ? vision_iou_permille(&pred[i], &dets[j].box)
                            : 0;
        }
    }
    /* Pass 1, greedy: the best remaining overlap, until none is good
     * enough. */
    for (;;) {
        uint32_t best = 0;
        int bi = -1;
        int bj = -1;

        for (i = 0; i < tr->count; i++) {
            if (tr->t[i].seen) {
                continue;
            }
            for (j = 0; j < n; j++) {
                if (!det_used[j] && iou[i][j] > best) {
                    best = iou[i][j];
                    bi = i;
                    bj = j;
                }
            }
        }
        if (bi < 0 || best < tr->iou_min) {
            break;
        }
        take(tr, &tr->t[bi], &dets[bj]);
        det_used[bj] = true;
    }
    /* Pass 2, greedy by distance: a confirmed track the overlap did not
     * find, and a detection of its size close to where it should be. */
    while (tr->reacquire_pm > 0) {
        int64_t best = -1;
        int bi = -1;
        int bj = -1;

        for (i = 0; i < tr->count; i++) {
            const struct vision_track *t = &tr->t[i];
            int64_t radius;
            int32_t pcx;
            int32_t pcy;

            if (t->seen || !t->confirmed) {
                continue;
            }
            radius = ((int64_t)larger_side(&pred[i]) * tr->reacquire_pm) / 1000;
            vision_box_centre(&pred[i], &pcx, &pcy);
            for (j = 0; j < n; j++) {
                int32_t dcx;
                int32_t dcy;
                int64_t dx;
                int64_t dy;
                int64_t d2;

                if (det_used[j] || !vision_tracker_compatible(tr, t->cls, dets[j].cls) ||
                    !similar_size(&pred[i], &dets[j].box)) {
                    continue;
                }
                vision_box_centre(&dets[j].box, &dcx, &dcy);
                dx = (int64_t)dcx - pcx;
                dy = (int64_t)dcy - pcy;
                d2 = dx * dx + dy * dy;
                if (d2 > radius * radius) {
                    continue;
                }
                if (best < 0 || d2 < best) {
                    best = d2;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (bi < 0) {
            break;
        }
        take(tr, &tr->t[bi], &dets[bj]);
        det_used[bj] = true;
        tr->reacquired++;
    }
    /* The unseen: coast, then expire. */
    for (i = 0; i < tr->count;) {
        struct vision_track *t = &tr->t[i];

        if (t->seen) {
            i++;
            continue;
        }
        t->misses++;
        if (t->misses > tr->max_misses) {
            remove_at(tr, i);
            continue;
        }
        t->box = pred[i];
        /* A coasting track slows: an object that stopped while unseen is
         * found near where it was, one that kept going is found by the
         * second pass. */
        t->vx = t->vx * 7 / 8;
        t->vy = t->vy * 7 / 8;
        i++;
    }
    /* The new. */
    for (j = 0; j < n; j++) {
        struct vision_track *t;

        if (det_used[j]) {
            continue;
        }
        if (tr->count >= VISION_MAX_TRACKS) {
            tr->dropped++;
            continue;
        }
        t = &tr->t[tr->count++];
        memset(t, 0, sizeof(*t));
        t->id = tr->next_id++;
        if (tr->next_id == 0) {
            tr->next_id = 1;
        }
        t->box = dets[j].box;
        t->cls = dets[j].cls;
        t->conf = dets[j].conf;
        t->hits = 1;
        t->seen = true;
        t->confirmed = tr->min_hits <= 1;
        vision_box_centre(&t->box, &t->ox, &t->oy);
    }
    return tr->count;
}
