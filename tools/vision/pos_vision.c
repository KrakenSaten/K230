/*
 * pos-vision: the Vision app's helper, the one process that opens the camera
 * and the detector for it (docs/apps/VISION.md).
 *
 *   pos-vision session [--backend NAME] [--fake SCRIPT] [--config CFG]
 *                      [--model FILE] [--kpu SCRIPT]
 *       The helper. stdin and stdout are a socketpair to the shell,
 *       descriptor 3 the shared memory; the protocol is
 *       core/pocketvision/pocketvision_proto.h. It lives exactly as long as
 *       the Vision screen: it leaves on `quit`, when the shell closes its
 *       end, on SIGTERM, and - through PR_SET_PDEATHSIG, set by the session
 *       before exec - when the shell dies.
 *   pos-vision probe [--backend NAME] [--fake SCRIPT] [--config CFG] [--model FILE]
 *       Open the camera and the model, say what they are, close both.
 *       Exit 0, 3 without a camera, 5 without a model.
 *   pos-vision bench [N] [--backend NAME] [--config CFG] [--model FILE]
 *                    [--turn R] [--save FILE.ppm [--save-every N]] [--image FILE.ppm]
 *       Stream N frames (default 100) through the detector and print the
 *       timings and what was found, for the unit A gate; and every vehicle
 *       candidate from VISION_BENCH_VEHICLE_FLOOR up, with its size in the
 *       model's pixels and whether the threshold takes it.
 *       --turn R gives the detector the frame turned clockwise by R, as the
 *       session does with the preview's turn (bench has no screen to take
 *       it from: unit A held landscape with the screen the right way up is
 *       180, portrait 90); boxes stay in frame pixels.
 *       --save writes frame VISION_BENCH_SAVE_AT (once the auto exposure
 *       has settled) as a PPM with that frame's vehicle boxes drawn in;
 *       --save-every N also every N frames after it, numbered.
 *       --image FILE.ppm feeds that picture to the detector as every frame
 *       instead of the camera, so one saved scene can be compared as it
 *       was taken and changed on the same model.
 *
 * The camera is Camera's own layer (core/pocketcam): the same backends, the
 * same V4L2 node, the same close on the way out. Nothing opens the sensor
 * twice, because Camera's helper and this one are never alive together:
 * each lives only while its own screen is open, and the driver refuses a
 * second opener anyway (CAMERA_PLATFORM_RESEARCH.md, U8).
 *
 * The camera config defaults to the preview node in planar BGR
 * (VISION_CAMERA_CONFIG), which the AI2D engine takes as it is; --config
 * (or $POCKETOS_VISION_CAMERA_CONFIG) replaces it, so a bench can try
 * another node, size or format without a rebuild.
 *
 * Per frame, synchronously: the detector (bounded by its own hardware, ~tens
 * of ms), decode, suppression, tracking, the line; then the preview picture
 * at most every POCKETCAM_PREVIEW_MIN_INTERVAL_MS. Frames that arrive while
 * this runs wait in the driver's buffers, or are dropped by it: the helper
 * never queues them, so it never falls behind.
 *
 * Exit codes: 0 done, 2 usage or setup, 3 no camera, 4 the camera went
 * away, 5 no model.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketcam/pocketcam.h"
#include "pocketcam/pocketcam_convert.h"
#include "pocketvision/pocketvision_proto.h"
#include "pocketvision/vision_decode.h"
#include "pocketvision/vision_geom.h"
#include "pocketvision/vision_kpu.h"
#include "pocketvision/vision_labels.h"
#include "pocketvision/vision_line.h"
#include "pocketvision/vision_nms.h"
#include "pocketvision/vision_pixels.h"
#include "pocketvision/vision_track.h"
#include "pocketvision/vision_traffic.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define EXIT_NOCAMERA 3
#define EXIT_LOST 4
#define EXIT_NOMODEL 5
#define FRAME_WAIT_MS 50
/* The preview node, planar BGR: what the KPU demos read (yolo/src/main.cc
 * asks /dev/video2 for BG3P). 640 x 360 is Camera's proven preview size. */
#define VISION_CAMERA_CONFIG "fmt=bg3p"
#define VISION_MODEL_DEFAULT "/usr/share/doors/vision/yolov8n.kmodel"
#define VISION_CONF_MIN 350   /* the vendor's default conf_thres 0.35 */
#define VISION_NMS_IOU 650    /* and nms_thres 0.65 */
/* bench: how far below the threshold vehicle candidates are listed, so a
 * run says whether a missed car was near the threshold or nowhere. */
#define VISION_BENCH_VEHICLE_FLOOR 100
/* The first frame bench saves: the ISP's auto exposure starts over with
 * every camera open and has settled well before this (a first frame saved
 * at a bright window, unit A 2026-09-29, was nearly white). */
#define VISION_BENCH_SAVE_AT 60
/* A box this much (per-mille of itself) inside a larger one of its class is
 * a duplicate (vision_nms.h). */
#define VISION_NESTED_PM 850
/* Frames in a row whose tensor is not a tensor: the model is not working. */
#define VISION_BAD_LIMIT 10
#define VISION_COLOR_TOL_DEFAULT 96

enum helper_mode {
    MODE_DETECT = 0,
    MODE_TRACK,      /* DETECT's pipeline; the screen shows ids and a line */
    MODE_TRAFFIC,
    MODE_COLOR,
    MODE_EDGE,
    MODE_TRACE
};

/* SIGTERM and SIGINT are BLOCKED for the helper's whole life and looked for
 * between frames, never taken by a handler. A signal handled while the
 * process waits on the KPU or the AI2D engine ends that wait early (EINTR)
 * with the hardware still working into buffers the helper then frees on its
 * way out - and on unit B (2026-09-28) a SIGTERM mid-stream was followed by
 * a silent whole-unit freeze, where the same runs to completion never froze
 * it. Blocked, a stop waits for the frame in hand to finish (tens of ms),
 * then the camera and the detector are closed with nothing in flight. */
static sigset_t stop_signals;

static bool stop_requested(void)
{
    sigset_t pending;

    return sigpending(&pending) == 0 &&
           (sigismember(&pending, SIGTERM) == 1 || sigismember(&pending, SIGINT) == 1);
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- events ------------------------------------------------------------------ */

static int out_broken;

static void say(const char *fmt, ...)
{
    char line[VISION_LINE_MAX];
    va_list ap;
    int n;
    size_t off = 0;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(line) - 2) {
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';
    while (off < (size_t)n && !out_broken) {
        ssize_t w = write(1, line + off, (size_t)n - off);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            out_broken = 1;
            break;
        }
        off += (size_t)w;
    }
}

static const char *clean(const char *s, char *buf, size_t len)
{
    size_t i;

    for (i = 0; s && s[i] && i + 1 < len; i++) {
        unsigned char c = (unsigned char)s[i];

        buf[i] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
    }
    buf[i] = '\0';
    return buf;
}

/* ---- the session -------------------------------------------------------------- */

