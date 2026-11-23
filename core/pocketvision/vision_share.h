/*
 * The picture Vision shares with a paired companion (ADR-006 amendment 1,
 * accepted 2026-11-23; docs/design/companion/VISION_PREVIEW.md): the RGB565
 * preview the helper already made for the unit's screen, reduced by a whole
 * divisor and written as a baseline JPEG into a buffer of a fixed size.
 *
 * Bounded by construction. The encoder writes into the caller's buffer and
 * nowhere else: a picture that does not fit is refused (-ENOSPC), never
 * grown into. The reduced picture is never larger than VISION_SHARE_MAX_SIDE
 * on its longer side and the file never larger than VISION_SHARE_MAX_BYTES,
 * which is what the shell's one large answer (ui/shell/app_ipc.h,
 * APP_IPC_VISION_PULL_MAX) and the gateway are sized for.
 *
 * JPEG through libjpeg when the build has it (POCKETCAM_HAVE_JPEG, as the
 * Camera's still encoder: the K230 image carries libjpeg). Without it there
 * is no sharing: vision_share_supported() is false and the helper never
 * offers it.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_SHARE_H
#define POCKETOS_VISION_SHARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The largest shared JPEG, in bytes. */
#define VISION_SHARE_MAX_BYTES (64u * 1024u)
/* The longer side of a shared picture: what may be asked for, and the
 * default (half of the reference panel's 528 x 938 picture fits it). */
#define VISION_SHARE_MAX_SIDE 640u
#define VISION_SHARE_MIN_SIDE 64u
#define VISION_SHARE_DEFAULT_SIDE 480u
/* JPEG quality: what may be asked for, the default, and the one more try a
 * picture gets when it did not fit. */
#define VISION_SHARE_QUALITY_MIN 20
#define VISION_SHARE_QUALITY_MAX 85
#define VISION_SHARE_QUALITY_DEFAULT 50
#define VISION_SHARE_QUALITY_RETRY 25
/* The helper makes a shared picture at most this often, whoever asks. */
#define VISION_SHARE_MIN_INTERVAL_MS 200

bool vision_share_supported(void);

/* The smallest whole divisor (at least 1) that brings a w x h picture's
 * longer side to max_side or under. 0 when the sizes are not usable. */
uint32_t vision_share_divisor(uint32_t w, uint32_t h, uint32_t max_side);

/* Encode the w x h RGB565 picture at px (stride pixels a row), each div x div
 * block averaged into one pixel, as a baseline JPEG of `quality` into out.
 * Returns the bytes written and the reduced size in *ow, *oh; or -EINVAL
 * (sizes), -ENOSPC (it does not fit cap; out holds nothing usable),
 * -ENOTSUP (no libjpeg in this build), -EIO (the encoder failed). */
int vision_share_encode(const uint16_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t div, int quality,
                        uint8_t *out, size_t cap, uint32_t *ow, uint32_t *oh);

#endif
