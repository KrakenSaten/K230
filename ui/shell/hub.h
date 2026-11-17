/*
 * The Games Hub (DS §60, Concept C "Poster Wall"): what the launcher shows
 * for the Games folder when it has the art. Every game is a poster on one
 * wall - RotorTerror and Relay Nine large, with FEATURED and PLAY, the other
 * seven as colour tiles - and the Multiplayer Lobby is a poster of its own
 * that only announces itself.
 *
 * This file draws; it decides nothing. Where things go is hub_layout.c
 * (pure C, host-tested); what a tap or a key does, when the page opens and
 * closes and who owns its art is the launcher's (home.c), which treats the
 * posters as the cells of a folder page. The art is ten RGB565 images per
 * orientation, read when the hub opens and released when it closes
 * (art.h); with any of them missing or of the wrong size there is no hub
 * and the launcher shows the ordinary folder page.
 *
 * The lobby is a preview: COMING SOON, with HOST GAME and JOIN GAME drawn
 * disabled. It is one object with nothing clickable in it, so it takes no
 * tap, and it is not among the posters, so it takes no focus. No
 * networking, discovery or session code stands behind it. A later phase can
 * make that one widget live without the wall being laid out again.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_HUB_H
#define DOORS_HUB_H

#include "hub_layout.h"
#include "lvgl.h"

#include <stdbool.h>

#define HUB_TITLE_TEXT "Games"
#define HUB_META_TEXT "9 GAMES \xC2\xB7 2 FEATURED"
#define HUB_HINT_TEXT "TAP A POSTER TO PLAY"
#define HUB_FEATURED_TEXT "FEATURED"
#define HUB_PLAY_TEXT "PLAY"
#define HUB_PORTRAIT_TEXT "PORTRAIT"
#define HUB_LOBBY_SOON_TEXT "COMING SOON"
#define HUB_LOBBY_SENTENCE "Not available yet. Reserved for linked play."
#define HUB_LOBBY_HOST_TEXT "HOST GAME"
#define HUB_LOBBY_JOIN_TEXT "JOIN GAME"

struct hub_view {
    lv_obj_t *root;                /* the page; deleting it deletes the hub */
    lv_obj_t *back;                /* the back slab (the corner of DS §48 is on it) */
    lv_obj_t *poster[HUB_GAMES];   /* one clickable object per game, in hub order */
    lv_obj_t *lobby;               /* the lobby widget: not clickable, nor anything in it */
    lv_obj_t *lobby_host;          /* its two disabled buttons, for the tests */
    lv_obj_t *lobby_join;
};

/* Read the ten images of this layout's orientation into art[]. All or
 * nothing: false, with nothing held, when one is missing, damaged or not
 * the size of its tile. */
bool hub_art_load(const struct hub_layout *l, lv_image_dsc_t *art[HUB_TILE_COUNT]);
/* Release what hub_art_load read (NULL entries are skipped) and clear art[].
 * Only after the objects that drew it are gone. */
void hub_art_free(lv_image_dsc_t *art[HUB_TILE_COUNT]);

/* Build the hub under parent, filling the whole of it, from a layout and
 * its art, which must outlive view->root. The posters and the back slab are
 * clickable and carry no handler: the launcher adds its own. */
void hub_build(lv_obj_t *parent, const struct hub_layout *l, lv_image_dsc_t *const art[HUB_TILE_COUNT],
               struct hub_view *view);

#endif
