/*
 * Where the Video app's pieces go: pure arithmetic from the body's size, the
 * panel's corner clearance and the screen's shape (tests/video_layout_test.c).
 *
 * The app takes the whole body (it zeroes the shell's body padding, as RIFT
 * does, so fullscreen really is the whole body) and puts VIDEO_LAYOUT_PAD
 * back inside. Landscape has no shell header (app.h NONE_LANDSCAPE), so the
 * list draws its own back slab there; portrait keeps the shell's header.
 *
 *   list, portrait         list, landscape
 *   [count      RESCAN]    [<] Video  count        [RESCAN]
 *   [file             ]    [file                          ]
 *   [file             ]    [file                          ]
 *
 *   player, portrait       player, landscape
 *   [   VIDEO FRAME   ]    [       VIDEO FRAME     ] [BACK ]
 *   name / status          [                       ] [PLAY ]
 *   [====progress=====]    [                       ] [STOP ]
 *   0:12          3:04     [                       ] [FULL ]
 *   [BACK][STOP][PLAY][FULL][====progress==========] name
 *                          0:12               3:04   status
 *
 * Fullscreen: the frame is the whole body and nothing else is placed; a tap
 * on the picture leaves fullscreen.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VIDEO_LAYOUT_H
#define POCKETOS_VIDEO_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_LAYOUT_PAD 20
#define VIDEO_LAYOUT_GAP 12
#define VIDEO_LAYOUT_BTN_H 64
#define VIDEO_LAYOUT_BACK_W 72
#define VIDEO_LAYOUT_SIDE_W 208
#define VIDEO_LAYOUT_SLIDER_H 40
#define VIDEO_LAYOUT_TIME_H 28
#define VIDEO_LAYOUT_STATUS_H 56
#define VIDEO_LAYOUT_RESCAN_W 168
/* The smallest frame worth playing in. */
#define VIDEO_LAYOUT_MIN_BOX 120

struct video_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct video_layout {
    bool landscape;
    bool fullscreen;
    /* the list */
    struct video_rect back;      /* landscape: back to home; zero in portrait */
    struct video_rect title;     /* the count (and "Video" in landscape) */
    struct video_rect rescan;
    struct video_rect list;
    /* the player */
    struct video_rect box;       /* the picture's place; pictures are fitted inside */
    struct video_rect status;    /* the file's name and what the player is doing */
    struct video_rect slider;
    struct video_rect elapsed;
    struct video_rect duration;
    struct video_rect btn_back;
    struct video_rect btn_stop;
    struct video_rect btn_play;
    struct video_rect btn_full;
};

/* w x h: the body. il..ib: the corner clearance inside it. Returns 0, or -1
 * when the body is too small for the player (the rects are then still set,
 * clipped to the body, so nothing is placed outside it). */
int video_layout_compute(struct video_layout *l, int32_t w, int32_t h, int32_t il, int32_t it,
                         int32_t ir, int32_t ib, bool landscape, bool fullscreen);

#endif
