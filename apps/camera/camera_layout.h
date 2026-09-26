/*
 * Camera's layout: where the picture and the controls go in the body the
 * shell gives the app. Pure arithmetic, no LVGL (tests/camera_layout_test.c).
 *
 * Chosen from the body, never from the orientation (DS section 21.2), and
 * chosen again whenever the body changes size. The picture keeps the shape of
 * the photo it will become, so what the preview shows is what is saved.
 *
 *   TALL (portrait; 528 x 1116 under NONE on the reference panel). The
 *   picture across the full width, 9:16, at the top; under it a status line,
 *   and the controls: the shutter in the middle, the last photo to its left.
 *   In review the two review buttons take the shutter's row.
 *
 *   WIDE (landscape; 1192 x 452). The picture at the full height, 16:9, on
 *   the left; a column on the right with the status line at the top, the
 *   last photo under it, the shutter in the middle of the column, and the
 *   review buttons stacked where the shutter was.
 *
 * PHOTOS, the way into the gallery, sits right of the shutter in the tall
 * shape and under it in the wide one (w == 0 when the body has no room).
 *
 * THE GALLERY (camera_gallery_layout_compute) has three views in the same body:
 *
 *   TALL. A status line at the top; the thumbnails in a grid under it, as
 *   many columns of about GALLERY_CELL_TARGET as the width takes; two rows of
 *   buttons at the foot (NEWER | OLDER, then CAMERA | SLIDESHOW). The photo
 *   view: the photo over the full width, three lines about it, NEWER | OLDER,
 *   then BACK | EXPORT | DELETE.
 *
 *   WIDE. The grid or the photo on the left at the full height; a column of
 *   GALLERY_SIDE_W on the right with the status line, the lines about the
 *   photo and the same buttons.
 *
 *   The slideshow takes the whole body but a status line at the foot.
 *
 * Every picture box is capped at CAMERA_PICTURE_MAX, what a slot holds.
 *
 * Every control is inside the box the unsafe area leaves (the insets
 * pocketui_layout_begin() reports), at least POCKETUI_TOUCH_MIN on both sides.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef CAMERA_LAYOUT_H
#define CAMERA_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#define CAMERA_GAP 16
#define CAMERA_SHUTTER_W 240
#define CAMERA_SHUTTER_H 96
#define CAMERA_THUMB 72
/* PHOTOS in the tall shape: at most this wide, and not at all narrower than
 * the least that holds its word. */
#define CAMERA_PHOTOS_W 128
#define CAMERA_PHOTOS_MIN_W 104
#define CAMERA_BTN_H 64
#define CAMERA_STATUS_H 28
/* The least room the controls get: a status line, a gap, the shutter and a
 * gap under it. The picture gives way when the body is shorter. */
#define CAMERA_CONTROLS_MIN (CAMERA_STATUS_H + CAMERA_GAP + CAMERA_SHUTTER_H + CAMERA_GAP)
/* The least width the wide shape's column gets. */
#define CAMERA_SIDE_MIN (CAMERA_SHUTTER_W + 2 * CAMERA_GAP)
/* The largest picture the helper can send (POCKETCAM_VIEW_MAX_W/H). */
#define CAMERA_PICTURE_MAX 1024

struct camera_rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

struct camera_layout {
    bool wide;
    struct camera_rect picture;
    struct camera_rect status;
    struct camera_rect shutter;
    struct camera_rect last;
    /* Delete and Keep; Cancel and Delete while confirming; in ERROR and
     * NO_DEVICE only btn_b, Try again. */
    struct camera_rect btn_a;
    struct camera_rect btn_b;
    struct camera_rect gallery; /* PHOTOS; w == 0: no room for it */
};

/* Lay out a w x h body whose unsafe area reaches in by the four insets, for a
 * photo of photo_w x photo_h as the owner holds the unit (the turned still:
 * 1080 x 1920 in portrait). -1 when the body is too small for the controls. */
int camera_layout_compute(struct camera_layout *l, int32_t w, int32_t h, int32_t inset_left,
                          int32_t inset_top, int32_t inset_right, int32_t inset_bottom,
                          uint32_t photo_w, uint32_t photo_h);

/* ---- the gallery ------------------------------------------------------------ */

#define GALLERY_GAP 8             /* between thumbnails */
#define GALLERY_CELL_TARGET 160   /* about this wide, as many as fit */
#define GALLERY_SIDE_W 344        /* the wide shape's column: three buttons in a row */
#define GALLERY_STATUS_H 56       /* two lines */
#define GALLERY_INFO_H 84         /* three lines about the photo */
#define GALLERY_CELLS_MAX 24      /* GALLERY_PAGE_MAX */

struct camera_gallery_layout {
    bool wide;
    struct camera_rect status;
    struct camera_rect panel;     /* where OPENING, FAILED and "No photos" say so */
    /* the grid */
    struct camera_rect grid;
    int cols;
    int rows;
    int cell;                     /* a thumbnail is cell x cell */
    struct camera_rect newer;
    struct camera_rect older;
    struct camera_rect left;      /* CAMERA */
    struct camera_rect middle;    /* SLIDESHOW, TRY AGAIN */
    /* one photo */
    struct camera_rect photo;
    struct camera_rect info;
    struct camera_rect p_newer;
    struct camera_rect p_older;
    struct camera_rect p_left;    /* BACK, CANCEL */
    struct camera_rect p_middle;  /* EXPORT */
    struct camera_rect p_right;   /* DELETE */
    /* the slideshow */
    struct camera_rect show;        /* the picture, capped at CAMERA_PICTURE_MAX */
    struct camera_rect show_touch;  /* what a tap stops it on: all but the status line */
    struct camera_rect show_status;
};

/* The cell of the grid, row by row. */
struct camera_rect camera_gallery_cell_rect(const struct camera_gallery_layout *l, int cell);

/* Lay the gallery out in a w x h body with these insets. -1 when the body is
 * too small for it. */
int camera_gallery_layout_compute(struct camera_gallery_layout *l, int32_t w, int32_t h,
                                  int32_t inset_left, int32_t inset_top, int32_t inset_right,
                                  int32_t inset_bottom);

#endif