struct session {
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct vision_kpu *kpu;
    struct vision_kpu_info model;
    uint8_t *shm;
    bool held[POCKETCAM_SLOTS];
    uint32_t view_w;
    uint32_t view_h;
    int display_rotation;
    bool streaming;
    int64_t last_sent_ms;
    int64_t last_frame_ms;
    int64_t last_stall_ms;
    int64_t last_stats_ms;
    int malformed_run;
    uint32_t bad_run;        /* tensors refused in a row */
    uint32_t bad_total;
    /* the pipeline */
    struct vision_det cands[VISION_MAX_CANDIDATES];
    struct vision_det dets[VISION_MAX_DETECTIONS];
    /* The frame turned upright for the detector (upright_input). */
    uint8_t *upright;
    size_t upright_size;
    struct vision_tracker tracker;
    /* The count line and the two speed lines, in frame pixels, and as
     * asked for (per-mille of the view; [i][0] < 0: none). */
    struct vision_line line[VISION_LINES];
    int32_t line_pm[VISION_LINES][4];
    struct vision_counts counts;
    bool traffic;
    struct vision_traffic tf;
    /* The pixel modes (vision_pixels.h): the detector idles, the preview
     * is the work. */
    enum helper_mode mode;
    struct vision_rgb target;
    bool have_target;
    uint32_t tol;
    int32_t sample_x;             /* a sample asked for: -1 none */
    int32_t sample_y;
    uint32_t edge_thr;
    bool trace_dark;
    /* measurements */
    uint32_t inferred;            /* frames through the detector since the last stats */
    int last_pre_ms;
    int last_infer_ms;
    int last_post_ms;
    struct timespec cpu_at;
    int64_t cpu_wall_ms;
    char in[VISION_LINE_MAX];
    size_t in_len;
    bool in_overlong;
    bool in_eof;
    bool quit;
    int exit_code;
};

static int map_shm(struct session *s)
{
    struct stat st;
    const struct pocketcam_shm_header *h;

    if (fstat(POCKETCAM_SHM_FD, &st) != 0 || st.st_size < (off_t)POCKETCAM_SHM_BYTES) {
        return -1;
    }
    s->shm = mmap(NULL, POCKETCAM_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, POCKETCAM_SHM_FD,
                  0);
    if (s->shm == MAP_FAILED) {
        s->shm = NULL;
        return -1;
    }
    h = (const struct pocketcam_shm_header *)s->shm;
    if (h->magic != POCKETCAM_SHM_MAGIC || h->version != POCKETCAM_PROTO_VERSION ||
        h->slots != POCKETCAM_SLOTS || h->slot_bytes != POCKETCAM_SLOT_BYTES) {
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        s->shm = NULL;
        return -1;
    }
    return 0;
}

static uint16_t *slot_pixels(struct session *s, int slot)
{
    return (uint16_t *)(void *)(s->shm + pocketcam_slot_offset((uint32_t)slot));
}

static int parse_rotation(const char *w, int *deg)
{
    char *end;
    long v = strtol(w, &end, 10);

    if (end == w || *end || (v != 0 && v != 90 && v != 180 && v != 270)) {
        return -1;
    }
    *deg = (int)v;
    return 0;
}

static void lose(struct session *s, int err)
{
    char t[96];

    say("lost %s", clean(pocketcam_strerror(err), t, sizeof(t)));
    s->exit_code = EXIT_LOST;
    s->quit = true;
}

static struct vision_view view_of(const struct session *s)
{
    struct vision_view v = {
        .frame_w = s->info.preview_w,
        .frame_h = s->info.preview_h,
        .rotation = pocketcam_view_rotation(s->info.mount_rotation, s->display_rotation),
        .mirror = s->info.mount_mirror,
        .view_w = s->view_w,
        .view_h = s->view_h,
    };

    return v;
}

/* A line is chosen on the picture (per-mille of the view) and counted
 * among the tracks, which live in frame pixels: its two ends go back
 * through the picture's geometry. A mirrored picture swaps the ends, so
 * "left of the line's direction" on the screen stays what it was. */
static void place_line(struct session *s, int idx)
{
    struct vision_view v = view_of(s);
    struct vision_line *l = &s->line[idx];
    const int32_t *pm = s->line_pm[idx];
    int32_t vx0;
    int32_t vy0;
    int32_t vx1;
    int32_t vy1;
    int32_t a[2];
    int32_t b[2];

    if (pm[0] < 0 || s->view_w == 0 || s->view_h == 0) {
        l->enabled = false;
        return;
    }
    vx0 = (int32_t)(((int64_t)pm[0] * (s->view_w - 1)) / 1000);
    vy0 = (int32_t)(((int64_t)pm[1] * (s->view_h - 1)) / 1000);
    vx1 = (int32_t)(((int64_t)pm[2] * (s->view_w - 1)) / 1000);
    vy1 = (int32_t)(((int64_t)pm[3] * (s->view_h - 1)) / 1000);
    if (vision_unmap_point(&v, vx0, vy0, &a[0], &a[1]) != 0 ||
        vision_unmap_point(&v, vx1, vy1, &b[0], &b[1]) != 0) {
        l->enabled = false;
        return;
    }
    if (v.mirror) {
        int32_t t0 = a[0];
        int32_t t1 = a[1];

        a[0] = b[0];
        a[1] = b[1];
        b[0] = t0;
        b[1] = t1;
    }
    l->x0 = a[0];
    l->y0 = a[1];
    l->x1 = b[0];
    l->y1 = b[1];
    /* The dead band's cap, from the frame: a box that spans most of it
     * still has to be able to cross. */
    l->dead_max = (int32_t)((s->info.preview_w < s->info.preview_h ? s->info.preview_w : s->info.preview_h) /
                            VISION_LINE_DEAD_DIV);
    l->enabled = true;
}

static void place_lines(struct session *s)
{
    int i;

    for (i = 0; i < VISION_LINES; i++) {
        place_line(s, i);
    }
}

static void say_counts(struct session *s)
{
    say("count %u %u", s->counts.ab, s->counts.ba);
}

static void say_traffic(struct session *s)
{
    char line[VISION_LINE_MAX];
    size_t off;
    int n;
    int i;

    n = snprintf(line, sizeof(line), "traffic %u %u %u %u %u %u %u %u", s->tf.total_ab, s->tf.total_ba,
                 s->tf.cur_kmh10, s->tf.last_kmh10, s->tf.max_kmh10, vision_traffic_mean_kmh10(&s->tf),
                 s->tf.n, s->tf.rejected);
    if (n < 0) {
        return;
    }
    off = (size_t)n;
    for (i = 0; i < VISION_TRAFFIC_CLASSES && off < sizeof(line); i++) {
        n = snprintf(line + off, sizeof(line) - off, " %u:%u", s->tf.count_ab[i], s->tf.count_ba[i]);
        if (n < 0) {
            return;
        }
        off += (size_t)n;
    }
    say("%s", line);
    s->tf.changed = false;
}

static bool pixel_mode(const struct session *s)
{
    return s->mode == MODE_COLOR || s->mode == MODE_EDGE || s->mode == MODE_TRACE;
}

/* The mode. In TRAFFIC the tracker matches across a class group and the
 * detections are filtered to traffic; in a pixel mode the detector idles
 * and the boxes go. */
static void set_mode(struct session *s, enum helper_mode mode)
{
    bool traffic = mode == MODE_TRAFFIC;

    if (s->mode == mode) {
        return;
    }
    s->mode = mode;
    s->traffic = traffic;
    if (traffic) {
        s->tracker.group = s->tf.group;
        s->tracker.group_classes = s->tf.classes;
    } else {
        s->tracker.group = NULL;
        s->tracker.group_classes = 0;
    }
    /* A new way of looking starts a new count. */
    vision_tracker_clear(&s->tracker);
    memset(&s->counts, 0, sizeof(s->counts));
    vision_traffic_reset(&s->tf);
    say_counts(s);
    if (traffic) {
        say_traffic(s);
    }
    if (pixel_mode(s)) {
        say("det 0 0");
    }
}

/* A pixel mode's pass over the preview just drawn into a slot, and its
 * word. */
