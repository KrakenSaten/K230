/*
 * The detector on the K230's KPU, through the nncase runtime in the pinned
 * SDK (libnncase 2.11.0, AI2D through functional_k230). See vision_kpu.h.
 *
 * This is the one place in Doors that speaks nncase, and the only C++ in
 * the Vision path. What it does, per frame, is what the vendor's yolo demo
 * (k230_linux_sdk/buildroot-overlay/package/yolo/src) does, in C behind a C
 * interface and without OpenCV:
 *
 *   1. copy the planar BGR frame into a tensor allocated from the shared
 *      pool, which is memory the AI2D engine can read (a V4L2 buffer is not:
 *      the runtime has no physical address for it), and write it back from
 *      the cache;
 *   2. AI2D: resize into the top-left of the model's input with the frame's
 *      aspect kept, the rest padded with 114 (the vendor's
 *      padding_resize_one_side_set), NCHW in, NCHW out;
 *   3. run the model; map its first output and copy the floats out, so the
 *      caller reads plain memory while the runtime keeps its own buffers.
 *
 * Errors come back as negative errno, never as an exception: the runtime's
 * result<> is checked at every step, and .expect() - which aborts - is never
 * called.
 *
 * Copyright (c) 2026 PocketOS authors.
 * Portions (the AI2D preprocessing set-up and the face alignment
 * parameters) are adapted from the K230 Linux SDK's yolo and ai_demo
 * samples, Copyright (c) 2024, Canaan Bright Sight Co., Ltd, BSD-2-Clause
 * (docs/legal/third-party/canaan-k230-linux-sdk-LICENSE.txt).
 * SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
 */
extern "C" {
#include "vision_kpu.h"
#include "vision_decode.h"
#include "mmz.h"
}

#include <nncase/functional/ai2d/ai2d_builder.h>
#include <nncase/runtime/interpreter.h>
#include <nncase/runtime/runtime_tensor.h>
#include <nncase/runtime/util.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <vector>

using namespace nncase;
using namespace nncase::runtime;
using namespace nncase::runtime::k230;
using namespace nncase::F::k230;

struct vision_kpu {
    interpreter interp;
    runtime_tensor model_in;
    runtime_tensor ai2d_in;
    std::unique_ptr<ai2d_builder> builder;
    bool built = false;
    uint32_t fw = 0;
    uint32_t fh = 0;
    bool have_frame = false;        /* ai2d_in holds the last frame run */
    /* The zoom window's schedule (vision_kpu_infer_window): the same frame,
     * cropped by AI2D itself; rebuilt when the window or the frame's size
     * changes. */
    std::unique_ptr<ai2d_builder> wbuilder;
    struct vision_box wbox = { 0, 0, 0, 0 };
    uint32_t wfw = 0;
    uint32_t wfh = 0;
    uint32_t in_w = 0;
    uint32_t in_h = 0;
    uint32_t classes = 0;
    uint32_t rows = 0;
    std::vector<float> out;
};

/* Objects of ours alive in the runtime (release_pool). */
static int live;

static int64_t mono_ms()
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void reason(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s", what);
    }
}

