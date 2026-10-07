/*
 * Camera's gallery: what the photo browser is doing, what it shows, and which
 * picture it needs next. Pure C, no LVGL, no processes, clock passed in, so
 * every transition is unit-tested on its own (tests/camera_gallery_test.c).
 *
 * The app feeds it the library helper's events (camera_session.h, cfg.library)
 * and the owner's taps; it answers with actions (GALLERY_DO_*) and, through
 * camera_gallery_next_request(), the pictures to ask the helper for. It never
 * calls anything itself.
 *
 *   OPENING    the library helper is starting, or listing the photos
 *   GRID       a page of thumbnails, newest first; NEWER and OLDER turn pages
 *   PHOTO      one photo fitted to the screen, what is known about it, NEWER
 *              and OLDER, EXPORT (a copy into Files), DELETE (confirmed)
 *   SLIDESHOW  the photos one after another every GALLERY_SLIDE_MS, from the
 *              one it was started on; a tap stops it
 *   FAILED     the helper could not start or list the photos: TRY AGAIN
 *
 * ORDER. Index 0 is the newest photo (pocketcam_store_list), so OLDER is
 * index + 1, and a page holds indices page * per_page onwards.
 *
 * PICTURES. At most CAMERA_PICTURE_SLOTS requests are out at once, one per
 * helper slot; the model remembers what each slot was asked for and, when the
 * answer comes, says where it goes - a grid cell, the photo view, the
 * slideshow's shown picture or the one prepared behind it - or that it is
 * stale (the page or the photo changed meanwhile) and is to be dropped.
 * Nothing is ever decoded at full size and only one page of thumbnails, the
 * photo and the slideshow's two pictures are held (the app's buffers).
 *
 * TWO HOSTS. Camera shows the gallery behind PHOTOS, with CAMERA as the way
 * back to the live picture. The Photo app (apps/photo, docs/apps/PHOTO.md)
 * shows the same gallery on its own: standalone, there is no CAMERA action,
 * and the shell's back slab is the only way out.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CAMERA_GALLERY_H
#define CAMERA_GALLERY_H

#include "camera_session.h"

#include <stdbool.h>
#include <stdint.h>

/* The most cells a page can have, whatever the body. */
#define GALLERY_PAGE_MAX 24
/* How long each photo of a slideshow stays. */
#define GALLERY_SLIDE_MS 4000
#define GALLERY_NOTE_MS 3000

enum gallery_view {
    GALLERY_OPENING = 0,
    GALLERY_GRID,
    GALLERY_PHOTO,
    GALLERY_SLIDESHOW,
    GALLERY_FAILED,
};

#define GALLERY_DO_NOTHING 0u
#define GALLERY_DO_START 0x01u      /* abandon any helper, start the library helper */
#define GALLERY_DO_LIST 0x02u       /* camera_session_list */
#define GALLERY_DO_TAKE_LIST 0x04u  /* camera_session_take_list, then camera_gallery_set_list */
#define GALLERY_DO_DELETE 0x08u     /* delete camera_gallery_current_name() */
#define GALLERY_DO_EXPORT 0x10u     /* export camera_gallery_current_name() */
#define GALLERY_DO_CAMERA 0x20u     /* leave the gallery for the camera */
#define GALLERY_DO_SWAP 0x40u       /* slideshow: show the prepared picture */
#define GALLERY_DO_PICTURES 0x80u   /* ask for what camera_gallery_next_request() gives */

enum gallery_purpose {
    GALLERY_FOR_THUMB = 0,
    GALLERY_FOR_PHOTO,
    GALLERY_FOR_SLIDE,
};

/* Where an answered picture goes. */
enum gallery_dest {
    GALLERY_DEST_DROP = 0,    /* stale: give the slot back, show nothing */
    GALLERY_DEST_THUMB,       /* into grid cell *cell */
    GALLERY_DEST_PHOTO,       /* the photo view */
    GALLERY_DEST_SLIDE_FRONT, /* the slideshow's picture, shown now */
    GALLERY_DEST_SLIDE_BACK,  /* the next one, prepared behind it */
};

enum gallery_cell_state {
    GALLERY_CELL_EMPTY = 0,   /* no photo at this place of the page */
    GALLERY_CELL_NEED,        /* a photo, no picture asked for yet */
    GALLERY_CELL_WAITING,
    GALLERY_CELL_SHOWN,
    GALLERY_CELL_BAD,         /* the file cannot be shown */
};

enum gallery_pic_state {
    GALLERY_PIC_NONE = 0,
    GALLERY_PIC_NEED,
    GALLERY_PIC_WAITING,
    GALLERY_PIC_SHOWN,
    GALLERY_PIC_BAD,
};

struct gallery_request {
    enum gallery_purpose purpose;
    int index;               /* into the list */
    int cell;                /* THUMB: the cell on the page */
    const char *name;
    uint32_t w;
    uint32_t h;
    bool cover;
};

struct gallery_slot {
    bool used;
    enum gallery_purpose purpose;
    int index;
    int cell;
    bool back;               /* SLIDE: for the prepared picture */
};

struct camera_gallery {
    enum gallery_view view;
    bool standalone;         /* the Photo app: no CAMERA action (kept by open and retry) */
    char names[CAMERA_LIBRARY_MAX][CAMERA_NAME_MAX];
    int count;               /* listed */
    uint32_t total;          /* in the folder (more than count only past the list's cap) */
    bool listed;             /* a list has arrived since the helper started */

    /* sizes, from the layout */
    int per_page;
    uint32_t thumb;          /* a cell's picture is thumb x thumb */
    uint32_t photo_w;
    uint32_t photo_h;
    uint32_t show_w;
    uint32_t show_h;

    int page;
    enum gallery_cell_state cells[GALLERY_PAGE_MAX];

