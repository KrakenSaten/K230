/*
 * Faces compared: what RECOGNIZE needs around a face-embedding model
 * (pocketvision.h, docs/apps/VISION.md "RECOGNIZE").
 *
 *   align    five face points (eyes, nose, mouth corners, as the face
 *            detector gives them) onto the 112 x 112 template the
 *            embedding model was trained on - the vendor's face
 *            verification demo's (ArcFace's) - by the least-squares
 *            similarity (scale, rotation, shift; no mirror, no shear): the
 *            2 x 3 matrix the AI2D engine's affine takes, picture to model
 *            input;
 *   compare  an embedding made unit length, and two compared by their
 *            cosine, as the vendor's demo scores it: 50 + 50 x cosine, per
 *            cent (here per-mille);
 *   owner    the enrolled face: the mean of a few unit embeddings of one
 *            face, made unit again, with the model it came from (a profile
 *            made with another model is not compared); its text form, and
 *            that form read back, refusing anything this code did not
 *            write.
 *
 * Nothing leaves the unit: the owner is a file the helper keeps in the
 * Vision state directory (0600). Reads floats; bounded, no allocation
 * (tests/vision_embed_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_EMBED_H
#define POCKETOS_VISION_EMBED_H

#include <stddef.h>

#include "pocketvision.h"

#define VISION_EMBED_SIZE 112          /* the template's side */
#define VISION_EMBED_MAX 512           /* values per embedding, at most */
#define VISION_OWNER_SAMPLES 5         /* views of the face enrolment takes */
#define VISION_OWNER_MATCH_PM 750      /* the vendor demo's threshold (75) */
#define VISION_OWNER_MODEL_MAX 64
#define VISION_OWNER_TEXT_MAX 12288    /* the text form's size, at most */

/* The matrix taking picture pixels to the template's: out = M x (x, y, 1).
 * pts are five (x, y) points in the order the face detector gives them.
 * 0, or -1 for points that do not make a face (all in one place). */
int vision_embed_align(const float pts[5][2], float m[6]);

/* in scaled to unit length into out (may be in). 0, or -1 for a vector of
 * length zero or with a value that is not a number. */
int vision_embed_unit(const float *in, size_t n, float *out);

/* The vendor's score of two unit vectors, per-mille: 500 + 500 x cosine. */
uint16_t vision_embed_score(const float *a, const float *b, size_t n);

struct vision_owner {
    char model[VISION_OWNER_MODEL_MAX];  /* the embedding model's file name */
    uint64_t model_bytes;                /* and its size: the same model */
    uint32_t dim;
    uint32_t samples;
    float v[VISION_EMBED_MAX];           /* unit length once finished */
};

/* A new enrolment for a model of dim values. 0, or -1. */
int vision_owner_begin(struct vision_owner *o, const char *model, uint64_t model_bytes, uint32_t dim);
/* One more view (a unit embedding of dim values). 0, or -1. */
int vision_owner_add(struct vision_owner *o, const float *unit);
/* The mean made unit: the owner. 0, or -1 with no views. */
int vision_owner_finish(struct vision_owner *o);
/* Whether a profile is this model's. */
bool vision_owner_fits(const struct vision_owner *o, const char *model, uint64_t model_bytes, uint32_t dim);

/* The text form into buf: its length, or -1 when it does not fit. */
int vision_owner_format(const struct vision_owner *o, char *buf, size_t len);
/* Read back. 0, or -1 for anything but a finished profile this code wrote
 * (a missing or extra value, a value not a number, a vector not of unit
 * length). */
int vision_owner_parse(const char *text, struct vision_owner *o);

#endif
