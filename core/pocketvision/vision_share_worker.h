/*
 * The shared picture's encoder, off the detector's loop.
 *
 * Measured on unit A (2026-11-23): encoding a shared picture in the helper's
 * own loop cost the detector about a tenth of its rate, because each 16 to
 * 26 ms encode stood between two frames. The hart is idle for most of each
 * frame all the same - the helper waits for the KPU - so the encode is done
 * here, in one worker thread that runs while the loop waits.
 *
 * One picture at a time, by copy: submit() copies the preview (the slot it
 * came from goes back to the screen at once), the worker reduces and encodes
 * it into its own buffer of VISION_SHARE_MAX_BYTES, and take() hands the
 * result to the loop, which alone writes the shared memory and speaks to the
 * session. Nothing is queued: a submit while a picture is in the making is
 * refused. The worker is the only caller of vision_share_encode() while it
 * lives.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_VISION_SHARE_WORKER_H
#define POCKETOS_VISION_SHARE_WORKER_H

#include "vision_share.h"

struct vision_share_worker;

/* NULL when the thread or its memory cannot be had. */
struct vision_share_worker *vision_share_worker_new(void);
/* Stops the thread (a picture in the making is finished first and dropped)
 * and frees everything. NULL is fine. */
void vision_share_worker_free(struct vision_share_worker *w);

/* Take a copy of the w x h RGB565 picture at px (stride pixels a row) and
 * encode it, reduced by div, at `quality`; a picture that does not fit is
 * tried once more at `retry_quality` when that is lower (0: no second try).
 * `tag` comes back with the result. 0, -EBUSY while one is in the making or
 * waiting to be taken, -EINVAL, -ENOMEM. */
int vision_share_worker_submit(struct vision_share_worker *w, const uint16_t *px, uint32_t pw, uint32_t ph,
                               uint32_t stride, uint32_t div, int quality, int retry_quality, uint32_t tag);

/* The finished picture, once: 1 with *n the bytes copied into dst (at most
 * cap) or a negative errno as vision_share_encode gives it (-ENOSPC also
 * when the result does not fit dst), its size, how long it took and its tag;
 * 0 when there is none to take. Never blocks on the encoder. */
int vision_share_worker_take(struct vision_share_worker *w, uint8_t *dst, size_t cap, int *n, uint32_t *ow,
                             uint32_t *oh, uint32_t *ms, uint32_t *tag);

/* Whether a picture is in the making or waiting to be taken. */
bool vision_share_worker_busy(struct vision_share_worker *w);

#endif
