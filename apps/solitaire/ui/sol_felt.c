/*
 * PG Solitaire's felt. See sol_felt.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sol_felt.h"

#include "timber_art.h"

const lv_image_dsc_t *sol_felt_image(void)
{
    const struct timber_art_sprite *felt = timber_art_felt();

    return felt ? felt->img : NULL;
}