extern "C" int vision_kpu_open(struct vision_kpu **kp, const char *path, const char *config,
                               struct vision_kpu_info *info, char *err, size_t errlen)
{
    (void)config;
    if (!kp || !path || !info) {
        return -EINVAL;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        reason(err, errlen, "model file not found");
        return -ENOENT;
    }
    std::unique_ptr<vision_kpu> k(new (std::nothrow) vision_kpu);
    if (!k) {
        return -ENOMEM;
    }
    if (!k->interp.load_model(ifs).is_ok()) {
        reason(err, errlen, "not a kmodel this runtime can load");
        return -EPROTO;
    }
    if (k->interp.inputs_size() != 1 || k->interp.outputs_size() < 1) {
        reason(err, errlen, "the model does not take one picture and give one tensor");
        return -EPROTO;
    }
    auto in_shape = k->interp.input_shape(0);
    auto in_desc = k->interp.input_desc(0);
    if (in_shape.size() != 4 || in_shape[0] != 1 || in_shape[1] != 3 || in_desc.datatype != dt_uint8) {
        reason(err, errlen, "the model input is not one 8-bit RGB picture");
        return -EPROTO;
    }
    k->in_h = (uint32_t)in_shape[2];
    k->in_w = (uint32_t)in_shape[3];
    k->rows = vision_decode_rows(k->in_w, k->in_h);
    auto out_shape = k->interp.output_shape(0);
    auto out_desc = k->interp.output_desc(0);
    if (k->rows == 0 || out_shape.size() != 3 || out_shape[0] != 1 || out_shape[1] < 5 ||
        out_shape[1] > 4 + VISION_MAX_CLASSES || out_shape[2] != k->rows ||
        out_desc.datatype != dt_float32) {
        reason(err, errlen, "the model output is not a YOLOv8 detection head for its input");
        return -EPROTO;
    }
    k->classes = (uint32_t)out_shape[1] - 4;
    auto t = hrt::create(dt_uint8, in_shape, hrt::pool_shared);
    if (!t.is_ok()) {
        reason(err, errlen, "no shared memory for the model input");
        return -ENOMEM;
    }
    k->model_in = t.unwrap();
    if (!k->interp.input_tensor(0, k->model_in).is_ok()) {
        reason(err, errlen, "the model refused its input tensor");
        return -EIO;
    }
    k->out.resize((size_t)(4 + k->classes) * k->rows);
    memset(info, 0, sizeof(*info));
    snprintf(info->backend, sizeof(info->backend), "nncase");
    {
        const char *base = strrchr(path, '/');

        snprintf(info->model, sizeof(info->model), "%s", base ? base + 1 : path);
    }
    info->in_w = k->in_w;
    info->in_h = k->in_h;
    info->classes = k->classes;
    info->rows = k->rows;
    *kp = k.release();
    live++;
    return 0;
}

/* The AI2D schedule for a frame of this size: built once, rebuilt only when
 * the frame's size changes. */
static int build(struct vision_kpu *k, uint32_t fw, uint32_t fh)
{
    float ratio_w = (float)k->in_w / (float)fw;
    float ratio_h = (float)k->in_h / (float)fh;
    float ratio = ratio_w < ratio_h ? ratio_w : ratio_h;
    int new_w = (int)(ratio * (float)fw);
    int new_h = (int)(ratio * (float)fh);
    int bottom = (int)k->in_h - new_h;
    int right = (int)k->in_w - new_w;
    dims_t in_shape { 1, 3, fh, fw };
    dims_t out_shape { 1, 3, k->in_h, k->in_w };
    ai2d_datatype_t dtype { ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT, dt_uint8, dt_uint8 };
    ai2d_crop_param_t crop { false, 0, 0, 0, 0 };
    ai2d_shift_param_t shift { false, 0 };
    ai2d_pad_param_t pad { true, { { 0, 0 }, { 0, 0 }, { 0, bottom }, { 0, right } },
                           ai2d_pad_mode::constant, { 114, 114, 114 } };
    ai2d_resize_param_t resize { true, ai2d_interp_method::tf_bilinear, ai2d_interp_mode::half_pixel };
    ai2d_affine_param_t affine { false, ai2d_interp_method::cv2_bilinear, 0, 0, 127, 1,
                                 { 0.5, 0.1, 0.0, 0.1, 0.5, 0.0 } };

    k->builder.reset();
    k->built = false;
    auto t = hrt::create(dt_uint8, in_shape, hrt::pool_shared);
    if (!t.is_ok()) {
        return -ENOMEM;
    }
    k->ai2d_in = t.unwrap();
    k->builder.reset(new (std::nothrow) ai2d_builder(in_shape, out_shape, dtype, crop, shift, pad,
                                                     resize, affine));
    if (!k->builder) {
        return -ENOMEM;
    }
    if (!k->builder->build_schedule().is_ok()) {
        k->builder.reset();
        return -EIO;
    }
    k->fw = fw;
    k->fh = fh;
    k->built = true;
    k->have_frame = false;
    k->wbuilder.reset();
    return 0;
}

