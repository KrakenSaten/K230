/*
 * Camera's state machine: what the screen is doing, what it shows, and what
 * has to happen next. Pure C, no LVGL, no processes, clock passed in, so every
 * transition is unit-tested on its own (tests/camera_state_test.c).
 *
 * The app feeds it the helper's events (camera_session.h) and the owner's
 * taps, and it answers with a set of actions for the app to carry out
 * (CAMERA_DO_*). It never calls anything itself.
 *
 *   INIT       the helper is starting and opening the camera
 *   PREVIEW    the live picture; the shutter works once a picture has come
 *              and the stream is not stalled
 *   CAPTURING  a still is being taken and saved; nothing else can start
 *   REVIEW     the photo just taken (or the last one of this visit): keep it,
 *              or delete it after a confirmation
 *   ERROR      the camera or its helper failed; Try again starts over
 *   NO_DEVICE  there is no camera (or no camera support in this build)
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef CAMERA_STATE_H
#define CAMERA_STATE_H

#include "camera_session.h"

#include <stdbool.h>
#include <stdint.h>

enum camera_state {
    CAMERA_INIT = 0,
    CAMERA_PREVIEW,
    CAMERA_CAPTURING,
    CAMERA_REVIEW,
    CAMERA_ERROR,
    CAMERA_NO_DEVICE,
};

/* Actions, as bits: more than one can be due at once. */
#define CAMERA_DO_NOTHING 0u
#define CAMERA_DO_START_SESSION 0x01u /* abandon any helper, start a new one */
#define CAMERA_DO_START_PREVIEW 0x02u /* send the view, then start */
#define CAMERA_DO_STOP_PREVIEW 0x04u
#define CAMERA_DO_CAPTURE 0x08u
#define CAMERA_DO_DELETE 0x10u        /* delete model.review_name */
#define CAMERA_DO_TAKE_REVIEW 0x20u   /* copy the review picture out of the session */
#define CAMERA_DO_END_SESSION 0x40u   /* the helper is of no more use */

/* How long a note (storage full, photo deleted) stays under the picture. */
#define CAMERA_NOTE_MS 3000

struct camera_model {
    enum camera_state state;
    bool live;            /* a preview picture has arrived since the preview started */
    bool stalled;         /* streaming, but no picture for a while */
    bool saving;          /* CAPTURING: the still is taken, the file is being written */
    bool confirm_delete;  /* REVIEW: asking whether to delete */
    bool deleting;        /* REVIEW: the delete has been sent */
    bool review_picture;  /* the app holds a picture of the photo being reviewed */
    bool have_last;       /* a photo was taken in this visit and is still there */
    bool simulated;       /* the fake backend: say so */
    uint32_t still_w;
    uint32_t still_h;
    uint32_t photos;      /* in the folder, as the helper last counted */
    char review_name[CAMERA_NAME_MAX];
    char last_name[CAMERA_NAME_MAX];
    char title[64];       /* INIT, ERROR, NO_DEVICE: the panel's title */
    char detail[CAMERA_EVENT_TEXT_MAX + 32];
    char note[96];        /* a passing note under the picture */
    int64_t note_until;
};

/* What the screen should show, derived from the model. */
struct camera_screen {
    bool show_picture;    /* the preview or the review picture, not the panel */
    bool picture_is_review;
    bool show_panel;      /* the title/detail panel in the picture's place */
    bool show_shutter;
    bool shutter_enabled;
    bool show_last;
    bool show_review_buttons; /* Delete and Keep */
    bool show_confirm;        /* Cancel and Delete */
    bool show_retry;
    const char *status;   /* the line under the picture, may be "" */
    const char *hint;     /* the header hint */
};

void camera_model_init(struct camera_model *m);

/* The screen has opened: start the helper. */
unsigned camera_model_open(struct camera_model *m);
unsigned camera_model_event(struct camera_model *m, const struct camera_event *ev, int64_t now);
unsigned camera_model_shutter(struct camera_model *m);
unsigned camera_model_keep(struct camera_model *m);
unsigned camera_model_delete(struct camera_model *m);
unsigned camera_model_delete_confirm(struct camera_model *m);
unsigned camera_model_delete_cancel(struct camera_model *m);
unsigned camera_model_show_last(struct camera_model *m);
unsigned camera_model_retry(struct camera_model *m);
/* Expire the note. True when something visible changed. */
bool camera_model_tick(struct camera_model *m, int64_t now);

void camera_model_screen(const struct camera_model *m, struct camera_screen *out);

const char *camera_state_name(enum camera_state s);

#endif
