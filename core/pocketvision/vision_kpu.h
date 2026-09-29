/*
 * The detector: a frame in, the model's raw output tensor out
 * (pocketvision.h, docs/apps/VISION.md).
 *
 * Two backends, chosen at build time (Makefile, POCKETVISION_KPU):
 *
 *   nncase  the K230's KPU through the nncase 2.x runtime in the pinned SDK
 *           (vision_kpu_nncase.cpp): the frame is copied into a tensor the
 *           AI2D engine can read, letterboxed by AI2D into the model's
 *           input, and run on the KPU. C++ behind this C interface, and the
 *           only file in Doors that includes an nncase header.
 *   fake    a host stand-in (vision_kpu_fake.c) that writes a tensor of the
 *           detector's shape from a script of boxes, so everything above it
 *           - the decoder, suppression, tracking, counting, the helper and
 *           the app - runs and is tested on a PC. It proves nothing about
 *           the KPU: not its speed, not its accuracy.
 *
 * The output is always a float tensor of rank 3, [1][4 + classes][rows],
 * the layout vision_decode.h reads; a backend that finds anything else in
 * its model refuses to open. The pointer a run hands back is valid until
 * the next run or close.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VISION_KPU_H
#define POCKETOS_VISION_KPU_H

#include "pocketcam/pocketcam.h"

#include <stddef.h>
#include <stdint.h>

#define VISION_KPU_NAME_MAX 48

struct vision_kpu;

struct vision_kpu_info {
    char backend[16];      /* "nncase" or "fake" */
    char model[VISION_KPU_NAME_MAX]; /* the file's base name, or the fake's */
    uint32_t in_w;         /* the model's input picture */
    uint32_t in_h;
    uint32_t classes;
    uint32_t rows;
};

/* Open a model. `path` is the .kmodel file (nncase) or ignored (fake);
 * `config` is the fake's script, may be NULL. 0, or a negative errno with
 * a reason in err. -ENOENT: no model file; -EPROTO: the model is not a
 * detector of the expected shape; -ENOTSUP: no KPU in this build. */
int vision_kpu_open(struct vision_kpu **k, const char *path, const char *config,
                    struct vision_kpu_info *info, char *err, size_t errlen);

/* Look at one frame: preprocess, infer, and hand back the raw output as
 * `count` floats at *out with shape dims[3]. pre_ms and infer_ms (may be
 * NULL) get how long each half took. 0, -EPROTO when the frame cannot be
 * fed (a format or size this backend does not take), -EIO when the run
 * failed. */
int vision_kpu_infer(struct vision_kpu *k, const struct pocketcam_frame *f, const float **out,
                     size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms);

/* The frames of the next runs are the sensor frame turned clockwise by
 * `rotation` (0, 90, 180, 270): the helper gives the detector the scene
 * upright (vision_geom.h). The KPU needs nothing for it - a picture is a
 * picture - but the fake detector's script is in sensor-frame pixels and is
 * turned the same way, so it keeps describing the same scene. 0, or
 * -EINVAL. */
int vision_kpu_turn(struct vision_kpu *k, int rotation);

void vision_kpu_close(struct vision_kpu *k);

/* The name of the backend this build carries. */
const char *vision_kpu_backend(void);

#endif