/* The window's schedule: AI2D crops the window out of the frame in its
 * memory, then letterboxes it into the model exactly as build() does the
 * whole frame (the runtime's crop parameter, DOCUMENTED in
 * nncase/runtime/k230/gnne_tile_utils.h: ai2d_crop_param_t). */
static int build_window(struct vision_kpu *k, const struct vision_box *w)
{
    float ratio_w = (float)k->in_w / (float)w->w;
    float ratio_h = (float)k->in_h / (float)w->h;
    float ratio = ratio_w < ratio_h ? ratio_w : ratio_h;
    int new_w = (int)(ratio * (float)w->w);
    int new_h = (int)(ratio * (float)w->h);
    int bottom = (int)k->in_h - new_h;
    int right = (int)k->in_w - new_w;
    dims_t in_shape { 1, 3, k->fh, k->fw };
    dims_t out_shape { 1, 3, k->in_h, k->in_w };
    ai2d_datatype_t dtype { ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT, dt_uint8, dt_uint8 };
    ai2d_crop_param_t crop { true, w->x, w->y, w->w, w->h };
    ai2d_shift_param_t shift { false, 0 };
    ai2d_pad_param_t pad { true, { { 0, 0 }, { 0, 0 }, { 0, bottom }, { 0, right } },
                           ai2d_pad_mode::constant, { 114, 114, 114 } };
    ai2d_resize_param_t resize { true, ai2d_interp_method::tf_bilinear, ai2d_interp_mode::half_pixel };
    ai2d_affine_param_t affine { false, ai2d_interp_method::cv2_bilinear, 0, 0, 127, 1,
                                 { 0.5, 0.1, 0.0, 0.1, 0.5, 0.0 } };

    k->wbuilder.reset(new (std::nothrow) ai2d_builder(in_shape, out_shape, dtype, crop, shift, pad, resize,
                                                      affine));
    if (!k->wbuilder) {
        return -ENOMEM;
    }
    if (!k->wbuilder->build_schedule().is_ok()) {
        k->wbuilder.reset();
        return -EIO;
    }
    k->wbox = *w;
    k->wfw = k->fw;
    k->wfh = k->fh;
    return 0;
}

/* The model's first output, copied out into k->out. */
static int copy_output(struct vision_kpu *k)
{
    auto ot = k->interp.output_tensor(0);
    if (!ot.is_ok()) {
        return -EIO;
    }
    auto host = ot.unwrap().impl()->to_host();
    if (!host.is_ok()) {
        return -EIO;
    }
    auto hb = host.unwrap()->buffer().as_host();
    if (!hb.is_ok()) {
        return -EIO;
    }
    auto m = hb.unwrap().map(map_access_::map_read);
    if (!m.is_ok()) {
        return -EIO;
    }
    auto mapped = std::move(m.unwrap());
    size_t bytes = k->out.size() * sizeof(float);

    if (mapped.buffer().size() < bytes) {
        return -EPROTO;
    }
    memcpy(k->out.data(), mapped.buffer().data(), bytes);
    return 0;
}

