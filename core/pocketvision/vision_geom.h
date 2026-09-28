/*
 * Where a sensor-frame box lands on the picture the screen shows
 * (pocketvision.h).
 *
 * The helper draws the preview with pocketcam_to_rgb565(): the frame turned
 * clockwise by a rotation, mirrored or not, and the largest centred part of
 * it with the destination's shape scaled to fill it (POCKETCAM_FIT_COVER).
 * A box found in the frame has to be drawn on that picture, so this is the
 * same arithmetic run the other way: a frame point to a picture point.
 * tests/vision_geom_test.c holds the two together by painting a pixel,
 * converting it, and looking where it went.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_GEOM_H
#define POCKETOS_VISION_GEOM_H

#include "pocketvision.h"

struct vision_view {
    uint32_t frame_w;  /* the sensor frame */
    uint32_t frame_h;
    int rotation;      /* 0, 90, 180, 270: the turn the preview is drawn with */
    bool mirror;
    uint32_t view_w;   /* the picture on screen */
    uint32_t view_h;
};

/* Frame pixel (sx, sy) on the picture: 0, or -EINVAL for a view that makes
 * no sense. The result may fall outside the picture (cover-fit cuts the
 * frame's edges off); the caller clamps or discards. */
int vision_map_point(const struct vision_view *v, int32_t sx, int32_t sy, int32_t *vx, int32_t *vy);

/* A frame box on the picture, clamped to it. Returns 1 when some of it is
 * visible, 0 when it is entirely outside the picture, -EINVAL as above. */
int vision_map_box(const struct vision_view *v, const struct vision_box *in, struct vision_box *out);

/* The other way: picture pixel (vx, vy) is drawn from frame pixel (*sx,
 * *sy) - exactly the pixel pocketcam_to_rgb565() samples for it. This is
 * how the counting line, chosen on the picture, is placed among the tracks,
 * which live in frame pixels. 0, or -EINVAL for a view that makes no sense
 * or a point outside the picture. */
int vision_unmap_point(const struct vision_view *v, int32_t vx, int32_t vy, int32_t *sx, int32_t *sy);

#endif
