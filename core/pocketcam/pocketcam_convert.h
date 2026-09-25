/*
 * pocketcam's pixel conversion: a camera frame (NV12, NV16 or RGB565) turned,
 * mirrored and scaled into what the screen shows (RGB565) or what an encoder
 * reads (RGB888 lines).
 *
 * Plain C on the CPU, nearest-neighbour, one pass. On the K230 this is the
 * cost that decides the preview frame rate (CAMERA_PLATFORM_RESEARCH.md,
 * "Performance"); the hardware that could do it instead - the ISP's own
 * scaler, the 2D engine, a DRM video plane - is either not reachable without
 * vendor code or unproven, so the first version does it here and measures it.
 *
 * Every function checks the frame first (pocketcam_frame_check) and refuses a
 * short or inconsistent one with -EPROTO; nothing reads past what the frame
 * says it holds.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_CONVERT_H
#define POCKETOS_POCKETCAM_CONVERT_H

#include "pocketcam.h"

#include <stdbool.h>
#include <stdint.h>

enum pocketcam_fit {
    /* Fill the destination, cutting what does not fit off both sides. */
    POCKETCAM_FIT_COVER = 0,
    /* Show all of the picture; the rest of the destination is black. */
    POCKETCAM_FIT_CONTAIN,
};

/* The size of a w x h picture turned by rotation (0, 90, 180, 270). */
void pocketcam_turned_size(uint32_t w, uint32_t h, int rotation, uint32_t *tw, uint32_t *th);

/* Draw f into dst (dst_w x dst_h pixels, dst_stride pixels per line),
 * turned clockwise by rotation, then mirrored left to right when mirror is
 * set. 0, -EINVAL for a bad destination or rotation, -EPROTO for a bad
 * frame. */
int pocketcam_to_rgb565(const struct pocketcam_frame *f, int rotation, bool mirror,
                        enum pocketcam_fit fit, uint16_t *dst, uint32_t dst_w, uint32_t dst_h,
                        uint32_t dst_stride);

/* Line y of f turned and mirrored at full size, as RGB888 into row (3 bytes
 * a pixel, the turned width long). 0, -EINVAL or -EPROTO. */
int pocketcam_row_rgb888(const struct pocketcam_frame *f, int rotation, bool mirror, uint32_t y,
                         uint8_t *row);

/* The turn a frame needs to look upright on the panel: the sensor's mount
 * rotation (the turn it needs on the panel's native orientation, rotation 0)
 * less the rotation the display is shown at (0, 90, 180, 270; the shell's
 * own number, POS_ROTATION_*). Unit A: mount 90, portrait 0 -> 90, the
 * shell's landscape 270 -> 180 (VERIFIED with printed text, 2026-09-25). */
int pocketcam_view_rotation(int mount_rotation, int display_rotation);

#endif
