/*
 * The fake detector: a tensor of the detector's shape, made up from a
 * script, so the rest of the Vision pipeline runs on a host. See
 * vision_kpu.h.
 *
 * THE SCRIPT, a comma-separated list of key=value (all optional):
 *
 *   in=WxH                 the pretend model input (default 320x320)
 *   classes=N              class count (default 80)
 *   box=CLS:CONF:X:Y:W:H[:DX:DY]
 *                          a detection in FRAME pixels with a per-mille
 *                          confidence, moved by (DX, DY) each frame; up to
 *                          VISION_FAKE_MAX_BOXES of them
 *   dup=N                  every box is also emitted N more times, shifted
 *                          by a pixel, so suppression has work to do
 *   until=N                after frame N the boxes are gone (an empty scene)
 *   malformed_at=N         frame N's tensor is full of NaN
 *   bad_from=N             from frame N on, every tensor is full of NaN
 *   shape_at=N             frame N's tensor claims the wrong shape
 *   fail_at=N              frame N's run fails (-EIO)
 *   delay=MS               every run takes this long (pretend inference)
 *
 * With a crop (vision_kpu_crop) the pretend model sees only that part of the
 * frame: a box whose centre is outside it is not reported, the rest are
 * written in the crop's own letterboxed pixels. The boxes are always in
 * sensor-frame pixels; when the frames come turned (vision_kpu_turn) they
 * are turned the same way first, so the scene stays the same one.
 *
 * The boxes are written into the letterboxed model-input space exactly as
 * the decoder expects to undo it, one box per row, the rest of the rows
 * zero. Frames count from 1.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "vision_kpu.h"
#include "vision_decode.h"
#include "vision_geom.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VISION_FAKE_MAX_BOXES 16

struct fake_box {
    uint32_t cls;
    uint32_t conf;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    int32_t dx;
    int32_t dy;
};

struct vision_kpu {
    uint32_t in_w;
    uint32_t in_h;
    uint32_t classes;
    uint32_t rows;
    struct fake_box box[VISION_FAKE_MAX_BOXES];
    int nbox;
    uint32_t dup;
    uint32_t until;
    uint32_t malformed_at;
    uint32_t bad_from;
    uint32_t shape_at;
    uint32_t fail_at;
    int delay_ms;
    uint32_t frames;
    float *out;
    size_t out_count;
    struct vision_box crop;     /* w == 0: the whole frame */
    int turn;                   /* the frames come turned by this (vision_kpu_turn) */
};

static int parse_u32(const char *s, uint32_t *v)
{
    char *end;
    unsigned long n;

    errno = 0;
    n = strtoul(s, &end, 10);
    if (errno || end == s || *end || n > 100000000UL) {
        return -EINVAL;
    }
    *v = (uint32_t)n;
    return 0;
}

static int parse_box(const char *s, struct fake_box *b)
{
    int n = sscanf(s, "%u:%u:%d:%d:%d:%d:%d:%d", &b->cls, &b->conf, &b->x, &b->y, &b->w, &b->h,
                   &b->dx, &b->dy);

    if (n == 6) {
        b->dx = 0;
        b->dy = 0;
        n = 8;
    }
    if (n != 8 || b->conf > VISION_CONF_SCALE || b->w <= 0 || b->h <= 0) {
        return -EINVAL;
    }
    return 0;
}

