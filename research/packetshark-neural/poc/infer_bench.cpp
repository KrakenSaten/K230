/*
 * infer-bench: the first KPU proof of concept for PacketShark NEURAL
 * (docs/apps/packetshark/neural/ROADMAP.md section 1). RESEARCH CODE: not a
 * DOORS component, never installed, no capture, no network, no files
 * written. It runs the DNA encoder dna-b64 ([1,3,64,32] float32 in,
 * [1,32,64,1] float32 out) two ways on the same inputs:
 *
 *   kpu  through the nncase 2.11 runtime with plain host tensors (no AI2D),
 *        the vendor keyword-spotting pattern: hrt::create(pool_shared),
 *        map + memcpy + sync_write_back, run(), map_read.
 *   ref  a portable C reference of the same network and weights.
 *
 * and prints one JSON object per measurement on stdout. Built without
 * WITH_NNCASE it has only the reference (host sanity check).
 *
 *   infer-bench --in F --batches N [--weights F] [--kmodel F] [--expect F]
 *               [--mode kpu|ref|both] [--iters N] [--lock F] [--profile]
 *               [--interval-ms N --duration-s N] [--hold]
 *
 * --expect: float outputs [N][32*64] to compare against (the float model or
 * the simulator). --lock: an exclusive flock is taken first, refused if held
 * (P10). --hold: stay alive after loading, for the SIGKILL test (P7).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef WITH_NNCASE
extern "C" {
#include "mmz.h"
}
#include <nncase/runtime/interpreter.h>
#include <nncase/runtime/runtime_tensor.h>
#include <nncase/runtime/util.h>
#include <nncase/runtime/stackvm/op_profile.h>
using namespace nncase;
using namespace nncase::runtime;
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

static const int C0 = 3, H = 64, W = 32, D = 32;
static const size_t IN_N = (size_t)C0 * H * W;  /* 6144 floats */
static const size_t OUT_N = (size_t)D * H;      /* 2048 floats */

