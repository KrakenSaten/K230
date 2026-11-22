/*
 * Card games redesign, milestone 0: a measurement spike, not product code.
 *
 * One Lamplight table - the cloth buffer, a composed card atlas, shadow
 * sprites - and a four-card deal, a turn and a sweep on a loop, logging how
 * long building and each frame took. Opened only by
 * PGBLACKJACK_SCREEN=lamplight-spike. The card art here is a stand-in; the
 * deck is milestone 1b. Delete this file when the milestone closes.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_SPIKE_H
#define CG_SPIKE_H

#include "lvgl.h"

void *cg_spike_create(lv_obj_t *body);
void cg_spike_destroy(void *spike);

#endif