static int parse_script(struct vision_kpu *k, const char *config)
{
    char *copy;
    char *save = NULL;
    char *item;
    int r = 0;

    if (!config || !*config) {
        return 0;
    }
    copy = strdup(config);
    if (!copy) {
        return -ENOMEM;
    }
    for (item = strtok_r(copy, ",", &save); item && r == 0; item = strtok_r(NULL, ",", &save)) {
        char *val = strchr(item, '=');

        if (!val) {
            r = -EINVAL;
            break;
        }
        *val++ = '\0';
        if (strcmp(item, "in") == 0) {
            unsigned a;
            unsigned b;
            char tail;

            if (sscanf(val, "%ux%u%c", &a, &b, &tail) != 2 || a == 0 || b == 0) {
                r = -EINVAL;
            } else {
                k->in_w = a;
                k->in_h = b;
            }
        } else if (strcmp(item, "classes") == 0) {
            r = parse_u32(val, &k->classes);
        } else if (strcmp(item, "box") == 0) {
            if (k->nbox >= VISION_FAKE_MAX_BOXES) {
                r = -EINVAL;
            } else {
                r = parse_box(val, &k->box[k->nbox]);
                if (r == 0) {
                    k->nbox++;
                }
            }
        } else if (strcmp(item, "dup") == 0) {
            r = parse_u32(val, &k->dup);
        } else if (strcmp(item, "until") == 0) {
            r = parse_u32(val, &k->until);
        } else if (strcmp(item, "malformed_at") == 0) {
            r = parse_u32(val, &k->malformed_at);
        } else if (strcmp(item, "bad_from") == 0) {
            r = parse_u32(val, &k->bad_from);
        } else if (strcmp(item, "shape_at") == 0) {
            r = parse_u32(val, &k->shape_at);
        } else if (strcmp(item, "fail_at") == 0) {
            r = parse_u32(val, &k->fail_at);
        } else if (strcmp(item, "delay") == 0) {
            uint32_t d;

            r = parse_u32(val, &d);
            k->delay_ms = (int)d;
        } else {
            r = -EINVAL;
        }
    }
    free(copy);
    return r;
}

int vision_kpu_open(struct vision_kpu **kp, const char *path, const char *config,
                    struct vision_kpu_info *info, char *err, size_t errlen)
{
    struct vision_kpu *k = calloc(1, sizeof(*k));
    int r;

    (void)path;
    if (!k) {
        return -ENOMEM;
    }
    k->in_w = 320;
    k->in_h = 320;
    k->classes = 80;
    r = parse_script(k, config);
    if (r == 0) {
        k->rows = vision_decode_rows(k->in_w, k->in_h);
        if (k->rows == 0 || k->classes == 0 || k->classes > VISION_MAX_CLASSES) {
            r = -EINVAL;
        }
    }
    if (r != 0) {
        snprintf(err, errlen, "fake detector: bad script");
        free(k);
        return r;
    }
    k->out_count = (size_t)(4 + k->classes) * k->rows;
    k->out = calloc(k->out_count, sizeof(float));
    if (!k->out) {
        free(k);
        return -ENOMEM;
    }
    memset(info, 0, sizeof(*info));
    snprintf(info->backend, sizeof(info->backend), "fake");
    snprintf(info->model, sizeof(info->model), "fake-detector");
    info->in_w = k->in_w;
    info->in_h = k->in_h;
    info->classes = k->classes;
    info->rows = k->rows;
    *kp = k;
    return 0;
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };

    if (ms > 0) {
        while (nanosleep(&d, &d) != 0 && errno == EINTR) {
        }
    }
}