static double now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long v = -1;
    size_t n = strlen(key);
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, n) && line[n] == ':') {
            v = strtol(line + n + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static long status_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t n = strlen(key);
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, n) && line[n] == ':') {
            v = strtol(line + n + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static bool read_floats(const char *path, std::vector<float> &v, size_t want)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    std::streamsize sz = f.tellg();
    if (sz < 0 || (size_t)sz != want * sizeof(float)) {
        fprintf(stderr, "%s: %lld bytes, expected %zu\n", path, (long long)sz, want * sizeof(float));
        return false;
    }
    v.resize(want);
    f.seekg(0);
    return (bool)f.read(reinterpret_cast<char *>(v.data()), sz);
}

/* ---- the C reference -------------------------------------------------- */

struct ref_net {
    const float *c1w, *c1b, *c2w, *c2b, *c3w, *c3b, *fcw, *fcb;
    std::vector<float> a1, a2, a3, pool;
};

static const size_t WEIGHTS_N = 32 * 3 * 3 + 32 + 64 * 32 * 3 + 64 + 64 * 64 * 3 + 64 + 32 * 64 + 32;

static void ref_bind(ref_net &r, const std::vector<float> &w)
{
    const float *p = w.data();
    r.c1w = p; p += 32 * 3 * 3;
    r.c1b = p; p += 32;
    r.c2w = p; p += 64 * 32 * 3;
    r.c2b = p; p += 64;
    r.c3w = p; p += 64 * 64 * 3;
    r.c3b = p; p += 64;
    r.fcw = p; p += 32 * 64;
    r.fcb = p;
    r.a1.resize((size_t)32 * H * 32);
    r.a2.resize((size_t)64 * H * 16);
    r.a3.resize((size_t)64 * H * 8);
    r.pool.resize((size_t)64 * H);
}

/* Conv (1 x 3), padding 1, stride s, ReLU: in [cin][H][win] -> out [cout][H][wout] */
static void conv13(const float *in, int cin, int win, const float *w, const float *b, int cout, int s, float *out)
{
    int wout = (win + 2 - 3) / s + 1;
    for (int co = 0; co < cout; co++) {
        for (int h = 0; h < H; h++) {
            float *o = out + ((size_t)co * H + h) * wout;
            for (int x = 0; x < wout; x++) {
                float acc = b[co];
                for (int ci = 0; ci < cin; ci++) {
                    const float *row = in + ((size_t)ci * H + h) * win;
                    const float *k = w + ((size_t)co * cin + ci) * 3;
                    for (int t = 0; t < 3; t++) {
                        int xi = x * s - 1 + t;
                        if (xi >= 0 && xi < win) {
                            acc += k[t] * row[xi];
                        }
                    }
                }
                o[x] = acc > 0 ? acc : 0;
            }
        }
    }
}

/* in [3][64][32] -> out [32][64] (the model's [1,32,64,1]) */
static void ref_run(ref_net &r, const float *in, float *out)
{
    conv13(in, 3, 32, r.c1w, r.c1b, 32, 1, r.a1.data());
    conv13(r.a1.data(), 32, 32, r.c2w, r.c2b, 64, 2, r.a2.data());
    conv13(r.a2.data(), 64, 16, r.c3w, r.c3b, 64, 2, r.a3.data());
    for (int c = 0; c < 64; c++) {
        for (int h = 0; h < H; h++) {
            const float *row = r.a3.data() + ((size_t)c * H + h) * 8;
            float s = 0;
            for (int x = 0; x < 8; x++) {
                s += row[x];
            }
            r.pool[(size_t)c * H + h] = s / 8.0f;
        }
    }
    for (int d = 0; d < D; d++) {
        for (int h = 0; h < H; h++) {
            float acc = r.fcb[d];
            for (int c = 0; c < 64; c++) {
                acc += r.fcw[(size_t)d * 64 + c] * r.pool[(size_t)c * H + h];
            }
            out[(size_t)d * H + h] = acc;
        }
    }
}

/* ---- comparison ------------------------------------------------------- */

/* per conversation h: cosine of the 32-d embedding (layout [32][64]) */
static void compare(const float *a, const float *b, double &cmin, double &cmean, int &n)
{
    for (int h = 0; h < H; h++) {
        double ab = 0, aa = 0, bb = 0;
        for (int d = 0; d < D; d++) {
            double x = a[(size_t)d * H + h], y = b[(size_t)d * H + h];
            ab += x * y;
            aa += x * x;
            bb += y * y;
        }
        double c = ab / (std::sqrt(aa * bb) + 1e-12);
        cmin = std::min(cmin, c);
        cmean += c;
        n++;
    }
}

static double pct(std::vector<double> v, double p)
{
    if (v.empty()) {
        return NAN;
    }
    std::sort(v.begin(), v.end());
    size_t i = (size_t)std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5));
    return v[i];
}

static void print_lat(const char *what, const std::vector<double> &v)
{
    printf("{\"what\":\"%s\",\"n\":%zu,\"p50_us\":%.1f,\"p95_us\":%.1f,\"p99_us\":%.1f,\"max_us\":%.1f}\n", what,
           v.size(), pct(v, .5), pct(v, .95), pct(v, .99), v.empty() ? NAN : *std::max_element(v.begin(), v.end()));
}