static void process_pixels(struct session *s, uint16_t *px)
{
    int64_t t0 = mono_ms();

    if (s->mode == MODE_COLOR) {
        struct vision_color_result r;

        if (s->sample_x >= 0 &&
            vision_pixels_sample(px, s->view_w, s->view_h, s->view_w, s->sample_x, s->sample_y, &s->target) == 0) {
            s->have_target = true;
        }
        s->sample_x = -1;
        if (s->have_target &&
            vision_pixels_color(px, s->view_w, s->view_h, s->view_w, s->target, s->tol, &r) == 0) {
            say("color %u %u %u %u %d %d", s->target.r, s->target.g, s->target.b,
                r.total ? (unsigned)(((uint64_t)r.matched * 1000) / r.total) : 0u, r.cx, r.cy);
        }
    } else if (s->mode == MODE_EDGE) {
        struct vision_edge_result r;

        if (vision_pixels_edge(px, s->view_w, s->view_h, s->view_w, s->edge_thr, &r) == 0) {
            say("edge %u", r.total ? (unsigned)(((uint64_t)r.strong * 1000) / r.total) : 0u);
        }
    } else if (s->mode == MODE_TRACE) {
        struct vision_trace_result r;

        if (vision_pixels_trace(px, s->view_w, s->view_h, s->view_w, s->trace_dark, &r) == 0) {
            say("trace %d %d %d %u", r.found ? 1 : 0, r.offset_pm, r.slope_pm, r.rows);
        }
    }
    s->last_post_ms = (int)(mono_ms() - t0);
    s->last_infer_ms = 0;
    s->last_pre_ms = 0;
    s->inferred++;
}

/* The way a track has gone on the picture: its displacement since it was
 * first seen, mapped into view pixels, when it is clearly more than
 * jitter (half the box's smaller side, at least 16 px). */
static int direction_of(const struct vision_view *v, const struct vision_track *t,
                        const struct vision_box *shown)
{
    int32_t cx;
    int32_t cy;
    int32_t ax;
    int32_t ay;
    int32_t bx;
    int32_t by;
    int32_t dx;
    int32_t dy;
    int32_t thr = shown->w < shown->h ? shown->w / 2 : shown->h / 2;

    if (thr < 16) {
        thr = 16;
    }
    vision_box_centre(&t->box, &cx, &cy);
    if (vision_map_point(v, t->ox, t->oy, &ax, &ay) != 0 || vision_map_point(v, cx, cy, &bx, &by) != 0) {
        return VISION_DIR_NONE;
    }
    dx = bx - ax;
    dy = by - ay;
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        if (dx >= thr) {
            return VISION_DIR_RIGHT;
        }
        if (-dx >= thr) {
            return VISION_DIR_LEFT;
        }
        return VISION_DIR_NONE;
    }
    if (dy >= thr) {
        return VISION_DIR_DOWN;
    }
    if (-dy >= thr) {
        return VISION_DIR_UP;
    }
    return VISION_DIR_NONE;
}

static void say_stats(struct session *s, int64_t now)
{
    struct rusage ru;
    int64_t cpu_ms = 0;
    int cpu_pct = 0;
    long rss_kb = 0;
    int64_t wall = now - s->cpu_wall_ms;
    FILE *fp;

    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        struct timespec c = { ru.ru_utime.tv_sec + ru.ru_stime.tv_sec,
                              (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000L };
        int64_t used = (int64_t)c.tv_sec * 1000 + c.tv_nsec / 1000000;
        int64_t before = (int64_t)s->cpu_at.tv_sec * 1000 + s->cpu_at.tv_nsec / 1000000;

        cpu_ms = used - before;
        s->cpu_at = c;
        rss_kb = ru.ru_maxrss;
    }
    /* The resident set now, not its high-water mark: /proc/self/statm's
     * second field, in pages. Falls back to getrusage's maximum. */
    fp = fopen("/proc/self/statm", "r");
    if (fp) {
        long size;
        long resident;

        if (fscanf(fp, "%ld %ld", &size, &resident) == 2) {
            rss_kb = resident * (sysconf(_SC_PAGESIZE) / 1024);
        }
        fclose(fp);
    }
    if (wall > 0) {
        cpu_pct = (int)((cpu_ms * 100) / wall);
    }
    s->cpu_wall_ms = now;
    say("stats %u %d %d %d %d %ld %u %u", wall > 0 ? (unsigned)((s->inferred * 10000) / wall) : 0u,
        s->last_infer_ms, s->last_pre_ms, s->last_post_ms, cpu_pct, rss_kb, s->bad_total,
        s->tracker.dropped);
    s->inferred = 0;
    s->last_stats_ms = now;
}

/* What the tracker holds, in view pixels, as one line. */
static void say_tracks(struct session *s, uint32_t seq)
{
    char line[VISION_LINE_MAX];
    struct vision_view v = view_of(s);
    char items[VISION_LINE_MAX];
    size_t off = 0;
    int shown = 0;
    int i;

    items[0] = '\0';
    if (s->view_w == 0) {
        return;
    }
    for (i = 0; i < s->tracker.count && shown < VISION_MAX_SHOWN; i++) {
        const struct vision_track *t = &s->tracker.t[i];
        struct vision_box b;
        int n;

        /* A track the frame did not confirm draws nothing this time; one
         * coasting on a prediction is still shown, so a box does not
         * flicker on a missed frame. */
        if (!t->confirmed && !t->seen) {
            continue;
        }
        if (vision_map_box(&v, &t->box, &b) != 1) {
            continue;
        }
        n = snprintf(items + off, sizeof(items) - off, " %u:%u:%u:%d:%d:%d:%d:%d:%u",
                     t->confirmed ? t->id : 0u, t->cls, t->conf, b.x, b.y, b.w, b.h,
                     t->confirmed ? direction_of(&v, t, &b) : VISION_DIR_NONE,
                     s->traffic && t->confirmed ? vision_traffic_track_speed(&s->tf, t->id) : 0u);
        if (n < 0 || off + (size_t)n >= sizeof(items) - 1) {
            break;
        }
        off += (size_t)n;
        shown++;
    }
    snprintf(line, sizeof(line), "det %u %d%s", seq, shown, items);
    say("%s", line);
}

/* The picture the detector is given: the sensor frame turned by `rotation`
 * - the preview's own turn, so the detector sees the scene upright as the
 * screen shows it (vision_geom.h). A planar BGR frame is turned into buf
 * (three planes of the frame's width x height, the caller's); any other
 * format is only ever the fake camera's, which only the fake detector
 * reads, and keeps its pixels with the turned size. 0, or -1. */
static int upright_input(const struct pocketcam_frame *f, int rotation, uint8_t *buf,
                         struct pocketcam_frame *in)
{
    uint32_t tw;
    uint32_t th;

    *in = *f;
    if (rotation == 0) {
        return 0;
    }
    vision_turned_size(f->width, f->height, rotation, &tw, &th);
    if (f->format == POCKETCAM_FMT_BG3P) {
        if (!buf || vision_turn_planes(f->data, f->width, f->height, f->stride, 3, rotation, buf) != 0) {
            return -1;
        }
        in->data = buf;
        in->bytes = (size_t)3 * tw * th;
    }
    in->width = tw;
    in->height = th;
    in->stride = tw;
    return 0;
}

/* The detector's boxes, found on the turned picture, back in frame pixels. */
static void unturn_dets(struct vision_det *d, int n, const struct pocketcam_frame *f, int rotation)
{
    int i;

    for (i = 0; rotation != 0 && i < n; i++) {
        vision_box_unturn(f->width, f->height, rotation, &d[i].box, &d[i].box);
    }
}

