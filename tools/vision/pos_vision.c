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
 *       before exec - when the shell dies; `bye` once the camera, the nets
 *       and the KPU are closed. A model opened later (READ, FACE,
 *       RECOGNIZE) is announced with `loading` first.
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
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketcam/pocketcam.h"
#include "pocketcam/pocketcam_convert.h"
#include "pocketpaths.h"
#include "pocketvision/pocketvision_proto.h"
#include "pocketvision/vision_decode.h"
#include "pocketvision/vision_embed.h"
#include "pocketvision/vision_face.h"
#include "pocketvision/vision_geom.h"
#include "pocketvision/vision_kpu.h"
#include "pocketvision/vision_labels.h"
#include "pocketvision/vision_line.h"
#include "pocketvision/vision_nms.h"
#include "pocketvision/vision_pixels.h"
#include "pocketvision/vision_range.h"
#include "pocketvision/vision_text.h"
#include "pocketvision/vision_track.h"
#include "pocketvision/vision_traffic.h"
#include "pocketvision/vision_window.h"

#include <errno.h>
#include <fcntl.h>
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
    MODE_TRACE,
    MODE_READ,       /* the text models instead of the detector */
    MODE_FACE,       /* the face detector instead of the detector */
    MODE_RECOGNIZE   /* FACE, and each face compared with the owner's */
};

/* READ's models and dictionary (docs/apps/VISION.md "Read"): not in the
 * image - installed by hand on a bench unit - so READ is offered only when
 * all three are there. */
#define VISION_TEXT_DET_DEFAULT "/usr/share/doors/vision/text_det.kmodel"
#define VISION_TEXT_REC_DEFAULT "/usr/share/doors/vision/text_rec.kmodel"
#define VISION_TEXT_DICT_DEFAULT "/usr/share/doors/vision/text_dict.txt"
#define VISION_READ_INTERVAL_MS 600   /* a read at most this often: the preview goes on between */
#define VISION_READ_LINES 6           /* lines recognised per read, the surest regions first */
#define VISION_READ_SHOWN 8           /* text boxes said on a line */
#define VISION_READ_TEXT 48           /* bytes of text kept per line */
/* FACE's model (docs/apps/VISION.md "FACE"): not in the image either, offered
 * only when it is there. Padded with the middle of the vendor's per-channel
 * mean (104, 117, 123). */
#define VISION_FACE_DET_DEFAULT "/usr/share/doors/vision/face_det.kmodel"
#define VISION_FACE_PAD 117
/* RECOGNIZE's embedding model (docs/apps/VISION.md "RECOGNIZE"), and the
 * owner it compares faces with: a file of the Vision state directory. */
#define VISION_FACE_EMBED_DEFAULT "/usr/share/doors/vision/face_embed.kmodel"
#define VISION_OWNER_FILE "owner.v1"
#define VISION_RECOG_INTERVAL_MS 250  /* a recognition round at most this often */
#define VISION_RECOG_FACES 3          /* faces embedded per round, the largest */
#define VISION_RECOG_MIN_SIDE 32.0f   /* a face smaller than this (picture pixels) is not compared */
#define VISION_ENROL_TIMEOUT_MS 20000 /* enrolment gives up after this */

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

/* ---- text: the dictionary ----------------------------------------------------- */

#define VISION_DICT_MAX 8192
#define VISION_DICT_BYTES (256 * 1024)

/* The recogniser's dictionary: one entry per line (UTF-8, CR stripped). */
struct vision_dict {
    char *buf;
    uint32_t off[VISION_DICT_MAX];
    uint32_t n;
};

static int dict_load(struct vision_dict *d, const char *path)
{
    FILE *fp = fopen(path, "rb");
    size_t got;
    size_t i;
    size_t start = 0;

    memset(d, 0, sizeof(*d));
    if (!fp) {
        return -ENOENT;
    }
    d->buf = malloc(VISION_DICT_BYTES + 1);
    if (!d->buf) {
        fclose(fp);
        return -ENOMEM;
    }
    got = fread(d->buf, 1, VISION_DICT_BYTES, fp);
    if (!feof(fp) && got == VISION_DICT_BYTES) {
        /* Larger than any dictionary this knows. */
        fclose(fp);
        free(d->buf);
        d->buf = NULL;
        return -EPROTO;
    }
    fclose(fp);
    d->buf[got] = '\0';
    for (i = 0; i <= got && d->n < VISION_DICT_MAX; i++) {
        if (i == got || d->buf[i] == '\n') {
            size_t end = i;

            if (end > start && d->buf[end - 1] == '\r') {
                end--;
            }
            d->buf[end] = '\0';
            if (i < got || end > start) {
                d->off[d->n++] = (uint32_t)start;
            }
            start = i + 1;
        }
    }
    return 0;
}

static const char *dict_word(const struct vision_dict *d, uint32_t i)
{
    return d->buf && i < d->n ? d->buf + d->off[i] : "?";
}

static void dict_free(struct vision_dict *d)
{
    free(d->buf);
    memset(d, 0, sizeof(*d));
}

/* A text box grown for the recogniser: 15 % in height (a line's ascenders
 * and descenders, the vendor's margin) and a third of the line's height at each
* end - with the vendor's 2.5 % in width, unit B lost the K of "K230", the
 * first letter of a region standing at its very edge. Kept inside the
 * frame. */
static struct vision_box text_margin(const struct vision_box *b, uint32_t fw, uint32_t fh)
{
    struct vision_box g = *b;
    int32_t dx = b->h / 3;
    int32_t dy = (b->h * 15) / 200;

    g.x -= dx;
    g.y -= dy;
    g.w += 2 * dx;
    g.h += 2 * dy;
    if (g.x < 0) {
        g.w += g.x;
        g.x = 0;
    }
    if (g.y < 0) {
        g.h += g.y;
        g.y = 0;
    }
    if ((uint32_t)(g.x + g.w) > fw) {
        g.w = (int32_t)fw - g.x;
    }
    if ((uint32_t)(g.y + g.h) > fh) {
        g.h = (int32_t)fh - g.y;
    }
    return g;
}

/* One recognised line into text: the dictionary's entries in order (the
 * blank is the model's last class, the vendor's convention, VERIFIED on
 * unit B with the vendor's recogniser and dictionary), cut to len. The
 * vendor's dictionary has no space, so words run together ("EXIT12"); the
 * steps do not show the gaps reliably enough to put them back (a space is
 * ~1.3 characters' pitch at this size, unevenly set digits as much). */
