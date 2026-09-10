/*
 * PocketTimber art lookups. See timber_art.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_art.h"

#include <stdio.h>
#include <string.h>

#if !POCKETTIMBER_ART
/* No renders were built in: an empty table, and the widget's placeholder. */
const struct timber_art_sprite timber_art_sprites[1] = { { NULL, NULL, 0, 0 } };
const int timber_art_sprite_count = 0;
const int timber_art_scale_px = 0;
const int timber_art_layer_px = 0;
#endif

int timber_art_available(void)
{
    return timber_art_sprite_count > 0;
}

const struct timber_art_sprite *timber_art_find(const char *key)
{
    int i;

    if (!key) {
        return NULL;
    }
    for (i = 0; i < timber_art_sprite_count; i++) {
        if (strcmp(timber_art_sprites[i].key, key) == 0) {
            return &timber_art_sprites[i];
        }
    }
    return NULL;
}

const struct timber_art_sprite *timber_art_block(int along_x, int tone, int pose)
{
    char key[32];
    const struct timber_art_sprite *s;

    if (tone < 0 || tone >= TIMBER_ART_TONES) {
        tone = 0;
    }
    if (pose < 0 || pose >= TIMBER_ART_POSES) {
        pose = 0;
    }
    snprintf(key, sizeof(key), "block_%c_t%d_p%d", along_x ? 'x' : 'y', tone, pose);
    s = timber_art_find(key);
    if (s) {
        return s;
    }
    /* The tone was not rendered: the same pose in the base tone. */
    snprintf(key, sizeof(key), "block_%c_t0_p%d", along_x ? 'x' : 'y', pose);
    s = timber_art_find(key);
    if (s) {
        return s;
    }
    /* Nor the pose: the flat base block. */
    snprintf(key, sizeof(key), "block_%c_t0_p0", along_x ? 'x' : 'y');
    return timber_art_find(key);
}

const struct timber_art_sprite *timber_art_felt(void)
{
    return timber_art_find("felt");
}

const struct timber_art_sprite *timber_art_shadow(void)
{
    return timber_art_find("shadow");
}
