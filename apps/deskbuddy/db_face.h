/*
 * DeskBuddy's eyes and mouth as numbers: the shape of each eye, and of the
 * mouth, for a db_face in a box of a given size. The screen
 * (deskbuddy_app.c) only copies these onto LVGL objects, so every expression
 * can be checked on the host - that it fits its box, that a blink is shut,
 * that a glance looks the right way - without a display
 * (tests/db_brain_test.c, tests/db_personality_test.c).
 *
 * An eye is drawn with fills only, in the theme's role styles (DS §4: no
 * colour of its own): a rounded "white" in the accent, a pupil in the
 * background colour, and two background-coloured cut-outs - a lid across
 * the top (sleepy, wary) and a disc from below that leaves an arch (^,
 * happy). The mouth is the same idea: an accent body with one
 * background-coloured cut-out - shifted up it leaves a smile, in the middle
 * an open "o".
 *
 * Between two faces the screen tweens: db_eye_shape_mix and
 * db_mouth_shape_mix give the shape part of the way from one to the other,
 * so an expression grows out of the last one instead of replacing it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_FACE_H
#define DB_FACE_H

#include "db_brain.h"

#include <stdbool.h>

struct db_eye_shape {
    int w;          /* the eye, centred in its box, moved by dy */
    int h;
    int radius;
    int dy;
    int pupil_d;    /* 0: no pupil */
    int pupil_dx;   /* the pupil's centre from the eye's centre */
    int pupil_dy;
    int lid;        /* px of the eye hidden from the top; 0 none */
    int arch_d;     /* diameter of the disc cut from below (happy); 0 none */
    int arch_y;     /* the disc's top, from the eye's top */
};

/* The mouth: a box centred dx right of the face's middle, which clips an
 * accent body inside it, which holds one background-coloured cut-out. A
 * smile is a large disc whose bottom alone shows in a shallow box, with the
 * same disc cut from it a little higher - the happy eye's arch, upside
 * down. Everything else is a body the size of its box. */
struct db_mouth_shape {
    int w;          /* the box; 0: no mouth */
    int h;
    int dx;
    int body_x;     /* the accent, from the box's top-left; it may overhang (clipped) */
    int body_y;
    int body_w;
    int body_h;
    int body_r;
    int cut_x;      /* the cut-out, from the body's top-left; cut_w 0: none */
    int cut_y;
    int cut_w;
    int cut_h;
    int cut_r;
};

/* The mouth sits DB_MOUTH_Y_PM below the eye line (db_brain.h), and the
 * character is DB_CHARACTER_H_PM tall. */

/* Shapes for the left and the right eye (as seen on the screen) in a box of
 * box_w x box_h each. Every shape fits inside its box. */
void db_face_eyes(const struct db_face *f, int box_w, int box_h, struct db_eye_shape *left,
                  struct db_eye_shape *right);
bool db_eye_shape_equal(const struct db_eye_shape *a, const struct db_eye_shape *b);

/* The mouth for a face whose eye box is box px. */
void db_face_mouth(const struct db_face *f, int box, struct db_mouth_shape *m);
bool db_mouth_shape_equal(const struct db_mouth_shape *a, const struct db_mouth_shape *b);

/* permille of the way from a to b (0: a, 1000: b), into out. */
void db_eye_shape_mix(const struct db_eye_shape *a, const struct db_eye_shape *b, int permille,
                      struct db_eye_shape *out);
void db_mouth_shape_mix(const struct db_mouth_shape *a, const struct db_mouth_shape *b, int permille,
                        struct db_mouth_shape *out);

#endif