static void print_mem(const char *when)
{
    printf("{\"what\":\"mem\",\"when\":\"%s\",\"CmaFree_kB\":%ld,\"MemAvailable_kB\":%ld,\"VmRSS_kB\":%ld,"
           "\"VmHWM_kB\":%ld}\n",
           when, meminfo_kb("CmaFree"), meminfo_kb("MemAvailable"), status_kb("VmRSS"), status_kb("VmHWM"));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *w_path = NULL, *k_path = NULL, *exp_path = NULL, *lock_path = NULL;
    std::string mode = "both";
    int batches = 0, iters = 1000, interval_ms = 0, duration_s = 0;
    bool profile = false, hold = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (a == "--in" && v) { in_path = v; i++; }
        else if (a == "--weights" && v) { w_path = v; i++; }
        else if (a == "--kmodel" && v) { k_path = v; i++; }
        else if (a == "--expect" && v) { exp_path = v; i++; }
        else if (a == "--lock" && v) { lock_path = v; i++; }
        else if (a == "--mode" && v) { mode = v; i++; }
        else if (a == "--batches" && v) { batches = atoi(v); i++; }
        else if (a == "--iters" && v) { iters = atoi(v); i++; }
        else if (a == "--interval-ms" && v) { interval_ms = atoi(v); i++; }
        else if (a == "--duration-s" && v) { duration_s = atoi(v); i++; }
        else if (a == "--profile") { profile = true; }
        else if (a == "--hold") { hold = true; }
        else { fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str()); return 2; }
    }
    if (!in_path || batches < 1 || batches > 4096 || iters < 1) {
        fprintf(stderr, "usage: infer-bench --in F --batches N [--weights F] [--kmodel F] [--expect F] "
                        "[--mode kpu|ref|both] [--iters N] [--lock F] [--profile] [--interval-ms N --duration-s N] "
                        "[--hold]\n");
        return 2;
    }
    int lock_fd = -1;
    if (lock_path) {
        lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            printf("{\"what\":\"lock\",\"ok\":false,\"errno\":%d}\n", errno);
            return 3;
        }
        printf("{\"what\":\"lock\",\"ok\":true}\n");
    }
    std::vector<float> in, expect, weights;
    if (!read_floats(in_path, in, IN_N * batches)) {
        return 1;
    }
    if (exp_path && !read_floats(exp_path, expect, OUT_N * batches)) {
        return 1;
    }
    print_mem("start");
    std::vector<float> out_ref(OUT_N * batches), out_kpu(OUT_N * batches);
    bool do_ref = mode == "ref" || mode == "both", do_kpu = mode == "kpu" || mode == "both";

    if (do_ref) {
        if (!w_path || !read_floats(w_path, weights, WEIGHTS_N)) {
            fprintf(stderr, "ref needs --weights (%zu floats)\n", WEIGHTS_N);
            return 1;
        }
        ref_net r{};
        ref_bind(r, weights);
        std::vector<double> lat;
        for (int it = 0; it < iters; it++) {
            int b = it % batches;
            double t0 = now_us();
            ref_run(r, &in[IN_N * b], &out_ref[OUT_N * b]);
            lat.push_back(now_us() - t0);
        }
        print_lat("ref_call", lat);
    }

