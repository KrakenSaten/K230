/*
 * Card games: the full-screen frame (docs/design/card-games, owner decision
 * 3, 2026-11-22). The game uses the whole screen: the shell's header row is
 * hidden and the cloth runs edge to edge, falling off to black under one
 * lamp. In its place the game shows its own way back - a chevron in the
 * top-left corner, clear of the panel's rounded corner - and its name,
 * small, in gold. Back and Home on the device and Esc are unchanged: the
 * shell still takes them.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_SCENE_H
#define CG_SCENE_H

#include "cg_table.h"
#include "lvgl.h"

/* The top band's height: the back mark's 64 px touch target. */
#define CG_SCENE_BAR_H 64
/* The content's margins: the sides, and the foot (clear of the bottom
 * corners' 30 px squares, DS 21.1). */
#define CG_SCENE_SIDE 20
#define CG_SCENE_FOOT 32

struct cg_scene {
    lv_obj_t *frame;   /* fills the app's body; draws the cloth */
    lv_obj_t *bar;     /* the top band */
    lv_obj_t *back;    /* the game's back mark */
    lv_obj_t *title;
    lv_obj_t *content; /* everything else: a flex column under the band */
    enum cg_cloth_kind cloth;
    int (*on_back)(void *user); /* the app's own Back first (app.h back) */
    void *user;
};

/* Build the frame in the shell's body and ask the shell for the whole
 * screen. Returns the content object the game builds in, or NULL. The back
 * mark runs `on_back(user)` outside its own event, as the shell's Back
 * would, and leaves for the launcher when that returns 0 (or is NULL). */
lv_obj_t *cg_scene_build(struct cg_scene *s, lv_obj_t *body, enum cg_cloth_kind cloth, const char *title,
                         int (*on_back)(void *user), void *user);
/* The app is closing: no pending back, and the decks and cloths it composed
 * are freed (nothing a card game draws outlives it). */
void cg_scene_close(struct cg_scene *s);

#endif