extern "C" int vision_kpu_infer(struct vision_kpu *k, const struct pocketcam_frame *f,
                                const float **out, size_t *count, uint32_t dims[3], int *pre_ms,
                                int *infer_ms)
{
    int64_t t0;
    int64_t t1;
    int64_t t2;

    if (!k || !out || !count || !dims || pocketcam_frame_check(f) != 0) {
        return -EPROTO;
    }
    /* Only the planar BGR the ISP can deliver directly: NCHW, which AI2D
     * takes as it is. Anything else would need a conversion nobody has
     * measured. */
    if (f->format != POCKETCAM_FMT_BG3P || f->width > VISION_MAX_COORD ||
        f->height > VISION_MAX_COORD) {
        return -EPROTO;
    }
    t0 = mono_ms();
    if (!k->built || k->fw != f->width || k->fh != f->height) {
        int r = build(k, f->width, f->height);

        if (r != 0) {
            return r;
        }
    }
    {
        auto m = hrt::map(k->ai2d_in, map_access_::map_write);
        if (!m.is_ok()) {
            return -EIO;
        }
        auto mapped = std::move(m.unwrap());
        uint8_t *dst = reinterpret_cast<uint8_t *>(mapped.buffer().data());
        size_t plane_out = (size_t)f->width * f->height;
        size_t plane_in = (size_t)f->stride * f->height;

        if (mapped.buffer().size() < 3 * plane_out) {
            return -EIO;
        }
        if (f->stride == f->width) {
            memcpy(dst, f->data, 3 * plane_out);
        } else {
            for (int p = 0; p < 3; p++) {
                for (uint32_t y = 0; y < f->height; y++) {
                    memcpy(dst + p * plane_out + (size_t)y * f->width,
                           f->data + p * plane_in + (size_t)y * f->stride, f->width);
                }
            }
        }
        if (!mapped.unmap().is_ok()) {
            return -EIO;
        }
    }
    if (!hrt::sync(k->ai2d_in, sync_op_t::sync_write_back, true).is_ok()) {
        return -EIO;
    }
    k->have_frame = true;
    if (!k->builder->invoke(k->ai2d_in, k->model_in).is_ok()) {
        return -EIO;
    }
    t1 = mono_ms();
    if (!k->interp.run().is_ok()) {
        return -EIO;
    }
    t2 = mono_ms();
    {
        int r = copy_output(k);

        if (r != 0) {
            return r;
        }
    }
    *out = k->out.data();
    *count = k->out.size();
    dims[0] = 1;
    dims[1] = 4 + k->classes;
    dims[2] = k->rows;
    if (pre_ms) {
        *pre_ms = (int)(t1 - t0);
    }
    if (infer_ms) {
        *infer_ms = (int)(t2 - t1);
    }
    return 0;
}

extern "C" int vision_kpu_infer_window(struct vision_kpu *k, const struct vision_box *win, const float **out,
                                       size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms)
{
    int64_t t0;
    int64_t t1;
    int64_t t2;

    if (!k || !win || !out || !count || !dims) {
        return -EINVAL;
    }
    if (!k->built || !k->have_frame) {
        return -EPROTO;
    }
    if (win->x < 0 || win->y < 0 || win->w <= 0 || win->h <= 0 || (uint32_t)(win->x + win->w) > k->fw ||
        (uint32_t)(win->y + win->h) > k->fh) {
        return -EINVAL;
    }
    t0 = mono_ms();
    if (!k->wbuilder || k->wfw != k->fw || k->wfh != k->fh || k->wbox.x != win->x || k->wbox.y != win->y ||
        k->wbox.w != win->w || k->wbox.h != win->h) {
        int r = build_window(k, win);

        if (r != 0) {
            return r;
        }
    }
    /* The frame is in ai2d_in already, written back from the cache by the
     * full run. */
    if (!k->wbuilder->invoke(k->ai2d_in, k->model_in).is_ok()) {
        return -EIO;
    }
    t1 = mono_ms();
    if (!k->interp.run().is_ok()) {
        return -EIO;
    }
    t2 = mono_ms();
    {
        int r = copy_output(k);

        if (r != 0) {
            return r;
        }
    }
    *out = k->out.data();
    *count = k->out.size();
    dims[0] = 1;
    dims[1] = 4 + k->classes;
    dims[2] = k->rows;
    if (pre_ms) {
        *pre_ms = (int)(t1 - t0);
    }
    if (infer_ms) {
        *infer_ms = (int)(t2 - t1);
    }
    return 0;
}

extern "C" int vision_kpu_turn(struct vision_kpu *k, int rotation)
{
    /* The picture arrives turned already; the engine and the model take it
     * as any other picture of its size. */
    return k && (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) ? 0 : -EINVAL;
}

