/* Relay Nine DOORS integration and real-app inspection seam.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef NEON_APP_H
#define NEON_APP_H
#include "app.h"
#include "relay.h"
extern const struct pocketos_app app_neon_outpost;
struct rn_game *neon_app_game(void *priv);
lv_obj_t *neon_app_button(void *priv,unsigned index);
lv_obj_t *neon_app_image(void *priv);
void neon_app_refresh(void *priv);
#endif