int vision_kpu_infer(struct vision_kpu *k, const struct pocketcam_frame *f, const float **out,
                     size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms)
{
    float ratio_w;
    float ratio_h;
    float ratio;
    uint32_t row = 0;
    int i;
    uint32_t d;
    /* The part of the frame the pretend model looks at: the crop, or all
     * of it. */
    int32_t rx = k->crop.w > 0 ? k->crop.x : 0;
    int32_t ry = k->crop.w > 0 ? k->crop.y : 0;
    int32_t rw;
    int32_t rh;

    if (pocketcam_frame_check(f) != 0) {
        return -EPROTO;
    }
    rw = k->crop.w > 0 ? k->crop.w : (int32_t)f->width;
    rh = k->crop.w > 0 ? k->crop.h : (int32_t)f->height;
    if ((int64_t)rx + rw > (int64_t)f->width || (int64_t)ry + rh > (int64_t)f->height) {
        return -EPROTO;
    }
    k->frames++;
    if (pre_ms) {
        *pre_ms = 0;
    }
    if (infer_ms) {
        *infer_ms = k->delay_ms;
    }
    sleep_ms(k->delay_ms);
    if (k->fail_at && k->frames == k->fail_at) {
        return -EIO;
    }
    dims[0] = 1;
    dims[1] = 4 + k->classes;
    dims[2] = k->rows;
    *out = k->out;
    *count = k->out_count;
    if (k->shape_at && k->frames == k->shape_at) {
        dims[1] = 5 + k->classes; /* a head this model does not have */
        return 0;
    }
    if ((k->malformed_at && k->frames == k->malformed_at) ||
        (k->bad_from && k->frames >= k->bad_from)) {
        size_t j;

        for (j = 0; j < k->out_count; j++) {
            k->out[j] = NAN;
        }
        return 0;
    }
    memset(k->out, 0, k->out_count * sizeof(float));
    if (k->until && k->frames > k->until) {
        return 0;
    }
    ratio_w = (float)k->in_w / (float)rw;
    ratio_h = (float)k->in_h / (float)rh;
    ratio = ratio_w < ratio_h ? ratio_w : ratio_h;
    /* A box is seen when its centre is in that part, in the part's own
     * pixels - what a real detector given the crop would report. */
    for (i = 0; i < k->nbox; i++) {
        const struct fake_box *b = &k->box[i];
        int32_t step = (int32_t)(k->frames - 1);
        /* The script is in sensor-frame pixels; the frame came turned. */
        struct vision_box sb = { b->x + b->dx * step, b->y + b->dy * step, b->w, b->h };
        struct vision_box tb = sb;
        uint32_t sw;
        uint32_t sh;

        vision_turned_size(f->width, f->height, k->turn, &sw, &sh); /* a turn is its own size swap */
        if (k->turn != 0 && vision_box_turn(sw, sh, k->turn, &sb, &tb) != 0) {
            continue;
        }
        float x = (float)(tb.x - rx);
        float y = (float)(tb.y - ry);
        float bw = (float)tb.w;
        float bh = (float)tb.h;
        float mx = x + 0.5f * bw;
        float my = y + 0.5f * bh;

        if (mx < 0.0f || my < 0.0f || mx >= (float)rw || my >= (float)rh) {
            continue;
        }
        for (d = 0; d <= k->dup && row < k->rows; d++, row++) {
            float cx = (x + (float)d + 0.5f * bw) * ratio;
            float cy = (y + 0.5f * bh) * ratio;

            k->out[0 * k->rows + row] = cx;
            k->out[1 * k->rows + row] = cy;
            k->out[2 * k->rows + row] = bw * ratio;
            k->out[3 * k->rows + row] = bh * ratio;
            if (b->cls < k->classes) {
                /* A duplicate scores a little less, so the original wins. */
                k->out[(4 + b->cls) * k->rows + row] =
                    ((float)b->conf - (float)d) / (float)VISION_CONF_SCALE;
            }
        }
    }
    return 0;
}

int vision_kpu_turn(struct vision_kpu *k, int rotation)
{
    if (!k || (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) {
        return -EINVAL;
    }
    k->turn = rotation;
    return 0;
}

int vision_kpu_crop(struct vision_kpu *k, const struct vision_box *crop)
{
    if (!k) {
        return -EINVAL;
    }
    if (!crop) {
        memset(&k->crop, 0, sizeof(k->crop));
        return 0;
    }
    if (crop->x < 0 || crop->y < 0 || crop->w < VISION_CROP_MIN || crop->h < VISION_CROP_MIN) {
        return -EINVAL;
    }
    k->crop = *crop;
    return 0;
}

void vision_kpu_close(struct vision_kpu *k)
{
    if (k) {
        free(k->out);
        free(k);
    }
}

const char *vision_kpu_backend(void)
{
    return "fake";
}
