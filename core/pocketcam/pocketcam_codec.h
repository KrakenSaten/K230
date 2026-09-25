/*
 * pocketcam's still encoder: a captured frame, turned upright, written to a
 * stream as an image file.
 *
 * JPEG through libjpeg when the build has it (POCKETCAM_HAVE_JPEG: the K230
 * image carries libjpeg 9 and its headers are in the Buildroot sysroot, the
 * vendor Camera app uses it). Without it - a host with no libjpeg headers -
 * the encoder writes binary PPM instead: exact, uncompressed and readable by
 * any viewer, so the simulator's photos are real pictures of the test
 * pattern. The extension follows the encoder, so a file is never called .jpg
 * when it is not one.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_POCKETCAM_CODEC_H
#define POCKETOS_POCKETCAM_CODEC_H

#include "pocketcam.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Quality for JPEG, 1..100. 88 keeps a 1080 x 1920 photo around a few
 * hundred kilobytes (ASSUMED until measured on unit A). */
#define POCKETCAM_JPEG_QUALITY 88

/* "jpg" or "ppm": what pocketcam_encode() writes in this build. */
const char *pocketcam_codec_ext(void);

/* An upper bound for the file a w x h still becomes, for the store's room
 * check before a capture. */
uint64_t pocketcam_codec_estimate(uint32_t w, uint32_t h);

/* Write f, turned clockwise by rotation and mirrored when asked, to fp. 0, or
 * a negative errno (-ENOSPC when the stream ran out of room). */
int pocketcam_encode(FILE *fp, const struct pocketcam_frame *f, int rotation, bool mirror);

#endif
