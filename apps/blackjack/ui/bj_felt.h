/*
 * PG Blackjack's felt: PocketTimber's felt tile, reused rather than redrawn.
 *
 * The felt is `felt_tile.png` from docs/design/timber-art/rendered (128 x 128,
 * base #1B3A2A, fibrous value noise; docs/apps/POCKETTIMBER_ART.md 6),
 * converted into the shell by Timber's own build step. This file is the one
 * place PG Blackjack names Timber (tests/bj_lint.sh), so moving the felt to a
 * shared Pocket Games art module later is a change here and nowhere else.
 *
 * NULL when the shell was built without Timber's art (no python3 at configure
 * time, or a test binary): the table then fills with the felt's base tone
 * from the card palette.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef PGBJ_FELT_H
#define PGBJ_FELT_H

#include "lvgl.h"

const lv_image_dsc_t *bj_felt_image(void);

#endif
