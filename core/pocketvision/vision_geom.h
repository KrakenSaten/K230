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

/* THE UPRIGHT PICTURE. The camera is mounted turned (unit A: 90 degrees),
 * so the sensor frame shows the scene sideways or upside down; the preview
 * turns it clockwise by vision_view.rotation and the screen shows it
 * upright. The detector must be given the same upright picture - YOLOv8n
 * on unit A's window scene found 0 of 7 parked cars upside down and 7 of 7
 * at 59-73 % upright - and its boxes brought back into frame pixels, where
 * tracks, lines and counts live. These are that turn and its inverse, with
 * exactly the preview's arithmetic (vision_map_point). The turned picture is
 * frame_h x frame_w for a quarter turn. */

/* The size of the w x h frame turned by rotation (0, 90, 180, 270). */
void vision_turned_size(uint32_t frame_w, uint32_t frame_h, int rotation, uint32_t *tw, uint32_t *th);
/* A frame box on the turned picture, and a turned-picture box back in the
 * frame. 0, or -EINVAL for an empty box, a rotation not one of the four or
 * a frame that makes no sense. */
int vision_box_turn(uint32_t frame_w, uint32_t frame_h, int rotation, const struct vision_box *in,
                    struct vision_box *out);
int vision_box_unturn(uint32_t frame_w, uint32_t frame_h, int rotation, const struct vision_box *in,
                      struct vision_box *out);
/* Turn a picture of `planes` 8-bit planes (each `stride` x h bytes, one
 * after another: the ISP's planar BGR) into dst, its planes packed, turned
 * width x turned height each. 0, or -EINVAL. */
int vision_turn_planes(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, uint32_t planes,
                       int rotation, uint8_t *dst);

/* The smallest side of a crop the detector is given, in frame pixels: a
 * road region smaller than this is not worth a model run of its own. */
#define VISION_CROP_MIN 32

/* A part of the frame for the detector (TRAFFIC's region of interest):
 * `in`, in frame pixels, clipped to the frame, its origin rounded down and
 * its size down to even numbers (whole pixel pairs for the AI2D engine).
 * 0 with *out set, or -EINVAL when what is left is smaller than
 * VISION_CROP_MIN on a side or the frame makes no sense. */
int vision_crop_fit(uint32_t frame_w, uint32_t frame_h, const struct vision_box *in, struct vision_box *out);

#endif