static void detect(struct session *s, const struct pocketcam_frame *f, int64_t now)
{
    const float *out;
    size_t count;
    uint32_t dims[3];
    int pre_ms = 0;
    int infer_ms = 0;
    int64_t t0;
    int r;
    int n;
    uint32_t bad = 0;
    /* The detector sees the scene upright, as the screen shows it. */
    int rotation = view_of(s).rotation;
    size_t need = (size_t)3 * f->width * f->height;
    struct pocketcam_frame in;
    int64_t turn_t0 = mono_ms();

    if (rotation != 0 && s->upright_size < need) {
        uint8_t *b = realloc(s->upright, need);

        if (b) {
            s->upright = b;
            s->upright_size = need;
        }
    }
    r = upright_input(f, rotation, s->upright_size >= need ? s->upright : NULL, &in);
    struct vision_decode_params p = {
        .in_w = s->model.in_w,
        .in_h = s->model.in_h,
        .classes = s->model.classes,
        .frame_w = in.width,
        .frame_h = in.height,
        .conf_min = VISION_CONF_MIN,
    };
    /* Turning the picture is part of preparing it. */
    int turn_ms = (int)(mono_ms() - turn_t0);

    if (r == 0) {
        r = vision_kpu_turn(s->kpu, rotation);
    }
    if (r == 0) {
        r = vision_kpu_infer(s->kpu, &in, &out, &count, dims, &pre_ms, &infer_ms);
    }
    pre_ms += turn_ms;
    if (r != 0) {
        char t[96];

        say("error infer %s", clean(r == -EPROTO ? "the frame is not what the model takes"
                                                  : "the detector failed",
                                    t, sizeof(t)));
        s->exit_code = EXIT_USAGE;
        s->quit = true;
        return;
    }
    t0 = mono_ms();
    n = vision_decode(out, count, dims, &p, s->cands, VISION_MAX_CANDIDATES, &bad);
    if (n < 0 || (bad > 0 && bad * 2 > s->model.rows)) {
        /* Not a detector's tensor: the shape lies, or the numbers are not
         * numbers. Counted, said, and the frame otherwise ignored; too many
         * in a row and the detector is declared broken. */
        s->bad_total++;
        s->bad_run++;
        say("malformed %u", s->bad_run);
        if (s->bad_run >= VISION_BAD_LIMIT) {
            say("error model the detector's output is not a tensor of its declared shape");
            s->exit_code = EXIT_NOMODEL;
            s->quit = true;
        }
        return;
    }
    s->bad_run = 0;
    unturn_dets(s->cands, n, f, rotation);
    n = vision_nms(s->cands, n, VISION_NMS_IOU, s->dets, VISION_MAX_DETECTIONS);
    n = vision_nms_nested(s->dets, n, VISION_NESTED_PM);
    if (s->traffic) {
        /* Only traffic is tracked: anything else the model saw is left
         * out here, so a chair never takes a track slot or a count. */
        int kept = 0;
        int i;

        for (i = 0; i < n; i++) {
            if (vision_traffic_wanted(&s->tf, s->dets[i].cls)) {
                s->dets[kept++] = s->dets[i];
            }
        }
        n = kept;
    }
    vision_tracker_update(&s->tracker, s->dets, n);
    {
        struct vision_crossing x[VISION_MAX_TRACKS];
        int nx;

        nx = vision_line_count(&s->line[VISION_LINE_COUNT], VISION_LINE_COUNT, &s->tracker, &s->counts, x,
                               VISION_MAX_TRACKS);
        if (nx > 0) {
            say_counts(s);
            if (s->traffic) {
                vision_traffic_counted(&s->tf, x, nx > VISION_MAX_TRACKS ? VISION_MAX_TRACKS : nx);
            }
        }
        if (s->traffic) {
            struct vision_counts unused = { 0, 0 };

            nx = vision_line_count(&s->line[VISION_LINE_SPEED_A], VISION_LINE_SPEED_A, &s->tracker, &unused,
                                   x, VISION_MAX_TRACKS);
            vision_traffic_crossed(&s->tf, 1, x, nx > VISION_MAX_TRACKS ? VISION_MAX_TRACKS : nx, now);
            nx = vision_line_count(&s->line[VISION_LINE_SPEED_B], VISION_LINE_SPEED_B, &s->tracker, &unused,
                                   x, VISION_MAX_TRACKS);
            vision_traffic_crossed(&s->tf, 2, x, nx > VISION_MAX_TRACKS ? VISION_MAX_TRACKS : nx, now);
            vision_traffic_settle(&s->tf, &s->tracker, now);
            if (s->tf.changed) {
                say_traffic(s);
            }
        }
    }
    s->last_pre_ms = pre_ms;
    s->last_infer_ms = infer_ms;
    s->last_post_ms = (int)(mono_ms() - t0);
    s->inferred++;
    say_tracks(s, f->seq);
}

/* `n` per-mille words, or the one word `off` (every value -1). 0, or -1
 * for anything else. */
static int parse_pm(char **save, int *v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        char *a = strtok_r(NULL, " ", save);

        if (i == 0 && a && strcmp(a, "off") == 0) {
            for (i = 0; i < n; i++) {
                v[i] = -1;
            }
            return 0;
        }
        if (!a || sscanf(a, "%d", &v[i]) != 1 || v[i] < 0 || v[i] > 1000) {
            return -1;
        }
    }
    return 0;
}