/* Objects of ours alive in the runtime: the detector and every net. The
 * runtime's shared pool (CMA, through libmmz) is given back when the last
 * one closes, as the vendor's programs do on exit - and it must be: the
 * driver does NOT reclaim it when the process ends. Unit B, 2026-09-29: the
 * text bench, whose nets never called this, leaked ~50 MB of CMA a run
 * until 428 of 512 MB were gone and every run faulted; only a reboot gave
 * it back. `live` is declared at the top. */
static void release_pool(void)
{
    if (live > 0 && --live == 0) {
        shrink_memory_pool();
        kd_mpi_mmz_deinit();
    }
}

extern "C" void vision_kpu_close(struct vision_kpu *k)
{
    if (!k) {
        return;
    }
    delete k;
    release_pool();
}

extern "C" const char *vision_kpu_backend(void)
{
    return "nncase";
}

/* ---- nets ------------------------------------------------------------------- */

struct vision_net {
    interpreter interp;
    runtime_tensor model_in;
    runtime_tensor ai2d_in;
    std::unique_ptr<ai2d_builder> builder;
    uint32_t in_w = 0;
    uint32_t in_h = 0;
    uint32_t fw = 0;               /* the frame in ai2d_in */
    uint32_t fh = 0;
    bool have_frame = false;
    /* What the schedule was built for. */
    struct vision_box bwin = { 0, 0, 0, 0 };
    bool bstretch = false;
    uint8_t bpad = 0;
    int outputs = 0;
    std::vector<float> out[VISION_NET_OUTPUTS];
    bool have_out = false;
};

extern "C" int vision_net_open(struct vision_net **np, const char *path, const char *script,
                               struct vision_net_info *info, char *err, size_t errlen)
{
    (void)script;
    if (!np || !path || !info) {
        return -EINVAL;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        reason(err, errlen, "model file not found");
        return -ENOENT;
    }
    std::unique_ptr<vision_net> n(new (std::nothrow) vision_net);
    if (!n) {
        return -ENOMEM;
    }
    if (!n->interp.load_model(ifs).is_ok()) {
        reason(err, errlen, "not a kmodel this runtime can load");
        return -EPROTO;
    }
    auto in_shape = n->interp.input_shape(0);
    if (n->interp.inputs_size() != 1 || in_shape.size() != 4 || in_shape[0] != 1 || in_shape[1] != 3 ||
        n->interp.input_desc(0).datatype != dt_uint8 || n->interp.outputs_size() < 1 ||
        n->interp.outputs_size() > VISION_NET_OUTPUTS) {
        reason(err, errlen, "the model does not take one 8-bit RGB picture");
        return -EPROTO;
    }
    memset(info, 0, sizeof(*info));
    n->in_h = (uint32_t)in_shape[2];
    n->in_w = (uint32_t)in_shape[3];
    info->in_w = n->in_w;
    info->in_h = n->in_h;
    n->outputs = (int)n->interp.outputs_size();
    info->outputs = n->outputs;
    for (int i = 0; i < n->outputs; i++) {
        auto s = n->interp.output_shape((size_t)i);
        size_t count = 1;

        if (n->interp.output_desc((size_t)i).datatype != dt_float32 || s.size() > VISION_NET_RANK) {
            reason(err, errlen, "an output is not a float tensor of rank 4 or less");
            return -EPROTO;
        }
        info->rank[i] = (uint32_t)s.size();
        for (size_t k = 0; k < s.size(); k++) {
            info->dims[i][k] = (uint32_t)s[k];
            count *= (size_t)s[k];
        }
        info->count[i] = count;
        n->out[i].resize(count);
    }
    auto t = hrt::create(dt_uint8, in_shape, hrt::pool_shared);
    if (!t.is_ok()) {
        reason(err, errlen, "no shared memory for the model input");
        return -ENOMEM;
    }
    n->model_in = t.unwrap();
    if (!n->interp.input_tensor(0, n->model_in).is_ok()) {
        reason(err, errlen, "the model refused its input tensor");
        return -EIO;
    }
    *np = n.release();
    live++;
    return 0;
}

