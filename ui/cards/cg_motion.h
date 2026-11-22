/*
 * Card games: the motion runner (docs/design/card-games, section 8).
 *
 * A game's engine settles a command in one call; the table shows the
 * difference between the picture before it and the picture after it as a
 * short script: a card that is new travels from the shoe, a card that moved
 * travels from where it was, a card that turned turns over, and a few
 * objects of the game's own (a result, chips) fade or slide. The engine is
 * never paused; the motion is only how the new picture arrives.
 *
 * One LVGL timer runs while something moves and is deleted when nothing
 * does, so an idle table costs nothing. Each frame invalidates only the old
 * and new bounds of what moved. Reduced motion (DS 12) is the caller's
 * check: it simply adds nothing, and the end state is drawn at once.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CG_MOTION_H
#define CG_MOTION_H

#include "lvgl.h"

#include <stdint.h>

#define CG_MOTION_MAX 20
#define CG_MOTION_FRAME_MS 33

enum cg_move {
    CG_MOVE_TRAVEL = 0, /* from one place to another, as it is */
    CG_MOVE_TURN,       /* travel, turning over on the way */
    CG_MOVE_FLIP,       /* turn over where it lies */
    CG_MOVE_HOP,        /* rise a little and settle where it lies */
    CG_MOVE_FADE        /* appear where it lies */
};

struct cg_flight {
    uint32_t key; /* the game's name for the object */
    uint8_t kind;
    uint8_t face_from, face_to; /* 1 face up */
    uint8_t hidden_before;      /* not drawn until it starts (still in the shoe) */
    lv_area_t from, to;         /* relative to the object's top-left */
    uint32_t start, dur;        /* on lv_tick_get()'s clock */
    lv_area_t drawn;            /* absolute bounds last invalidated */
};

/* Where something is drawn now. */
struct cg_pose {
    lv_area_t rect;  /* relative to the object's top-left */
    int scale_x;     /* 256ths of its width, about its centre line */
    int face;        /* 1 face up */
    lv_opa_t opa;
};

/* A picture of the table: every card (or object) that is drawn, by key. */
#define CG_SPOTS 64
struct cg_spot {
    uint32_t key;
    lv_area_t rect; /* relative to the object's top-left */
    uint8_t face;
};
struct cg_spots {
    int n;
    struct cg_spot s[CG_SPOTS];
};

/* How a difference is played: new objects leave `delay` ms after the
 * command, `stagger` ms apart, in the order the picture lists them; a card
 * travels in `travel` ms and turns over in `turn` ms. Objects that only
 * moved or turned start at once. */
struct cg_plan {
    uint32_t delay, stagger, travel, turn;
};

struct cg_motion {
    lv_obj_t *obj;
    lv_timer_t *timer;
    int margin; /* pixels round a card that it may also paint (its shadow) */
    int n;
    struct cg_flight f[CG_MOTION_MAX];
    void (*done)(void *user);
    void *user;
    /* The clock the poses are read at: set when motion starts and once per
     * frame, so a redraw between frames draws exactly what the last frame
     * invalidated (never a pose no frame has covered). */
    uint32_t now;
    /* Measured while moving, for the tests and the PR's notes. */
    uint32_t frames;
    uint32_t max_px; /* the largest area one frame invalidated */
};

void cg_motion_init(struct cg_motion *m, lv_obj_t *obj, int margin);
/* Called once whenever the last moving object has arrived. */
void cg_motion_on_done(struct cg_motion *m, void (*done)(void *user), void *user);
/* Everything arrives now: the end state, no timer. Safe to call twice. */
void cg_motion_stop(struct cg_motion *m);
int cg_motion_busy(const struct cg_motion *m);

/* One object's motion. Returns 0, or -1 (nothing added) when the runner is
 * full. `face_from`/`face_to` matter to TURN and FLIP. */
int cg_motion_add(struct cg_motion *m, uint32_t key, enum cg_move kind, const lv_area_t *from, const lv_area_t *to,
                  int face_from, int face_to, uint32_t delay, uint32_t dur);
/* Play the difference between two pictures; new objects come from `source`
 * (NULL: they appear at once). If it would take more than the runner holds
 * (a new deal of the whole deck) nothing moves and the after picture simply
 * is. Returns the number of objects set moving. */
int cg_motion_diff(struct cg_motion *m, const struct cg_spots *before, const struct cg_spots *after,
                   const lv_area_t *source, const struct cg_plan *plan);
/* Whether `key` is in motion: -1 no (draw it where it lies), 0 yes but not
 * drawn yet, 1 drawn at `pose`. */
int cg_motion_pose(const struct cg_motion *m, uint32_t key, struct cg_pose *pose);
/* The moving objects, in the order they were added (draw them last, over
 * the still ones). */
int cg_motion_count(const struct cg_motion *m);
/* Milliseconds from now until the last moving object arrives (0 if none). */
uint32_t cg_motion_end(const struct cg_motion *m);
uint32_t cg_motion_key(const struct cg_motion *m, int index);

/* Pure helpers for the pose at `now` (tests). */
int cg_motion_pose_at(const struct cg_flight *f, uint32_t now, struct cg_pose *pose);

#endif