static void command(struct session *s, char *line)
{
    char *save = NULL;
    char *w = strtok_r(line, " ", &save);

    if (!w) {
        return;
    }
    if (strcmp(w, "view") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        char *b = strtok_r(NULL, " ", &save);
        char *c = strtok_r(NULL, " ", &save);
        unsigned vw;
        unsigned vh;
        int rot;

        if (!a || !b || !c || sscanf(a, "%u", &vw) != 1 || sscanf(b, "%u", &vh) != 1 ||
            parse_rotation(c, &rot) != 0 || vw == 0 || vh == 0 || vw > POCKETCAM_VIEW_MAX_W ||
            vh > POCKETCAM_VIEW_MAX_H) {
            return;
        }
        s->view_w = vw;
        s->view_h = vh;
        s->display_rotation = rot;
        place_lines(s);
    } else if (strcmp(w, "start") == 0) {
        if (!s->streaming) {
            int r = pocketcam_start(&s->cam);

            if (r != 0) {
                lose(s, r);
                return;
            }
            s->streaming = true;
            s->last_frame_ms = mono_ms();
            s->last_stall_ms = s->last_frame_ms;
            s->last_stats_ms = s->last_frame_ms;
            s->cpu_wall_ms = s->last_frame_ms;
            s->inferred = 0;
        }
    } else if (strcmp(w, "stop") == 0) {
        if (s->streaming) {
            pocketcam_stop(&s->cam);
            s->streaming = false;
        }
        say("stopped");
    } else if (strcmp(w, "release") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        int slot;

        if (a && sscanf(a, "%d", &slot) == 1 && slot >= 0 && slot < POCKETCAM_SLOTS) {
            s->held[slot] = false;
        }
    } else if (strcmp(w, "line") == 0) {
        int v[4];

        if (parse_pm(&save, v, 4) != 0) {
            return;
        }
        memcpy(s->line_pm[VISION_LINE_COUNT], v, sizeof(v));
        place_line(s, VISION_LINE_COUNT);
        /* A new line starts a new count; the tracks learn their side afresh. */
        vision_line_forget(&s->tracker, VISION_LINE_COUNT);
        memset(&s->counts, 0, sizeof(s->counts));
        say_counts(s);
        if (s->traffic) {
            memset(s->tf.count_ab, 0, sizeof(s->tf.count_ab));
            memset(s->tf.count_ba, 0, sizeof(s->tf.count_ba));
            s->tf.total_ab = 0;
            s->tf.total_ba = 0;
            say_traffic(s);
        }
    } else if (strcmp(w, "speed") == 0) {
        int v[8];

        if (parse_pm(&save, v, 8) != 0) {
            return;
        }
        memcpy(s->line_pm[VISION_LINE_SPEED_A], v, 4 * sizeof(v[0]));
        memcpy(s->line_pm[VISION_LINE_SPEED_B], v + 4, 4 * sizeof(v[0]));
        place_line(s, VISION_LINE_SPEED_A);
        place_line(s, VISION_LINE_SPEED_B);
        vision_line_forget(&s->tracker, VISION_LINE_SPEED_A);
        vision_line_forget(&s->tracker, VISION_LINE_SPEED_B);
        /* Lines that moved: a crossing timed against the old ones is
         * worth nothing. */
        memset(s->tf.slot, 0, sizeof(s->tf.slot));
        s->tf.cur_kmh10 = 0;
        s->tf.cur_id = 0;
        if (s->traffic) {
            say_traffic(s);
        }
    } else if (strcmp(w, "distance") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        unsigned cm;

        if (!a || sscanf(a, "%u", &cm) != 1 || vision_traffic_set_distance(&s->tf, cm) != 0) {
            return;
        }
        if (s->traffic) {
            say_traffic(s);
        }
    } else if (strcmp(w, "mode") == 0) {
        char *a = strtok_r(NULL, " ", &save);

        if (a && strcmp(a, "traffic") == 0) {
            set_mode(s, MODE_TRAFFIC);
        } else if (a && strcmp(a, "detect") == 0) {
            set_mode(s, MODE_DETECT);
        } else if (a && strcmp(a, "track") == 0) {
            set_mode(s, MODE_TRACK);
        } else if (a && strcmp(a, "color") == 0) {
            set_mode(s, MODE_COLOR);
        } else if (a && strcmp(a, "edge") == 0) {
            set_mode(s, MODE_EDGE);
        } else if (a && strcmp(a, "trace") == 0) {
            set_mode(s, MODE_TRACE);
        }
    } else if (strcmp(w, "color") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        char *b = strtok_r(NULL, " ", &save);
        char *c = strtok_r(NULL, " ", &save);
        unsigned v[3];

        if (a && strcmp(a, "off") == 0) {
            s->have_target = false;
            s->sample_x = -1;
            return;
        }
        if (!a || !b || !c || sscanf(a, "%u", &v[0]) != 1 || sscanf(b, "%u", &v[1]) != 1 ||
            sscanf(c, "%u", &v[2]) != 1 || v[0] > 255 || v[1] > 255 || v[2] > 255) {
            return;
        }
        s->target.r = (uint8_t)v[0];
        s->target.g = (uint8_t)v[1];
        s->target.b = (uint8_t)v[2];
        s->have_target = true;
    } else if (strcmp(w, "sample") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        char *b = strtok_r(NULL, " ", &save);
        int x;
        int y;

        if (!a || !b || sscanf(a, "%d", &x) != 1 || sscanf(b, "%d", &y) != 1 || x < 0 || y < 0 ||
            x >= (int)POCKETCAM_VIEW_MAX_W || y >= (int)POCKETCAM_VIEW_MAX_H) {
            return;
        }
        s->sample_x = x;
        s->sample_y = y;
    } else if (strcmp(w, "tol") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        unsigned tol;

        if (a && sscanf(a, "%u", &tol) == 1 && tol <= VISION_COLOR_TOL_MAX) {
            s->tol = tol;
        }
    } else if (strcmp(w, "edge") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        unsigned thr;

        if (a && sscanf(a, "%u", &thr) == 1 && thr <= 255) {
            s->edge_thr = thr;
        }
    } else if (strcmp(w, "trace") == 0) {
        char *a = strtok_r(NULL, " ", &save);

        if (a && strcmp(a, "dark") == 0) {
            s->trace_dark = true;
        } else if (a && strcmp(a, "light") == 0) {
            s->trace_dark = false;
        }
    } else if (strcmp(w, "reset") == 0) {
        memset(&s->counts, 0, sizeof(s->counts));
        vision_tracker_clear(&s->tracker);
        vision_traffic_reset(&s->tf);
        say_counts(s);
        if (s->traffic) {
            say_traffic(s);
        }
    } else if (strcmp(w, "quit") == 0) {
        s->quit = true;
    }
    /* Anything else: a word this helper does not know. Ignored. */
}

static void read_commands(struct session *s, int timeout_ms)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    char buf[512];
    ssize_t n;
    ssize_t i;

    if (poll(&p, 1, timeout_ms) <= 0) {
        return;
    }
    n = read(0, buf, sizeof(buf));
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        s->in_eof = true;
        return;
    }
    for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            if (!s->in_overlong) {
                s->in[s->in_len] = '\0';
                command(s, s->in);
            }
            s->in_len = 0;
            s->in_overlong = false;
            if (s->quit) {
                return;
            }
        } else if (s->in_overlong) {
            continue;
        } else if (s->in_len + 1 >= sizeof(s->in)) {
            s->in_overlong = true;
            s->in_len = 0;
        } else {
            s->in[s->in_len++] = buf[i];
        }
    }
}

static int free_preview_slot(const struct session *s)
{
    int i;

    for (i = 0; i < POCKETCAM_PREVIEW_SLOTS; i++) {
        if (!s->held[i]) {
            return i;
        }
    }
    return -1;
}

static void stream_once(struct session *s)
{
    struct pocketcam_frame f;
    int64_t now;
    int r = pocketcam_next(&s->cam, FRAME_WAIT_MS, &f);

    now = mono_ms();
    if (r == -ETIMEDOUT) {
        if (now - s->last_frame_ms >= POCKETCAM_STALL_MS &&
            now - s->last_stall_ms >= POCKETCAM_STALL_MS) {
            say("stall %lld", (long long)(now - s->last_frame_ms));
            s->last_stall_ms = now;
        }
        return;
    }
    if (r == -EPROTO) {
        say("malformed %d", ++s->malformed_run);
        if (s->malformed_run >= POCKETCAM_MALFORMED_LIMIT) {
            lose(s, -EPROTO);
        }
        return;
    }
    if (r != 0) {
        lose(s, r);
        return;
    }
    s->malformed_run = 0;
    s->last_frame_ms = now;
    /* The detector first, on the frame as it came; the picture after, so
     * the boxes said for this frame are drawn on this frame. In a pixel
     * mode the detector idles: the picture is the work, at the preview's
     * own rate. */
    if (!pixel_mode(s)) {
        detect(s, &f, now);
    }
    if (!s->quit && s->view_w && now - s->last_sent_ms >= POCKETCAM_PREVIEW_MIN_INTERVAL_MS) {
        int slot = free_preview_slot(s);

        if (slot >= 0 &&
            pocketcam_to_rgb565(&f, pocketcam_view_rotation(s->info.mount_rotation, s->display_rotation),
                                s->info.mount_mirror, POCKETCAM_FIT_COVER, slot_pixels(s, slot),
                                s->view_w, s->view_h, s->view_w) == 0) {
            if (pixel_mode(s)) {
                process_pixels(s, slot_pixels(s, slot));
            }
            s->held[slot] = true;
            s->last_sent_ms = now;
            say("frame %d %u %u %u", slot, f.seq, s->view_w, s->view_h);
        }
    }
    pocketcam_release(&s->cam, &f);
    if (now - s->last_stats_ms >= VISION_STATS_INTERVAL_MS) {
        say_stats(s, now);
    }
}

static const char *pick_backend(const char *arg)
{
    const char *env = getenv("POCKETOS_CAMERA_BACKEND");

    if (arg && *arg) {
        return arg;
    }
    return env && *env ? env : "v4l2";
}

static const char *pick_config(const char *arg, const char *backend, const char *script)
{
    const char *env = getenv("POCKETOS_VISION_CAMERA_CONFIG");

    if (arg && *arg) {
        return arg;
    }
    if (env && *env) {
        return env;
    }
    /* The fake takes its script here; the real camera the planar default. */
    return strcmp(backend, "fake") == 0 ? script : VISION_CAMERA_CONFIG;
}