extern "C" int vision_net_frame(struct vision_net *n, const struct pocketcam_frame *f)
{
    if (!n || pocketcam_frame_check(f) != 0 || f->format != POCKETCAM_FMT_BG3P || f->width > VISION_MAX_COORD ||
        f->height > VISION_MAX_COORD) {
        return -EPROTO;
    }
    if (n->fw != f->width || n->fh != f->height) {
        dims_t shape { 1, 3, f->height, f->width };
        auto t = hrt::create(dt_uint8, shape, hrt::pool_shared);

        n->builder.reset();
        n->have_frame = false;
        if (!t.is_ok()) {
            return -ENOMEM;
        }
        n->ai2d_in = t.unwrap();
        n->fw = f->width;
        n->fh = f->height;
    }
    {
        auto m = hrt::map(n->ai2d_in, map_access_::map_write);
        if (!m.is_ok()) {
            return -EIO;
        }
        auto mapped = std::move(m.unwrap());
        uint8_t *dst = reinterpret_cast<uint8_t *>(mapped.buffer().data());
        size_t plane_out = (size_t)f->width * f->height;
        size_t plane_in = (size_t)f->stride * f->height;

        if (mapped.buffer().size() < 3 * plane_out) {
            return -EIO;
        }
        for (int p = 0; p < 3; p++) {
            if (f->stride == f->width) {
                memcpy(dst + p * plane_out, f->data + p * plane_in, plane_out);
            } else {
                for (uint32_t y = 0; y < f->height; y++) {
                    memcpy(dst + p * plane_out + (size_t)y * f->width, f->data + p * plane_in + (size_t)y * f->stride,
                           f->width);
                }
            }
        }
        if (!mapped.unmap().is_ok()) {
            return -EIO;
        }
    }
    if (!hrt::sync(n->ai2d_in, sync_op_t::sync_write_back, true).is_ok()) {
        return -EIO;
    }
    n->have_frame = true;
    return 0;
}

static int net_infer(struct vision_net *n, int64_t t0, int64_t t1, int *pre_ms, int *infer_ms);

/* The schedule for a window of the frame into the model: cropped, then
 * letterboxed (top-left, padded right and bottom) or stretched. */
static int net_build(struct vision_net *n, const struct vision_box *w, bool stretch, uint8_t pad)
{
    int bottom = 0;
    int right = 0;

    if (!stretch) {
        float ratio_w = (float)n->in_w / (float)w->w;
        float ratio_h = (float)n->in_h / (float)w->h;
        float ratio = ratio_w < ratio_h ? ratio_w : ratio_h;

        bottom = (int)n->in_h - (int)(ratio * (float)w->h);
        right = (int)n->in_w - (int)(ratio * (float)w->w);
        bottom = bottom < 0 ? 0 : bottom;
        right = right < 0 ? 0 : right;
    }
    dims_t in_shape { 1, 3, n->fh, n->fw };
    dims_t out_shape { 1, 3, n->in_h, n->in_w };
    ai2d_datatype_t dtype { ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT, dt_uint8, dt_uint8 };
    bool whole = w->x == 0 && w->y == 0 && (uint32_t)w->w == n->fw && (uint32_t)w->h == n->fh;
    ai2d_crop_param_t crop { !whole, w->x, w->y, w->w, w->h };
    ai2d_shift_param_t shift { false, 0 };
    ai2d_pad_param_t padp { bottom > 0 || right > 0, { { 0, 0 }, { 0, 0 }, { 0, bottom }, { 0, right } },
                            ai2d_pad_mode::constant, { pad, pad, pad } };
    ai2d_resize_param_t resize { true, ai2d_interp_method::tf_bilinear, ai2d_interp_mode::half_pixel };
    ai2d_affine_param_t affine { false, ai2d_interp_method::cv2_bilinear, 0, 0, 127, 1,
                                 { 0.5, 0.1, 0.0, 0.1, 0.5, 0.0 } };

    n->builder.reset(new (std::nothrow) ai2d_builder(in_shape, out_shape, dtype, crop, shift, padp, resize, affine));
    if (!n->builder) {
        return -ENOMEM;
    }
    if (!n->builder->build_schedule().is_ok()) {
        n->builder.reset();
        return -EIO;
    }
    n->bwin = *w;
    n->bstretch = stretch;
    n->bpad = pad;
    return 0;
}

