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
#include "pocketvision.h"

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

/* A second look at the frame of the last vision_kpu_infer: the window `win`
 * of it (in that frame's pixels) letterboxed into the model the same way and
 * run - a zoom, for objects too small in the full picture
 * (vision_range.h). The output is laid out as vision_kpu_infer's and its
 * boxes are in the window's own pixels. The frame is not copied again: the
 * engine reads the one already in its memory. The results are
 * vision_kpu_infer's, and -EINVAL for a window not inside the frame, -EPROTO
 * with no frame run yet. */
int vision_kpu_infer_window(struct vision_kpu *k, const struct vision_box *win, const float **out,
                            size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms);

/* The frames of the next runs are the sensor frame turned clockwise by
 * `rotation` (0, 90, 180, 270): the helper gives the detector the scene
 * upright (vision_geom.h). The KPU needs nothing for it - a picture is a
 * picture - but the fake detector's script is in sensor-frame pixels and is
 * turned the same way, so it keeps describing the same scene. 0, or
 * -EINVAL. */
int vision_kpu_turn(struct vision_kpu *k, int rotation);

void vision_kpu_close(struct vision_kpu *k);

/* What a model file is: its inputs and outputs, their shapes and element
 * types, one per line into out. For bringing up a model on the bench
 * (`pos-vision describe`); nothing is run. 0, -ENOENT, -EPROTO for a file
 * this runtime cannot load, -ENOTSUP in a build with no runtime. */
int vision_kpu_describe(const char *path, char *out, size_t len);

/* ---- other models: text, faces, embeddings -----------------------------------
 *
 * A net is any model that takes one 8-bit RGB picture (u8 [1,3,H,W]) and
 * gives up to VISION_NET_OUTPUTS float tensors. It is given a frame (the
 * upright planar picture the detector gets), copied once into memory the
 * AI2D engine reads; then run on the whole frame or on a window of it,
 * letterboxed into the model's input (the picture scaled to fit, kept in
 * the top-left corner, the rest filled with `pad`) or stretched to it. The
 * outputs are copied out and stay valid until the next run.
 *
 * The fake backend emulates the model its file name says (`ocr_det`,
 * `ocr_rec`, `face_det`, `face_embed` in the base name) from the fake
 * detector's script (vision_kpu_fake.c), so the pipelines above it run on a
 * host; it opens only when the file exists, as the real one does. */
#define VISION_NET_OUTPUTS 9
#define VISION_NET_RANK 4

struct vision_net;

struct vision_net_info {
    uint32_t in_w;
    uint32_t in_h;
    int outputs;
    uint32_t rank[VISION_NET_OUTPUTS];
    uint32_t dims[VISION_NET_OUTPUTS][VISION_NET_RANK];
    size_t count[VISION_NET_OUTPUTS];  /* floats in each */
};

int vision_net_open(struct vision_net **n, const char *path, const char *script, struct vision_net_info *info,
                    char *err, size_t errlen);
/* The frame the next runs look at: planar BG3P, the size the net was last
 * given or any other. 0, -EPROTO for another format, -ENOMEM, -EIO. */
int vision_net_frame(struct vision_net *n, const struct pocketcam_frame *f);
/* Run on window `win` of the frame (NULL: all of it). stretch: fill the
 * input; otherwise letterbox with `pad`. 0, -EPROTO with no frame, -EINVAL
 * for a window not inside it, -EIO. */
int vision_net_run(struct vision_net *n, const struct vision_box *win, bool stretch, uint8_t pad, int *pre_ms,
                   int *infer_ms);
/* Output i of the last run; NULL when there is none. */
const float *vision_net_output(const struct vision_net *n, int i, size_t *count);
/* As vision_kpu_turn: the frames come turned by rotation (only the fake
 * needs to know). */
int vision_net_turn(struct vision_net *n, int rotation);
void vision_net_close(struct vision_net *n);

/* The name of the backend this build carries. */
const char *vision_kpu_backend(void);

#endif