    /* PHOTO */
    int current;
    enum gallery_pic_state photo;
    enum camera_imgfail photo_fail;
    struct camera_image_meta meta;
    char description[CAMERA_EVENT_TEXT_MAX];
    bool confirm_delete;
    bool deleting;
    bool exporting;

    /* SLIDESHOW: `current` is the photo shown (-1 before the first) */
    int slide_want;          /* the photo asked for next */
    enum gallery_pic_state slide_front;
    enum gallery_pic_state slide_back;
    int slide_back_index;
    int64_t slide_due;       /* when the shown photo has had its time */
    int slide_fails;         /* photos skipped in a row */
    int slides_shown;        /* for the test: how many were shown in all */

    struct gallery_slot slots[CAMERA_PICTURE_SLOTS];

    char title[64];          /* OPENING, FAILED, an empty GRID */
    char detail[CAMERA_EVENT_TEXT_MAX + 48];
    char note[CAMERA_PATH_TEXT_MAX + 32];
    bool note_warn;
    int64_t note_until;
};

/* What the screen shows, derived from the model. */
struct gallery_screen {
    bool show_panel;         /* title and detail */
    bool show_grid;
    bool show_photo;         /* the photo view's picture (or its failure text) */
    bool show_slideshow;
    bool show_newer;
    bool newer_enabled;
    bool show_older;
    bool older_enabled;
    const char *left;        /* the left action: "CAMERA" (not standalone), "BACK", "CANCEL" or NULL */
    bool left_enabled;
    const char *middle;      /* "SLIDESHOW", "EXPORT", "TRY AGAIN" or NULL */
    bool middle_enabled;
    bool middle_primary;
    const char *right;       /* "DELETE" or NULL */
    bool right_enabled;
    bool right_primary;
    char status[CAMERA_PATH_TEXT_MAX + 32]; /* the status line, may be "" */
    bool status_warn;
};

void camera_gallery_init(struct camera_gallery *g);
/* Standalone (the Photo app): OPENING, FAILED and GRID have no CAMERA, and
 * camera_gallery_left() there does nothing. Camera's gallery is not. */
void camera_gallery_set_standalone(struct camera_gallery *g, bool standalone);

/* The gallery opens: start the library helper. */
unsigned camera_gallery_open(struct camera_gallery *g);
unsigned camera_gallery_retry(struct camera_gallery *g);
unsigned camera_gallery_event(struct camera_gallery *g, const struct camera_event *ev,
                              int64_t now);
/* The names camera_session_take_list() gave (after GALLERY_DO_TAKE_LIST). */
unsigned camera_gallery_set_list(struct camera_gallery *g, char (*names)[CAMERA_NAME_MAX], int n,
                                 uint32_t total);
/* The layout's sizes. A new page size keeps the photo the first cell showed
 * on the page shown. */
unsigned camera_gallery_set_sizes(struct camera_gallery *g, int per_page, uint32_t thumb,
                                  uint32_t photo_w, uint32_t photo_h, uint32_t show_w,
                                  uint32_t show_h);

/* Taps. */
unsigned camera_gallery_tap_cell(struct camera_gallery *g, int cell);
unsigned camera_gallery_newer(struct camera_gallery *g);
unsigned camera_gallery_older(struct camera_gallery *g);
unsigned camera_gallery_left(struct camera_gallery *g);    /* CAMERA, BACK or CANCEL */
unsigned camera_gallery_middle(struct camera_gallery *g, int64_t now); /* SLIDESHOW, EXPORT, TRY AGAIN */
unsigned camera_gallery_right(struct camera_gallery *g);   /* DELETE, and DELETE again to confirm */
unsigned camera_gallery_tap_slideshow(struct camera_gallery *g); /* stops it */
unsigned camera_gallery_slideshow(struct camera_gallery *g, int64_t now);

/* Time passing: the note expires, the slideshow moves on. *acts gets any
 * actions due. True when something visible changed. */
bool camera_gallery_tick(struct camera_gallery *g, int64_t now, unsigned *acts);

/* The next picture to ask for, if any is needed and a slot is free in the
 * model's own count; then camera_gallery_requested() with the slot the
 * session gave (-1 when it could not send it). */
bool camera_gallery_next_request(struct camera_gallery *g, struct gallery_request *req);
void camera_gallery_requested(struct camera_gallery *g, const struct gallery_request *req,
                              int slot);
/* An IMAGE or IMGFAIL event: where the picture goes (for IMGFAIL always
 * DROP, with the failure recorded). *cell for THUMB. */
enum gallery_dest camera_gallery_arrived(struct camera_gallery *g, const struct camera_event *ev,
                                         int64_t now, int *cell, unsigned *acts);
/* The app could not take the picture it was told to put somewhere. */
void camera_gallery_lost(struct camera_gallery *g, enum gallery_dest dest, int cell, int64_t now);

/* The photo the view (or the slideshow) is on, or NULL. */
const char *camera_gallery_current_name(const struct camera_gallery *g);
/* The index a grid cell shows, or -1. */
int camera_gallery_cell_index(const struct camera_gallery *g, int cell);
int camera_gallery_pages(const struct camera_gallery *g);

void camera_gallery_screen(const struct camera_gallery *g, struct gallery_screen *out);

/* The photo view's three lines: the name; when it was taken (or why that is
 * not known); its size, file size and format. */
void camera_gallery_info(const struct camera_gallery *g, char *name, size_t name_len, char *when,
                         size_t when_len, char *what, size_t what_len);
/* "412 KB", "1.4 MB", "900 B". */
void camera_gallery_bytes_text(uint64_t bytes, char *out, size_t len);

const char *camera_gallery_view_name(enum gallery_view v);

#endif
