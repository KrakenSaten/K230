/*
 * Non-maximum suppression. See vision_nms.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_nms.h"

int vision_nms(struct vision_det *cands, int n, uint32_t iou_max, struct vision_det *out, int max)
{
    int i;
    int j;
    int kept = 0;

    if (!cands || !out || n <= 0 || max <= 0) {
        return 0;
    }
    /* Insertion sort, most confident first: n is at most
     * VISION_MAX_CANDIDATES and usually a handful. Stable, so equal scores
     * keep the decoder's row order and the result is deterministic. */
    for (i = 1; i < n; i++) {
        struct vision_det d = cands[i];

        for (j = i - 1; j >= 0 && cands[j].conf < d.conf; j--) {
            cands[j + 1] = cands[j];
        }
        cands[j + 1] = d;
    }
    for (i = 0; i < n && kept < max; i++) {
        bool drop = false;

        for (j = 0; j < kept; j++) {
            if (out[j].cls == cands[i].cls &&
                vision_iou_permille(&out[j].box, &cands[i].box) > iou_max) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            out[kept++] = cands[i];
        }
    }
    return kept;
}

static int64_t area(const struct vision_box *b)
{
    return (int64_t)b->w * b->h;
}

/* The area of a inside b. */
static int64_t inside(const struct vision_box *a, const struct vision_box *b)
{
    int64_t x1 = a->x > b->x ? a->x : b->x;
    int64_t y1 = a->y > b->y ? a->y : b->y;
    int64_t x2 = (int64_t)a->x + a->w < (int64_t)b->x + b->w ? (int64_t)a->x + a->w : (int64_t)b->x + b->w;
    int64_t y2 = (int64_t)a->y + a->h < (int64_t)b->y + b->h ? (int64_t)a->y + a->h : (int64_t)b->y + b->h;

    if (x2 <= x1 || y2 <= y1) {
        return 0;
    }
    return (x2 - x1) * (y2 - y1);
}

int vision_nms_nested(struct vision_det *dets, int n, uint32_t inside_pm)
{
    int i;
    int j;
    int kept = 0;

    if (!dets || n <= 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        bool drop = false;
        int64_t a = area(&dets[i].box);

        for (j = 0; j < n && a > 0; j++) {
            if (j == i || dets[j].cls != dets[i].cls || area(&dets[j].box) <= a) {
                continue;
            }
            if (inside(&dets[i].box, &dets[j].box) * 1000 >= a * (int64_t)inside_pm) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            dets[kept++] = dets[i];
        }
    }
    return kept;
}

/* Whether one of a and b lies inside the other by inside_pm of its own
 * area. */
static bool nested(const struct vision_box *a, const struct vision_box *b, uint32_t inside_pm)
{
    int64_t aa = area(a);
    int64_t bb = area(b);
    int64_t small = aa < bb ? aa : bb;

    return small > 0 && inside(a, b) * 1000 >= small * (int64_t)inside_pm;
}

int vision_nms_add_weak(struct vision_det *dets, int n, const struct vision_det *weak, int nw,
                        uint32_t iou_max, uint32_t inside_pm, int max)
{
    int i;
    int j;

    if (!dets || n < 0) {
        return 0;
    }
    if (!weak || nw <= 0) {
        return n;
    }
    for (i = 0; i < nw && n < max; i++) {
        bool drop = false;

        for (j = 0; j < n; j++) {
            if (dets[j].cls == weak[i].cls &&
                (vision_iou_permille(&dets[j].box, &weak[i].box) > iou_max ||
                 nested(&dets[j].box, &weak[i].box, inside_pm))) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            dets[n++] = weak[i];
        }
    }
    return n;
}