extern "C" int vision_net_run(struct vision_net *n, const struct vision_box *win, bool stretch, uint8_t pad,
                              int *pre_ms, int *infer_ms)
{
    struct vision_box w;
    int64_t t0;
    int64_t t1;

    if (!n) {
        return -EINVAL;
    }
    if (!n->have_frame) {
        return -EPROTO;
    }
    if (win) {
        w = *win;
    } else {
        w.x = 0;
        w.y = 0;
        w.w = (int32_t)n->fw;
        w.h = (int32_t)n->fh;
    }
    if (w.x < 0 || w.y < 0 || w.w <= 0 || w.h <= 0 || (uint32_t)(w.x + w.w) > n->fw ||
        (uint32_t)(w.y + w.h) > n->fh) {
        return -EINVAL;
    }
    t0 = mono_ms();
    if (!n->builder || n->bwin.x != w.x || n->bwin.y != w.y || n->bwin.w != w.w || n->bwin.h != w.h ||
        n->bstretch != stretch || n->bpad != pad) {
        int r = net_build(n, &w, stretch, pad);

        if (r != 0) {
            return r;
        }
    }
    if (!n->builder->invoke(n->ai2d_in, n->model_in).is_ok()) {
        return -EIO;
    }
    t1 = mono_ms();
    return net_infer(n, t0, t1, pre_ms, infer_ms);
}

extern "C" int vision_net_run_affine(struct vision_net *n, const float m[6], int *pre_ms, int *infer_ms)
{
    int64_t t0;
    int64_t t1;

    if (!n || !m) {
        return -EINVAL;
    }
    if (!n->have_frame) {
        return -EPROTO;
    }
    for (int i = 0; i < 6; i++) {
        if (!std::isfinite(m[i])) {
            return -EINVAL;
        }
    }
    t0 = mono_ms();
    {
        /* As the vendor's face verification aligns a face (ai_demo
         * common/utils.cc Utils::affine): the affine alone, bilinear, the
         * matrix taking the frame to the model's input. Built per run: the
         * matrix is the face's. */
        dims_t in_shape { 1, 3, n->fh, n->fw };
        dims_t out_shape { 1, 3, n->in_h, n->in_w };
        ai2d_datatype_t dtype { ai2d_format::NCHW_FMT, ai2d_format::NCHW_FMT, dt_uint8, dt_uint8 };
        ai2d_crop_param_t crop { false, 0, 0, 0, 0 };
        ai2d_shift_param_t shift { false, 0 };
        ai2d_pad_param_t padp { false, { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } }, ai2d_pad_mode::constant, { 0, 0, 0 } };
        ai2d_resize_param_t resize { false, ai2d_interp_method::tf_bilinear, ai2d_interp_mode::half_pixel };
        ai2d_affine_param_t affine { true, ai2d_interp_method::cv2_bilinear, 0, 0, 127, 1,
                                     { m[0], m[1], m[2], m[3], m[4], m[5] } };
        std::unique_ptr<ai2d_builder> b(
            new (std::nothrow) ai2d_builder(in_shape, out_shape, dtype, crop, shift, padp, resize, affine));

        if (!b) {
            return -ENOMEM;
        }
        if (!b->build_schedule().is_ok() || !b->invoke(n->ai2d_in, n->model_in).is_ok()) {
            return -EIO;
        }
    }
    t1 = mono_ms();
    return net_infer(n, t0, t1, pre_ms, infer_ms);
}

