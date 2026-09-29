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
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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
    uint32_t in_w = 0;
    uint32_t in_h = 0;
    uint32_t classes = 0;
    uint32_t rows = 0;
    std::vector<float> out;
};

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
    if (!k->builder->invoke(k->ai2d_in, k->model_in).is_ok()) {
        return -EIO;
    }
    t1 = mono_ms();
    if (!k->interp.run().is_ok()) {
        return -EIO;
    }
    t2 = mono_ms();
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

extern "C" void vision_kpu_close(struct vision_kpu *k)
{
    if (!k) {
        return;
    }
    delete k;
    /* The runtime's shared pool is returned once nothing of ours holds it,
     * as the vendor's programs do on exit. */
    shrink_memory_pool();
    kd_mpi_mmz_deinit();
}

extern "C" const char *vision_kpu_backend(void)
{
    return "nncase";
}
