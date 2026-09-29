/*
 * Detection range. See vision_range.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_range.h"

#include <string.h>

static const char *const words[VISION_RANGES] = { "near", "normal", "far" };

void vision_range_params(enum vision_range r, struct vision_range_params *p)
{
    memset(p, 0, sizeof(*p));
    switch (r) {
    case VISION_RANGE_NEAR:
        /* The threshold stays: unit B's KPU scored the nearest car on the
         * road picture at 44 %, and a higher one lost it (VISION.md). */
        p->conf_min = 350;
        p->min_side_pm = 125;
        p->zoom = false;
        p->min_hits = 2;
        p->max_misses = 10;
        break;
    case VISION_RANGE_FAR:
        p->conf_min = 350;
        p->min_side_pm = 0;
        p->zoom = true;
        p->min_hits = 3;
        p->max_misses = 20;
        break;
    default:
        p->conf_min = 350;
        p->min_side_pm = 0;
        p->zoom = false;
        p->min_hits = 2;
        p->max_misses = 15;
        break;
    }
}

const char *vision_range_word(enum vision_range r)
{
    return (int)r >= 0 && r < VISION_RANGES ? words[r] : "normal";
}

int vision_range_parse(const char *word)
{
    int i;

    for (i = 0; word && i < VISION_RANGES; i++) {
        if (strcmp(word, words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int vision_range_zoom_window(uint32_t pic_w, uint32_t pic_h, uint32_t in_w, uint32_t in_h, struct vision_box *win)
{
    uint32_t w;
    uint32_t h;

    if (pic_w == 0 || pic_h == 0 || in_w == 0 || in_h == 0 || !win) {
        return -1;
    }
    /* The full pass already gives the model every pixel it can take: a
     * window would add nothing. */
    if (pic_w <= in_w && pic_h <= in_h) {
        return -1;
    }
    /* The model's shape, as large as the model and no larger than the
     * picture; shrunk evenly when the picture is the smaller. */
    w = in_w;
    h = in_h;
    if (w > pic_w) {
        h = (uint32_t)(((uint64_t)h * pic_w) / w);
        w = pic_w;
    }
    if (h > pic_h) {
        w = (uint32_t)(((uint64_t)w * pic_h) / h);
        h = pic_h;
    }
    if (w == 0 || h == 0) {
        return -1;
    }
    win->x = (int32_t)((pic_w - w) / 2);
    win->y = (int32_t)((pic_h - h) / 2);
    win->w = (int32_t)w;
    win->h = (int32_t)h;
    return 0;
}

static bool same_kind(uint32_t a, uint32_t b, const uint8_t *group, uint32_t n)
{
    if (a == b) {
        return true;
    }
    return group && a < n && b < n && group[a] != 0 && group[a] == group[b];
}

/* The share (per-mille) of a's area that lies inside b. */
static uint32_t inside_pm(const struct vision_box *a, const struct vision_box *b)
{
    int64_t x0 = a->x > b->x ? a->x : b->x;
    int64_t y0 = a->y > b->y ? a->y : b->y;
    int64_t x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    int64_t y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    int64_t area = (int64_t)a->w * a->h;

    if (x1 <= x0 || y1 <= y0 || area <= 0) {
        return 0;
    }
    return (uint32_t)(((x1 - x0) * (y1 - y0) * 1000) / area);
}

/* Whether the box touches an edge of the window that is not an edge of the
 * picture. */
static bool cut_by_window(const struct vision_box *b, const struct vision_box *win, uint32_t pic_w, uint32_t pic_h)
{
    int32_t e = VISION_RANGE_EDGE_PX;

    if (win->x > 0 && b->x <= win->x + e) {
        return true;
    }
    if (win->y > 0 && b->y <= win->y + e) {
        return true;
    }
    if ((uint32_t)(win->x + win->w) < pic_w && b->x + b->w >= win->x + win->w - e) {
        return true;
    }
    if ((uint32_t)(win->y + win->h) < pic_h && b->y + b->h >= win->y + win->h - e) {
        return true;
    }
    return false;
}

int vision_range_merge(struct vision_det *full, int nfull, int max, const struct vision_det *zoom, int nzoom,
                       const struct vision_box *win, uint32_t pic_w, uint32_t pic_h, const uint8_t *group,
                       uint32_t group_classes)
{
    int n = nfull < 0 ? 0 : nfull;
    int z;

    if (n > max) {
        n = max;
    }
    for (z = 0; z < nzoom; z++) {
        const struct vision_det *d = &zoom[z];
        int i;
        int same = -1;

        if (d->box.w <= 0 || d->box.h <= 0 || cut_by_window(&d->box, win, pic_w, pic_h)) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (!same_kind(full[i].cls, d->cls, group, group_classes)) {
                continue;
            }
            if (vision_iou_permille(&full[i].box, &d->box) >= VISION_RANGE_MERGE_IOU ||
                inside_pm(&d->box, &full[i].box) >= VISION_RANGE_MERGE_INSIDE ||
                inside_pm(&full[i].box, &d->box) >= VISION_RANGE_MERGE_INSIDE) {
                same = i;
                break;
            }
        }
        if (same >= 0) {
            /* One object seen twice: the surer sighting stands. */
            if (d->conf > full[same].conf) {
                full[same] = *d;
            }
            continue;
        }
        if (n < max) {
            full[n++] = *d;
        }
    }
    return n;
}

int vision_range_filter(struct vision_det *d, int n, const struct vision_range_params *p, uint32_t pic_w,
                        uint32_t pic_h)
{
    uint32_t side = pic_w < pic_h ? pic_w : pic_h;
    int32_t min_side = (int32_t)(((uint64_t)side * p->min_side_pm) / 1000);
    int kept = 0;
    int i;

    for (i = 0; i < n; i++) {
        int32_t s = d[i].box.w < d[i].box.h ? d[i].box.w : d[i].box.h;

        if (d[i].conf < p->conf_min || s < min_side) {
            continue;
        }
        d[kept++] = d[i];
    }
    return kept;
}
