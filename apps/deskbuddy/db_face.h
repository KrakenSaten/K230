/*
 * DeskBuddy's eyes as numbers: the shape of each eye for a db_face in a box
 * of a given size. The screen (deskbuddy_app.c) only copies these onto
 * LVGL objects, so every expression can be checked on the host - that it
 * fits its box, that a blink is shut, that a glance looks the right way -
 * without a display (tests/db_brain_test.c).
 *
 * An eye is drawn with fills only, in the theme's role styles (DS §4: no
 * colour of its own): a rounded "white" in the accent, a pupil in the
 * background colour, and two background-coloured cut-outs - a lid across
 * the top (sleepy, wary) and a disc from below that leaves an arch (^,
 * happy).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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

/* Shapes for the left and the right eye (as seen on the screen) in a box of
 * box_w x box_h each. Every shape fits inside its box. */
void db_face_eyes(const struct db_face *f, int box_w, int box_h, struct db_eye_shape *left,
                  struct db_eye_shape *right);
bool db_eye_shape_equal(const struct db_eye_shape *a, const struct db_eye_shape *b);

#endif
