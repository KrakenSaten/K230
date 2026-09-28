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
 *       Stream N frames (default 100) through the detector and print the
 *       timings and what was found, for the unit A gate.
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
#include "pocketvision/vision_track.h"

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
/* Frames in a row whose tensor is not a tensor: the model is not working. */
#define VISION_BAD_LIMIT 10

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
    struct vision_tracker tracker;
    struct vision_line line;      /* in view pixels */
    int32_t line_pm[4];           /* as asked for, per-mille; -1: none */
    struct vision_counts counts;
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

/* The line is chosen on the picture (per-mille of the view) and counted
 * among the tracks, which live in frame pixels: its two ends go back
 * through the picture's geometry. A mirrored picture swaps the ends, so
 * "left of the line's direction" on the screen stays what it was. */
static void place_line(struct session *s)
{
    struct vision_view v = view_of(s);
    int32_t vx0;
    int32_t vy0;
    int32_t vx1;
    int32_t vy1;
    int32_t a[2];
    int32_t b[2];

    if (s->line_pm[0] < 0 || s->view_w == 0 || s->view_h == 0) {
        s->line.enabled = false;
        return;
    }
    vx0 = (int32_t)(((int64_t)s->line_pm[0] * (s->view_w - 1)) / 1000);
    vy0 = (int32_t)(((int64_t)s->line_pm[1] * (s->view_h - 1)) / 1000);
    vx1 = (int32_t)(((int64_t)s->line_pm[2] * (s->view_w - 1)) / 1000);
    vy1 = (int32_t)(((int64_t)s->line_pm[3] * (s->view_h - 1)) / 1000);
    if (vision_unmap_point(&v, vx0, vy0, &a[0], &a[1]) != 0 ||
        vision_unmap_point(&v, vx1, vy1, &b[0], &b[1]) != 0) {
        s->line.enabled = false;
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
    s->line.x0 = a[0];
    s->line.y0 = a[1];
    s->line.x1 = b[0];
    s->line.y1 = b[1];
    s->line.enabled = true;
}

static void say_counts(struct session *s)
{
    say("count %u %u", s->counts.ab, s->counts.ba);
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
        n = snprintf(items + off, sizeof(items) - off, " %u:%u:%u:%d:%d:%d:%d",
                     t->confirmed ? t->id : 0u, t->cls, t->conf, b.x, b.y, b.w, b.h);
        if (n < 0 || off + (size_t)n >= sizeof(items) - 1) {
            break;
        }
        off += (size_t)n;
        shown++;
    }
    snprintf(line, sizeof(line), "det %u %d%s", seq, shown, items);
    say("%s", line);
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
    struct vision_decode_params p = {
        .in_w = s->model.in_w,
        .in_h = s->model.in_h,
        .classes = s->model.classes,
        .frame_w = f->width,
        .frame_h = f->height,
        .conf_min = VISION_CONF_MIN,
    };

    r = vision_kpu_infer(s->kpu, f, &out, &count, dims, &pre_ms, &infer_ms);
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
    n = vision_nms(s->cands, n, VISION_NMS_IOU, s->dets, VISION_MAX_DETECTIONS);
    vision_tracker_update(&s->tracker, s->dets, n);
    if (s->line.enabled && vision_line_count(&s->line, &s->tracker, &s->counts) > 0) {
        say_counts(s);
    }
    s->last_pre_ms = pre_ms;
    s->last_infer_ms = infer_ms;
    s->last_post_ms = (int)(mono_ms() - t0);
    s->inferred++;
    say_tracks(s, f->seq);
    (void)now;
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
        place_line(s);
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
        char *a = strtok_r(NULL, " ", &save);
        char *b = strtok_r(NULL, " ", &save);
        char *c = strtok_r(NULL, " ", &save);
        char *d = strtok_r(NULL, " ", &save);
        int v[4];
        int i;

        if (a && strcmp(a, "off") == 0) {
            s->line_pm[0] = -1;
            place_line(s);
            return;
        }
        if (!a || !b || !c || !d || sscanf(a, "%d", &v[0]) != 1 || sscanf(b, "%d", &v[1]) != 1 ||
            sscanf(c, "%d", &v[2]) != 1 || sscanf(d, "%d", &v[3]) != 1) {
            return;
        }
        for (i = 0; i < 4; i++) {
            if (v[i] < 0 || v[i] > 1000) {
                return;
            }
            s->line_pm[i] = v[i];
        }
        place_line(s);
        /* A new line starts a new count; the tracks learn their side afresh. */
        for (i = 0; i < s->tracker.count; i++) {
            s->tracker.t[i].side = 0;
        }
        memset(&s->counts, 0, sizeof(s->counts));
        say_counts(s);
    } else if (strcmp(w, "reset") == 0) {
        memset(&s->counts, 0, sizeof(s->counts));
        vision_tracker_clear(&s->tracker);
        say_counts(s);
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
     * the boxes said for this frame are drawn on this frame. */
    detect(s, &f, now);
    if (!s->quit && s->view_w && now - s->last_sent_ms >= POCKETCAM_PREVIEW_MIN_INTERVAL_MS) {
        int slot = free_preview_slot(s);

        if (slot >= 0 &&
            pocketcam_to_rgb565(&f, pocketcam_view_rotation(s->info.mount_rotation, s->display_rotation),
                                s->info.mount_mirror, POCKETCAM_FIT_COVER, slot_pixels(s, slot),
                                s->view_w, s->view_h, s->view_w) == 0) {
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

    if (!s) {
        say("error memory out of memory");
        return EXIT_USAGE;
    }
    s->line_pm[0] = -1;
    vision_tracker_init(&s->tracker);
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
    say("ready %s %u %u %d %s %u %u %u", s->info.name, s->info.preview_w, s->info.preview_h,
        s->info.simulated ? 1 : 0, s->model.model, s->model.in_w, s->model.in_h, s->model.classes);
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

/* Frames through the whole pipeline, timings printed, nothing drawn: the
 * measurement the gate reads. */
static int run_bench(const char *backend, const char *config, const char *model,
                     const char *kpu_script, int frames)
{
    struct pocketcam_backend cam;
    struct pocketcam_info info;
    struct vision_kpu *kpu = NULL;
    struct vision_kpu_info mi;
    struct vision_det *cands = calloc(VISION_MAX_CANDIDATES, sizeof(*cands));
    struct vision_det dets[VISION_MAX_DETECTIONS];
    struct vision_tracker tr;
    int64_t pre_sum = 0;
    int64_t infer_sum = 0;
    int64_t post_sum = 0;
    int64_t t_start;
    int done = 0;
    int r;

    if (!cands) {
        return EXIT_USAGE;
    }
    if (open_camera(&cam, &info, backend, config, false) != 0) {
        free(cands);
        return EXIT_NOCAMERA;
    }
    if (open_model(&kpu, &mi, model, kpu_script, false) != 0) {
        pocketcam_close(&cam);
        free(cands);
        return EXIT_NOMODEL;
    }
    printf("camera %s %ux%u, model %s %ux%u classes %u rows %u, %d frames\n", info.name,
           info.preview_w, info.preview_h, mi.model, mi.in_w, mi.in_h, mi.classes, mi.rows, frames);
    vision_tracker_init(&tr);
    r = pocketcam_start(&cam);
    t_start = mono_ms();
    while (r == 0 && done < frames && !stop_requested()) {
        struct pocketcam_frame f;
        const float *out;
        size_t count;
        uint32_t dims[3];
        int pre_ms;
        int infer_ms;
        int64_t t0;
        int n;
        uint32_t bad;
        struct vision_decode_params p = {
            .in_w = mi.in_w, .in_h = mi.in_h, .classes = mi.classes,
            .frame_w = info.preview_w, .frame_h = info.preview_h, .conf_min = VISION_CONF_MIN,
        };

        r = pocketcam_next(&cam, 1000, &f);
        if (r == -ETIMEDOUT) {
            r = 0;
            continue;
        }
        if (r != 0) {
            break;
        }
        r = vision_kpu_infer(kpu, &f, &out, &count, dims, &pre_ms, &infer_ms);
        if (r != 0) {
            pocketcam_release(&cam, &f);
            fprintf(stderr, "pos-vision: infer failed (%d)\n", r);
            break;
        }
        t0 = mono_ms();
        n = vision_decode(out, count, dims, &p, cands, VISION_MAX_CANDIDATES, &bad);
        if (n >= 0) {
            int i;

            n = vision_nms(cands, n, VISION_NMS_IOU, dets, VISION_MAX_DETECTIONS);
            vision_tracker_update(&tr, dets, n);
            if (done == 0 || done == frames - 1) {
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
        } else {
            printf("frame %u: malformed output (%d)\n", f.seq, n);
        }
        pocketcam_release(&cam, &f);
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
    }
    pocketcam_close(&cam);
    vision_kpu_close(kpu);
    free(cands);
    return r == 0 ? 0 : EXIT_LOST;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: pos-vision session|probe [--backend NAME] [--fake SCRIPT] [--config CFG]\n"
            "                                [--model FILE] [--kpu SCRIPT]\n"
            "       pos-vision bench [N] [--backend NAME] [--config CFG] [--model FILE]\n");
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
    int frames = 100;
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
        } else if (strcmp(cmd, "bench") == 0 && i == 2 && atoi(argv[i]) > 0) {
            frames = atoi(argv[i]);
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
    if (strcmp(cmd, "bench") == 0) {
        return run_bench(backend, config, model, kpu_script, frames);
    }
    usage();
    return EXIT_USAGE;
}
