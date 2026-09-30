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
 *   minpx=N                a box whose smaller side is under N of the
 *                          MODEL's pixels is not found: what makes a far
 *                          object too small for the full picture and big
 *                          enough in a zoom window (vision_range.h)
 *
 * A window run (vision_kpu_infer_window) looks at the same frame again -
 * the frame count does not move - through the window: boxes are clipped to
 * it and written in its own pixels, letterboxed from its size.
 *
 * The boxes are in sensor-frame pixels. When the frames come turned
 * (vision_kpu_turn: the helper turns them upright) the boxes are turned the
 * same way first, so the script keeps describing the same scene.
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
    uint32_t minpx;
    uint32_t frames;
    float *out;
    size_t out_count;
    int turn;                   /* the frames come turned by this (vision_kpu_turn) */
    uint32_t last_w;            /* the last frame run, for a window run; 0 none */
    uint32_t last_h;
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
        } else if (strcmp(item, "minpx") == 0) {
            r = parse_u32(val, &k->minpx);
        } else if (strcmp(item, "text") == 0 || strcmp(item, "face") == 0 || strcmp(item, "net_open_ms") == 0) {
            /* The fake nets' (below); the detector does not see them. */
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

/* The tensor for the picture `win` of the last frame (fw x fh, turned): the
 * script's boxes clipped to the window, in its pixels, letterboxed from its
 * size. The frame count is the caller's. */
static int emit(struct vision_kpu *k, uint32_t fw, uint32_t fh, const struct vision_box *win, const float **out,
                size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms)
{
    float ratio_w;
    float ratio_h;
    float ratio;
    uint32_t row = 0;
    int i;
    uint32_t d;

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
    ratio_w = (float)k->in_w / (float)win->w;
    ratio_h = (float)k->in_h / (float)win->h;
    ratio = ratio_w < ratio_h ? ratio_w : ratio_h;
    for (i = 0; i < k->nbox; i++) {
        const struct fake_box *b = &k->box[i];
        int32_t step = (int32_t)(k->frames - 1);
        /* The script is in sensor-frame pixels; the frame may come turned
         * (a turn is its own size swap, so the sensor frame's size is the
         * turned size of this one). */
        struct vision_box sb = { b->x + b->dx * step, b->y + b->dy * step, b->w, b->h };
        struct vision_box tb = sb;
        uint32_t sw;
        uint32_t sh;
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;
        float side;

        vision_turned_size(fw, fh, k->turn, &sw, &sh);
        if (k->turn != 0 && vision_box_turn(sw, sh, k->turn, &sb, &tb) != 0) {
            continue;
        }
        /* Into the window's pixels, clipped to it. */
        x0 = tb.x - win->x;
        y0 = tb.y - win->y;
        x1 = x0 + tb.w;
        y1 = y0 + tb.h;
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > win->w ? win->w : x1;
        y1 = y1 > win->h ? win->h : y1;
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        side = (float)((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) * ratio;
        if (k->minpx && side < (float)k->minpx) {
            continue; /* too few of the model's pixels to be seen */
        }
        for (d = 0; d <= k->dup && row < k->rows; d++, row++) {
            float cx = ((float)x0 + (float)d + 0.5f * (float)(x1 - x0)) * ratio;
            float cy = ((float)y0 + 0.5f * (float)(y1 - y0)) * ratio;

            k->out[0 * k->rows + row] = cx;
            k->out[1 * k->rows + row] = cy;
            k->out[2 * k->rows + row] = (float)(x1 - x0) * ratio;
            k->out[3 * k->rows + row] = (float)(y1 - y0) * ratio;
            if (b->cls < k->classes) {
                /* A duplicate scores a little less, so the original wins. */
                k->out[(4 + b->cls) * k->rows + row] =
                    ((float)b->conf - (float)d) / (float)VISION_CONF_SCALE;
            }
        }
    }
    return 0;
}

int vision_kpu_infer(struct vision_kpu *k, const struct pocketcam_frame *f, const float **out,
                     size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms)
{
    struct vision_box whole;

    if (pocketcam_frame_check(f) != 0) {
        return -EPROTO;
    }
    k->frames++;
    k->last_w = f->width;
    k->last_h = f->height;
    whole.x = 0;
    whole.y = 0;
    whole.w = (int32_t)f->width;
    whole.h = (int32_t)f->height;
    return emit(k, f->width, f->height, &whole, out, count, dims, pre_ms, infer_ms);
}

int vision_kpu_infer_window(struct vision_kpu *k, const struct vision_box *win, const float **out,
                            size_t *count, uint32_t dims[3], int *pre_ms, int *infer_ms)
{
    if (!k || !win || !out || !count || !dims) {
        return -EINVAL;
    }
    if (k->last_w == 0) {
        return -EPROTO;
    }
    if (win->x < 0 || win->y < 0 || win->w <= 0 || win->h <= 0 || (uint32_t)(win->x + win->w) > k->last_w ||
        (uint32_t)(win->y + win->h) > k->last_h) {
        return -EINVAL;
    }
    return emit(k, k->last_w, k->last_h, win, out, count, dims, pre_ms, infer_ms);
}

int vision_kpu_turn(struct vision_kpu *k, int rotation)
{
    if (!k || (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) {
        return -EINVAL;
    }
    k->turn = rotation;
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

/* ---- nets ---------------------------------------------------------------------
 *
 * The models of vision_kpu.h's nets, emulated from the same script:
 *
 *   text=X:Y:W:H:WORD      a line of printed text in FRAME pixels; WORD is
 *                          printable ASCII without ',' or ':'; '_' is a
 *                          space (up to FAKE_TEXTS of them)
 *
 * text_det / ocr_det: [1,512,512,2], the first channel 0.9 over the middle
 * of every line (a DB map marks a line shrunk, the reader grows it back),
 * letterboxed from the window as the real model is.
 * text_rec / ocr_rec: [128,1,95], the line whose middle the window holds,
 * one character every four steps (a space leaves eight), the classes '!'
 * to '~' in order and the blank last - what tests/vision_session_test.c's
 * dictionary says.
 *
 *   face=X:Y:W:H[:WHO]     a face in FRAME pixels; WHO (1-9, default 1) is
 *                          whose it is, for the embedding net (up to
 *                          FAKE_FACES of them)
 *   net_open_ms=MS         opening each net takes this long (a large
 *                          kmodel read and checked by the runtime)
 *
 * face_det: RetinaFace's nine outputs for 320 x 320 (vision_face.h), every
 * anchor background but the one each face falls on - its stride and size
 * the nearest to the face's, its cell the face's middle - which says the
 * face as the model would (95 %), with five points where eyes, nose and
 * mouth corners sit on a face. Letterboxed from the window, padded right
 * and below as the real run is.
 *
 * face_embed / face_recognition: [1,512] for a 112 x 112 aligned face
 * (vision_net_run_affine): the face whose middle the alignment brings to
 * the template's middle says whose it is - a common part and one of its
 * own, so one person's views score 1000 and two people's about 540; with
 * no face there, nobody's. */

#define FAKE_TEXTS 8
#define FAKE_TEXT_LEN 32
#define FAKE_REC_STEPS 128
#define FAKE_REC_CLASSES 95   /* '!' .. '~', then the blank */
#define FAKE_FACES 8
#define FAKE_EMBED 512

enum fake_kind {
    FAKE_TEXT_DET = 1,
    FAKE_TEXT_REC,
    FAKE_FACE_DET,
    FAKE_FACE_EMBED,
};

struct fake_text {
    struct vision_box box;
    char word[FAKE_TEXT_LEN];
};

struct fake_face {
    struct vision_box box;
    int who;
};

struct vision_net {
    enum fake_kind kind;
    uint32_t open_ms;             /* net_open_ms: how long opening it takes */
    struct vision_net_info info;
    struct fake_text text[FAKE_TEXTS];
    int ntext;
    struct fake_face face[FAKE_FACES];
    int nface;
    uint32_t fw;
    uint32_t fh;
    bool have_frame;
    int turn;
    float *o[VISION_NET_OUTPUTS];
    float *out;                   /* o[0] */
    bool have_out;
};

static int net_script(struct vision_net *n, const char *script)
{
    char *copy;
    char *save = NULL;
    char *item;
    int r = 0;

    if (!script || !*script) {
        return 0;
    }
    copy = strdup(script);
    if (!copy) {
        return -ENOMEM;
    }
    for (item = strtok_r(copy, ",", &save); item && r == 0; item = strtok_r(NULL, ",", &save)) {
        char *val = strchr(item, '=');

        if (!val) {
            continue;
        }
        *val++ = '\0';
        if (strcmp(item, "text") == 0) {
            struct fake_text *t;
            int used = 0;
            size_t i;

            if (n->ntext >= FAKE_TEXTS) {
                r = -EINVAL;
                break;
            }
            t = &n->text[n->ntext];
            if (sscanf(val, "%d:%d:%d:%d:%n", &t->box.x, &t->box.y, &t->box.w, &t->box.h, &used) != 4 || used == 0 ||
                t->box.w <= 0 || t->box.h <= 0 || strlen(val + used) == 0 || strlen(val + used) >= FAKE_TEXT_LEN) {
                r = -EINVAL;
                break;
            }
            snprintf(t->word, sizeof(t->word), "%s", val + used);
            for (i = 0; t->word[i]; i++) {
                if (t->word[i] < '!' || t->word[i] > '~' || t->word[i] == ':') {
                    r = -EINVAL;
                }
            }
            n->ntext++;
        } else if (strcmp(item, "face") == 0) {
            struct fake_face *f;
            int k;

            if (n->nface >= FAKE_FACES) {
                r = -EINVAL;
                break;
            }
            f = &n->face[n->nface];
            f->who = 1;
            k = sscanf(val, "%d:%d:%d:%d:%d", &f->box.x, &f->box.y, &f->box.w, &f->box.h, &f->who);
            if (k < 4 || f->box.w <= 0 || f->box.h <= 0 || f->who < 1 || f->who > 9) {
                r = -EINVAL;
                break;
            }
            n->nface++;
        } else if (strcmp(item, "net_open_ms") == 0) {
            r = parse_u32(val, &n->open_ms);
        }
    }
    free(copy);
    return r;
}

/* A face into RetinaFace's outputs, `b` in the model input's pixels. */
static void face_put(struct vision_net *n, const struct vision_box *b)
{
    static const uint32_t steps[3] = { 8, 16, 32 };
    static const float sizes[6] = { 16, 32, 64, 128, 256, 512 };
    /* where the five points sit, as fractions of the box */
    static const float px[5] = { 0.30f, 0.70f, 0.50f, 0.35f, 0.65f };
    static const float py[5] = { 0.40f, 0.40f, 0.60f, 0.80f, 0.80f };
    float side = (float)(b->w > b->h ? b->w : b->h);
    float best = 1e9f;
    int pick = 0;
    int i;
    int s;
    uint32_t k;
    uint32_t fw;
    uint32_t fh;
    uint32_t size;
    uint32_t col;
    uint32_t row;
    uint32_t cell;
    float acx;
    float acy;
    float aw;
    float ah;
    float cx = (float)b->x + (float)b->w / 2.0f;
    float cy = (float)b->y + (float)b->h / 2.0f;

    for (i = 0; i < 6; i++) {
        float d = fabsf(logf(side / sizes[i]));

        if (d < best) {
            best = d;
            pick = i;
        }
    }
    s = pick / 2;
    k = (uint32_t)(pick % 2);
    fw = n->info.in_w / steps[s];
    fh = n->info.in_h / steps[s];
    size = fw * fh;
    col = (uint32_t)(cx / (float)steps[s]);
    row = (uint32_t)(cy / (float)steps[s]);
    col = col >= fw ? fw - 1 : col;
    row = row >= fh ? fh - 1 : row;
    cell = row * fw + col;
    acx = ((float)col + 0.5f) * (float)steps[s];
    acy = ((float)row + 0.5f) * (float)steps[s];
    aw = sizes[pick];
    ah = sizes[pick];
    n->o[3 + s][(k * 2 + 0) * size + cell] = 0.0f;
    n->o[3 + s][(k * 2 + 1) * size + cell] = 3.0f;
    n->o[s][(k * 4 + 0) * size + cell] = (cx - acx) / (0.1f * aw);
    n->o[s][(k * 4 + 1) * size + cell] = (cy - acy) / (0.1f * ah);
    n->o[s][(k * 4 + 2) * size + cell] = logf((float)b->w / aw) / 0.2f;
    n->o[s][(k * 4 + 3) * size + cell] = logf((float)b->h / ah) / 0.2f;
    for (i = 0; i < 5; i++) {
        n->o[6 + s][(k * 10 + (uint32_t)i * 2) * size + cell] =
            ((float)b->x + px[i] * (float)b->w - acx) / (0.1f * aw);
        n->o[6 + s][(k * 10 + (uint32_t)i * 2 + 1) * size + cell] =
            ((float)b->y + py[i] * (float)b->h - acy) / (0.1f * ah);
    }
}

int vision_net_open(struct vision_net **np, const char *path, const char *script, struct vision_net_info *info,
                    char *err, size_t errlen)
{
    struct vision_net *n;
    const char *base;
    FILE *fp;

    if (!np || !path || !info) {
        return -EINVAL;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        snprintf(err, errlen, "model file not found");
        return -ENOENT;
    }
    fclose(fp);
    n = calloc(1, sizeof(*n));
    if (!n) {
        return -ENOMEM;
    }
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strstr(base, "text_det") || strstr(base, "ocr_det")) {
        n->kind = FAKE_TEXT_DET;
        n->info.in_w = 512;
        n->info.in_h = 512;
        n->info.outputs = 1;
        n->info.rank[0] = 4;
        n->info.dims[0][0] = 1;
        n->info.dims[0][1] = 512;
        n->info.dims[0][2] = 512;
        n->info.dims[0][3] = 2;
        n->info.count[0] = (size_t)512 * 512 * 2;
    } else if (strstr(base, "text_rec") || strstr(base, "ocr_rec")) {
        n->kind = FAKE_TEXT_REC;
        n->info.in_w = 512;
        n->info.in_h = 32;
        n->info.outputs = 1;
        n->info.rank[0] = 3;
        n->info.dims[0][0] = FAKE_REC_STEPS;
        n->info.dims[0][1] = 1;
        n->info.dims[0][2] = FAKE_REC_CLASSES;
        n->info.count[0] = (size_t)FAKE_REC_STEPS * FAKE_REC_CLASSES;
    } else if (strstr(base, "face_det")) {
        static const uint32_t steps[3] = { 8, 16, 32 };
        static const uint32_t ch[3] = { 8, 4, 20 };
        int i;

        n->kind = FAKE_FACE_DET;
        n->info.in_w = 320;
        n->info.in_h = 320;
        n->info.outputs = 9;
        for (i = 0; i < 9; i++) {
            n->info.rank[i] = 4;
            n->info.dims[i][0] = 1;
            n->info.dims[i][1] = ch[i / 3];
            n->info.dims[i][2] = 320 / steps[i % 3];
            n->info.dims[i][3] = 320 / steps[i % 3];
            n->info.count[i] = (size_t)ch[i / 3] * (320 / steps[i % 3]) * (320 / steps[i % 3]);
        }
    } else if (strstr(base, "face_embed") || strstr(base, "face_recognition")) {
        n->kind = FAKE_FACE_EMBED;
        n->info.in_w = 112;
        n->info.in_h = 112;
        n->info.outputs = 1;
        n->info.rank[0] = 2;
        n->info.dims[0][0] = 1;
        n->info.dims[0][1] = FAKE_EMBED;
        n->info.count[0] = FAKE_EMBED;
    } else {
        snprintf(err, errlen, "fake nets: no emulation of %s", base);
        free(n);
        return -EPROTO;
    }
    if (net_script(n, script) != 0) {
        snprintf(err, errlen, "fake nets: bad script");
        free(n);
        return -EINVAL;
    }
    /* A large model read and checked by the runtime, pretended. */
    sleep_ms((int)n->open_ms);
    for (int i = 0; i < n->info.outputs; i++) {
        n->o[i] = calloc(n->info.count[i], sizeof(float));
        if (!n->o[i]) {
            vision_net_close(n);
            return -ENOMEM;
        }
    }
    n->out = n->o[0];
    *info = n->info;
    *np = n;
    return 0;
}

int vision_net_frame(struct vision_net *n, const struct pocketcam_frame *f)
{
    if (!n || pocketcam_frame_check(f) != 0) {
        return -EPROTO;
    }
    n->fw = f->width;
    n->fh = f->height;
    n->have_frame = true;
    return 0;
}

int vision_net_turn(struct vision_net *n, int rotation)
{
    if (!n || (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) {
        return -EINVAL;
    }
    n->turn = rotation;
    return 0;
}

/* A scripted box (sensor pixels) on the frame as it comes (turned). */
static struct vision_box on_frame(const struct vision_net *n, const struct vision_box *b)
{
    struct vision_box t = *b;
    uint32_t sw;
    uint32_t sh;

    vision_turned_size(n->fw, n->fh, n->turn, &sw, &sh);
    if (n->turn != 0) {
        vision_box_turn(sw, sh, n->turn, b, &t);
    }
    return t;
}

int vision_net_run(struct vision_net *n, const struct vision_box *win, bool stretch, uint8_t pad, int *pre_ms,
                   int *infer_ms)
{
    struct vision_box w;
    float rx;
    float ry;
    int i;

    (void)pad;
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
    if (pre_ms) {
        *pre_ms = 0;
    }
    if (infer_ms) {
        *infer_ms = 0;
    }
    rx = (float)n->info.in_w / (float)w.w;
    ry = (float)n->info.in_h / (float)w.h;
    if (!stretch) {
        rx = ry = rx < ry ? rx : ry;
    }
    for (i = 0; i < n->info.outputs; i++) {
        memset(n->o[i], 0, n->info.count[i] * sizeof(float));
    }
    if (n->kind == FAKE_FACE_DET) {
        /* Every anchor background, sure; then the faces. */
        for (i = 3; i < 6; i++) {
            size_t size = n->info.count[i] / 4;
            size_t j;

            for (j = 0; j < size; j++) {
                n->o[i][j] = 4.0f;
                n->o[i][2 * size + j] = 4.0f;
            }
        }
        for (i = 0; i < n->nface; i++) {
            struct vision_box b = on_frame(n, &n->face[i].box);
            struct vision_box in = {
                (int32_t)((float)(b.x - w.x) * rx), (int32_t)((float)(b.y - w.y) * ry),
                (int32_t)((float)b.w * rx), (int32_t)((float)b.h * ry),
            };

            /* A face whose middle is outside the window is not in it. */
            if (b.x + b.w / 2 < w.x || b.x + b.w / 2 >= w.x + w.w || b.y + b.h / 2 < w.y ||
                b.y + b.h / 2 >= w.y + w.h || in.w <= 0 || in.h <= 0) {
                continue;
            }
            face_put(n, &in);
        }
    } else if (n->kind == FAKE_FACE_EMBED) {
        /* Only an aligned face (vision_net_run_affine) says whose it is. */
    } else if (n->kind == FAKE_TEXT_DET) {
        for (i = 0; i < n->ntext; i++) {
            struct vision_box b = on_frame(n, &n->text[i].box);
            /* The middle of the line: a quarter of its smaller side off
             * each edge. */
            int32_t m = (b.w < b.h ? b.w : b.h) / 4;
            int32_t x0 = (int32_t)((float)(b.x + m - w.x) * rx);
            int32_t x1 = (int32_t)((float)(b.x + b.w - m - w.x) * rx);
            int32_t y0 = (int32_t)((float)(b.y + m - w.y) * ry);
            int32_t y1 = (int32_t)((float)(b.y + b.h - m - w.y) * ry);
            int32_t x;
            int32_t y;

            for (y = y0 < 0 ? 0 : y0; y < y1 && y < 512; y++) {
                for (x = x0 < 0 ? 0 : x0; x < x1 && x < 512; x++) {
                    n->out[((size_t)y * 512 + (size_t)x) * 2] = 0.9f;
                }
            }
        }
    } else {
        const struct fake_text *t = NULL;
        int step;
        size_t c;

        for (i = 0; i < n->ntext && !t; i++) {
            struct vision_box b = on_frame(n, &n->text[i].box);
            int32_t cx = b.x + b.w / 2;
            int32_t cy = b.y + b.h / 2;

            if (cx >= w.x && cx < w.x + w.w && cy >= w.y && cy < w.y + w.h) {
                t = &n->text[i];
            }
        }
        for (step = 0; step < FAKE_REC_STEPS; step++) {
            n->out[(size_t)step * FAKE_REC_CLASSES + FAKE_REC_CLASSES - 1] = 1.0f; /* the blank */
        }
        step = 2;
        for (c = 0; t && t->word[c] && step < FAKE_REC_STEPS; c++) {
            if (t->word[c] == '_') {
                step += 8;
                continue;
            }
            n->out[(size_t)step * FAKE_REC_CLASSES + FAKE_REC_CLASSES - 1] = 0.0f;
            n->out[(size_t)step * FAKE_REC_CLASSES + (size_t)(t->word[c] - '!')] = 1.0f;
            step += 4;
        }
    }
    n->have_out = true;
    return 0;
}

int vision_net_run_affine(struct vision_net *n, const float m[6], int *pre_ms, int *infer_ms)
{
    int best = -1;
    float best_d = 30.0f;   /* template pixels: further than this is no face */
    int i;

    if (!n || !m) {
        return -EINVAL;
    }
    if (!n->have_frame) {
        return -EPROTO;
    }
    for (i = 0; i < 6; i++) {
        if (!isfinite(m[i])) {
            return -EINVAL;
        }
    }
    if (pre_ms) {
        *pre_ms = 0;
    }
    if (infer_ms) {
        *infer_ms = 0;
    }
    for (i = 0; i < n->info.outputs; i++) {
        memset(n->o[i], 0, n->info.count[i] * sizeof(float));
    }
    if (n->kind != FAKE_FACE_EMBED) {
        n->have_out = true;
        return 0;
    }
    for (i = 0; i < n->nface; i++) {
        struct vision_box b = on_frame(n, &n->face[i].box);
        /* The middle of the face's points (vision_face's fake puts them at
         * x 0.5, y 0.6 of the box on average). */
        float cx = (float)b.x + 0.5f * (float)b.w;
        float cy = (float)b.y + 0.6f * (float)b.h;
        float tx = m[0] * cx + m[1] * cy + m[2];
        float ty = m[3] * cx + m[4] * cy + m[5];
        float d = hypotf(tx - 56.0f, ty - 71.9f);

        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    n->o[0][0] = 0.3f;
    n->o[0][best >= 0 ? 8 * n->face[best].who + 1 : FAKE_EMBED - 1] = 1.0f;
    n->have_out = true;
    return 0;
}

const float *vision_net_output(const struct vision_net *n, int i, size_t *count)
{
    if (!n || !n->have_out || i < 0 || i >= n->info.outputs) {
        return NULL;
    }
    if (count) {
        *count = n->info.count[i];
    }
    return n->o[i];
}

void vision_net_close(struct vision_net *n)
{
    if (n) {
        for (int i = 0; i < VISION_NET_OUTPUTS; i++) {
            free(n->o[i]);
        }
        free(n);
    }
}

int vision_kpu_describe(const char *path, char *out, size_t len)
{
    (void)path;
    if (out && len) {
        snprintf(out, len, "fake detector: no model runtime in this build\n");
    }
    return -ENOTSUP;
}
