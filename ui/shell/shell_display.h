/*
 * The shell's display ownership: which panel this build drives, and (with
 * the orientation policy) which geometry the display, touch and PocketUI all
 * use. There is one geometry per shell run; nothing else computes one.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_SHELL_DISPLAY_H
#define POCKETOS_SHELL_DISPLAY_H

#include "pos_display.h"

/* The panel: the T-Display K230's RM69A10, 568x1232 native portrait, as
 * described in platform.h. Both backends describe the same panel so the
 * simulator lays out what the board shows. POCKETOS_SAFE_CORNERS=tl,tr,br,bl
 * (non-negative pixels) overrides the corner squares on the bench or in a
 * test; an invalid value logs a warning and the default stays. */
void shell_display_panel(struct pos_panel *out);

#endif