static void line_text(const struct vision_dict *d, const uint32_t *cls, int n, char *out, size_t len)
{
    size_t off = 0;
    int i;

    out[0] = '\0';
    for (i = 0; i < n && off + 1 < len; i++) {
        const char *w = dict_word(d, cls[i]);
        size_t wl = strlen(w);

        if (off + wl >= len) {
            break;
        }
        memcpy(out + off, w, wl);
        off += wl;
    }
    out[off] = '\0';
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
    bool view_contain;       /* `view ... contain`: the whole frame, letterboxed */
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
    /* TRAFFIC's detection range (vision_range.h) and its recent statistics
     * (vision_window.h). */
    enum vision_range range;
    struct vision_window win;
    bool recent_dirty;
    bool last_zoom;               /* the last frame had a zoom pass */
    /* READ: the text models, opened the first time READ is asked for. */
    const char *text_det_path;
    const char *text_rec_path;
    const char *text_dict_path;
    const char *net_script;       /* the fake nets' script (host builds) */
    /* The detector is optional: a unit without its model file (an image
     * that does not ship it, docs/apps/VISION.md "The model") still runs
     * the pixel modes and the modes with models of their own. */
    bool detect_offered;
    bool text_offered;
    bool text_tried;
    struct vision_net *tdet;
    struct vision_net *trec;
    struct vision_net_info tdi;
    struct vision_net_info tri;
    struct vision_dict *dict;
    /* FACE: the face detector, opened the first time FACE is asked for. */
    const char *face_det_path;
    bool face_offered;
    bool face_tried;
    struct vision_net *fdet;
    struct vision_net_info fdi;
    /* RECOGNIZE: the embedding model, the owner, an enrolment under way,
     * and the last score of each track. */
    const char *face_embed_path;
    bool recog_offered;
    bool embed_tried;
    struct vision_net *femb;
    struct vision_net_info fei;
    char embed_name[VISION_OWNER_MODEL_MAX];
    uint64_t embed_bytes;
    uint32_t embed_dim;
    struct vision_owner owner;
    bool have_owner;
    struct vision_owner enrol;
    bool enrolling;
    int64_t enrol_since_ms;
    int64_t last_recog_ms;
    struct {
        uint32_t id;
        uint16_t score;
        int64_t at;
    } who[VISION_MAX_TRACKS];
    int64_t last_read_ms;
    /* REPLAY (backend "image"): saved pictures instead of the camera, for a
     * bench with nothing in front of it (replay_open). */
    bool replay;
    char *replay_list;
    int replay_count;
    int replay_idx;
    uint8_t *replay_buf;
    struct pocketcam_frame replay_frame;
    int64_t replay_due_ms;
    int replay_period_ms;
    uint32_t replay_seq;
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
        /* A replayed picture is upright already and shown as it is. */
        .rotation = s->replay ? 0 : pocketcam_view_rotation(s->info.mount_rotation, s->display_rotation),
        .mirror = s->info.mount_mirror,
        .view_w = s->view_w,
        .view_h = s->view_h,
        .contain = s->view_contain,
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

/* TRAFFIC's recent statistics: `recent window_s crossed ab ba c0..c5 speeds
 * mean_kmh10 saturated`, after an event and as events age out. */
static void say_recent(struct session *s, int64_t now)
{
    struct vision_window_summary w;

    vision_window_summary(&s->win, now, &w);
    say("recent %u %u %u %u %u %u %u %u %u %u %u %u %d", w.window_s, w.crossed, w.ab, w.ba, w.cls[0], w.cls[1],
        w.cls[2], w.cls[3], w.cls[4], w.cls[5], w.speeds, w.mean_kmh10, w.saturated ? 1 : 0);
    s->recent_dirty = false;
}

/* The tracker as the mode and TRAFFIC's range want it: a FAR track is
 * confirmed later and kept longer (vision_range.h); every other mode tracks
 * as NORMAL. */
static void tracker_params(struct session *s)
{
    struct vision_range_params rp;

    vision_range_params(s->traffic ? s->range : VISION_RANGE_NORMAL, &rp);
    s->tracker.min_hits = rp.min_hits;
    s->tracker.max_misses = rp.max_misses;
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
    tracker_params(s);
    memset(&s->counts, 0, sizeof(s->counts));
    vision_traffic_reset(&s->tf);
    vision_window_init(&s->win);
    say_counts(s);
    if (traffic) {
        say_traffic(s);
        say_recent(s, mono_ms());
    }
    if (pixel_mode(s) || mode == MODE_READ || mode == MODE_FACE || mode == MODE_RECOGNIZE) {
        say("det 0 0");
    }
    memset(s->who, 0, sizeof(s->who));
    s->enrolling = false;
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

/* ---- the detector's pipeline, shared by the session and bench ---------------- */

struct pipe_out {
    int n;              /* detections in dets, frame pixels */
    int ncands;         /* the full pass's candidates in cands, frame pixels, from the floor */
    bool zoomed;        /* a zoom pass ran on this frame */
    int zoom_boxes;     /* what it found, before the merge */
    int pre_ms;         /* turning, copying and AI2D, both passes */
    int infer_ms;       /* the KPU, both passes */
    uint32_t bad;       /* rows refused */
};

/* A window's tensor decoded into candidates in picture pixels (the window's
 * offset added back). Returns how many, or -EPROTO. */
static int decode_window(const float *out, size_t count, const uint32_t dims[3], const struct vision_kpu_info *mi,
                         const struct vision_box *win, uint16_t conf_min, struct vision_det *cands, uint32_t *bad)
{
    struct vision_decode_params p = {
        .in_w = mi->in_w, .in_h = mi->in_h, .classes = mi->classes,
        .frame_w = (uint32_t)win->w, .frame_h = (uint32_t)win->h, .conf_min = conf_min,
    };
    int n = vision_decode(out, count, dims, &p, cands, VISION_MAX_CANDIDATES, bad);
    int i;

    for (i = 0; i < n; i++) {
        cands[i].box.x += win->x;
        cands[i].box.y += win->y;
    }
    return n;
}

/* One frame through the detector: turned upright (the preview's turn), the
 * full picture run, and with a range that asks for it the zoom window run
 * as well (vision_range.h) and merged in; suppression; the boxes back in
 * frame pixels; the range's own filter. `floor` is how low the full pass's
 * candidates are kept in `cands` (bench lists them); what reaches `dets` is
 * the range's confidence and up. 0; -EPROTO when a tensor is not one
 * (o->bad says how badly); another negative errno when a run failed. */
static int pipeline(struct vision_kpu *kpu, const struct vision_kpu_info *mi, uint8_t **upright,
                    size_t *upright_size, const struct pocketcam_frame *f, int rotation,
                    const struct vision_range_params *rp, uint16_t floor, const uint8_t *group,
                    uint32_t group_classes, struct vision_det *cands, struct vision_det *dets, struct pipe_out *o)
{
    static struct vision_det work[VISION_MAX_CANDIDATES];
    static struct vision_det zdets[VISION_MAX_DETECTIONS];
    const float *out;
    size_t count;
    uint32_t dims[3];
    size_t need = (size_t)3 * f->width * f->height;
    struct pocketcam_frame in;
    struct vision_box win;
    int64_t t0 = mono_ms();
    int kpu_pre = 0;
    int infer = 0;
    int r;
    int n;
    int nw;
    int i;

    memset(o, 0, sizeof(*o));
    if (rotation != 0 && *upright_size < need) {
        uint8_t *b = realloc(*upright, need);

        if (b) {
            *upright = b;
            *upright_size = need;
        }
    }
    r = upright_input(f, rotation, *upright_size >= need ? *upright : NULL, &in);
    /* Turning the picture is part of preparing it. */
    o->pre_ms = (int)(mono_ms() - t0);
    if (r == 0) {
        r = vision_kpu_turn(kpu, rotation);
    }
    if (r == 0) {
        r = vision_kpu_infer(kpu, &in, &out, &count, dims, &kpu_pre, &infer);
    }
    o->pre_ms += kpu_pre;
    o->infer_ms = infer;
    if (r != 0) {
        return r == -EPROTO ? -EINVAL : r;
    }
    {
        struct vision_decode_params p = {
            .in_w = mi->in_w, .in_h = mi->in_h, .classes = mi->classes,
            .frame_w = in.width, .frame_h = in.height, .conf_min = floor,
        };

        n = vision_decode(out, count, dims, &p, cands, VISION_MAX_CANDIDATES, &o->bad);
    }
    if (n < 0 || (o->bad > 0 && o->bad * 2 > mi->rows)) {
        return -EPROTO;
    }
    /* What the range takes, suppressed. */
    nw = 0;
    for (i = 0; i < n; i++) {
        if (cands[i].conf >= rp->conf_min) {
            work[nw++] = cands[i];
        }
    }
    o->n = vision_nms(work, nw, VISION_NMS_IOU, dets, VISION_MAX_DETECTIONS);
    /* FAR: the centre again at the model's own resolution. */
    if (rp->zoom && vision_range_zoom_window(in.width, in.height, mi->in_w, mi->in_h, &win) == 0) {
        uint32_t zbad = 0;

        r = vision_kpu_infer_window(kpu, &win, &out, &count, dims, &kpu_pre, &infer);
        if (r != 0) {
            return r == -EPROTO ? -EINVAL : r;
        }
        o->pre_ms += kpu_pre;
        o->infer_ms += infer;
        nw = decode_window(out, count, dims, mi, &win, rp->conf_min, work, &zbad);
        o->bad += zbad;
        if (nw < 0 || (zbad > 0 && zbad * 2 > mi->rows)) {
            return -EPROTO;
        }
        nw = vision_nms(work, nw, VISION_NMS_IOU, zdets, VISION_MAX_DETECTIONS);
        o->zoomed = true;
        o->zoom_boxes = nw;
        o->n = vision_range_merge(dets, o->n, VISION_MAX_DETECTIONS, zdets, nw, &win, in.width, in.height, group,
                                  group_classes);
    }
    /* Found on the turned picture; tracked and drawn in frame pixels. */
    unturn_dets(dets, o->n, f, rotation);
    unturn_dets(cands, n, f, rotation);
    o->ncands = n;
    o->n = vision_nms_nested(dets, o->n, VISION_NESTED_PM);
    o->n = vision_range_filter(dets, o->n, rp, f->width, f->height);
    return 0;
}

static void detect(struct session *s, const struct pocketcam_frame *f, int64_t now)
{
    struct pipe_out o;
    struct vision_range_params rp;
    int64_t t0 = mono_ms();
    int r;
    int n;

    /* The range is TRAFFIC's; everything else looks as NORMAL does. */
    vision_range_params(s->traffic ? s->range : VISION_RANGE_NORMAL, &rp);
    /* The detector sees the scene upright, as the screen shows it. */
    r = pipeline(s->kpu, &s->model, &s->upright, &s->upright_size, f, view_of(s).rotation, &rp, VISION_CONF_MIN,
                 s->tracker.group, s->tracker.group_classes, s->cands, s->dets, &o);
    if (r == -EPROTO) {
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
    if (r != 0) {
        char t[96];

        say("error infer %s", clean(r == -EINVAL ? "the frame is not what the model takes"
                                                  : "the detector failed",
                                    t, sizeof(t)));
        s->exit_code = EXIT_USAGE;
        s->quit = true;
        return;
    }
    s->bad_run = 0;
    n = o.n;
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
                nx = nx > VISION_MAX_TRACKS ? VISION_MAX_TRACKS : nx;
                vision_traffic_counted(&s->tf, x, nx);
                vision_window_crossed(&s->win, &s->tf, x, nx, now);
                s->recent_dirty = true;
            }
        }
        if (s->traffic) {
            struct vision_counts unused = { 0, 0 };
            uint32_t kmh10[VISION_MAX_TRACKS];
            int line;

            for (line = 1; line <= 2; line++) {
                int m;
                int k;

                nx = vision_line_count(&s->line[line == 1 ? VISION_LINE_SPEED_A : VISION_LINE_SPEED_B],
                                       line == 1 ? VISION_LINE_SPEED_A : VISION_LINE_SPEED_B, &s->tracker, &unused,
                                       x, VISION_MAX_TRACKS);
                m = vision_traffic_crossed_speeds(&s->tf, line, x, nx > VISION_MAX_TRACKS ? VISION_MAX_TRACKS : nx,
                                                  now, kmh10, VISION_MAX_TRACKS);
                for (k = 0; k < m && k < VISION_MAX_TRACKS; k++) {
                    vision_window_speed(&s->win, kmh10[k], now);
                    s->recent_dirty = true;
                }
            }
            vision_traffic_settle(&s->tf, &s->tracker, now);
            if (s->tf.changed) {
                say_traffic(s);
            }
            if (s->recent_dirty) {
                say_recent(s, now);
            }
        }
    }
    s->last_pre_ms = o.pre_ms;
    s->last_infer_ms = o.infer_ms;
    s->last_post_ms = (int)(mono_ms() - t0) - o.pre_ms - o.infer_ms;
    if (s->last_post_ms < 0) {
        s->last_post_ms = 0;
    }
    s->last_zoom = o.zoomed;
    s->inferred++;
    say_tracks(s, f->seq);
}

/* ---- the models opened while the session runs ------------------------------------ */

/* READ's, FACE's and RECOGNIZE's nets are opened when the mode is first
 * asked for, synchronously: seconds for a large kmodel, with nothing said
 * meanwhile. `loading` first tells the session so, and it holds its
 * deadlines (and a leave's grace) for the load instead of killing the
 * helper in the middle of it - a kill there can leave the runtime's CMA
 * pool allocated until a reboot (vision_kpu_nncase.cpp). */
static int load_net(struct session *s, const char *what, struct vision_net **n, const char *path,
                    struct vision_net_info *info, char *err, size_t errlen)
{
    say("loading %s", what);
    return vision_net_open(n, path, s->net_script, info, err, errlen);
}

/* ---- READ ---------------------------------------------------------------------- */

static void text_close(struct session *s)
{
    vision_net_close(s->trec);
    vision_net_close(s->tdet);
    s->trec = NULL;
    s->tdet = NULL;
    if (s->dict) {
        dict_free(s->dict);
        free(s->dict);
        s->dict = NULL;
    }
}

/* The text models, the first time READ is asked for: 0, or -1 having said
 * why (`readfail`); READ then reads nothing and the screen says so. */
static int text_open(struct session *s)
{
    char err[96] = "";
    char t[96];

    if (s->tdet && s->trec && s->dict) {
        return 0;
    }
    if (s->text_tried) {
        return -1;
    }
    s->text_tried = true;
    s->dict = calloc(1, sizeof(*s->dict));
    if (!s->dict || dict_load(s->dict, s->text_dict_path) != 0 || s->dict->n < 2) {
        snprintf(err, sizeof(err), "the text dictionary could not be read");
    } else if (load_net(s, "text_det", &s->tdet, s->text_det_path, &s->tdi, err, sizeof(err)) != 0 ||
               load_net(s, "text_rec", &s->trec, s->text_rec_path, &s->tri, err, sizeof(err)) != 0) {
        /* err says which. */
    } else if (s->tdi.outputs < 1 || s->tdi.rank[0] != 4 || s->tri.outputs < 1 || s->tri.rank[0] < 2 ||
               s->tri.dims[0][s->tri.rank[0] - 1] != s->dict->n) {
        snprintf(err, sizeof(err), "the text models and the dictionary do not fit together");
    } else {
        return 0;
    }
    say("readfail %s", clean(err, t, sizeof(t)));
    text_close(s);
    return -1;
}

