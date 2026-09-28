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
}

void vision_tracker_clear(struct vision_tracker *tr)
{
    memset(tr->t, 0, sizeof(tr->t));
    tr->count = 0;
}

static void predicted(const struct vision_track *t, struct vision_box *b)
{
    *b = t->box;
    b->x += t->vx;
    b->y += t->vy;
}

static void remove_at(struct vision_tracker *tr, int i)
{
    if (i < tr->count - 1) {
        memmove(&tr->t[i], &tr->t[i + 1], (size_t)(tr->count - 1 - i) * sizeof(tr->t[0]));
    }
    tr->count--;
}

int vision_tracker_update(struct vision_tracker *tr, const struct vision_det *dets, int n)
{
    /* Overlap of every track's prediction with every detection, computed
     * once. Both dimensions are bounded, so this is a fixed 32 x 32 table. */
    static uint32_t iou[VISION_MAX_TRACKS][VISION_MAX_DETECTIONS];
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
        struct vision_box pb;

        tr->t[i].seen = false;
        predicted(&tr->t[i], &pb);
        for (j = 0; j < n; j++) {
            iou[i][j] = tr->t[i].cls == dets[j].cls ? vision_iou_permille(&pb, &dets[j].box) : 0;
        }
    }
    /* Greedy: the best remaining pair, until none is good enough. */
    for (;;) {
        uint32_t best = 0;
        int bi = -1;
        int bj = -1;
        struct vision_track *t;
        int32_t ocx;
        int32_t ocy;
        int32_t ncx;
        int32_t ncy;

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
        t = &tr->t[bi];
        vision_box_centre(&t->box, &ocx, &ocy);
        vision_box_centre(&dets[bj].box, &ncx, &ncy);
        /* Motion from the last actual sighting; a track that was coasting
         * on its prediction takes the new box as the truth. */
        if (t->misses == 0 && t->hits > 0) {
            t->vx = ncx - ocx;
            t->vy = ncy - ocy;
        }
        t->box = dets[bj].box;
        t->conf = dets[bj].conf;
        if (t->hits < 0xffff) {
            t->hits++;
        }
        t->misses = 0;
        t->seen = true;
        if (t->hits >= tr->min_hits) {
            t->confirmed = true;
        }
        det_used[bj] = true;
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
        predicted(t, &t->box);
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
    }
    return tr->count;
}
