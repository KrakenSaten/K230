/*
 * The owner-recognition half of a future vision provider: data structures
 * and the interface only. NOTHING HERE IS IMPLEMENTED IN v0.1, and nothing
 * in DeskBuddy calls it; the mock provider (db_vision_mock.h) stands in.
 *
 * The planned pipeline is two stages, both inside the provider, never in the
 * DeskBuddy screen:
 *
 *   1. person / face detection - the detector already on the KPU finds a
 *      person (COCO class 0) today; a face detector is still to be chosen;
 *   2. a face embedding of each face found, compared with the owner's
 *      enrolled embeddings by cosine similarity against a threshold.
 *
 * The provider then reports DB_VISION_OWNER_RECOGNIZED or
 * DB_VISION_UNKNOWN_PERSON with the similarity as confidence_pm, and the
 * brain never sees an embedding.
 *
 * PRIVACY. The owner profile is biometric data. It stays on the device:
 * $POCKETOS_STATE_DIR/deskbuddy/owner.v1, 0600 in a 0700 directory, never
 * sent over IPC, the network or a log line, never in a crash report, and
 * deleted whole by one "forget me" action. No face image is kept for
 * enrolment once its embedding is computed.
 *
 * Nothing here is ASSUMED to exist on the K230: the embedding model, its
 * input size and its dimension are open (docs/apps/DESKBUDDY.md, "Deferred").
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_IDENTITY_H
#define DB_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

/* Upper bound for the file format; the real model decides the dimension. */
#define DB_EMBED_DIM_MAX 512
/* A few enrolment samples (angles, glasses on and off). */
#define DB_OWNER_SAMPLES_MAX 8

struct db_face_embedding {
    uint16_t dim;                   /* <= DB_EMBED_DIM_MAX */
    float v[DB_EMBED_DIM_MAX];      /* L2-normalised by the producer */
};

struct db_owner_profile {
    uint32_t version;               /* file format version */
    char model_id[48];              /* which embedding model made the samples */
    uint16_t samples;               /* <= DB_OWNER_SAMPLES_MAX */
    struct db_face_embedding sample[DB_OWNER_SAMPLES_MAX];
    uint16_t threshold_pm;          /* similarity at or above which a face is the owner */
    int64_t enrolled_wall_s;
};

/* What a recognizer returns for one face. */
struct db_identity_match {
    uint16_t similarity_pm;         /* best cosine similarity against the samples, 0..1000 */
    uint8_t is_owner;               /* similarity_pm >= threshold_pm */
};

/* The recognizer a real provider will implement. Every call may be slow (a
 * model runs) and therefore runs in the provider's worker, never on the LVGL
 * thread. */
struct db_recognizer_ops {
    const char *name;
    /* Embed one face crop (pixel format and size belong to the model). */
    int (*embed)(void *ctx, const void *face_pixels, size_t len, struct db_face_embedding *out);
    /* Compare one embedding with the profile. */
    int (*match)(void *ctx, const struct db_owner_profile *owner, const struct db_face_embedding *face,
                 struct db_identity_match *out);
};

#endif