/* The model on its input as it stands, and its outputs copied out. */
static int net_infer(struct vision_net *n, int64_t t0, int64_t t1, int *pre_ms, int *infer_ms)
{
    int64_t t2;

    if (!n->interp.run().is_ok()) {
        return -EIO;
    }
    t2 = mono_ms();
    for (int i = 0; i < n->outputs; i++) {
        auto ot = n->interp.output_tensor((size_t)i);
        if (!ot.is_ok()) {
            return -EIO;
        }
        auto host = ot.unwrap().impl()->to_host();
        if (!host.is_ok()) {
            return -EIO;
        }
        auto hb = host.unwrap()->buffer().as_host();
        if (!hb.is_ok()) {
            return -EIO;
        }
        auto m = hb.unwrap().map(map_access_::map_read);
        if (!m.is_ok()) {
            return -EIO;
        }
        auto mapped = std::move(m.unwrap());
        size_t bytes = n->out[i].size() * sizeof(float);

        if (mapped.buffer().size() < bytes) {
            return -EPROTO;
        }
        memcpy(n->out[i].data(), mapped.buffer().data(), bytes);
    }
    n->have_out = true;
    if (pre_ms) {
        *pre_ms = (int)(t1 - t0);
    }
    if (infer_ms) {
        *infer_ms = (int)(t2 - t1);
    }
    return 0;
}

extern "C" const float *vision_net_output(const struct vision_net *n, int i, size_t *count)
{
    if (!n || !n->have_out || i < 0 || i >= n->outputs) {
        return NULL;
    }
    if (count) {
        *count = n->out[i].size();
    }
    return n->out[i].data();
}

extern "C" int vision_net_turn(struct vision_net *n, int rotation)
{
    return n && (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270) ? 0 : -EINVAL;
}

extern "C" void vision_net_close(struct vision_net *n)
{
    if (!n) {
        return;
    }
    delete n;
    release_pool();
}

static const char *type_name(typecode_t t)
{
    if (t == dt_uint8) {
        return "u8";
    }
    if (t == dt_int8) {
        return "i8";
    }
    if (t == dt_int16) {
        return "i16";
    }
    if (t == dt_float32) {
        return "f32";
    }
    if (t == dt_float16) {
        return "f16";
    }
    return "?";
}

static size_t say_shape(char *out, size_t len, size_t off, const char *what, size_t i, const dims_t &d,
                        typecode_t t)
{
    int n;

    if (off >= len) {
        return off;
    }
    n = snprintf(out + off, len - off, "%s %zu %s [", what, i, type_name(t));
    off += n > 0 ? (size_t)n : 0;
    for (size_t k = 0; k < d.size() && off < len; k++) {
        n = snprintf(out + off, len - off, "%s%zu", k ? "," : "", (size_t)d[k]);
        off += n > 0 ? (size_t)n : 0;
    }
    if (off < len) {
        n = snprintf(out + off, len - off, "]\n");
        off += n > 0 ? (size_t)n : 0;
    }
    return off;
}

extern "C" int vision_kpu_describe(const char *path, char *out, size_t len)
{
    std::ifstream ifs(path, std::ios::binary);
    size_t off = 0;

    if (!out || len == 0) {
        return -EINVAL;
    }
    out[0] = '\0';
    if (!ifs) {
        return -ENOENT;
    }
    std::unique_ptr<interpreter> in(new (std::nothrow) interpreter);
    if (!in) {
        return -ENOMEM;
    }
    if (!in->load_model(ifs).is_ok()) {
        return -EPROTO;
    }
    for (size_t i = 0; i < in->inputs_size(); i++) {
        off = say_shape(out, len, off, "input", i, in->input_shape(i), in->input_desc(i).datatype);
    }
    for (size_t i = 0; i < in->outputs_size(); i++) {
        off = say_shape(out, len, off, "output", i, in->output_shape(i), in->output_desc(i).datatype);
    }
    return 0;
}
