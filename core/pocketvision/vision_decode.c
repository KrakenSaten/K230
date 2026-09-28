/*
 * The detector's raw output into boxes. See vision_decode.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_decode.h"

#include <errno.h>
#include <math.h>
#include <string.h>

uint32_t vision_iou_permille(const struct vision_box *a, const struct vision_box *b)
{
    int64_t ax1 = a->x;
    int64_t ay1 = a->y;
    int64_t ax2 = (int64_t)a->x + a->w;
    int64_t ay2 = (int64_t)a->y + a->h;
    int64_t bx1 = b->x;
    int64_t by1 = b->y;
    int64_t bx2 = (int64_t)b->x + b->w;
    int64_t by2 = (int64_t)b->y + b->h;
    int64_t ix1 = ax1 > bx1 ? ax1 : bx1;
    int64_t iy1 = ay1 > by1 ? ay1 : by1;
    int64_t ix2 = ax2 < bx2 ? ax2 : bx2;
    int64_t iy2 = ay2 < by2 ? ay2 : by2;
    int64_t inter;
    int64_t uni;

    if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0 || ix2 <= ix1 || iy2 <= iy1) {
        return 0;
    }
    inter = (ix2 - ix1) * (iy2 - iy1);
    uni = (int64_t)a->w * a->h + (int64_t)b->w * b->h - inter;
    if (uni <= 0) {
        return 0;
    }
    return (uint32_t)((inter * VISION_CONF_SCALE) / uni);
}

uint32_t vision_decode_rows(uint32_t in_w, uint32_t in_h)
{
    if (in_w == 0 || in_h == 0 || in_w % 32 || in_h % 32 || in_w > VISION_MAX_COORD ||
        in_h > VISION_MAX_COORD) {
        return 0;
    }
    return (in_w / 8) * (in_h / 8) + (in_w / 16) * (in_h / 16) + (in_w / 32) * (in_h / 32);
}

/* A finite value, in a range a box or score may have: what a corrupt or
 * misread tensor produces is NaN, infinities and values of the wrong
 * magnitude. */
static bool sane(float v, float limit)
{
    return isfinite(v) && v >= -limit && v <= limit;
}

int vision_decode(const float *out, size_t count, const uint32_t dims[3],
                  const struct vision_decode_params *p, struct vision_det *dets, int max,
                  uint32_t *bad)
{
    uint32_t rows;
    uint32_t features;
    uint32_t i;
    int n = 0;
    uint32_t skipped = 0;
    float ratio_w;
    float ratio_h;
    float ratio;
    float limit;

    if (bad) {
        *bad = 0;
    }
    if (!out || !dims || !p || !dets || max <= 0 || p->classes == 0 ||
        p->classes > VISION_MAX_CLASSES || p->frame_w == 0 || p->frame_h == 0 ||
        p->frame_w > VISION_MAX_COORD || p->frame_h > VISION_MAX_COORD) {
        return -EINVAL;
    }
    rows = vision_decode_rows(p->in_w, p->in_h);
    if (rows == 0) {
        return -EINVAL;
    }
    features = 4 + p->classes;
    if (dims[0] != 1 || dims[1] != features || dims[2] != rows ||
        count != (size_t)features * rows) {
        return -EPROTO;
    }
    ratio_w = (float)p->in_w / (float)p->frame_w;
    ratio_h = (float)p->in_h / (float)p->frame_h;
    ratio = ratio_w < ratio_h ? ratio_w : ratio_h;
    /* A box may reach a little outside the input while still meaning
     * something; four times the input is a tensor that is not a detector's. */
    limit = 4.0f * (float)(p->in_w > p->in_h ? p->in_w : p->in_h);
    for (i = 0; i < rows; i++) {
        float cx = out[0 * rows + i];
        float cy = out[1 * rows + i];
        float bw = out[2 * rows + i];
        float bh = out[3 * rows + i];
        float best = -1.0f;
        uint32_t best_cls = 0;
        uint32_t c;
        bool ok = true;
        struct vision_det d;
        float x;
        float y;
        float w;
        float h;

        for (c = 0; c < p->classes; c++) {
            float s = out[(4 + c) * rows + i];

            if (!isfinite(s)) {
                ok = false;
                break;
            }
            if (s > best) {
                best = s;
                best_cls = c;
            }
        }
        if (!ok || best < -0.001f || best > 1.001f) {
            skipped++;
            continue;
        }
        if ((uint32_t)(best * VISION_CONF_SCALE + 0.5f) < p->conf_min) {
            continue;
        }
        if (!sane(cx, limit) || !sane(cy, limit) || !sane(bw, limit) || !sane(bh, limit) ||
            bw <= 0.0f || bh <= 0.0f) {
            skipped++;
            continue;
        }
        /* Model-input pixels back to the frame: the ratio alone, since the
         * padding sits on the right and bottom. */
        x = (cx - 0.5f * bw) / ratio;
        y = (cy - 0.5f * bh) / ratio;
        w = bw / ratio;
        h = bh / ratio;
        if (x < 0.0f) {
            w += x;
            x = 0.0f;
        }
        if (y < 0.0f) {
            h += y;
            y = 0.0f;
        }
        if (x >= (float)p->frame_w || y >= (float)p->frame_h) {
            skipped++;
            continue;
        }
        if (x + w > (float)p->frame_w) {
            w = (float)p->frame_w - x;
        }
        if (y + h > (float)p->frame_h) {
            h = (float)p->frame_h - y;
        }
        if (w < 1.0f || h < 1.0f) {
            skipped++;
            continue;
        }
        d.box.x = (int32_t)x;
        d.box.y = (int32_t)y;
        d.box.w = (int32_t)(w + 0.5f);
        d.box.h = (int32_t)(h + 0.5f);
        if (d.box.w < 1) {
            d.box.w = 1;
        }
        if (d.box.h < 1) {
            d.box.h = 1;
        }
        d.cls = (uint16_t)best_cls;
        d.conf = (uint16_t)(best * VISION_CONF_SCALE + 0.5f);
        if (d.conf > VISION_CONF_SCALE) {
            d.conf = VISION_CONF_SCALE;
        }
        if (n < max) {
            dets[n++] = d;
        } else {
            /* Full: keep the best. Replace the weakest if this beats it. */
            int weakest = 0;
            int k;

            for (k = 1; k < n; k++) {
                if (dets[k].conf < dets[weakest].conf) {
                    weakest = k;
                }
            }
            if (d.conf > dets[weakest].conf) {
                dets[weakest] = d;
            }
        }
    }
    if (bad) {
        *bad = skipped;
    }
    return n;
}
