/*
 * pocketcam's image reader: what a photo file is, and a picture of it at the
 * size the screen needs - for the gallery's thumbnails, its viewer and its
 * slideshow.
 *
 * WHO. pos-camera only (`pos-camera library`, docs/apps/CAMERA.md): decoding
 * runs in the helper process, never in the shell, for the reasons ADR-006
 * gives for the camera itself - libjpeg on a damaged file is vendor code on
 * untrusted input, and a decode costs CPU the LVGL thread must not spend.
 *
 * FORMATS. JPEG, the one Camera writes on the device, through libjpeg when
 * the build has it (POCKETCAM_HAVE_JPEG); without it a JPEG is probed (size
 * and metadata, which need no decoder) but refused with -ENOTSUP when a
 * picture is asked for. Binary PPM (P6, 8 bits), what a host without libjpeg
 * writes, always. Anything else is -ENOTSUP.
 *
 * BOUNDED. A picture is never decoded whole at full size. JPEG is scaled
 * inside libjpeg's DCT to the smallest of 1/8 .. 8/8 that still covers the
 * box, and both formats are then streamed one line at a time straight into
 * the destination, nearest-neighbour, turned for the EXIF orientation on the
 * way. Memory is the destination plus one line; a picture larger than
 * POCKETCAM_IMAGE_MAX_DIM on a side is refused before anything is allocated.
 *
 * DAMAGE. A file that ends early or has damaged data still gives what libjpeg
 * could recover, with info->damaged set, so the owner sees the photo and is
 * told; a file whose header cannot be read is -EBADMSG; one that is gone is
 * -ENOENT. No input makes this read outside its buffers or abort.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_IMAGE_H
#define POCKETOS_POCKETCAM_IMAGE_H

#include "pocketcam_convert.h"
#include "pocketcam_exif.h"

#include <stdbool.h>
#include <stdint.h>

/* The largest picture, either side, that is read at all. */
#define POCKETCAM_IMAGE_MAX_DIM 8192

enum pocketcam_image_kind {
    POCKETCAM_IMAGE_JPEG = 0,
    POCKETCAM_IMAGE_PPM,
};

struct pocketcam_image_info {
    enum pocketcam_image_kind kind;
    uint32_t width;      /* as stored */
    uint32_t height;
    uint32_t shown_w;    /* upright, after the EXIF orientation */
    uint32_t shown_h;
    uint64_t bytes;      /* the file's size */
    int64_t mtime;       /* the file's modification time, epoch seconds */
    bool damaged;        /* decoded, but the data was damaged or cut short */
    struct pocketcam_exif exif;
};

/* What the file at path is, reading only its header (and, for JPEG, its
 * markers up to the image data). 0, -ENOENT, -EBADMSG (not a readable
 * picture), -ENOTSUP (another format), -EFBIG (too large), or -errno. */
int pocketcam_image_probe(const char *path, struct pocketcam_image_info *info);

/* The size a shown_w x shown_h picture takes in a box_w x box_h box: the box
 * itself for COVER, the largest box of the picture's shape inside it (at
 * least 1 x 1) for CONTAIN. */
void pocketcam_image_fit_size(uint32_t shown_w, uint32_t shown_h, enum pocketcam_fit fit,
                              uint32_t box_w, uint32_t box_h, uint32_t *out_w, uint32_t *out_h);

/* Probe, then draw the picture upright into dst as RGB565, tightly packed at
 * *out_w x *out_h (pocketcam_image_fit_size for the box; COVER cuts equally
 * off both sides). dst must hold box_w x box_h pixels. info may be NULL.
 * 0, or what probe returns, or -ENOMEM. */
int pocketcam_image_decode(const char *path, enum pocketcam_fit fit, uint16_t *dst, uint32_t box_w,
                           uint32_t box_h, uint32_t *out_w, uint32_t *out_h,
                           struct pocketcam_image_info *info);

/* "missing", "corrupt", "unsupported", "toolarge" or "io": a word for an
 * error of the two above, for the helper's protocol. */
const char *pocketcam_image_error_word(int err);

#endif
