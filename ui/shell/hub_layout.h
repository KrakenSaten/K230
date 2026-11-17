/*
 * The Games Hub's geometry (DS §60, Concept C "Poster Wall"), as a pure
 * function of the content area. No LVGL, so the host suite can hold the wall
 * to its rules in both orientations: no two posters overlap, everything fits
 * the content area, nothing lies under the status cluster or in a corner the
 * panel may cut, and the keys reach every poster.
 *
 * The hub is what the launcher shows for the Games folder (home.h) when it
 * has the art for it. Nine posters, one per game, and the Multiplayer Lobby
 * preview, which is a tile like the others but is no game: it takes no tap
 * and no focus.
 *
 *   portrait   RotorTerror across the top, Relay Nine beside a 2 x 2 block,
 *              a row of three, the lobby across the foot
 *   landscape  the two featured posters side by side at the left, a 3 x 2
 *              block, then 2048 and the lobby on the bottom row
 *
 * The wall is drawn at the panel's size (568 x 1232, 1232 x 568) and never
 * scaled: its art is made for exactly these rectangles (tools/design/
 * gen_games_hub.py, checked against this file by tests/doors_ui_assets_test.sh).
 * On a wider content area it is centred; on one too small for it there is no
 * layout, and the launcher shows the ordinary folder page instead.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_HUB_LAYOUT_H
#define DOORS_HUB_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The posters in the hub's order (owner decision 2026-11-16): the two
 * featured games, then the other seven; the lobby last. */
enum hub_tile {
    HUB_TILE_ROTORTERROR = 0,
    HUB_TILE_RELAY_NINE,
    HUB_TILE_FLEET,
    HUB_TILE_RADAR,
    HUB_TILE_TIMBER,
    HUB_TILE_SOLITAIRE,
    HUB_TILE_BLACKJACK,
    HUB_TILE_POKER,
    HUB_TILE_2048,
    HUB_GAMES,                   /* how many of the tiles are games */
    HUB_TILE_LOBBY = HUB_GAMES,
    HUB_TILE_COUNT
};

#define HUB_FEATURED 2           /* tiles 0 .. HUB_FEATURED-1 carry FEATURED and PLAY */

/* The design sizes: the panel in each orientation. */
#define HUB_PORTRAIT_W 568
#define HUB_PORTRAIT_H 1232
#define HUB_LANDSCAPE_W 1232
#define HUB_LANDSCAPE_H 568

/* The way back: the folder page's slab (home_layout.h), so the corner of
 * DS §48 is the same box on both pages. */
#define HUB_BACK_W 72
#define HUB_BACK_H 56
#define HUB_BACK_Y 8
#define HUB_HEADER_H 72          /* the header row: the back corner's foot */

/* The smallest thing a finger launches from, and PLAY's floor (README). */
#define HUB_TOUCH_MIN_W 96
#define HUB_TOUCH_MIN_H 118
#define HUB_PLAY_MIN_W 134
#define HUB_PLAY_MIN_H 46

struct hub_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct hub_game {
    const char *id;        /* the app's id in the shell's registry */
    const char *art;       /* its art is hub-<art>-<orientation> */
    bool featured;
    bool portrait_only;    /* DS §53: badged PORTRAIT on the landscape wall */
};

/* A game tile's entry, or NULL for the lobby and anything out of range. */
const struct hub_game *hub_game(int tile);
/* The tile of the game with this app id, or -1. */
int hub_tile_find(const char *app_id);
/* Whether ids[0..n-1] are exactly the hub's nine games, in any order: the
 * wall has a poster for each of them and for nothing else, so any other set
 * is shown as the ordinary folder page. */
bool hub_games_match(const char *const *ids, int n);
/* "hub-<art>-portrait" or "-landscape" for a tile (the lobby's art is
 * "lobby") into out; -1 for a tile out of range or a buffer too small. */
int hub_art_name(int tile, bool landscape, char *out, size_t out_len);

/* What the hub is laid out in: the launcher's content area (the whole
 * display at home, DS §36).
 *   corner_*: the square each rounded corner of the panel may cut (DS §21.1:
 *             30 px in portrait; 50 px at the top in landscape);
 *   keepout:  the status cluster's widest box, in content coordinates;
 *             w == 0: none. */
struct hub_frame {
    bool landscape;
    int32_t width;
    int32_t height;
    int32_t corner_top_left;
    int32_t corner_top_right;
    int32_t corner_bottom_left;
    int32_t corner_bottom_right;
    struct hub_rect keepout;
};

struct hub_layout {
    bool landscape;
    struct hub_rect back;                    /* the slab; the corner box is DS §48's */
    struct hub_rect title;                   /* "Games", beside it */
    struct hub_rect meta;                    /* "9 GAMES · 2 FEATURED", right-aligned */
    struct hub_rect wall;                    /* the bounding box of every tile */
    struct hub_rect tile[HUB_TILE_COUNT];
    struct hub_rect chip[HUB_FEATURED];      /* FEATURED, on its poster */
    struct hub_rect play[HUB_FEATURED];      /* PLAY, on its poster */
    struct hub_rect badge;                   /* Timber's PORTRAIT; w == 0 in portrait */
    struct hub_rect hint;                    /* the line under the wall; w == 0 in landscape */
    /* The lobby's parts, inside its tile. */
    struct hub_rect lobby_chip;              /* COMING SOON */
    struct hub_rect lobby_text;              /* the sentence; w == 0 on the narrow lobby */
    struct hub_rect lobby_host;              /* HOST GAME, disabled */
    struct hub_rect lobby_join;              /* JOIN GAME, disabled */
};

/* The layout for a frame. 0, or -1 when the frame cannot hold the wall:
 * smaller than the design size, or the status cluster or a corner square
 * would cover part of it. Nothing is shrunk to fit. */
int hub_layout(const struct hub_frame *f, struct hub_layout *out);

/* Whether two rectangles share any pixel. */
bool hub_rects_overlap(const struct hub_rect *a, const struct hub_rect *b);
/* Whether inner lies wholly inside outer. */
bool hub_rect_inside(const struct hub_rect *inner, const struct hub_rect *outer);

/* ---- keys ------------------------------------------------------------------ *
 *
 * The arrows move the focus across the wall as the eye does: to the nearest
 * poster in that direction that lies beside the focused one (their spans
 * across the direction overlap); of several equally near, the one whose near
 * edge starts closest to the focused poster's (so Down from RotorTerror is
 * Relay Nine, and Right from Relay Nine is the tile level with its top).
 * Nothing in that direction: the focus stays. The lobby is never a
 * neighbour. */
enum hub_dir {
    HUB_DIR_LEFT = 0,
    HUB_DIR_RIGHT,
    HUB_DIR_UP,
    HUB_DIR_DOWN
};

/* The game tile the focus moves to from `from`, or `from` itself. */
int hub_neighbour(const struct hub_layout *l, int from, enum hub_dir dir);

#endif