#ifdef WITH_NNCASE
    if (do_kpu) {
        if (!k_path) {
            fprintf(stderr, "kpu needs --kmodel\n");
            return 1;
        }
        {
            interpreter interp;
            double t0 = now_us();
            std::ifstream ifs(k_path, std::ios::binary);
            if (!ifs || !interp.load_model(ifs).is_ok()) {
                fprintf(stderr, "%s: not a kmodel this runtime can load\n", k_path);
                return 1;
            }
            double t_load = now_us() - t0;
            auto ish = interp.input_shape(0);
            auto osh = interp.output_shape(0);
            if (interp.inputs_size() != 1 || interp.outputs_size() != 1 || ish.size() != 4 || ish[1] != 3 ||
                ish[2] != (size_t)H || ish[3] != (size_t)W || interp.input_desc(0).datatype != dt_float32 ||
                interp.output_desc(0).datatype != dt_float32) {
                fprintf(stderr, "unexpected model signature\n");
                return 1;
            }
            auto t = hrt::create(dt_float32, ish, hrt::pool_shared);
            if (!t.is_ok()) {
                fprintf(stderr, "no shared memory for the input\n");
                return 1;
            }
            runtime_tensor model_in = t.unwrap();
            if (!interp.input_tensor(0, model_in).is_ok()) {
                fprintf(stderr, "the model refused its input tensor\n");
                return 1;
            }
            if (profile) {
                interp.set_profiling(1);
            }
            printf("{\"what\":\"load\",\"us\":%.1f,\"out_rank\":%zu}\n", t_load, osh.size());
            print_mem("loaded");
            std::vector<double> l_in, l_sync, l_run, l_out, l_all;
            auto one = [&](int b, bool timed) -> bool {
                double a0 = now_us();
                {
                    auto m = hrt::map(model_in, map_access_::map_write);
                    if (!m.is_ok()) {
                        return false;
                    }
                    auto mapped = std::move(m.unwrap());
                    if (mapped.buffer().size() < IN_N * sizeof(float)) {
                        return false;
                    }
                    memcpy(mapped.buffer().data(), &in[IN_N * b], IN_N * sizeof(float));
                    if (!mapped.unmap().is_ok()) {
                        return false;
                    }
                }
                double a1 = now_us();
                if (!hrt::sync(model_in, sync_op_t::sync_write_back, true).is_ok()) {
                    return false;
                }
                double a2 = now_us();
                if (!interp.run().is_ok()) {
                    return false;
                }
                double a3 = now_us();
                auto ot = interp.output_tensor(0);
                if (!ot.is_ok()) {
                    return false;
                }
                auto host = ot.unwrap().impl()->to_host();
                if (!host.is_ok()) {
                    return false;
                }
                auto hb = host.unwrap()->buffer().as_host();
                if (!hb.is_ok()) {
                    return false;
                }
                auto mo = hb.unwrap().map(map_access_::map_read);
                if (!mo.is_ok()) {
                    return false;
                }
                auto mapped = std::move(mo.unwrap());
                if (mapped.buffer().size() < OUT_N * sizeof(float)) {
                    return false;
                }
                memcpy(&out_kpu[OUT_N * b], mapped.buffer().data(), OUT_N * sizeof(float));
                double a4 = now_us();
                if (timed) {
                    l_in.push_back(a1 - a0);
                    l_sync.push_back(a2 - a1);
                    l_run.push_back(a3 - a2);
                    l_out.push_back(a4 - a3);
                    l_all.push_back(a4 - a0);
                }
                return true;
            };
            double c0 = now_us();
            if (!one(0, false)) {
                fprintf(stderr, "first run failed\n");
                return 1;
            }
            printf("{\"what\":\"first_call\",\"us\":%.1f}\n", now_us() - c0);
            for (int b = 0; b < batches; b++) {
                if (!one(b, false)) {
                    return 1;
                }
            }
            if (duration_s > 0) {  /* P9: a duty cycle, memory read every 60 s */
                double end = now_us() + duration_s * 1e6, next_mem = now_us() + 60e6;
                int k = 0;
                while (now_us() < end) {
                    if (!one(k++ % batches, true)) {
                        return 1;
                    }
                    if (now_us() >= next_mem) {
                        print_mem("duty");
                        next_mem += 60e6;
                    }
                    usleep(interval_ms > 0 ? interval_ms * 1000 : 1000000);
                }
            } else {
                for (int it = 0; it < iters; it++) {
                    if (!one(it % batches, true)) {
                        return 1;
                    }
                }
            }
            print_lat("kpu_copy_in", l_in);
            print_lat("kpu_sync", l_sync);
            print_lat("kpu_run", l_run);
            print_lat("kpu_copy_out", l_out);
            print_lat("kpu_call", l_all);
            if (profile) {
                fflush(stdout);
                ::op_profile::print();  /* declared at global scope in stackvm/op_profile.h (SDK 2.11) */
            }
            print_mem("before_release");
            if (hold) {
                printf("{\"what\":\"hold\",\"pid\":%d}\n", (int)getpid());
                fflush(stdout);
                for (;;) {
                    pause();
                }
            }
        }
        shrink_memory_pool();
        kd_mpi_mmz_deinit();
        print_mem("released");
    }
#else
    if (do_kpu) {
        fprintf(stderr, "built without WITH_NNCASE: --mode ref only\n");
        return 1;
    }
#endif

    for (int which = 0; which < 2; which++) {
        const std::vector<float> &x = which ? out_kpu : out_ref;
        if ((which && !do_kpu) || (!which && !do_ref)) {
            continue;
        }
        if (!expect.empty()) {
            double cmin = 1, cmean = 0;
            int n = 0;
            for (int b = 0; b < batches; b++) {
                compare(&x[OUT_N * b], &expect[OUT_N * b], cmin, cmean, n);
            }
            printf("{\"what\":\"agree\",\"a\":\"%s\",\"b\":\"expect\",\"cos_min\":%.6f,\"cos_mean\":%.6f,"
                   "\"n\":%d}\n", which ? "kpu" : "ref", cmin, cmean / n, n);
        }
    }
    if (do_ref && do_kpu) {
        double cmin = 1, cmean = 0;
        int n = 0;
        for (int b = 0; b < batches; b++) {
            compare(&out_kpu[OUT_N * b], &out_ref[OUT_N * b], cmin, cmean, n);
        }
        printf("{\"what\":\"agree\",\"a\":\"kpu\",\"b\":\"ref\",\"cos_min\":%.6f,\"cos_mean\":%.6f,\"n\":%d}\n",
               cmin, cmean / n, n);
    }
    print_mem("end");
    if (lock_fd >= 0) {
        close(lock_fd);
    }
    return 0;
}
