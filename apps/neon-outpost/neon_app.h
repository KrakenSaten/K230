/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NEON_APP_H
#define NEON_APP_H
#include "app.h"
#include "neon.h"
extern const struct pocketos_app app_neon_outpost;
/* Inspection seam for real LVGL integration tests, no alternate game logic. */
struct neon_game *neon_app_game(void *priv);
void neon_app_refresh(void *priv);
lv_obj_t *neon_app_button(void *priv, unsigned index);
#endif