static const char *pick_model(const char *arg)
{
    const char *env = getenv("POCKETOS_VISION_MODEL");

    if (arg && *arg) {
        return arg;
    }
    return env && *env ? env : VISION_MODEL_DEFAULT;
}

static int open_camera(struct pocketcam_backend *cam, struct pocketcam_info *info,
                       const char *backend, const char *config, bool events)
{
    char t[96];
    int r = pocketcam_open(cam, backend, config, info);

    if (r == 0) {
        return 0;
    }
    if (events) {
        if (r == -ENODEV || r == -ENOTSUP) {
            say("nodevice %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        } else if (r == -EBUSY) {
            say("error busy %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        } else {
            say("error open %s", clean(pocketcam_strerror(r), t, sizeof(t)));
        }
    } else {
        fprintf(stderr, "pos-vision: %s: %s\n", backend, pocketcam_strerror(r));
    }
    return r;
}

static int open_model(struct vision_kpu **kpu, struct vision_kpu_info *info, const char *model,
                      const char *script, bool events)
{
    char err[96] = "";
    char t[96];
    int r = vision_kpu_open(kpu, model, script, info, err, sizeof(err));

    if (r == 0) {
        return 0;
    }
    if (events) {
        say("nomodel %s", clean(err[0] ? err : "the detector could not be opened", t, sizeof(t)));
    } else {
        fprintf(stderr, "pos-vision: %s: %s\n", model, err[0] ? err : "cannot open");
    }
    return r;
}

static int run_session(const char *backend, const char *config, const char *model,
                       const char *kpu_script)
{
    struct session *s = calloc(1, sizeof(*s));
    int code;
    int i;

    if (!s) {
        say("error memory out of memory");
        return EXIT_USAGE;
    }
    for (i = 0; i < VISION_LINES; i++) {
        s->line_pm[i][0] = -1;
    }
    s->sample_x = -1;
    s->sample_y = -1;
    s->tol = VISION_COLOR_TOL_DEFAULT;
    s->trace_dark = true;
    vision_tracker_init(&s->tracker);
    vision_traffic_init(&s->tf);
    say("hello %d %s %s", VISION_PROTO_VERSION, backend, vision_kpu_backend());
    if (map_shm(s) != 0) {
        say("error shm no usable shared memory on descriptor %d", POCKETCAM_SHM_FD);
        free(s);
        return EXIT_USAGE;
    }
    if (open_camera(&s->cam, &s->info, backend, config, true) != 0) {
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        free(s);
        return EXIT_NOCAMERA;
    }
    if (open_model(&s->kpu, &s->model, model, kpu_script, true) != 0) {
        pocketcam_close(&s->cam);
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        free(s);
        return EXIT_NOMODEL;
    }
    /* The model's classes onto the traffic classes, by name: the traffic
     * logic never learns which detector this is. */
    vision_traffic_map_names(&s->tf, s->model.classes, vision_label);
    say("ready %s %u %u %d %s %u %u %u", s->info.name, s->info.preview_w, s->info.preview_h,
        s->info.simulated ? 1 : 0, s->model.model, s->model.in_w, s->model.in_h, s->model.classes);
    /* What this helper can run: the detector's modes and the pixel modes. */
    say("caps detect track traffic color edge trace");
    while (!s->quit && !s->in_eof && !stop_requested() && !out_broken) {
        read_commands(s, s->streaming ? 0 : 250);
        if (s->streaming && !s->quit) {
            stream_once(s);
        }
    }
    /* The camera first, then the detector: the sensor is what another
     * screen may be waiting for. */
    pocketcam_close(&s->cam);
    vision_kpu_close(s->kpu);
    if (s->quit && s->exit_code == 0) {
        say("bye");
    }
    munmap(s->shm, POCKETCAM_SHM_BYTES);
    code = s->exit_code;
    free(s->upright);
    free(s);
    return code;
}

static int run_probe(const char *backend, const char *config, const char *model,
                     const char *kpu_script)
{
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct vision_kpu *kpu = NULL;
    struct vision_kpu_info mi;

    if (open_camera(&cam, &info, backend, config, false) != 0) {
        return EXIT_NOCAMERA;
    }
    printf("camera %s preview %ux%u mount %d%s%s\n", info.name, info.preview_w, info.preview_h,
           info.mount_rotation, info.mount_mirror ? "m" : "", info.simulated ? " simulated" : "");
    pocketcam_close(&cam);
    if (open_model(&kpu, &mi, model, kpu_script, false) != 0) {
        return EXIT_NOMODEL;
    }
    printf("model %s backend %s input %ux%u classes %u rows %u\n", mi.model, mi.backend, mi.in_w,
           mi.in_h, mi.classes, mi.rows);
    vision_kpu_close(kpu);
    return 0;
}

/* bench's options. */
struct bench_opts {
    int frames;
    const char *save;
    int save_every;              /* also every this many frames after the first save; 0: once */
    const char *image;           /* a PPM fed to the detector as every frame, instead of the camera */
    int turn;                    /* the frame turned by this for the detector (upright_input) */
};

/* A binary PPM (P6, 8-bit) as a planar R, G, B frame - the layout the ISP's
 * BG3P preview has - so a saved picture can be fed to the detector as it
 * is, or turned or changed beforehand, and the result compared. 0 with
 * *buf allocated (the caller frees it), or -1 with the reason on stderr. */
static int load_image(const char *path, struct pocketcam_frame *f, uint8_t **buf)
{
    FILE *fp = fopen(path, "rb");
    unsigned w;
    unsigned h;
    unsigned maxv;
    size_t plane;
    uint8_t *rgb;
    size_t i;

    if (!fp || fscanf(fp, "P6 %u %u %u", &w, &h, &maxv) != 3 || maxv != 255 || fgetc(fp) == EOF || w == 0 ||
        h == 0 || w > VISION_MAX_COORD || h > VISION_MAX_COORD) {
        fprintf(stderr, "pos-vision: %s is not an 8-bit binary PPM\n", path);
        if (fp) {
            fclose(fp);
        }
        return -1;
    }
    plane = (size_t)w * h;
    rgb = malloc(3 * plane);
    *buf = malloc(3 * plane);
    if (!rgb || !*buf || fread(rgb, 1, 3 * plane, fp) != 3 * plane) {
        fprintf(stderr, "pos-vision: %s is short\n", path);
        free(rgb);
        free(*buf);
        *buf = NULL;
        fclose(fp);
        return -1;
    }
    fclose(fp);
    for (i = 0; i < plane; i++) {
        (*buf)[i] = rgb[3 * i];
        (*buf)[plane + i] = rgb[3 * i + 1];
        (*buf)[2 * plane + i] = rgb[3 * i + 2];
    }
    free(rgb);
    memset(f, 0, sizeof(*f));
    f->format = POCKETCAM_FMT_BG3P;
    f->width = w;
    f->height = h;
    f->stride = w;
    f->data = *buf;
    f->bytes = 3 * plane;
    return 0;
}

/* Whether (x, y) is on the outline of b, t pixels thick. */
static bool on_outline(int32_t x, int32_t y, const struct vision_box *b, int32_t t)
{
    return b->w > 0 && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h &&
           (x < b->x + t || x >= b->x + b->w - t || y < b->y + t || y >= b->y + b->h - t);
}

/* The frame as a binary PPM (the ISP's planar BGR holds R, G and B planes in
 * that order, VERIFIED on unit B), with this frame's vehicle boxes drawn in:
 * green at the threshold, red below it. 0, or -1 with the reason on
 * stderr. */
static int save_frame(const char *path, const struct pocketcam_frame *f, const struct vision_det *veh, int nveh)
{
    size_t plane = (size_t)f->stride * f->height;
    uint8_t *row = malloc((size_t)f->width * 3);
    FILE *fp;
    uint32_t x;
    uint32_t y;

    if (f->format != POCKETCAM_FMT_BG3P || !row) {
        fprintf(stderr, "pos-vision: --save needs the planar BGR preview\n");
        free(row);
        return -1;
    }
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "pos-vision: cannot write %s\n", path);
        free(row);
        return -1;
    }
    fprintf(fp, "P6\n%u %u\n255\n", f->width, f->height);
    for (y = 0; y < f->height; y++) {
        for (x = 0; x < f->width; x++) {
            size_t at = (size_t)y * f->stride + x;
            uint8_t *px = &row[3 * x];
            int k;

            px[0] = f->data[at];
            px[1] = f->data[plane + at];
            px[2] = f->data[2 * plane + at];
            for (k = 0; k < nveh; k++) {
                if (on_outline((int32_t)x, (int32_t)y, &veh[k].box, 1)) {
                    bool detect = veh[k].conf >= VISION_CONF_MIN;

                    px[0] = detect ? 0 : 255;
                    px[1] = detect ? 255 : 0;
                    px[2] = 0;
                }
            }
        }
        fwrite(row, 1, (size_t)f->width * 3, fp);
    }
    free(row);
    if (fclose(fp) != 0) {
        return -1;
    }
    printf("saved %ux%u frame to %s\n", f->width, f->height, path);
    return 0;
}

