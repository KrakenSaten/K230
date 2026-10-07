/*
 * Faces compared. See vision_embed.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_embed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The template: where the five points sit on a 112 x 112 face (the vendor
 * demo's umeyama_args_112, ArcFace's reference points). */
static const float tmpl[5][2] = {
    { 38.2946f, 51.6963f }, { 73.5318f, 51.5014f }, { 56.0252f, 71.7366f },
    { 41.5493f, 92.3655f }, { 70.7299f, 92.2041f },
};

/* The pinned riscv64 GCC's vectoriser warns that an unnamed temporary of
 * the second loop "may be used uninitialized" (every value here is set
 * before it is read; the host compiler, and the sanitizers, see none).
 * Silenced for this function only. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
int vision_embed_align(const float pts[5][2], float m[6])
{
    float px[5] = { 0 };
    float py[5] = { 0 };
    float sx = 0.0f;
    float sy = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float num_re = 0.0f;
    float num_im = 0.0f;
    float den = 0.0f;
    float a;
    float b;
    int i;

    if (!pts || !m) {
        return -1;
    }
    for (i = 0; i < 5; i++) {
        px[i] = pts[i][0];
        py[i] = pts[i][1];
    }
    for (i = 0; i < 5; i++) {
        if (!isfinite(px[i]) || !isfinite(py[i])) {
            return -1;
        }
    }
    for (i = 0; i < 5; i++) {
        sx += px[i];
        sy += py[i];
        dx += tmpl[i][0];
        dy += tmpl[i][1];
    }
    sx /= 5.0f;
    sy /= 5.0f;
    dx /= 5.0f;
    dy /= 5.0f;
    /* As complex numbers, z the picture's points and w the template's
     * (both about their means): the scale-and-rotation a minimising
     * sum |a z - w|^2 is sum(conj(z) w) / sum |z|^2. */
    for (i = 0; i < 5; i++) {
        float zr = px[i] - sx;
        float zi = py[i] - sy;
        float wr = tmpl[i][0] - dx;
        float wi = tmpl[i][1] - dy;

        num_re += zr * wr + zi * wi;
        num_im += zr * wi - zi * wr;
        den += zr * zr + zi * zi;
    }
    if (den < 1.0f) {
        return -1;
    }
    a = num_re / den;
    b = num_im / den;
    m[0] = a;
    m[1] = -b;
    m[2] = dx - (a * sx - b * sy);
    m[3] = b;
    m[4] = a;
    m[5] = dy - (b * sx + a * sy);
    return 0;
}
#pragma GCC diagnostic pop

int vision_embed_unit(const float *in, size_t n, float *out)
{
    double sum = 0.0;
    size_t i;

    if (!in || !out || n == 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (!isfinite(in[i])) {
            return -1;
        }
        sum += (double)in[i] * (double)in[i];
    }
    if (!(sum > 1e-12)) {
        return -1;
    }
    sum = sqrt(sum);
    for (i = 0; i < n; i++) {
        out[i] = (float)((double)in[i] / sum);
    }
    return 0;
}

uint16_t vision_embed_score(const float *a, const float *b, size_t n)
{
    double c = 0.0;
    size_t i;

    if (!a || !b) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        c += (double)a[i] * (double)b[i];
    }
    if (!(c >= -1.0)) {
        c = -1.0;
    }
    if (c > 1.0) {
        c = 1.0;
    }
    return (uint16_t)lround(500.0 + 500.0 * c);
}

int vision_owner_begin(struct vision_owner *o, const char *model, uint64_t model_bytes, uint32_t dim)
{
    if (!o || !model || !*model || strlen(model) >= VISION_OWNER_MODEL_MAX || strpbrk(model, " \t\r\n") ||
        dim == 0 || dim > VISION_EMBED_MAX) {
        return -1;
    }
    memset(o, 0, sizeof(*o));
    snprintf(o->model, sizeof(o->model), "%s", model);
    o->model_bytes = model_bytes;
    o->dim = dim;
    return 0;
}

int vision_owner_add(struct vision_owner *o, const float *unit)
{
    uint32_t i;

    if (!o || !unit || o->dim == 0 || o->samples >= 1000) {
        return -1;
    }
    for (i = 0; i < o->dim; i++) {
        if (!isfinite(unit[i])) {
            return -1;
        }
    }
    for (i = 0; i < o->dim; i++) {
        o->v[i] += unit[i];
    }
    o->samples++;
    return 0;
}

int vision_owner_finish(struct vision_owner *o)
{
    if (!o || o->samples == 0) {
        return -1;
    }
    return vision_embed_unit(o->v, o->dim, o->v);
}

bool vision_owner_fits(const struct vision_owner *o, const char *model, uint64_t model_bytes, uint32_t dim)
{
    return o && model && o->samples > 0 && o->dim == dim && o->model_bytes == model_bytes &&
           strcmp(o->model, model) == 0;
}

int vision_owner_format(const struct vision_owner *o, char *buf, size_t len)
{
    size_t off;
    uint32_t i;
    int n;

    if (!o || !buf || o->dim == 0 || o->dim > VISION_EMBED_MAX || o->samples == 0) {
        return -1;
    }
    n = snprintf(buf, len, "doors-vision-owner 1\nmodel %s %llu\ndim %u\nsamples %u\nv", o->model,
                 (unsigned long long)o->model_bytes, o->dim, o->samples);
    if (n < 0 || (size_t)n >= len) {
        return -1;
    }
    off = (size_t)n;
    for (i = 0; i < o->dim; i++) {
        n = snprintf(buf + off, len - off, " %.7g", (double)o->v[i]);
        if (n < 0 || off + (size_t)n >= len) {
            return -1;
        }
        off += (size_t)n;
    }
    if (off + 2 > len) {
        return -1;
    }
    buf[off++] = '\n';
    buf[off] = '\0';
    return (int)off;
}

int vision_owner_parse(const char *text, struct vision_owner *o)
{
    struct vision_owner r;
    char model[VISION_OWNER_MODEL_MAX];
    unsigned long long bytes;
    unsigned dim;
    unsigned samples;
    int used = 0;
    const char *p;
    char *end;
    double sum = 0.0;
    uint32_t i;

    if (!text || !o || strlen(text) >= VISION_OWNER_TEXT_MAX) {
        return -1;
    }
    if (sscanf(text, "doors-vision-owner 1\nmodel %63s %llu\ndim %u\nsamples %u\nv%n", model, &bytes, &dim, &samples,
               &used) != 4 ||
        used == 0 || dim == 0 || dim > VISION_EMBED_MAX || samples == 0 || samples > 1000) {
        return -1;
    }
    memset(&r, 0, sizeof(r));
    snprintf(r.model, sizeof(r.model), "%s", model);
    r.model_bytes = bytes;
    r.dim = dim;
    r.samples = samples;
    p = text + used;
    for (i = 0; i < dim; i++) {
        float v;

        if (*p != ' ') {
            return -1;
        }
        v = strtof(p, &end);
        if (end == p || !isfinite(v)) {
            return -1;
        }
        r.v[i] = v;
        sum += (double)v * (double)v;
        p = end;
    }
    if (strcmp(p, "\n") != 0 || fabs(sum - 1.0) > 1e-3) {
        return -1;
    }
    *o = r;
    return 0;
}