/* Percent-encode text for the protocol: a byte that is not a printable
 * ASCII word character - space, ':', '%', control, or any byte of a
 * multi-byte character - becomes %XX. */
static void pct(const char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (; *in && o + 4 < len; in++) {
        unsigned char c = (unsigned char)*in;

        if (c <= ' ' || c == ':' || c == '%' || c >= 0x7f) {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* One read: the text regions of the upright picture, the surest lines read,
 * each said in view pixels with its confidence and text:
 * `text seq n x:y:w:h:conf:TEXT ...`. */
static void read_text(struct session *s, const struct pocketcam_frame *f, int64_t now)
{
    struct vision_text_box tb[VISION_TEXT_MAX];
    struct vision_view v = view_of(s);
    struct pocketcam_frame in;
    char line[VISION_LINE_MAX];
    size_t off;
    size_t count;
    const float *map;
    float ratio;
    int pre = 0;
    int inf = 0;
    int pre_sum = 0;
    int inf_sum = 0;
    int64_t t0 = mono_ms();
    int n;
    int i;
    int said = 0;
    size_t need = (size_t)3 * f->width * f->height;
    uint32_t classes;
    uint32_t steps;

    s->last_read_ms = now;
    if (text_open(s) != 0) {
        return;
    }
    if (v.rotation != 0 && s->upright_size < need) {
        uint8_t *b = realloc(s->upright, need);

        if (b) {
            s->upright = b;
            s->upright_size = need;
        }
    }
    if (upright_input(f, v.rotation, s->upright_size >= need ? s->upright : NULL, &in) != 0 ||
        vision_net_turn(s->tdet, v.rotation) != 0 || vision_net_turn(s->trec, v.rotation) != 0 ||
        vision_net_frame(s->tdet, &in) != 0 || vision_net_run(s->tdet, NULL, false, 0, &pre, &inf) != 0) {
        say("readfail the text detector failed on a frame");
        return;
    }
    pre_sum += pre;
    inf_sum += inf;
    map = vision_net_output(s->tdet, 0, &count);
    n = vision_text_regions(map, s->tdi.dims[0][2], s->tdi.dims[0][1], s->tdi.dims[0][3], VISION_TEXT_THRESHOLD,
                            VISION_TEXT_BOX_MIN, tb, VISION_READ_LINES);
    ratio = (float)s->tdi.in_w / (float)in.width < (float)s->tdi.in_h / (float)in.height
                ? (float)s->tdi.in_w / (float)in.width
                : (float)s->tdi.in_h / (float)in.height;
    classes = s->tri.dims[0][s->tri.rank[0] - 1];
    steps = (uint32_t)(s->tri.count[0] / classes);
    if (n > 0 && vision_net_frame(s->trec, &in) != 0) {
        n = 0;
    }
    off = (size_t)snprintf(line, sizeof(line), "text %u", f->seq);
    {
        char items[VISION_LINE_MAX];
        size_t io = 0;

        items[0] = '\0';
        for (i = 0; i < n && said < VISION_READ_SHOWN; i++) {
            struct vision_box b = {
                (int32_t)((float)tb[i].box.x / ratio), (int32_t)((float)tb[i].box.y / ratio),
                (int32_t)((float)tb[i].box.w / ratio), (int32_t)((float)tb[i].box.h / ratio),
            };
            struct vision_box g;
            struct vision_box fb;
            struct vision_box vb;
            uint32_t cls[VISION_TEXT_CHARS];
            uint16_t pos[VISION_TEXT_CHARS];
            uint16_t conf = 0;
            char text[VISION_READ_TEXT];
            char enc[VISION_READ_TEXT * 3 + 1];
            const float *sc;
            int m;
            int w;

            if ((uint32_t)(b.x + b.w) > in.width) {
                b.w = (int32_t)in.width - b.x;
            }
            if ((uint32_t)(b.y + b.h) > in.height) {
                b.h = (int32_t)in.height - b.y;
            }
            g = text_margin(&b, in.width, in.height);
            if (g.w <= 0 || g.h <= 0 || vision_net_run(s->trec, &g, false, 0, &pre, &inf) != 0) {
                continue;
            }
            pre_sum += pre;
            inf_sum += inf;
            sc = vision_net_output(s->trec, 0, &count);
            m = vision_text_ctc_pos(sc, steps, classes, classes - 1, cls, pos, VISION_TEXT_CHARS, &conf);
            if (m <= 0) {
                continue;
            }
            line_text(s->dict, cls, m, text, sizeof(text));
            pct(text, enc, sizeof(enc));
            /* The box: upright picture -> frame -> view. */
            fb = b;
            if (v.rotation != 0) {
                vision_box_unturn(f->width, f->height, v.rotation, &b, &fb);
            }
            if (vision_map_box(&v, &fb, &vb) != 1) {
                continue;
            }
            w = snprintf(items + io, sizeof(items) - io, " %d:%d:%d:%d:%u:%s", vb.x, vb.y, vb.w, vb.h, conf, enc);
            if (w < 0 || io + (size_t)w + off + 16 >= sizeof(line)) {
                break;
            }
            io += (size_t)w;
            said++;
        }
        snprintf(line + off, sizeof(line) - off, " %d%s", said, items);
    }
    say("%s", line);
    s->last_pre_ms = pre_sum;
    s->last_infer_ms = inf_sum;
    s->last_post_ms = (int)(mono_ms() - t0) - pre_sum - inf_sum;
    if (s->last_post_ms < 0) {
        s->last_post_ms = 0;
    }
    s->inferred++;
}

/* ---- FACE ---------------------------------------------------------------------- */

static void face_close(struct session *s)
{
    vision_net_close(s->fdet);
    s->fdet = NULL;
}

/* The face model, the first time FACE is asked for: 0, or -1 having said
 * why (`facefail`); FACE then finds nothing and the screen says so. */
static int face_open(struct session *s)
{
    char err[96] = "";
    char t[96];

    if (s->fdet) {
        return 0;
    }
    if (s->face_tried) {
        return -1;
    }
    s->face_tried = true;
    if (load_net(s, "face_det", &s->fdet, s->face_det_path, &s->fdi, err, sizeof(err)) != 0) {
        /* err says why. */
    } else if (vision_face_check(s->fdi.outputs, s->fdi.rank, (const uint32_t (*)[4])s->fdi.dims, s->fdi.in_w,
                                 s->fdi.in_h) != 0) {
        snprintf(err, sizeof(err), "the face model's outputs are not a face detector's");
    } else {
        return 0;
    }
    say("facefail %s", clean(err, t, sizeof(t)));
    face_close(s);
    return -1;
}

/* ---- RECOGNIZE ------------------------------------------------------------------ */

static void embed_close(struct session *s)
{
    vision_net_close(s->femb);
    s->femb = NULL;
}

/* The embedding model, the first time RECOGNIZE is asked for: 0, or -1
 * having said why (`recogfail`). */
static int embed_open(struct session *s)
{
    char err[96] = "";
    char t[96];
    struct stat st;

    if (s->femb) {
        return 0;
    }
    if (s->embed_tried) {
        return -1;
    }
    s->embed_tried = true;
    if (stat(s->face_embed_path, &st) != 0) {
        snprintf(err, sizeof(err), "the face embedding model is not there");
    } else if (load_net(s, "face_embed", &s->femb, s->face_embed_path, &s->fei, err, sizeof(err)) != 0) {
        /* err says why. */
    } else if (s->fei.outputs != 1 || s->fei.in_w != VISION_EMBED_SIZE || s->fei.in_h != VISION_EMBED_SIZE ||
               s->fei.count[0] < 64 || s->fei.count[0] > VISION_EMBED_MAX) {
        snprintf(err, sizeof(err), "the face embedding model does not take a 112 x 112 face");
    } else {
        const char *base = strrchr(s->face_embed_path, '/');

        snprintf(s->embed_name, sizeof(s->embed_name), "%s", base ? base + 1 : s->face_embed_path);
        s->embed_bytes = (uint64_t)st.st_size;
        s->embed_dim = (uint32_t)s->fei.count[0];
        return 0;
    }
    say("recogfail %s", clean(err, t, sizeof(t)));
    embed_close(s);
    return -1;
}

static void owner_path(char *out, size_t len)
{
    snprintf(out, len, "%s/vision/%s", pocketos_state_dir(), VISION_OWNER_FILE);
}

/* The owner kept on the unit, if there is one for this model. */
static void owner_load(struct session *s)
{
    char path[POCKETOS_PATH_MAX + 32];
    static char text[VISION_OWNER_TEXT_MAX];
    size_t got;
    FILE *fp;

    s->have_owner = false;
    owner_path(path, sizeof(path));
    fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    got = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[got] = '\0';
    if (vision_owner_parse(text, &s->owner) == 0 &&
        vision_owner_fits(&s->owner, s->embed_name, s->embed_bytes, s->embed_dim)) {
        s->have_owner = true;
    }
}

/* Written as the settings are: a temporary file, fsync, rename; 0600 in a
 * 0700 directory. 0, or -1. */
static int owner_save(const struct vision_owner *o)
{
    char dir[POCKETOS_PATH_MAX + 16];
    char path[POCKETOS_PATH_MAX + 32];
    char tmp[POCKETOS_PATH_MAX + 40];
    static char text[VISION_OWNER_TEXT_MAX];
    int n = vision_owner_format(o, text, sizeof(text));
    int fd;

    snprintf(dir, sizeof(dir), "%s/vision", pocketos_state_dir());
    if (n < 0 || pocketos_mkdir_p(dir, 0700) != 0) {
        return -1;
    }
    owner_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, text, (size_t)n) != (ssize_t)n || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void say_owner(struct session *s)
{
    say("owner %d %u", s->have_owner ? 1 : 0, s->have_owner ? s->owner.samples : 0u);
}

/* The track a face (frame pixels) went to this frame: the seen track it
 * overlaps most. 0 for none. */
static uint32_t track_of(const struct session *s, const struct vision_box *b)
{
    uint32_t best = 0;
    uint32_t best_iou = 300;
    int k;

    for (k = 0; k < s->tracker.count; k++) {
        const struct vision_track *t = &s->tracker.t[k];
        uint32_t iou;

        if (!t->seen || !t->confirmed) {
            continue;
        }
        iou = vision_iou_permille(&t->box, b);
        if (iou > best_iou) {
            best_iou = iou;
            best = t->id;
        }
    }
    return best;
}

/* What RECOGNIZE knows of each track: its score against the owner, and
 * when it was scored. The table follows the tracker; a track that is gone
 * is forgotten. */
static void who_set(struct session *s, uint32_t id, uint16_t score, int64_t now)
{
    int free_at = -1;
    int k;

    for (k = 0; k < VISION_MAX_TRACKS; k++) {
        if (s->who[k].id == id) {
            s->who[k].score = score;
            s->who[k].at = now;
            return;
        }
        if (s->who[k].id == 0 && free_at < 0) {
            free_at = k;
        }
    }
    if (free_at >= 0) {
        s->who[free_at].id = id;
        s->who[free_at].score = score;
        s->who[free_at].at = now;
    }
}

/* When track id was last scored; 0 for never. */
static int64_t who_at(const struct session *s, uint32_t id)
{
    int k;

    for (k = 0; k < VISION_MAX_TRACKS; k++) {
        if (s->who[k].id == id) {
            return s->who[k].at;
        }
    }
    return 0;
}

static void say_who(struct session *s, uint32_t seq, int faces)
{
    char line[VISION_LINE_MAX];
    size_t off;
    int said = 0;
    int k;

    /* Forget the tracks the tracker no longer holds. */
    for (k = 0; k < VISION_MAX_TRACKS; k++) {
        int j;
        bool held = false;

        if (s->who[k].id == 0) {
            continue;
        }
        for (j = 0; j < s->tracker.count && !held; j++) {
            held = s->tracker.t[j].id == s->who[k].id;
        }
        if (!held) {
            s->who[k].id = 0;
        }
    }
    off = (size_t)snprintf(line, sizeof(line), "who %u %d", seq, faces);
    {
        char items[VISION_LINE_MAX - 32];
        size_t io = 0;

        items[0] = '\0';
        for (k = 0; k < VISION_MAX_TRACKS && said < VISION_MAX_SHOWN; k++) {
            int w;

            if (s->who[k].id == 0) {
                continue;
            }
            w = snprintf(items + io, sizeof(items) - io, " %u:%u:%d", s->who[k].id, s->who[k].score,
                         s->have_owner && s->who[k].score >= VISION_OWNER_MATCH_PM ? 1 : 0);
            if (w < 0 || io + (size_t)w >= sizeof(items)) {
                break;
            }
            io += (size_t)w;
            said++;
        }
        snprintf(line + off, sizeof(line) - off, " %d%s", said, items);
    }
    say("%s", line);
}

/* A recognition round: the largest faces aligned and embedded; while
 * enrolling, the one face in view becomes a view of the owner; otherwise
 * each face is scored against the owner and its track labelled. Faces and
 * points are the face model's (input pixels, over ratio the upright
 * picture's). */
static void recognise(struct session *s, const struct pocketcam_frame *f, const struct pocketcam_frame *in,
                      const struct vision_face *faces, int n, float ratio, int64_t now)
{
    int order[VISION_FACE_MAX];
    int done = 0;
    int i;
    int j;

    if (now - s->last_recog_ms < VISION_RECOG_INTERVAL_MS) {
        return;
    }
    s->last_recog_ms = now;
    if (s->enrolling && now - s->enrol_since_ms > VISION_ENROL_TIMEOUT_MS) {
        s->enrolling = false;
        say("enrolfail no single face held still long enough");
    }
    if (embed_open(s) != 0) {
        return;
    }
    if (s->enrolling && n != 1) {
        /* One face, the owner's, and nobody else's. */
        say_who(s, f->seq, n);
        return;
    }
    /* A round looks at VISION_RECOG_FACES faces: those never scored first,
     * then those scored longest ago, the larger first among equals - so
     * every face in a crowd is looked at in turn, not the largest three
     * over and over. While enrolling there is one face. */
    {
        int64_t age[VISION_FACE_MAX];

        for (i = 0; i < n; i++) {
            uint32_t id = s->enrolling ? 0 : track_of(s, &s->dets[i].box);

            order[i] = i;
            age[i] = id ? now - who_at(s, id) : 0;
            if (id && who_at(s, id) == 0) {
                age[i] = INT64_MAX;
            }
        }
        for (i = 1; i < n; i++) {
            int k = order[i];

            for (j = i; j > 0; j--) {
                int p = order[j - 1];
                bool before = age[k] > age[p] ||
                              (age[k] == age[p] && faces[k].box.w * faces[k].box.h > faces[p].box.w * faces[p].box.h);

                if (!before) {
                    break;
                }
                order[j] = p;
            }
            order[j] = k;
        }
    }
    if (n > 0 && (vision_net_turn(s->femb, view_of(s).rotation) != 0 || vision_net_frame(s->femb, in) != 0)) {
        say("recogfail the face embedding model failed on a frame");
        embed_close(s);
        return;
    }
    for (i = 0; i < n && done < VISION_RECOG_FACES; i++) {
        const struct vision_face *fc = &faces[order[i]];
        float pts[5][2];
        float m[6];
        float emb[VISION_EMBED_MAX];
        const float *raw;
        size_t count = 0;

        if ((float)(fc->box.w < fc->box.h ? fc->box.w : fc->box.h) / ratio < VISION_RECOG_MIN_SIDE) {
            continue;
        }
        for (j = 0; j < 5; j++) {
            pts[j][0] = (float)fc->pt[j][0] / ratio;
            pts[j][1] = (float)fc->pt[j][1] / ratio;
        }
        if (vision_embed_align(pts, m) != 0 || vision_net_run_affine(s->femb, m, NULL, NULL) != 0) {
            continue;
        }
        raw = vision_net_output(s->femb, 0, &count);
        if (!raw || count != s->embed_dim || vision_embed_unit(raw, count, emb) != 0) {
            continue;
        }
        done++;
        if (s->enrolling) {
            if (vision_owner_add(&s->enrol, emb) == 0) {
                say("enrol %u %d", s->enrol.samples, VISION_OWNER_SAMPLES);
            }
            if (s->enrol.samples >= VISION_OWNER_SAMPLES) {
                s->enrolling = false;
                if (vision_owner_finish(&s->enrol) != 0 || owner_save(&s->enrol) != 0) {
                    say("enrolfail the owner could not be kept on the unit");
                } else {
                    s->owner = s->enrol;
                    s->have_owner = true;
                    say_owner(s);
                }
            }
            continue;
        }
        if (s->have_owner) {
            uint32_t id = track_of(s, &s->dets[order[i]].box);

            if (id) {
                who_set(s, id, vision_embed_score(emb, s->owner.v, s->embed_dim), now);
            }
        }
    }
    say_who(s, f->seq, n);
}

/* One frame through the face detector: faces on the upright picture, back
 * to frame pixels, into the tracker (for ids), said as a `det` line with
 * class 0 - the screen knows FACE's boxes are faces. */
static void detect_faces(struct session *s, const struct pocketcam_frame *f, int64_t now)
{
    struct vision_face faces[VISION_FACE_MAX];
    const float *out[VISION_FACE_OUTPUTS];
    size_t count[VISION_FACE_OUTPUTS];
    struct pocketcam_frame in;
    struct vision_view v = view_of(s);
    int64_t t0 = mono_ms();
    size_t need = (size_t)3 * f->width * f->height;
    uint32_t bad = 0;
    float ratio;
    int pre = 0;
    int inf = 0;
    int n;
    int i;

    (void)now;
    if (face_open(s) != 0) {
        return;
    }
    if (v.rotation != 0 && s->upright_size < need) {
        uint8_t *b = realloc(s->upright, need);

        if (b) {
            s->upright = b;
            s->upright_size = need;
        }
    }
    if (upright_input(f, v.rotation, s->upright_size >= need ? s->upright : NULL, &in) != 0 ||
        vision_net_turn(s->fdet, v.rotation) != 0 || vision_net_frame(s->fdet, &in) != 0 ||
        vision_net_run(s->fdet, NULL, false, VISION_FACE_PAD, &pre, &inf) != 0) {
        say("facefail the face detector failed on a frame");
        face_close(s);
        return;
    }
    for (i = 0; i < VISION_FACE_OUTPUTS; i++) {
        out[i] = vision_net_output(s->fdet, i, &count[i]);
    }
    n = vision_face_decode(out, count, s->fdi.in_w, s->fdi.in_h, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, faces,
                           VISION_FACE_MAX, &bad);
    if (n < 0) {
        n = 0;
        bad++;
    }
    s->bad_total += bad;
    /* Letterboxed, padded right and below: the input's pixels over the
     * ratio are the upright picture's. */
    ratio = (float)s->fdi.in_w / (float)in.width < (float)s->fdi.in_h / (float)in.height
                ? (float)s->fdi.in_w / (float)in.width
                : (float)s->fdi.in_h / (float)in.height;
    n = n < VISION_MAX_DETECTIONS ? n : VISION_MAX_DETECTIONS;
    for (i = 0; i < n; i++) {
        struct vision_box b = {
            (int32_t)((float)faces[i].box.x / ratio), (int32_t)((float)faces[i].box.y / ratio),
            (int32_t)((float)faces[i].box.w / ratio), (int32_t)((float)faces[i].box.h / ratio),
        };

        if ((uint32_t)(b.x + b.w) > in.width) {
            b.w = (int32_t)in.width - b.x;
        }
        if ((uint32_t)(b.y + b.h) > in.height) {
            b.h = (int32_t)in.height - b.y;
        }
        s->dets[i].box = b;
        s->dets[i].cls = 0;
        s->dets[i].conf = faces[i].conf;
    }
    unturn_dets(s->dets, n, f, v.rotation);
    vision_tracker_update(&s->tracker, s->dets, n);
    if (s->mode == MODE_RECOGNIZE) {
        recognise(s, f, &in, faces, n, ratio, now);
    }
    s->last_pre_ms = pre;
    s->last_infer_ms = inf;
    s->last_post_ms = (int)(mono_ms() - t0) - pre - inf;
    if (s->last_post_ms < 0) {
        s->last_post_ms = 0;
    }
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
        char *d = strtok_r(NULL, " ", &save);
        struct vision_line was[VISION_LINES];
        bool speed_moved = false;
        unsigned vw;
        unsigned vh;
        int rot;
        int i;

        if (!a || !b || !c || sscanf(a, "%u", &vw) != 1 || sscanf(b, "%u", &vh) != 1 ||
            parse_rotation(c, &rot) != 0 || vw == 0 || vh == 0 || vw > POCKETCAM_VIEW_MAX_W ||
            vh > POCKETCAM_VIEW_MAX_H || (d && strcmp(d, "contain") != 0)) {
            return;
        }
        memcpy(was, s->line, sizeof(was));
        s->view_w = vw;
        s->view_h = vh;
        s->display_rotation = rot;
        s->view_contain = d != NULL;
        place_lines(s);
        /* A turn or another picture size puts the lines somewhere else
         * among the tracks, which stay where they are in the frame: a side
         * remembered against the old line would count a crossing nobody
         * made, and a speed timed from the old line A is worth nothing. So
         * every line that moved is learnt afresh and the speeds in flight
         * go; the counts so far stay (the line command resets those). */
        for (i = 0; i < VISION_LINES; i++) {
            const struct vision_line *o = &was[i];
            const struct vision_line *n = &s->line[i];

            if (o->enabled == n->enabled && (!n->enabled || (o->x0 == n->x0 && o->y0 == n->y0 &&
                                                              o->x1 == n->x1 && o->y1 == n->y1 &&
                                                              o->dead_max == n->dead_max))) {
                continue;
            }
            vision_line_forget(&s->tracker, i);
            speed_moved |= i != VISION_LINE_COUNT;
        }
        if (speed_moved) {
            memset(s->tf.slot, 0, sizeof(s->tf.slot));
            s->tf.cur_kmh10 = 0;
            s->tf.cur_id = 0;
            if (s->traffic) {
                say_traffic(s);
            }
        }
    } else if (strcmp(w, "start") == 0) {
        if (!s->streaming) {
            int r = s->replay ? 0 : pocketcam_start(&s->cam);

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
            if (!s->replay) {
                pocketcam_stop(&s->cam);
            }
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

        if (a && strcmp(a, "traffic") == 0 && s->detect_offered) {
            set_mode(s, MODE_TRAFFIC);
        } else if (a && strcmp(a, "detect") == 0 && s->detect_offered) {
            set_mode(s, MODE_DETECT);
        } else if (a && strcmp(a, "track") == 0 && s->detect_offered) {
            set_mode(s, MODE_TRACK);
        } else if (a && strcmp(a, "color") == 0) {
            set_mode(s, MODE_COLOR);
        } else if (a && strcmp(a, "edge") == 0) {
            set_mode(s, MODE_EDGE);
        } else if (a && strcmp(a, "trace") == 0) {
            set_mode(s, MODE_TRACE);
        } else if (a && strcmp(a, "read") == 0 && s->text_offered) {
            set_mode(s, MODE_READ);
            /* The models load now, not on the first frame: a failure is
             * said at once. */
            text_open(s);
        } else if (a && strcmp(a, "face") == 0 && s->face_offered) {
            set_mode(s, MODE_FACE);
            face_open(s);
        } else if (a && strcmp(a, "recognize") == 0 && s->recog_offered) {
            set_mode(s, MODE_RECOGNIZE);
            if (face_open(s) == 0 && embed_open(s) == 0) {
                owner_load(s);
            }
            say_owner(s);
        }
    } else if (strcmp(w, "enrol") == 0) {
        /* `enrol`: the next VISION_OWNER_SAMPLES views of the one face in
         * view become the owner; `enrol off` gives up. */
        char *a = strtok_r(NULL, " ", &save);

        if (a && strcmp(a, "off") == 0) {
            s->enrolling = false;
        } else if (s->mode != MODE_RECOGNIZE || !s->femb) {
            say("enrolfail RECOGNIZE is not running");
        } else if (vision_owner_begin(&s->enrol, s->embed_name, s->embed_bytes, s->embed_dim) != 0) {
            say("enrolfail the face embedding model cannot make an owner");
        } else {
            s->enrolling = true;
            s->enrol_since_ms = mono_ms();
            say("enrol 0 %d", VISION_OWNER_SAMPLES);
        }
    } else if (strcmp(w, "forget") == 0) {
        /* The owner goes, from the unit too. */
        char path[POCKETOS_PATH_MAX + 32];

        owner_path(path, sizeof(path));
        unlink(path);
        s->have_owner = false;
        s->enrolling = false;
        memset(s->who, 0, sizeof(s->who));
        say_owner(s);
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
    } else if (strcmp(w, "range") == 0) {
        char *a = strtok_r(NULL, " ", &save);
        int r = vision_range_parse(a);

        if (r < 0 || (enum vision_range)r == s->range) {
            return;
        }
        s->range = (enum vision_range)r;
        /* Another way of detecting: the tracks start again (a track keeps
         * the side of the line it was on, so nothing already counted counts
         * again) and so does any speed in progress. The counts stay. */
        vision_tracker_clear(&s->tracker);
        tracker_params(s);
        memset(s->tf.slot, 0, sizeof(s->tf.slot));
        s->tf.cur_kmh10 = 0;
        s->tf.cur_id = 0;
        if (s->traffic) {
            say_traffic(s);
        }
    } else if (strcmp(w, "reset") == 0) {
        memset(&s->counts, 0, sizeof(s->counts));
        vision_tracker_clear(&s->tracker);
        vision_traffic_reset(&s->tf);
        vision_window_init(&s->win);
        say_counts(s);
        if (s->traffic) {
            say_traffic(s);
            say_recent(s, mono_ms());
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

/* ---- REPLAY ---------------------------------------------------------------------
 *
 * `--backend image` with `--config A.ppm,B.ppm,...` (or the same through
 * $POCKETOS_CAMERA_BACKEND and $POCKETOS_VISION_CAMERA_CONFIG): the session
 * plays saved pictures (binary PPM, as bench --save writes and --image
 * reads) in turn, one every $POCKETOS_VISION_REPLAY_MS (default 100), round
 * and round, instead of the camera - for a bench whose camera sees nothing
 * useful (a dark room, no traffic), so the screen, the detector and the
 * other models run on a real scene. The picture is upright as it is, the
 * camera is never opened, and the screen says SIMULATED. */
#define VISION_REPLAY_DEFAULT_MS 100

static int load_image(const char *path, struct pocketcam_frame *f, uint8_t **buf);

static int replay_name(const struct session *s, int idx, char *out, size_t len)
{
    const char *p = s->replay_list;
    int i;
    size_t n;

    for (i = 0; i < idx && p; i++) {
        p = strchr(p, ',');
        p = p ? p + 1 : NULL;
    }
    if (!p) {
        return -1;
    }
    n = strcspn(p, ",");
    if (n == 0 || n >= len) {
        return -1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static int replay_load(struct session *s, int idx)
{
    char name[256];
    uint8_t *buf = NULL;
    struct pocketcam_frame f;

    if (replay_name(s, idx, name, sizeof(name)) != 0 || load_image(name, &f, &buf) != 0) {
        return -1;
    }
    if (s->replay_frame.width && (f.width != s->replay_frame.width || f.height != s->replay_frame.height)) {
        free(buf);
        return -1; /* every picture of a replay is the first's size */
    }
    free(s->replay_buf);
    s->replay_buf = buf;
    s->replay_frame = f;
    s->replay_idx = idx;
    return 0;
}

static int replay_open(struct session *s, const char *list)
{
    const char *env = getenv("POCKETOS_VISION_REPLAY_MS");
    const char *p;
    char t[96];

    s->replay = true;
    s->replay_list = list ? strdup(list) : NULL;
    s->replay_count = 0;
    for (p = s->replay_list; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        s->replay_count++;
    }
    s->replay_period_ms = env && atoi(env) > 0 ? atoi(env) : VISION_REPLAY_DEFAULT_MS;
    if (s->replay_count == 0 || replay_load(s, 0) != 0) {
        say("nodevice %s", clean("replay: no picture to play (--config A.ppm,B.ppm)", t, sizeof(t)));
        return -ENODEV;
    }
    memset(&s->info, 0, sizeof(s->info));
    snprintf(s->info.name, sizeof(s->info.name), "replay");
    s->info.preview_w = s->replay_frame.width;
    s->info.preview_h = s->replay_frame.height;
    s->info.simulated = true;
    return 0;
}

static void replay_close(struct session *s)
{
    free(s->replay_buf);
    free(s->replay_list);
    s->replay_buf = NULL;
    s->replay_list = NULL;
}

/* The next picture when it is due; -ETIMEDOUT before. */
static int replay_next(struct session *s, struct pocketcam_frame *f)
{
    int64_t now = mono_ms();

    if (now < s->replay_due_ms) {
        int64_t wait = s->replay_due_ms - now;

        poll(NULL, 0, (int)(wait < FRAME_WAIT_MS ? wait : FRAME_WAIT_MS));
        if (mono_ms() < s->replay_due_ms) {
            return -ETIMEDOUT;
        }
    }
    s->replay_due_ms = mono_ms() + s->replay_period_ms;
    if (s->replay_count > 1 && replay_load(s, (s->replay_idx + 1) % s->replay_count) != 0) {
        return -EPROTO;
    }
    *f = s->replay_frame;
    f->seq = ++s->replay_seq;
    return 0;
}

static void stream_once(struct session *s)
{
    struct pocketcam_frame f;
    int64_t now;
    int r = s->replay ? replay_next(s, &f) : pocketcam_next(&s->cam, FRAME_WAIT_MS, &f);

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
    if (s->mode == MODE_READ) {
        /* A read is slow (the detector and a run per line): at most every
         * VISION_READ_INTERVAL_MS, the preview going on between. */
        if (now - s->last_read_ms >= VISION_READ_INTERVAL_MS) {
            read_text(s, &f, now);
        }
    } else if (s->mode == MODE_FACE || s->mode == MODE_RECOGNIZE) {
        detect_faces(s, &f, now);
    } else if (!pixel_mode(s)) {
        detect(s, &f, now);
    }
    if (!s->quit && s->view_w && now - s->last_sent_ms >= POCKETCAM_PREVIEW_MIN_INTERVAL_MS) {
        int slot = free_preview_slot(s);

        if (slot >= 0 &&
            pocketcam_to_rgb565(&f, view_of(s).rotation, s->info.mount_mirror,
                                s->view_contain ? POCKETCAM_FIT_CONTAIN : POCKETCAM_FIT_COVER,
                                slot_pixels(s, slot), s->view_w, s->view_h, s->view_w) == 0) {
            if (pixel_mode(s)) {
                process_pixels(s, slot_pixels(s, slot));
            }
            s->held[slot] = true;
            s->last_sent_ms = now;
            say("frame %d %u %u %u", slot, f.seq, s->view_w, s->view_h);
        }
    }
    if (!s->replay) {
        pocketcam_release(&s->cam, &f);
    }
    if (now - s->last_stats_ms >= VISION_STATS_INTERVAL_MS) {
        say_stats(s, now);
        if (s->traffic) {
            /* The window moves on whether or not anything crosses. */
            say_recent(s, now);
        }
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
    s->range = VISION_RANGE_NORMAL;
    vision_window_init(&s->win);
    tracker_params(s);
    s->text_det_path = getenv("POCKETOS_VISION_TEXT_DET");
    s->text_rec_path = getenv("POCKETOS_VISION_TEXT_REC");
    s->text_dict_path = getenv("POCKETOS_VISION_TEXT_DICT");
    s->text_det_path = s->text_det_path && *s->text_det_path ? s->text_det_path : VISION_TEXT_DET_DEFAULT;
    s->text_rec_path = s->text_rec_path && *s->text_rec_path ? s->text_rec_path : VISION_TEXT_REC_DEFAULT;
    s->text_dict_path = s->text_dict_path && *s->text_dict_path ? s->text_dict_path : VISION_TEXT_DICT_DEFAULT;
    s->face_det_path = getenv("POCKETOS_VISION_FACE_DET");
    s->face_det_path = s->face_det_path && *s->face_det_path ? s->face_det_path : VISION_FACE_DET_DEFAULT;
    s->face_embed_path = getenv("POCKETOS_VISION_FACE_EMBED");
    s->face_embed_path = s->face_embed_path && *s->face_embed_path ? s->face_embed_path : VISION_FACE_EMBED_DEFAULT;
    s->net_script = kpu_script;
    say("hello %d %s %s", VISION_PROTO_VERSION, backend, vision_kpu_backend());
    if (map_shm(s) != 0) {
        say("error shm no usable shared memory on descriptor %d", POCKETCAM_SHM_FD);
        free(s);
        return EXIT_USAGE;
    }
    if (strcmp(backend, "image") == 0 ? replay_open(s, config) != 0
                                      : open_camera(&s->cam, &s->info, backend, config, true) != 0) {
        replay_close(s);
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        free(s);
        return EXIT_NOCAMERA;
    }
    /* No detector file at all is not a failure: the modes that need it are
     * not offered (caps below), and the others run. A file that is there
     * and does not open is one, as before. The host's fake detector reads
     * no file, so it is only asked when a model was named (a test). */
    if ((strcmp(vision_kpu_backend(), "fake") != 0 || strcmp(model, VISION_MODEL_DEFAULT) != 0) &&
        access(model, F_OK) != 0 && errno == ENOENT) {
        fprintf(stderr, "pos-vision: %s: model file not found; DETECT, TRACK and TRAFFIC are not offered
",
                model);
        s->detect_offered = false;
        snprintf(s->model.model, sizeof(s->model.model), "none");
        s->mode = MODE_COLOR;
    } else if (open_model(&s->kpu, &s->model, model, kpu_script, true) != 0) {
        if (s->replay) {
            replay_close(s);
        } else {
            pocketcam_close(&s->cam);
        }
        munmap(s->shm, POCKETCAM_SHM_BYTES);
        free(s);
        return EXIT_NOMODEL;
    } else {
        s->detect_offered = true;
        /* The model's classes onto the traffic classes, by name: the
         * traffic logic never learns which detector this is. */
        vision_traffic_map_names(&s->tf, s->model.classes, vision_label);
    }
    say("ready %s %u %u %d %s %u %u %u", s->info.name, s->info.preview_w, s->info.preview_h,
        s->info.simulated ? 1 : 0, s->model.model, s->model.in_w, s->model.in_h, s->model.classes);
    /* What this helper can run: the detector's modes and the pixel modes,
     * and READ when its models are on the unit. */
    s->text_offered = access(s->text_det_path, R_OK) == 0 && access(s->text_rec_path, R_OK) == 0 &&
                      access(s->text_dict_path, R_OK) == 0;
    s->face_offered = access(s->face_det_path, R_OK) == 0;
    s->recog_offered = s->face_offered && access(s->face_embed_path, R_OK) == 0;
    say("caps%s color edge trace%s%s%s", s->detect_offered ? " detect track traffic" : "",
        s->text_offered ? " read" : "", s->face_offered ? " face" : "", s->recog_offered ? " recognize" : "");
    while (!s->quit && !s->in_eof && !stop_requested() && !out_broken) {
        read_commands(s, s->streaming ? 0 : 250);
        if (s->streaming && !s->quit) {
            stream_once(s);
        }
    }
    /* The camera first, then the nets and the detector: the sensor is what
     * another screen may be waiting for; the detector returns the runtime's
     * shared pool, so it goes last. */
    if (s->replay) {
        replay_close(s);
    } else {
        pocketcam_close(&s->cam);
    }
    text_close(s);
    face_close(s);
    embed_close(s);
    vision_kpu_close(s->kpu);
    /* Only now, with the camera closed and the KPU pool given back: `bye`
     * is the session's word that a kill is no longer needed. Said on every
     * orderly end (quit, SIGTERM, the shell's end closed), not after a
     * failure, which has said its own word already. */
    if (s->exit_code == 0) {
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
    const char *images;          /* PPMs, comma-separated, fed in turn as a sequence (repeated to N frames) */
    int turn;                    /* the frame turned by this for the detector (upright_input) */
    enum vision_range range;     /* the pipeline preset (vision_range.h) */
    bool tracks;                 /* every frame's tracks, one line each, for a tracking replay */
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

/* The idx-th name of a comma-separated list (wrapping round), into out. 0, or
 * -1 for an empty list. */
static int nth_name(const char *list, int idx, char *out, size_t len)
{
    int count = 1;
    const char *p;
    int i;

    for (p = list; *p; p++) {
        count += *p == ',';
    }
    idx %= count;
    p = list;
    for (i = 0; i < idx; i++) {
        p = strchr(p, ',') + 1;
    }
    i = (int)strcspn(p, ",");
    if (i == 0 || (size_t)i >= len) {
        return -1;
    }
    memcpy(out, p, (size_t)i);
    out[i] = '\0';
    return 0;
}

/* Whether (x, y) is on the outline of b, t pixels thick. */
static bool on_outline(int32_t x, int32_t y, const struct vision_box *b, int32_t t)
{
    return b->w > 0 && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h &&
           (x < b->x + t || x >= b->x + b->w - t || y < b->y + t || y >= b->y + b->h - t);
}

/* The frame as a binary PPM (the ISP's planar BGR holds R, G and B planes in
 * that order, VERIFIED on unit B), with this frame's boxes drawn in: the
 * range's detections in green, and the full picture's vehicle candidates
 * below the threshold in red. 0, or -1 with the reason on stderr. */
static int save_frame(const char *path, const struct pocketcam_frame *f, const struct vision_det *veh, int nveh,
                      const struct vision_det *dets, int ndets)
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
                if (veh[k].conf < VISION_CONF_MIN && on_outline((int32_t)x, (int32_t)y, &veh[k].box, 1)) {
                    px[0] = 255;
                    px[1] = 0;
                    px[2] = 0;
                }
            }
            for (k = 0; k < ndets; k++) {
                if (on_outline((int32_t)x, (int32_t)y, &dets[k].box, 1)) {
                    px[0] = 0;
                    px[1] = 255;
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

/* Frames through the session's own pipeline at a range, timings printed,
 * nothing drawn: the measurement the gate reads. With --image or --images a
 * saved picture (or a sequence of them) stands in for the camera, so one
 * scene can be compared at every range on the same model. */
static int run_bench(const char *backend, const char *config, const char *model,
                     const char *kpu_script, const struct bench_opts *o)
{
    static uint8_t ids_seen[65536 / 8];
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
    struct vision_range_params rp;
    struct pipe_out po;
    struct pocketcam_frame image;
    uint8_t *image_buf = NULL;
    uint8_t *turn_buf = NULL;
    size_t turn_size = 0;
    bool from_file = o->image || o->images;
    int64_t pre_sum = 0;
    int64_t infer_sum = 0;
    int64_t post_sum = 0;
    int64_t t_start;
    /* Vehicle candidates of the full picture at and below the threshold, and
     * of those it takes: in how many frames, their height in the model's own
     * pixels, and the best confidence seen. */
    uint32_t veh_detect = 0;
    uint32_t veh_below = 0;
    uint32_t veh_frames = 0;
    int32_t h_min = -1;
    int32_t h_max = -1;
    int64_t h_sum = 0;
    uint16_t conf_max = 0;
    /* What the range's pipeline gave: vehicles, the zoom pass's own boxes,
     * and the tracks it made. */
    uint32_t range_veh = 0;
    uint32_t range_other = 0;
    uint32_t zoom_boxes = 0;
    uint32_t confirmed_ids = 0;
    int max_live = 0;
    uint32_t ratio_pm;
    int save_at = o->frames > VISION_BENCH_SAVE_AT ? VISION_BENCH_SAVE_AT : o->frames - 1;
    int done = 0;
    int r;

    memset(ids_seen, 0, sizeof(ids_seen));
    if (!cands || !vcands) {
        free(cands);
        free(vcands);
        return EXIT_USAGE;
    }
    if (from_file) {
        char first[512];

        if ((o->images && nth_name(o->images, 0, first, sizeof(first)) != 0) ||
            load_image(o->image ? o->image : first, &image, &image_buf) != 0) {
            free(cands);
            free(vcands);
            return EXIT_USAGE;
        }
        memset(&info, 0, sizeof(info));
        snprintf(info.name, sizeof(info.name), o->images ? "images" : "image");
        info.preview_w = image.width;
        info.preview_h = image.height;
    } else if (open_camera(&cam, &info, backend, config, false) != 0) {
        free(cands);
        free(vcands);
        return EXIT_NOCAMERA;
    }
    if (open_model(&kpu, &mi, model, kpu_script, false) != 0) {
        if (!from_file) {
            pocketcam_close(&cam);
        }
        free(image_buf);
        free(cands);
        free(vcands);
        return EXIT_NOMODEL;
    }
    vision_range_params(o->range, &rp);
    /* The letterbox: frame pixels to the model's (the same for the turned
     * picture, the smaller ratio of the two sides either way). */
    ratio_pm = mi.in_w * 1000u / info.preview_w < mi.in_h * 1000u / info.preview_h
                   ? mi.in_w * 1000u / info.preview_w
                   : mi.in_h * 1000u / info.preview_h;
    printf("camera %s %ux%u, model %s %ux%u classes %u rows %u, %d frames\n", info.name,
           info.preview_w, info.preview_h, mi.model, mi.in_w, mi.in_h, mi.classes, mi.rows, o->frames);
    printf("range %s: threshold %u%%, smallest box %u per mille of the side, zoom %s, confirm %u, kept %u\n",
           vision_range_word(o->range), rp.conf_min / 10, rp.min_side_pm, rp.zoom ? "yes" : "no", rp.min_hits,
           rp.max_misses);
    printf("vehicles: full-picture candidates shown from %u%%; model px = frame px x %u.%03u\n",
           VISION_BENCH_VEHICLE_FLOOR / 10, ratio_pm / 1000, ratio_pm % 1000);
    vision_traffic_init(&tf);
    vision_traffic_map_names(&tf, mi.classes, vision_label);
    vision_tracker_init(&tr);
    /* Tracked as TRAFFIC tracks at this range: vehicles across their group,
     * confirmed and kept as the range says. */
    tr.group = tf.group;
    tr.group_classes = tf.classes;
    tr.min_hits = rp.min_hits;
    tr.max_misses = rp.max_misses;
    if (o->turn != 0) {
        printf("upright: the frame is turned %d degrees for the detector\n", o->turn);
    }
    r = from_file ? 0 : pocketcam_start(&cam);
    t_start = mono_ms();
    while (r == 0 && done < o->frames && !stop_requested()) {
        struct pocketcam_frame f;
        int pre_ms;
        int infer_ms;
        int64_t t0;
        int n;
        uint32_t bad;

        if (o->images && done > 0) {
            char name[512];

            free(image_buf);
            image_buf = NULL;
            if (nth_name(o->images, done, name, sizeof(name)) != 0 || load_image(name, &image, &image_buf) != 0) {
                r = -EIO;
                break;
            }
        }
        if (from_file) {
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
        /* The detector's picture: upright (--turn), as the session gives it,
         * through the session's own pipeline at the chosen range. */
        t0 = mono_ms();
        r = pipeline(kpu, &mi, &turn_buf, &turn_size, &f, o->turn, &rp, VISION_BENCH_VEHICLE_FLOOR, tf.group,
                     tf.classes, cands, dets, &po);
        pre_ms = po.pre_ms;
        infer_ms = po.infer_ms;
        bad = po.bad;
        if (r != 0 && r != -EPROTO) {
            if (!from_file) {
                pocketcam_release(&cam, &f);
            }
            fprintf(stderr, "pos-vision: infer failed (%d)\n", r);
            break;
        }
        r = 0;
        n = po.ncands;
        if (po.bad * 2 <= mi.rows) {
            int nv = 0;
            int i;
            int live = 0;
            bool seen = false;

            /* Apart from what the range takes (the boxes line, as always),
             * every vehicle candidate of the full picture down to the
             * bench's floor. */
            for (i = 0; i < n; i++) {
                if (vision_traffic_vehicle(&tf, cands[i].cls)) {
                    vcands[nv++] = cands[i];
                }
            }
            nv = vision_nms(vcands, nv, VISION_NMS_IOU, vdets, VISION_MAX_DETECTIONS);
            nv = vision_nms_nested(vdets, nv, VISION_NESTED_PM);
            n = po.n;
            for (i = 0; i < n; i++) {
                if (vision_traffic_vehicle(&tf, dets[i].cls)) {
                    range_veh++;
                } else {
                    range_other++;
                }
            }
            zoom_boxes += (uint32_t)po.zoom_boxes;
            vision_tracker_update(&tr, dets, n);
            for (i = 0; i < tr.count; i++) {
                uint32_t id = tr.t[i].id;

                if (!tr.t[i].confirmed) {
                    continue;
                }
                live++;
                if (id < 65536 && !(ids_seen[id / 8] & (1u << (id % 8)))) {
                    ids_seen[id / 8] |= (uint8_t)(1u << (id % 8));
                    confirmed_ids++;
                }
            }
            max_live = live > max_live ? live : max_live;
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
                save_frame(path, &f, vdets, nv, dets, n);
            }
            printf("frame %u: pre %d ms infer %d ms, %d boxes%s, %u rows refused:", f.seq, pre_ms, infer_ms, n,
                   po.zoomed ? " (zoom pass)" : "", bad);
            for (i = 0; i < n && i < 8; i++) {
                printf(" %s %u%% (%d,%d %dx%d)", vision_label(dets[i].cls), dets[i].conf / 10,
                       dets[i].box.x, dets[i].box.y, dets[i].box.w, dets[i].box.h);
            }
            printf("; %d tracks, %d confirmed\n", tr.count, live);
            if (o->tracks) {
                /* One line per frame: every track, confirmed or not, with
                 * its box, sightings and misses - what a replay analysis
                 * reads to see ids survive, split or double. */
                printf("  tracks %u:", f.seq);
                for (i = 0; i < tr.count; i++) {
                    const struct vision_track *k = &tr.t[i];

                    printf(" %u:%s:%d,%d,%d,%d:%u:%u:%c", k->id, vision_label(k->cls), k->box.x, k->box.y, k->box.w,
                           k->box.h, k->hits, k->misses, k->confirmed ? 'C' : 'n');
                }
                printf("\n");
            }
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
            printf("frame %u: malformed output (%u rows refused)\n", f.seq, bad);
        }
        if (!from_file) {
            pocketcam_release(&cam, &f);
        }
        pre_sum += pre_ms;
        infer_sum += infer_ms;
        post_sum += (mono_ms() - t0) - pre_ms - infer_ms;
        done++;
    }
    if (done > 0) {
        int64_t wall = mono_ms() - t_start;

        printf("%d frames in %lld ms: %.1f fps; mean pre %.1f ms, infer %.1f ms, post %.1f ms\n",
               done, (long long)wall, wall > 0 ? done * 1000.0 / (double)wall : 0.0,
               (double)pre_sum / done, (double)infer_sum / done, (double)post_sum / done);
        printf("full-picture vehicle candidates: %u at the threshold, %u below it", veh_detect, veh_below);
        if (veh_detect > 0) {
            printf("; those are %d..%d model px tall (mean %.1f), in %u of %d frames", h_min, h_max,
                   (double)h_sum / veh_detect, veh_frames, done);
        }
        if (veh_detect + veh_below > 0) {
            printf(", best %u%%", conf_max / 10);
        }
        printf("\n");
        printf("range %s: %u vehicle detections (%.2f a frame), %u others; zoom pass boxes %u; "
               "%u confirmed track ids, at most %d at once\n",
               vision_range_word(o->range), range_veh, (double)range_veh / done, range_other, zoom_boxes,
               confirmed_ids, max_live);
    }
    if (!from_file) {
        pocketcam_close(&cam);
    }
    vision_kpu_close(kpu);
    free(image_buf);
    free(turn_buf);
    free(cands);
    free(vcands);
    return r == 0 ? 0 : EXIT_LOST;
}


/* pos-vision text: the text models on a saved picture, as READ runs them,
 * every region read with its confidence and the step each character came
 * at (what the word breaks are made from). On unit B the vendor's pair read
 * under the blank-last convention (the characters from class 0, the blank
 * the last class) and gave nonsense under PaddleOCR's blank-first one. */
static int run_text_bench(const char *image, const char *det_path, const char *rec_path, const char *dict_path,
                          int frames)
{
    struct pocketcam_frame f;
    uint8_t *buf = NULL;
    struct vision_net *det = NULL;
    struct vision_net *rec = NULL;
    struct vision_net_info di;
    struct vision_net_info ri;
    struct vision_dict dict;
    char err[96] = "";
    int k;
    int rc = 0;

    if (load_image(image, &f, &buf) != 0) {
        return EXIT_USAGE;
    }
    if (dict_load(&dict, dict_path) != 0) {
        fprintf(stderr, "pos-vision: %s: no dictionary\n", dict_path);
        free(buf);
        return EXIT_NOMODEL;
    }
    if (vision_net_open(&det, det_path, NULL, &di, err, sizeof(err)) != 0 ||
        vision_net_open(&rec, rec_path, NULL, &ri, err, sizeof(err)) != 0) {
        fprintf(stderr, "pos-vision: text models: %s\n", err);
        vision_net_close(det);
        dict_free(&dict);
        free(buf);
        return EXIT_NOMODEL;
    }
    printf("picture %ux%u; detector %ux%u -> [%u,%u,%u,%u]; recogniser %ux%u -> [%u,%u,%u]; dictionary %u entries\n",
           f.width, f.height, di.in_w, di.in_h, di.dims[0][0], di.dims[0][1], di.dims[0][2], di.dims[0][3], ri.in_w,
           ri.in_h, ri.dims[0][0], ri.dims[0][1], ri.dims[0][2], dict.n);
    for (k = 0; k < frames && rc == 0; k++) {
        struct vision_text_box tb[VISION_TEXT_MAX];
        int64_t t0 = mono_ms();
        int pre = 0;
        int inf = 0;
        size_t count;
        const float *map;
        float ratio;
        int n;
        int i;
        int64_t rec_ms = 0;

        if (vision_net_frame(det, &f) != 0 || vision_net_run(det, NULL, false, 0, &pre, &inf) != 0) {
            rc = EXIT_LOST;
            break;
        }
        map = vision_net_output(det, 0, &count);
        n = vision_text_regions(map, di.dims[0][2], di.dims[0][1], di.dims[0][3] ? di.dims[0][3] : 1,
                                VISION_TEXT_THRESHOLD, VISION_TEXT_BOX_MIN, tb, VISION_TEXT_MAX);
        ratio = (float)di.in_w / (float)f.width < (float)di.in_h / (float)f.height ? (float)di.in_w / (float)f.width
                                                                                    : (float)di.in_h / (float)f.height;
        printf("frame %d: detector pre %d ms run %d ms, %d regions\n", k + 1, pre, inf, n);
        if (vision_net_frame(rec, &f) != 0) {
            rc = EXIT_LOST;
            break;
        }
        for (i = 0; i < n; i++) {
            struct vision_box b = {
                (int32_t)((float)tb[i].box.x / ratio), (int32_t)((float)tb[i].box.y / ratio),
                (int32_t)((float)tb[i].box.w / ratio), (int32_t)((float)tb[i].box.h / ratio),
            };
            struct vision_box g;
            uint32_t cls[VISION_TEXT_CHARS];
            const float *sc;
            /* The classes are the last dimension, whatever the layout
             * ([1, T, C] or [T, 1, C]); the steps are the rest. */
            uint32_t classes = ri.dims[0][ri.rank[0] > 0 ? ri.rank[0] - 1 : 0];
            uint32_t steps = classes ? (uint32_t)(ri.count[0] / classes) : 0;
            uint16_t conf = 0;
            int64_t r0 = mono_ms();
            int m;
            int j;

            if ((uint32_t)(b.x + b.w) > f.width) {
                b.w = (int32_t)f.width - b.x;
            }
            if ((uint32_t)(b.y + b.h) > f.height) {
                b.h = (int32_t)f.height - b.y;
            }
            g = text_margin(&b, f.width, f.height);
            if (g.w <= 0 || g.h <= 0 || vision_net_run(rec, &g, false, 0, NULL, NULL) != 0) {
                continue;
            }
            sc = vision_net_output(rec, 0, &count);
            rec_ms += mono_ms() - r0;
            {
                uint16_t pos[VISION_TEXT_CHARS];
                char text[VISION_READ_TEXT * 2];

                m = vision_text_ctc_pos(sc, steps, classes, classes - 1, cls, pos, VISION_TEXT_CHARS, &conf);
                line_text(&dict, cls, m > 0 ? m : 0, text, sizeof(text));
                printf("  region %d (%d,%d %dx%d) score %u%%: %3u%% \"%s\"  steps", i, g.x, g.y, g.w, g.h,
                       tb[i].score / 10, conf / 10, text);
                for (j = 0; j < m; j++) {
                    printf(" %u", pos[j]);
                }
                printf("\n");
            }
        }
        printf("  frame %d: %lld ms in all, recogniser %lld ms\n", k + 1, (long long)(mono_ms() - t0),
               (long long)rec_ms);
    }
    vision_net_close(rec);
    vision_net_close(det);
    dict_free(&dict);
    free(buf);
    return rc;
}

/* FACE's model on a picture: the faces, their points and the times. */
static int run_face_bench(const char *image, const char *path, int frames)
{
    struct pocketcam_frame f;
    uint8_t *buf = NULL;
    struct vision_net *det = NULL;
    struct vision_net_info di;
    char err[96] = "";
    int k;
    int rc = 0;

    if (load_image(image, &f, &buf) != 0) {
        return EXIT_USAGE;
    }
    if (vision_net_open(&det, path, NULL, &di, err, sizeof(err)) != 0) {
        fprintf(stderr, "pos-vision: face model: %s\n", err);
        free(buf);
        return EXIT_NOMODEL;
    }
    if (vision_face_check(di.outputs, di.rank, (const uint32_t (*)[4])di.dims, di.in_w, di.in_h) != 0) {
        fprintf(stderr, "pos-vision: %s: not a face detector's outputs\n", path);
        vision_net_close(det);
        free(buf);
        return EXIT_NOMODEL;
    }
    printf("picture %ux%u; face detector %ux%u, %d outputs\n", f.width, f.height, di.in_w, di.in_h, di.outputs);
    for (k = 0; k < frames && rc == 0; k++) {
        struct vision_face faces[VISION_FACE_MAX];
        const float *out[VISION_FACE_OUTPUTS];
        size_t count[VISION_FACE_OUTPUTS];
        int64_t t0 = mono_ms();
        int64_t t1;
        uint32_t bad = 0;
        int pre = 0;
        int inf = 0;
        float ratio;
        int n;
        int i;

        if (vision_net_frame(det, &f) != 0 || vision_net_run(det, NULL, false, VISION_FACE_PAD, &pre, &inf) != 0) {
            rc = EXIT_LOST;
            break;
        }
        for (i = 0; i < VISION_FACE_OUTPUTS; i++) {
            out[i] = vision_net_output(det, i, &count[i]);
        }
        t1 = mono_ms();
        n = vision_face_decode(out, count, di.in_w, di.in_h, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, faces,
                               VISION_FACE_MAX, &bad);
        ratio = (float)di.in_w / (float)f.width < (float)di.in_h / (float)f.height ? (float)di.in_w / (float)f.width
                                                                                    : (float)di.in_h / (float)f.height;
        printf("frame %d: pre %d ms run %d ms decode %lld ms, %d faces, %u skipped\n", k + 1, pre, inf,
               (long long)(mono_ms() - t1), n, bad);
        for (i = 0; i < n; i++) {
            printf("  face %d: %d,%d %dx%d %u%%  eyes %d,%d %d,%d  nose %d,%d\n", i,
                   (int)((float)faces[i].box.x / ratio), (int)((float)faces[i].box.y / ratio),
                   (int)((float)faces[i].box.w / ratio), (int)((float)faces[i].box.h / ratio), faces[i].conf / 10,
                   (int)((float)faces[i].pt[0][0] / ratio), (int)((float)faces[i].pt[0][1] / ratio),
                   (int)((float)faces[i].pt[1][0] / ratio), (int)((float)faces[i].pt[1][1] / ratio),
                   (int)((float)faces[i].pt[2][0] / ratio), (int)((float)faces[i].pt[2][1] / ratio));
        }
        printf("  frame %d: %lld ms in all\n", k + 1, (long long)(mono_ms() - t0));
    }
    vision_net_close(det);
    free(buf);
    return rc;
}

/* RECOGNIZE's models on pictures: every face found, aligned and embedded,
 * and the score of every pair (the vendor's, per cent: 50 + 50 x cosine;
 * RECOGNIZE calls 75 and over the same person). For judging a model on
 * known faces before trusting it. */
#define EMBED_BENCH_FACES 24
static int run_embed_bench(const char *det_path, const char *emb_path, int nimg, char **images)
{
    static float emb[EMBED_BENCH_FACES][VISION_EMBED_MAX];
    int from[EMBED_BENCH_FACES][2];
    struct vision_net *det = NULL;
    struct vision_net *enet = NULL;
    struct vision_net_info di;
    struct vision_net_info ei;
    char err[96] = "";
    int total = 0;
    int k;
    int rc = 0;

    if (vision_net_open(&det, det_path, NULL, &di, err, sizeof(err)) != 0 ||
        vision_net_open(&enet, emb_path, NULL, &ei, err, sizeof(err)) != 0) {
        fprintf(stderr, "pos-vision: face models: %s\n", err);
        vision_net_close(det);
        return EXIT_NOMODEL;
    }
    if (vision_face_check(di.outputs, di.rank, (const uint32_t (*)[4])di.dims, di.in_w, di.in_h) != 0 ||
        ei.outputs != 1 || ei.in_w != VISION_EMBED_SIZE || ei.in_h != VISION_EMBED_SIZE ||
        ei.count[0] > VISION_EMBED_MAX) {
        fprintf(stderr, "pos-vision: not a face detector and a 112 x 112 face embedding model\n");
        vision_net_close(enet);
        vision_net_close(det);
        return EXIT_NOMODEL;
    }
    printf("embedding %ux%u -> %zu values\n", ei.in_w, ei.in_h, ei.count[0]);
    for (k = 0; k < nimg && rc == 0; k++) {
        struct pocketcam_frame f;
        uint8_t *buf = NULL;
        struct vision_face faces[VISION_FACE_MAX];
        const float *out[VISION_FACE_OUTPUTS];
        size_t count[VISION_FACE_OUTPUTS];
        float ratio;
        int n;
        int i;
        int j;

        if (load_image(images[k], &f, &buf) != 0) {
            rc = EXIT_USAGE;
            break;
        }
        if (vision_net_frame(det, &f) != 0 || vision_net_run(det, NULL, false, VISION_FACE_PAD, NULL, NULL) != 0 ||
            vision_net_frame(enet, &f) != 0) {
            free(buf);
            rc = EXIT_LOST;
            break;
        }
        for (i = 0; i < VISION_FACE_OUTPUTS; i++) {
            out[i] = vision_net_output(det, i, &count[i]);
        }
        n = vision_face_decode(out, count, di.in_w, di.in_h, VISION_FACE_CONF_PM, VISION_FACE_NMS_PM, faces,
                               VISION_FACE_MAX, NULL);
        ratio = (float)di.in_w / (float)f.width < (float)di.in_h / (float)f.height ? (float)di.in_w / (float)f.width
                                                                                    : (float)di.in_h / (float)f.height;
        printf("picture %d %s: %d faces\n", k, images[k], n);
        for (i = 0; i < n && total < EMBED_BENCH_FACES; i++) {
            float pts[5][2];
            float m[6];
            const float *raw;
            size_t c = 0;
            int pre = 0;
            int inf = 0;

            for (j = 0; j < 5; j++) {
                pts[j][0] = (float)faces[i].pt[j][0] / ratio;
                pts[j][1] = (float)faces[i].pt[j][1] / ratio;
            }
            if (vision_embed_align(pts, m) != 0 || vision_net_run_affine(enet, m, &pre, &inf) != 0) {
                printf("  face %d.%d: no alignment\n", k, i);
                continue;
            }
            raw = vision_net_output(enet, 0, &c);
            if (!raw || vision_embed_unit(raw, c, emb[total]) != 0) {
                printf("  face %d.%d: no embedding\n", k, i);
                continue;
            }
            printf("  face %d.%d = #%d: %d,%d %dx%d %u%%, align %d ms embed %d ms\n", k, i, total,
                   (int)((float)faces[i].box.x / ratio), (int)((float)faces[i].box.y / ratio),
                   (int)((float)faces[i].box.w / ratio), (int)((float)faces[i].box.h / ratio), faces[i].conf / 10,
                   pre, inf);
            from[total][0] = k;
            from[total][1] = i;
            total++;
        }
        free(buf);
    }
    printf("scores (per cent; 75 and over: the same person)\n    ");
    for (k = 0; k < total; k++) {
        printf(" %3d", k);
    }
    printf("\n");
    for (k = 0; k < total; k++) {
        int j;

        printf("%3d:", k);
        for (j = 0; j < total; j++) {
            printf(" %3u", (unsigned)((vision_embed_score(emb[k], emb[j], ei.count[0]) + 5) / 10));
        }
        printf("   (%d.%d)\n", from[k][0], from[k][1]);
    }
    vision_net_close(enet);
    vision_net_close(det);
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: pos-vision session|probe [--backend NAME] [--fake SCRIPT] [--config CFG]\n"
            "                                [--model FILE] [--kpu SCRIPT]\n"
            "       pos-vision bench [N] [--backend NAME] [--config CFG] [--model FILE]\n"
            "                        [--turn 0|90|180|270] [--save FILE.ppm [--save-every N]]\n"
            "                        [--image FILE.ppm | --images A.ppm,B.ppm,...]\n"
            "                        [--range near|normal|far] [--tracks]\n"
            "       pos-vision describe MODEL.kmodel\n"
            "       pos-vision text IMAGE.ppm DET REC DICT [N]\n"
            "       pos-vision face IMAGE.ppm MODEL [N]\n"
            "       pos-vision embed FACE_DET FACE_EMBED IMAGE.ppm...\n");
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
    struct bench_opts bo = { 100, NULL, 0, NULL, NULL, 0, VISION_RANGE_NORMAL, false };
    bool bench = cmd && strcmp(cmd, "bench") == 0;
    int i;

    if (!cmd) {
        usage();
        return EXIT_USAGE;
    }
    if (strcmp(cmd, "text") == 0) {
        /* pos-vision text IMAGE.ppm DET REC DICT [FRAMES] */
        if (argc < 6 || argc > 7) {
            usage();
            return EXIT_USAGE;
        }
        return run_text_bench(argv[2], argv[3], argv[4], argv[5], argc == 7 ? atoi(argv[6]) : 1);
    }
    if (strcmp(cmd, "face") == 0) {
        /* pos-vision face IMAGE.ppm MODEL [FRAMES] */
        if (argc < 4 || argc > 5) {
            usage();
            return EXIT_USAGE;
        }
        return run_face_bench(argv[2], argv[3], argc == 5 ? atoi(argv[4]) : 1);
    }
    if (strcmp(cmd, "embed") == 0) {
        /* pos-vision embed DET EMBED IMAGE.ppm... */
        if (argc < 5) {
            usage();
            return EXIT_USAGE;
        }
        return run_embed_bench(argv[2], argv[3], argc - 4, argv + 4);
    }
    if (strcmp(cmd, "describe") == 0) {
        /* A model's inputs and outputs, for bringing one up on the bench. */
        char buf[4096];
        int r;

        if (argc != 3) {
            usage();
            return EXIT_USAGE;
        }
        r = vision_kpu_describe(argv[2], buf, sizeof(buf));
        fputs(buf, stdout);
        if (r != 0) {
            fprintf(stderr, "pos-vision: %s: %s\n", argv[2], r == -ENOENT ? "no such file" :
                    r == -ENOTSUP ? "no model runtime in this build" : "not a model this runtime loads");
            return EXIT_NOMODEL;
        }
        return 0;
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
        } else if (bench && strcmp(argv[i], "--images") == 0 && i + 1 < argc) {
            bo.images = argv[++i];
        } else if (bench && strcmp(argv[i], "--tracks") == 0) {
            bo.tracks = true;
        } else if (bench && strcmp(argv[i], "--range") == 0 && i + 1 < argc) {
            int rg = vision_range_parse(argv[++i]);

            if (rg < 0) {
                usage();
                return EXIT_USAGE;
            }
            bo.range = (enum vision_range)rg;
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
