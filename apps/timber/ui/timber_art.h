/*
 * PocketTimber art: the pre-rendered sprites and how the table finds them.
 *
 * The sprites are rendered by Blender from docs/design/timber-art (the
 * master scene, the canonical camera and light) and converted to LVGL
 * image arrays at build time by docs/design/timber-art/tools/png2lvgl.py,
 * which also writes the table below: every sprite with its anchor, the
 * pixel the engine's far-bottom corner of a block's cell lands on. The
 * view places a sprite at (projected corner - anchor), so a sprite's
 * geometry coincides with the view model's, and picking, selection and
 * the ghost keep using the view's quads.
 *
 * Which sprite a block gets is a function of engine state only: its
 * orientation from its layer's axis while it stands and from its pose
 * while it falls, its tone from its seeded variant, its tilt from its
 * pose. No art state is kept anywhere; nothing here is written.
 *
 * Built with POCKETTIMBER_ART 1 the table is the generated one; built
 * without it (no python3, or no renders) the table is empty and the table
 * widget draws its placeholder blocks. Setting POCKETTIMBER_PLACEHOLDER in
 * the environment forces the placeholder for comparison.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETTIMBER_ART_H
#define POCKETTIMBER_ART_H

#include "lvgl.h"

#ifndef POCKETTIMBER_ART
#define POCKETTIMBER_ART 0
#endif

/* Tones of wood and tumbling poses the production set provides; the proof
 * set carries the base tone flat, and every lookup falls back to it. */
#define TIMBER_ART_TONES 3
#define TIMBER_ART_POSES 3

struct timber_art_sprite {
    const char *key;            /* "block_x_t0_p0", "felt", "shadow" */
    const lv_image_dsc_t *img;
    int ax;                     /* the anchor, pixels from the top-left */
    int ay;
};

extern const struct timber_art_sprite timber_art_sprites[];
extern const int timber_art_sprite_count;
/* The projection the sprites were rendered for; the view's constants must
 * agree (checked at start, logged if not). */
extern const int timber_art_scale_px;
extern const int timber_art_layer_px;

/* 1 when sprites were built in. */
int timber_art_available(void);
const struct timber_art_sprite *timber_art_find(const char *key);
/* The sprite for a block along x (1) or y (0), of a tone and in a pose,
 * falling back to the nearest rendered variant and finally to the flat
 * base tone. NULL when no art is built in. */
const struct timber_art_sprite *timber_art_block(int along_x, int tone, int pose);
const struct timber_art_sprite *timber_art_felt(void);
const struct timber_art_sprite *timber_art_shadow(void);

#endif