/* Frames through the whole pipeline, timings printed, nothing drawn: the
 * measurement the gate reads. */
static int run_bench(const char *backend, const char *config, const char *model,
                     const char *kpu_script, const struct bench_opts *o)
{
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct vision_kpu *kpu = NULL;
    struct vision_kpu_info mi;
    struct vision_det *cands = calloc(VISION_MAX_CANDIDATES, sizeof(*cands));
    struct vision_det *vcands = calloc(VISION_MAX_CANDIDATES, sizeof(*vcands));
    struct vision_det dets[VISION_MAX_DETECTIONS];
    struct vision_det vdets[VISION_MAX_DETECTIONS];
    struct vision_tracker tr;
    struct vision_traffic tf;
    struct pocketcam_frame image;
    uint8_t *image_buf = NULL;
    uint8_t *turn_buf = NULL;
    int64_t pre_sum = 0;
    int64_t infer_sum = 0;
    int64_t post_sum = 0;
    int64_t t_start;
    /* Vehicle sightings at and below the threshold, and of those it takes:
     * in how many frames, their height in the model's own pixels, and the
     * best confidence seen. */
    uint32_t veh_detect = 0;
    uint32_t veh_below = 0;
    uint32_t veh_frames = 0;
    int32_t h_min = -1;
    int32_t h_max = -1;
    int64_t h_sum = 0;
    uint16_t conf_max = 0;
    uint32_t ratio_pm;
    int save_at = o->frames > VISION_BENCH_SAVE_AT ? VISION_BENCH_SAVE_AT : o->frames - 1;
    int done = 0;
    int r;

    if (!cands || !vcands) {
        free(cands);
        free(vcands);
        return EXIT_USAGE;
    }
    if (o->image) {
        if (load_image(o->image, &image, &image_buf) != 0) {
            free(cands);
            free(vcands);
            return EXIT_USAGE;
        }
        memset(&info, 0, sizeof(info));
        snprintf(info.name, sizeof(info.name), "image");
        info.preview_w = image.width;
        info.preview_h = image.height;
    } else if (open_camera(&cam, &info, backend, config, false) != 0) {
        free(cands);
        free(vcands);
        return EXIT_NOCAMERA;
    }
    if (open_model(&kpu, &mi, model, kpu_script, false) != 0) {
        if (!o->image) {
            pocketcam_close(&cam);
        }
        free(image_buf);
        free(cands);
        free(vcands);
        return EXIT_NOMODEL;
    }
    /* The letterbox: frame pixels to the model's (the same for the turned
     * picture, the smaller ratio of the two sides either way). */
    ratio_pm = mi.in_w * 1000u / info.preview_w < mi.in_h * 1000u / info.preview_h
                   ? mi.in_w * 1000u / info.preview_w
                   : mi.in_h * 1000u / info.preview_h;
    printf("camera %s %ux%u, model %s %ux%u classes %u rows %u, %d frames\n", info.name,
           info.preview_w, info.preview_h, mi.model, mi.in_w, mi.in_h, mi.classes, mi.rows, o->frames);
    printf("vehicles: shown from %u%%, the threshold %u%%; model px = frame px x %u.%03u\n",
           VISION_BENCH_VEHICLE_FLOOR / 10, VISION_CONF_MIN / 10, ratio_pm / 1000, ratio_pm % 1000);
    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, mi.classes, vision_label);
    vision_tracker_init(&tr);
    if (o->turn != 0) {
        turn_buf = malloc((size_t)3 * info.preview_w * info.preview_h);
        printf("upright: the frame is turned %d degrees for the detector\n", o->turn);
    }
    r = o->image ? 0 : pocketcam_start(&cam);
    t_start = mono_ms();
    while (r == 0 && done < o->frames && !stop_requested()) {
        struct pocketcam_frame f;
        struct pocketcam_frame in;
        const float *out;
        size_t count;
        uint32_t dims[3];
        int pre_ms;
        int infer_ms;
        int64_t t0;
        int64_t turn_t0;
        int n;
        uint32_t bad;

        if (o->image) {
            f = image;
            f.seq = (uint32_t)done + 1;
            r = 0;
        } else {
            r = pocketcam_next(&cam, 1000, &f);
        }
        if (r == -ETIMEDOUT) {
            r = 0;
            continue;
        }
        if (r != 0) {
            break;
        }
        /* The detector's picture: upright (--turn), as the session gives it. */
        turn_t0 = mono_ms();
        r = upright_input(&f, o->turn, turn_buf, &in);
        pre_ms = (int)(mono_ms() - turn_t0);
        if (r == 0) {
            r = vision_kpu_turn(kpu, o->turn);
        }
        if (r == 0) {
            int kpu_pre = 0;

            r = vision_kpu_infer(kpu, &in, &out, &count, dims, &kpu_pre, &infer_ms);
            pre_ms += kpu_pre;
        }
        if (r != 0) {
            if (!o->image) {
                pocketcam_release(&cam, &f);
            }
            fprintf(stderr, "pos-vision: infer failed (%d)\n", r);
            break;
        }
        t0 = mono_ms();
        {
            struct vision_decode_params p = {
                .in_w = mi.in_w, .in_h = mi.in_h, .classes = mi.classes,
                .frame_w = in.width, .frame_h = in.height, .conf_min = VISION_BENCH_VEHICLE_FLOOR,
            };

            n = vision_decode(out, count, dims, &p, cands, VISION_MAX_CANDIDATES, &bad);
        }
        if (n >= 0) {
            int strong = 0;
            int nv = 0;
            int i;
            bool seen = false;

            /* Found on the turned picture; tallied and drawn in frame pixels. */
            unturn_dets(cands, n, &f, o->turn);
            /* What the threshold takes (the boxes line, as always), and apart
             * from it every vehicle candidate down to the bench's floor. */
            for (i = 0; i < n; i++) {
                const struct vision_det d = cands[i];

                if (vision_traffic_vehicle(&tf, d.cls)) {
                    vcands[nv++] = d;
                }
                if (d.conf >= VISION_CONF_MIN) {
                    cands[strong++] = d;
                }
            }
            nv = vision_nms(vcands, nv, VISION_NMS_IOU, vdets, VISION_MAX_DETECTIONS);
            nv = vision_nms_nested(vdets, nv, VISION_NESTED_PM);
            n = vision_nms(cands, strong, VISION_NMS_IOU, dets, VISION_MAX_DETECTIONS);
            n = vision_nms_nested(dets, n, VISION_NESTED_PM);
            vision_tracker_update(&tr, dets, n);
            if (o->save && (done == save_at || (o->save_every > 0 && done > save_at &&
                                                (done - save_at) % o->save_every == 0))) {
                char path[512];

                if (o->save_every > 0) {
                    size_t len = strlen(o->save);

                    if (len > 4 && strcmp(o->save + len - 4, ".ppm") == 0) {
                        len -= 4;
                    }
                    snprintf(path, sizeof(path), "%.*s-%05d.ppm", (int)len, o->save, done);
                } else {
                    snprintf(path, sizeof(path), "%s", o->save);
                }
                save_frame(path, &f, vdets, nv);
            }
            if (done == 0 || done == o->frames - 1) {
                /* What the model really gives back, for the gate: the range
                 * of the box values and of the class scores. */
                float bmin = out[0];
                float bmax = out[0];
                float smin = out[4 * dims[2]];
                float smax = smin;
                size_t k;

                for (k = 0; k < (size_t)4 * dims[2]; k++) {
                    bmin = out[k] < bmin ? out[k] : bmin;
                    bmax = out[k] > bmax ? out[k] : bmax;
                }
                for (k = (size_t)4 * dims[2]; k < count; k++) {
                    smin = out[k] < smin ? out[k] : smin;
                    smax = out[k] > smax ? out[k] : smax;
                }
                printf("tensor: box values %.4f .. %.4f, scores %.4f .. %.4f\n", (double)bmin, (double)bmax,
                       (double)smin, (double)smax);
            }
            printf("frame %u: pre %d ms infer %d ms, %d boxes, %u rows refused:", f.seq, pre_ms, infer_ms, n,
                   bad);
            for (i = 0; i < n && i < 6; i++) {
                printf(" %s %u%% (%d,%d %dx%d)", vision_label(dets[i].cls), dets[i].conf / 10,
                       dets[i].box.x, dets[i].box.y, dets[i].box.w, dets[i].box.h);
            }
            printf("; %d tracks\n", tr.count);
            if (nv > 0) {
                /* Every vehicle: a far car is the least confident and would
                 * be cut off by near ones if only some were listed. */
                printf("  vehicles:");
                for (i = 0; i < nv; i++) {
                    const struct vision_det *d = &vdets[i];
                    /* Its size as the model saw it: on the upright picture. */
                    struct vision_box ub = d->box;
                    int32_t mh;
                    int32_t mw;

                    vision_box_turn(f.width, f.height, o->turn, &d->box, &ub);
                    mh = (int32_t)((int64_t)ub.h * ratio_pm / 1000);
                    mw = (int32_t)((int64_t)ub.w * ratio_pm / 1000);
                    if (d->conf >= VISION_CONF_MIN) {
                        veh_detect++;
                        h_min = h_min < 0 || mh < h_min ? mh : h_min;
                        h_max = mh > h_max ? mh : h_max;
                        h_sum += mh;
                        seen = true;
                    } else {
                        veh_below++;
                    }
                    conf_max = d->conf > conf_max ? d->conf : conf_max;
                    printf(" %s %u%% (%d,%d %dx%d, model %dx%d) %s", vision_label(d->cls), d->conf / 10,
                           d->box.x, d->box.y, d->box.w, d->box.h, mw, mh,
                           d->conf >= VISION_CONF_MIN ? "DETECT" : "below");
                }
                printf("\n");
            }
            veh_frames += seen ? 1 : 0;
        } else {
            printf("frame %u: malformed output (%d)\n", f.seq, n);
        }
        if (!o->image) {
            pocketcam_release(&cam, &f);
        }
        pre_sum += pre_ms;
        infer_sum += infer_ms;
        post_sum += mono_ms() - t0;
        done++;
    }
    if (done > 0) {
        int64_t wall = mono_ms() - t_start;

        printf("%d frames in %lld ms: %.1f fps; mean pre %.1f ms, infer %.1f ms, post %.1f ms\n",
               done, (long long)wall, wall > 0 ? done * 1000.0 / (double)wall : 0.0,
               (double)pre_sum / done, (double)infer_sum / done, (double)post_sum / done);
        printf("vehicle sightings: %u at the threshold, %u below it", veh_detect, veh_below);
        if (veh_detect > 0) {
            printf("; those are %d..%d model px tall (mean %.1f), in %u of %d frames", h_min, h_max,
                   (double)h_sum / veh_detect, veh_frames, done);
        }
        if (veh_detect + veh_below > 0) {
            printf(", best %u%%", conf_max / 10);
        }
        printf("\n");
    }
    if (!o->image) {
        pocketcam_close(&cam);
    }
    vision_kpu_close(kpu);
    free(image_buf);
    free(turn_buf);
    free(cands);
    free(vcands);
    return r == 0 ? 0 : EXIT_LOST;
}


