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
