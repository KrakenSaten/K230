/*
 * The fake backend's pattern, exposed so tests can compute the pixels they
 * expect instead of hard-coding them (pocketcam_fake.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_POCKETCAM_FAKE_H
#define POCKETOS_POCKETCAM_FAKE_H

#include "pocketcam.h"

#include <stdint.h>

/* The colour of sensor pixel (x, y) of frame seq in a w x h frame. */
void pocketcam_fake_rgb_at(uint32_t x, uint32_t y, uint32_t seq, uint32_t w, uint32_t h,
                           uint8_t rgb[3]);
/* BT.601 limited range, as the fake writes it. */
void pocketcam_fake_rgb_to_yuv(const uint8_t rgb[3], uint8_t *y, uint8_t *u, uint8_t *v);
/* Write frame seq as NV12 or NV16 with stride w into dst (the size
 * pocketcam_frame_bytes() gives). */
void pocketcam_fake_fill(uint8_t *dst, enum pocketcam_format fmt, uint32_t w, uint32_t h,
                         uint32_t seq);

#endif