static void usage(void)
{
    fprintf(stderr,
            "usage: pos-vision session|probe [--backend NAME] [--fake SCRIPT] [--config CFG]\n"
            "                                [--model FILE] [--kpu SCRIPT]\n"
            "       pos-vision bench [N] [--backend NAME] [--config CFG] [--model FILE]\n"
            "                        [--turn 0|90|180|270] [--save FILE.ppm [--save-every N]]\n"
            "                        [--image FILE.ppm]\n");
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : NULL;
    const char *backend_arg = NULL;
    const char *fake = NULL;
    const char *config_arg = NULL;
    const char *model_arg = NULL;
    const char *kpu_script = NULL;
    const char *backend;
    const char *config;
    const char *model;
    struct bench_opts bo = { 100, NULL, 0, NULL, 0 };
    bool bench = cmd && strcmp(cmd, "bench") == 0;
    int i;

    if (!cmd) {
        usage();
        return EXIT_USAGE;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_arg = argv[++i];
        } else if (strcmp(argv[i], "--fake") == 0 && i + 1 < argc) {
            fake = argv[++i];
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_arg = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_arg = argv[++i];
        } else if (strcmp(argv[i], "--kpu") == 0 && i + 1 < argc) {
            kpu_script = argv[++i];
        } else if (bench && i == 2 && atoi(argv[i]) > 0) {
            bo.frames = atoi(argv[i]);
        } else if (bench && strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            bo.save = argv[++i];
        } else if (bench && strcmp(argv[i], "--save-every") == 0 && i + 1 < argc && atoi(argv[i + 1]) > 0) {
            bo.save_every = atoi(argv[++i]);
        } else if (bench && strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            bo.image = argv[++i];
        } else if (bench && strcmp(argv[i], "--turn") == 0 && i + 1 < argc) {
            bo.turn = atoi(argv[++i]);
            if (bo.turn != 0 && bo.turn != 90 && bo.turn != 180 && bo.turn != 270) {
                usage();
                return EXIT_USAGE;
            }
        } else {
            usage();
            return EXIT_USAGE;
        }
    }
    backend = pick_backend(backend_arg);
    config = pick_config(config_arg, backend, fake);
    model = pick_model(model_arg);
    if (!kpu_script) {
        kpu_script = getenv("POCKETOS_VISION_KPU_SCRIPT");
    }
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    sigprocmask(SIG_BLOCK, &stop_signals, NULL);
    signal(SIGPIPE, SIG_IGN);
    if (strcmp(cmd, "session") == 0) {
        return run_session(backend, config, model, kpu_script);
    }
    if (strcmp(cmd, "probe") == 0) {
        return run_probe(backend, config, model, kpu_script);
    }
    if (bench) {
        if (bo.save_every > 0 && !bo.save) {
            usage();
            return EXIT_USAGE;
        }
        return run_bench(backend, config, model, kpu_script, &bo);
    }
    usage();
    return EXIT_USAGE;
}
