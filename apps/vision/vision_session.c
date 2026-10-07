/*
 * Vision's helper client. See vision_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "vision_session.h"
#include "vision_settings.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CAMERA_BACKEND_DEFAULT
#define CAMERA_BACKEND_DEFAULT "v4l2"
#endif
#define CHILD_FD_SCAN_MAX 64
/* A speed a det or traffic line may carry: anything above is not a measurement. */
#define VISION_KMH10_MAX 100000

void vision_session_init(struct vision_session *s)
{
    memset(s, 0, sizeof(*s));
    s->pid = -1;
    s->fd = -1;
    s->shm_fd = -1;
    s->frame_slot = -1;
}

const char *vision_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_VISION_HELPER");

    return p && *p ? p : "/usr/bin/pos-vision";
}

const char *vision_session_backend(void)
{
    const char *p = getenv("POCKETOS_CAMERA_BACKEND");

    return p && *p ? p : CAMERA_BACKEND_DEFAULT;
}

bool vision_session_active(const struct vision_session *s)
{
    return s->running;
}

/* ---- the queue ------------------------------------------------------------------ */

static void push(struct vision_session *s, const struct vision_event *ev)
{
    if (s->q_count >= VISION_EVENT_QUEUE) {
        s->dropped++;
        return;
    }
    s->queue[(s->q_head + s->q_count) % VISION_EVENT_QUEUE] = *ev;
    s->q_count++;
}

static int pop(struct vision_session *s, struct vision_event *ev)
{
    if (s->q_count == 0) {
        return 0;
    }
    *ev = s->queue[s->q_head];
    s->q_head = (s->q_head + 1) % VISION_EVENT_QUEUE;
    s->q_count--;
    return 1;
}

/* ---- lines -------------------------------------------------------------------- */

static int send_line(struct vision_session *s, const char *fmt, ...)
{
    char line[VISION_LINE_MAX];
    va_list ap;
    int n;
    size_t off = 0;

    if (!s->running || s->fd < 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n > sizeof(line) - 2) {
        return -1;
    }
    line[n++] = '\n';
    while (off < (size_t)n) {
        ssize_t w = send(s->fd, line + off, (size_t)n - off, MSG_NOSIGNAL | MSG_DONTWAIT);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* A full socket is a helper that stopped reading: the deadline
             * on its next reply catches that. */
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static bool word_ok(const char *w, size_t max)
{
    size_t i;

    for (i = 0; w[i]; i++) {
        if (i >= max || w[i] == ' ' || (unsigned char)w[i] < 0x20) {
            return false;
        }
    }
    return i > 0;
}

/* The rest of a line after n words, as text. */
static const char *rest(const char *line, int words)
{
    const char *p = line;

    while (words-- > 0) {
        while (*p && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
    }
    return p;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* A percent-encoded word (up to a space or the end) into out; the input
 * moved past it. 0, or -1 for a broken escape, a control byte, or text too
 * long for out. */
static int unpct(const char **pp, char *out, size_t len)
{
    const char *p = *pp;
    size_t o = 0;

    while (*p && *p != ' ') {
        int c;

        if (*p == '%') {
            int h = hexval(p[1]);
            int l = h >= 0 ? hexval(p[2]) : -1;

            if (h < 0 || l < 0) {
                return -1;
            }
            c = h * 16 + l;
            p += 3;
        } else {
            c = (unsigned char)*p++;
        }
        if (c < 0x20 || c == 0x7f || o + 1 >= len) {
            return -1;
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
    *pp = p;
    return 0;
}

static int parse_text(struct vision_session *s, const char *line)
{
    struct vision_text_report t;
    unsigned seq;
    int n;
    int used = 0;
    const char *p;
    int i;

    memset(&t, 0, sizeof(t));
    if (sscanf(line, "text %u %d%n", &seq, &n, &used) != 2 || n < 0 || n > VISION_TEXT_LINES) {
        return 0;
    }
    p = line + used;
    for (i = 0; i < n; i++) {
        struct vision_text_line *l = &t.line[i];
        unsigned conf;
        int u = 0;

        if (sscanf(p, " %d:%d:%d:%d:%u:%n", &l->x, &l->y, &l->w, &l->h, &conf, &u) != 5 || u == 0 ||
            conf > VISION_CONF_SCALE || l->w <= 0 || l->h <= 0 || l->x < 0 || l->y < 0 ||
            l->x + l->w > (int)POCKETCAM_VIEW_MAX_W || l->y + l->h > (int)POCKETCAM_VIEW_MAX_H) {
            return 0;
        }
        l->conf = (uint16_t)conf;
        p += u;
        if (unpct(&p, l->text, sizeof(l->text)) != 0) {
            return 0;
        }
    }
    while (*p == ' ') {
        p++;
    }
    if (*p) {
        return 0;
    }
    t.seq = seq;
    t.n = n;
    if (s) {
        s->text = t;
    }
    return 1;
}

static int parse_det(struct vision_session *s, const char *line)
{
    unsigned seq;
    int n;
    int consumed = 0;
    const char *p;
    int i;
    struct vision_shown shown[VISION_MAX_SHOWN];

    if (sscanf(line, "det %u %d%n", &seq, &n, &consumed) != 2 || n < 0 || n > VISION_MAX_SHOWN) {
        return 0;
    }
    p = line + consumed;
    for (i = 0; i < n; i++) {
        unsigned id;
        unsigned cls;
        unsigned conf;
        int x;
        int y;
        int w;
        int h;
        unsigned dir;
        unsigned kmh10;
        int used = 0;

        if (sscanf(p, " %u:%u:%u:%d:%d:%d:%d:%u:%u%n", &id, &cls, &conf, &x, &y, &w, &h, &dir, &kmh10,
                   &used) != 9 ||
            conf > VISION_CONF_SCALE || w <= 0 || h <= 0 || x < 0 || y < 0 ||
            x + w > (int)POCKETCAM_VIEW_MAX_W || y + h > (int)POCKETCAM_VIEW_MAX_H ||
            dir > VISION_DIR_DOWN || kmh10 > VISION_KMH10_MAX) {
            return 0;
        }
        shown[i].id = id;
        shown[i].cls = (uint16_t)cls;
        shown[i].conf = (uint16_t)conf;
        shown[i].x = x;
        shown[i].y = y;
        shown[i].w = w;
        shown[i].h = h;
        shown[i].dir = (uint8_t)dir;
        shown[i].kmh10 = kmh10;
        p += used;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p) {
        return 0; /* more items than it said, or junk */
    }
    if (s) {
        memcpy(s->shown, shown, (size_t)n * sizeof(shown[0]));
        s->shown_count = n;
        s->shown_seq = seq;
    }
    return 1;
}

int vision_session_parse_line(struct vision_session *s, const char *line, struct vision_event *ev)
{
    char w1[VISION_NAME_MAX];
    char w2[VISION_NAME_MAX];
    unsigned a;
    unsigned b;
    unsigned c;
    unsigned d;
    int sim;

    memset(ev, 0, sizeof(*ev));
    if (strncmp(line, "ready ", 6) == 0) {
        unsigned in_w;
        unsigned in_h;

        if (sscanf(line, "ready %47s %u %u %d %47s %u %u %u", w1, &a, &b, &sim, w2, &in_w, &in_h,
                   &c) != 8 ||
            !word_ok(w1, VISION_NAME_MAX - 1) || !word_ok(w2, VISION_NAME_MAX - 1) ||
            (sim != 0 && sim != 1) || a == 0 || b == 0 || a > VISION_MAX_COORD ||
            b > VISION_MAX_COORD || c > VISION_MAX_CLASSES ||
            /* No classes only from a helper without a detector, which
             * names its model "none". */
            (c == 0 && strcmp(w2, "none") != 0)) {
            return 0;
        }
        ev->kind = VISION_EV_READY;
        snprintf(ev->text, sizeof(ev->text), "%s", w1);
        snprintf(ev->name, sizeof(ev->name), "%s", w2);
        ev->w = a;
        ev->h = b;
        ev->simulated = sim == 1;
        ev->value = (int)c;
        return 1;
    }
    if (strncmp(line, "caps", 4) == 0 && (line[4] == ' ' || line[4] == '\0')) {
        /* The modes this helper can run, by their words; a word this build
         * does not know is a mode of a newer helper, and is skipped. */
        const char *p = line + 4;
        uint32_t mask = 0;

        while (*p) {
            char word[16];
            size_t n;
            int m;

            while (*p == ' ') {
                p++;
            }
            n = strcspn(p, " ");
            if (n == 0) {
                break;
            }
            if (n < sizeof(word)) {
                memcpy(word, p, n);
                word[n] = '\0';
                m = vision_mode_parse(word);
                if (m >= 0) {
                    mask |= 1u << m;
                }
            }
            p += n;
        }
        ev->kind = VISION_EV_CAPS;
        ev->value = (int)mask;
        return 1;
    }
    if (strncmp(line, "frame ", 6) == 0) {
        if (sscanf(line, "frame %u %u %u %u", &a, &b, &c, &d) != 4 || a >= POCKETCAM_PREVIEW_SLOTS ||
            c == 0 || d == 0 || c > POCKETCAM_VIEW_MAX_W || d > POCKETCAM_VIEW_MAX_H) {
            return 0;
        }
        ev->kind = VISION_EV_FRAME;
        ev->value = (int)a;
        ev->w = c;
        ev->h = d;
        return 1;
    }
    if (strncmp(line, "det ", 4) == 0) {
        if (!parse_det(s, line)) {
            return 0;
        }
        ev->kind = VISION_EV_DET;
        sscanf(line, "det %u", &a);
        ev->value = (int)a;
        return 1;
    }
    if (strncmp(line, "count ", 6) == 0) {
        if (sscanf(line, "count %u %u", &a, &b) != 2) {
            return 0;
        }
        ev->kind = VISION_EV_COUNT;
        if (s) {
            s->count_ab = a;
            s->count_ba = b;
        }
        return 1;
    }
    if (strncmp(line, "traffic ", 8) == 0) {
        struct vision_traffic_report t;
        int consumed = 0;
        const char *p;
        int i;

        memset(&t, 0, sizeof(t));
        if (sscanf(line, "traffic %u %u %u %u %u %u %u %u%n", &t.total_ab, &t.total_ba, &t.cur_kmh10,
                   &t.last_kmh10, &t.max_kmh10, &t.mean_kmh10, &t.n, &t.rejected, &consumed) != 8 ||
            t.cur_kmh10 > VISION_KMH10_MAX || t.last_kmh10 > VISION_KMH10_MAX ||
            t.max_kmh10 > VISION_KMH10_MAX || t.mean_kmh10 > VISION_KMH10_MAX) {
            return 0;
        }
        p = line + consumed;
        for (i = 0; i < VISION_PROTO_TRAFFIC_CLASSES; i++) {
            int used = 0;

            if (sscanf(p, " %u:%u%n", &t.cls_ab[i], &t.cls_ba[i], &used) != 2) {
                return 0;
            }
            p += used;
        }
        while (*p == ' ') {
            p++;
        }
        if (*p) {
            return 0;
        }
        ev->kind = VISION_EV_TRAFFIC;
        if (s) {
            s->traffic = t;
        }
        return 1;
    }
    if (strncmp(line, "text ", 5) == 0) {
        if (!parse_text(s, line)) {
            return 0;
        }
        ev->kind = VISION_EV_TEXT;
        return 1;
    }
    if (strncmp(line, "facefail", 8) == 0) {
        ev->kind = VISION_EV_FACEFAIL;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "recogfail", 9) == 0) {
        ev->kind = VISION_EV_RECOGFAIL;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "enrolfail", 9) == 0) {
        ev->kind = VISION_EV_ENROLFAIL;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "enrol ", 6) == 0) {
        unsigned k;
        unsigned n;
        char tail;

        if (sscanf(line, "enrol %u %u%c", &k, &n, &tail) != 2 || n == 0 || n > 100 || k > n) {
            return 0;
        }
        ev->kind = VISION_EV_ENROL;
        ev->w = k;
        ev->h = n;
        return 1;
    }
    if (strncmp(line, "owner ", 6) == 0) {
        int have;
        unsigned views;
        char tail;

        if (sscanf(line, "owner %d %u%c", &have, &views, &tail) != 2 || (have != 0 && have != 1) || views > 1000) {
            return 0;
        }
        ev->kind = VISION_EV_OWNER;
        ev->value = have;
        ev->w = views;
        return 1;
    }
    if (strncmp(line, "who ", 4) == 0) {
        struct vision_who_report t;
        const char *p;
        int used = 0;
        int i;

        memset(&t, 0, sizeof(t));
        if (sscanf(line, "who %u %d %d%n", &t.seq, &t.faces, &t.n, &used) != 3 || used == 0 || t.faces < 0 ||
            t.faces > 1000 || t.n < 0 || t.n > VISION_WHO_MAX) {
            return 0;
        }
        p = line + used;
        for (i = 0; i < t.n; i++) {
            unsigned id;
            unsigned score;
            int flag;
            int k = 0;

            if (sscanf(p, " %u:%u:%d%n", &id, &score, &flag, &k) != 3 || k == 0 || id == 0 || score > 1000 ||
                (flag != 0 && flag != 1)) {
                return 0;
            }
            t.t[i].id = id;
            t.t[i].score = (uint16_t)score;
            t.t[i].owner = flag == 1;
            p += k;
        }
        if (*p) {
            return 0;
        }
        ev->kind = VISION_EV_WHO;
        ev->value = (int)t.seq;
        if (s) {
            s->who = t;
        }
        return 1;
    }
    if (strncmp(line, "readfail", 8) == 0) {
        ev->kind = VISION_EV_READFAIL;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "recent ", 7) == 0) {
        struct vision_recent_report t;
        int sat;
        int used = 0;

        memset(&t, 0, sizeof(t));
        if (sscanf(line, "recent %u %u %u %u %u %u %u %u %u %u %u %u %d%n", &t.window_s, &t.crossed, &t.ab, &t.ba,
                   &t.cls[0], &t.cls[1], &t.cls[2], &t.cls[3], &t.cls[4], &t.cls[5], &t.speeds, &t.mean_kmh10, &sat,
                   &used) != 13 ||
            line[used] != '\0' || (sat != 0 && sat != 1) || t.window_s == 0 || t.ab + t.ba != t.crossed ||
            t.mean_kmh10 > VISION_KMH10_MAX || (t.speeds == 0 && t.mean_kmh10 != 0)) {
            return 0;
        }
        t.saturated = sat == 1;
        ev->kind = VISION_EV_RECENT;
        if (s) {
            s->recent = t;
        }
        return 1;
    }
    if (strncmp(line, "color ", 6) == 0) {
        unsigned r;
        unsigned g;
        unsigned bl;
        unsigned pm;
        int cx;
        int cy;

        if (sscanf(line, "color %u %u %u %u %d %d", &r, &g, &bl, &pm, &cx, &cy) != 6 || r > 255 || g > 255 ||
            bl > 255 || pm > 1000 || cx < -1 || cy < -1 || cx >= (int)POCKETCAM_VIEW_MAX_W ||
            cy >= (int)POCKETCAM_VIEW_MAX_H) {
            return 0;
        }
        ev->kind = VISION_EV_COLOR;
        if (s) {
            s->pixels.color.r = (uint8_t)r;
            s->pixels.color.g = (uint8_t)g;
            s->pixels.color.b = (uint8_t)bl;
            s->pixels.color.matched_pm = pm;
            s->pixels.color.cx = cx;
            s->pixels.color.cy = cy;
        }
        return 1;
    }
    if (strncmp(line, "edge ", 5) == 0) {
        if (sscanf(line, "edge %u", &a) != 1 || a > 1000) {
            return 0;
        }
        ev->kind = VISION_EV_EDGE;
        if (s) {
            s->pixels.edge_pm = a;
        }
        return 1;
    }
    if (strncmp(line, "trace ", 6) == 0) {
        int found;
        int off;
        int slope;
        unsigned rows;

        if (sscanf(line, "trace %d %d %d %u", &found, &off, &slope, &rows) != 4 || (found != 0 && found != 1) ||
            off < -1000 || off > 1000 || slope < -100000 || slope > 100000 || rows > 4096) {
            return 0;
        }
        ev->kind = VISION_EV_TRACE;
        if (s) {
            s->pixels.trace.found = found == 1;
            s->pixels.trace.offset_pm = off;
            s->pixels.trace.slope_pm = slope;
            s->pixels.trace.rows = rows;
        }
        return 1;
    }
    if (strncmp(line, "stats ", 6) == 0) {
        struct vision_stats st;
        long rss;

        if (sscanf(line, "stats %u %d %d %d %d %ld %u %u", &st.fps_x10, &st.infer_ms, &st.pre_ms,
                   &st.post_ms, &st.cpu_pct, &rss, &st.bad, &st.dropped) != 8) {
            return 0;
        }
        st.rss_kb = rss;
        ev->kind = VISION_EV_STATS;
        if (s) {
            s->stats = st;
        }
        return 1;
    }
    if (strncmp(line, "malformed ", 10) == 0) {
        if (sscanf(line, "malformed %u", &a) != 1) {
            return 0;
        }
        ev->kind = VISION_EV_MALFORMED;
        ev->value = (int)a;
        return 1;
    }
    if (strncmp(line, "stall ", 6) == 0) {
        if (sscanf(line, "stall %u", &a) != 1) {
            return 0;
        }
        ev->kind = VISION_EV_STALL;
        ev->value = (int)a;
        return 1;
    }
    if (strcmp(line, "stopped") == 0) {
        ev->kind = VISION_EV_STOPPED;
        return 1;
    }
    if (strncmp(line, "nodevice", 8) == 0) {
        ev->kind = VISION_EV_NODEVICE;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "nomodel", 7) == 0) {
        ev->kind = VISION_EV_NOMODEL;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "error", 5) == 0) {
        ev->kind = VISION_EV_ERROR;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    if (strncmp(line, "lost", 4) == 0) {
        ev->kind = VISION_EV_LOST;
        snprintf(ev->text, sizeof(ev->text), "%s", rest(line, 1));
        return 1;
    }
    return 0;
}

/* ---- starting ------------------------------------------------------------------- */

static void say_err(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

static int make_shm(struct vision_session *s)
{
    struct pocketcam_shm_header h = {
        .magic = POCKETCAM_SHM_MAGIC,
        .version = POCKETCAM_PROTO_VERSION,
        .slots = POCKETCAM_SLOTS,
        .slot_bytes = POCKETCAM_SLOT_BYTES,
        .max_w = POCKETCAM_VIEW_MAX_W,
        .max_h = POCKETCAM_VIEW_MAX_H,
    };
    void *map;

    s->shm_fd = memfd_create("doors-vision", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (s->shm_fd < 0) {
        return -1;
    }
    if (ftruncate(s->shm_fd, POCKETCAM_SHM_BYTES) != 0 ||
        pwrite(s->shm_fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h) ||
        fcntl(s->shm_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        return -1;
    }
    map = mmap(NULL, POCKETCAM_SHM_BYTES, PROT_READ, MAP_SHARED, s->shm_fd, 0);
    if (map == MAP_FAILED) {
        return -1;
    }
    s->shm = map;
    return 0;
}

static void drop_shm(struct vision_session *s)
{
    if (s->shm) {
        munmap((void *)s->shm, POCKETCAM_SHM_BYTES);
        s->shm = NULL;
    }
    if (s->shm_fd >= 0) {
        close(s->shm_fd);
        s->shm_fd = -1;
    }
}

int vision_session_start(struct vision_session *s, const struct vision_session_config *cfg,
                         int64_t now_ms, char *err, size_t errlen)
{
    const char *helper = cfg && cfg->helper ? cfg->helper : vision_session_helper_path();
    const char *backend = cfg && cfg->backend ? cfg->backend : vision_session_backend();
    char *argv[14];
    int argc = 0;
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (s->running) {
        if (err && errlen) {
            snprintf(err, errlen, "the camera is already running");
        }
        return -1;
    }
    vision_session_init(s);
    if (strlen(helper) >= VISION_HELPER_PATH_MAX || strlen(backend) >= VISION_ARG_MAX ||
        (cfg && cfg->fake && strlen(cfg->fake) >= VISION_ARG_MAX) ||
        (cfg && cfg->config && strlen(cfg->config) >= VISION_ARG_MAX) ||
        (cfg && cfg->model && strlen(cfg->model) >= VISION_ARG_MAX) ||
        (cfg && cfg->kpu && strlen(cfg->kpu) >= VISION_ARG_MAX)) {
        if (err && errlen) {
            snprintf(err, errlen, "vision helper arguments too long");
        }
        return -1;
    }
    argv[argc++] = (char *)helper;
    argv[argc++] = "session";
    argv[argc++] = "--backend";
    argv[argc++] = (char *)backend;
    if (cfg && cfg->fake && *cfg->fake) {
        argv[argc++] = "--fake";
        argv[argc++] = (char *)cfg->fake;
    }
    if (cfg && cfg->config && *cfg->config) {
        argv[argc++] = "--config";
        argv[argc++] = (char *)cfg->config;
    }
    if (cfg && cfg->model && *cfg->model) {
        argv[argc++] = "--model";
        argv[argc++] = (char *)cfg->model;
    }
    if (cfg && cfg->kpu && *cfg->kpu) {
        argv[argc++] = "--kpu";
        argv[argc++] = (char *)cfg->kpu;
    }
    argv[argc] = NULL;

    if (make_shm(s) != 0) {
        say_err(err, errlen, "shared memory");
        drop_shm(s);
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        say_err(err, errlen, "socketpair");
        drop_shm(s);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        say_err(err, errlen, "fork");
        close(sv[0]);
        close(sv[1]);
        drop_shm(s);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int null;
        int fd;

        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(sv[1], 0) < 0 || dup2(sv[1], 1) < 0) {
            _exit(127);
        }
        if (s->shm_fd == POCKETCAM_SHM_FD) {
            fcntl(POCKETCAM_SHM_FD, F_SETFD, 0);
        } else if (dup2(s->shm_fd, POCKETCAM_SHM_FD) < 0) {
            _exit(127);
        }
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 2);
        }
        for (fd = POCKETCAM_SHM_FD + 1; fd < CHILD_FD_SCAN_MAX; fd++) {
            close(fd);
        }
        execv(helper, argv);
        {
            char line[160];
            int n = snprintf(line, sizeof(line), "error exec %s: %s\n", helper, strerror(errno));

            if (n > 0 && write(1, line, (size_t)n) < 0) {
                /* nothing more can be said */
            }
        }
        _exit(127);
    }
    close(sv[1]);
    s->fd = sv[0];
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    s->pid = pid;
    s->running = true;
    s->hello_by = now_ms + VISION_HELLO_MS;
    s->open_by = now_ms + VISION_OPEN_MS;
    return 0;
}

/* ---- running ------------------------------------------------------------------ */

static void release_slot(struct vision_session *s, int slot)
{
    if (slot >= 0) {
        send_line(s, "release %d", slot);
    }
}

static void kill_helper(struct vision_session *s, enum vision_exit why)
{
    if (s->running && s->pid > 0 && !s->killed) {
        kill(s->pid, SIGKILL);
        s->killed = true;
        s->exit_reason = why;
    }
}

/* A deadline from now, but never before a model being opened is due. */
static int64_t due(const struct vision_session *s, int64_t now, int ms)
{
    int64_t d = now + ms;

    return s->loading && s->load_by > d ? s->load_by : d;
}

/* `loading`: nothing more comes until the model is open, so every deadline
 * that is running is moved to the load's. */
static void loading(struct vision_session *s, int64_t now)
{
    s->loading = true;
    s->load_by = now + VISION_LOAD_MS;
    if (s->silence_by && s->silence_by < s->load_by) {
        s->silence_by = s->load_by;
    }
    if (s->reply_by && s->reply_by < s->load_by) {
        s->reply_by = s->load_by;
    }
    if (s->open_by && s->open_by < s->load_by) {
        s->open_by = s->load_by;
    }
}

static void handle(struct vision_session *s, struct vision_event *ev, int64_t now)
{
    if (s->streaming) {
        s->silence_by = now + VISION_SILENCE_MS;
    }
    switch (ev->kind) {
    case VISION_EV_READY:
    case VISION_EV_NODEVICE:
    case VISION_EV_NOMODEL:
    case VISION_EV_ERROR:
        s->open_by = 0;
        break;
    case VISION_EV_FRAME:
        if (!s->streaming) {
            release_slot(s, ev->value);
            return;
        }
        if (s->frame_slot >= 0 && s->frame_slot != ev->value) {
            release_slot(s, s->frame_slot);
        }
        s->frame_slot = ev->value;
        s->frame_w = ev->w;
        s->frame_h = ev->h;
        if (s->frame_queued) {
            return;
        }
        s->frame_queued = true;
        break;
    case VISION_EV_STOPPED:
        s->reply_by = 0;
        break;
    case VISION_EV_LOST:
        s->streaming = false;
        s->silence_by = 0;
        break;
    default:
        break;
    }
    push(s, ev);
}

static void feed(struct vision_session *s, const char *buf, size_t n, int64_t now)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                kill_helper(s, VISION_EXIT_PROTOCOL);
            } else {
                struct vision_event ev;

                s->line[s->line_len] = '\0';
                /* Any line but `loading` is a helper no longer loading. */
                if (strncmp(s->line, "loading", 7) == 0 && (s->line[7] == ' ' || s->line[7] == '\0')) {
                    loading(s, now);
                    s->line_len = 0;
                    continue;
                }
                s->loading = false;
                if (strcmp(s->line, "bye") == 0) {
                    s->bye = true;
                } else if (strncmp(s->line, "hello", 5) == 0) {
                    unsigned version = 0;

                    s->hello_by = 0;
                    if (sscanf(s->line, "hello %u", &version) != 1 ||
                        version != VISION_PROTO_VERSION) {
                        kill_helper(s, VISION_EXIT_PROTOCOL);
                    }
                } else if (vision_session_parse_line(s, s->line, &ev)) {
                    handle(s, &ev, now);
                } else if (strncmp(s->line, "frame", 5) == 0 || strncmp(s->line, "ready", 5) == 0 ||
                           strncmp(s->line, "det", 3) == 0) {
                    /* A picture or box event that does not add up: not a
                     * helper to keep talking to. */
                    kill_helper(s, VISION_EXIT_PROTOCOL);
                }
            }
            s->line_len = 0;
            s->overlong = false;
        } else if (s->overlong) {
            continue;
        } else if (s->line_len + 1 >= sizeof(s->line)) {
            s->overlong = true;
            s->line_len = 0;
        } else {
            s->line[s->line_len++] = c;
        }
    }
}

static void finish(struct vision_session *s, int status, int64_t now)
{
    struct vision_event ev;

    if (s->fd >= 0) {
        char buf[512];
        ssize_t n;

        while ((n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            feed(s, buf, (size_t)n, now);
        }
        close(s->fd);
        s->fd = -1;
    }
    drop_shm(s);
    s->frame_slot = -1;
    s->streaming = false;
    s->hello_by = s->open_by = s->silence_by = s->reply_by = 0;
    memset(&ev, 0, sizeof(ev));
    ev.kind = VISION_EV_EXITED;
    if (WIFEXITED(status)) {
        ev.value = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ev.value = 128 + WTERMSIG(status);
    } else {
        ev.value = 255;
    }
    if (s->killed) {
        ev.reason = s->exit_reason;
    } else if (WIFSIGNALED(status)) {
        ev.reason = VISION_EXIT_CRASHED;
    } else {
        ev.reason = VISION_EXIT_NORMAL;
    }
    push(s, &ev);
    s->pid = -1;
    s->running = false;
}

static bool passed(int64_t deadline, int64_t now)
{
    return deadline != 0 && now >= deadline;
}

int vision_session_poll(struct vision_session *s, struct vision_event *ev, int64_t now_ms)
{
    if (s->running) {
        char buf[1024];
        int status;
        pid_t r;

        while (s->fd >= 0 && !s->eof) {
            ssize_t n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);

            if (n > 0) {
                feed(s, buf, (size_t)n, now_ms);
            } else if (n == 0) {
                s->eof = true;
            } else if (errno == EINTR) {
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    s->eof = true;
                }
                break;
            }
        }
        if (passed(s->hello_by, now_ms) || passed(s->open_by, now_ms) ||
            passed(s->silence_by, now_ms) || passed(s->reply_by, now_ms)) {
            kill_helper(s, VISION_EXIT_HUNG);
        }
        r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) {
            finish(s, status, now_ms);
        } else if (r < 0 && errno == ECHILD) {
            finish(s, 255 << 8, now_ms);
        }
    }
    return pop(s, ev);
}

/* ---- commands ------------------------------------------------------------------ */

static int view(struct vision_session *s, uint32_t w, uint32_t h, int display_rotation, bool whole)
{
    if (w == 0 || h == 0 || w > POCKETCAM_VIEW_MAX_W || h > POCKETCAM_VIEW_MAX_H) {
        return -1;
    }
    if (s->frame_slot >= 0 && (s->frame_w != w || s->frame_h != h)) {
        release_slot(s, s->frame_slot);
        s->frame_slot = -1;
    }
    /* Boxes said for the old view are of no use on the new one. */
    s->shown_count = 0;
    return send_line(s, "view %u %u %d%s", w, h, display_rotation, whole ? " contain" : "");
}

int vision_session_view(struct vision_session *s, uint32_t w, uint32_t h, int display_rotation)
{
    return view(s, w, h, display_rotation, false);
}

int vision_session_view_whole(struct vision_session *s, uint32_t w, uint32_t h, int display_rotation)
{
    return view(s, w, h, display_rotation, true);
}

int vision_session_stream(struct vision_session *s, bool on, int64_t now_ms)
{
    int r;

    if (on) {
        r = send_line(s, "start");
        if (r == 0) {
            s->streaming = true;
            s->silence_by = due(s, now_ms, VISION_SILENCE_MS);
        }
        return r;
    }
    s->streaming = false;
    s->silence_by = 0;
    if (s->frame_slot >= 0) {
        release_slot(s, s->frame_slot);
        s->frame_slot = -1;
    }
    r = send_line(s, "stop");
    if (r == 0) {
        s->reply_by = due(s, now_ms, VISION_REPLY_MS);
    }
    return r;
}

int vision_session_line(struct vision_session *s, const int32_t pm[4])
{
    int i;

    if (!pm) {
        return send_line(s, "line off");
    }
    for (i = 0; i < 4; i++) {
        if (pm[i] < 0 || pm[i] > 1000) {
            return -1;
        }
    }
    return send_line(s, "line %d %d %d %d", pm[0], pm[1], pm[2], pm[3]);
}

int vision_session_speed_lines(struct vision_session *s, const int32_t pm[8])
{
    int i;

    if (!pm) {
        return send_line(s, "speed off");
    }
    for (i = 0; i < 8; i++) {
        if (pm[i] < 0 || pm[i] > 1000) {
            return -1;
        }
    }
    return send_line(s, "speed %d %d %d %d %d %d %d %d", pm[0], pm[1], pm[2], pm[3], pm[4], pm[5], pm[6],
                     pm[7]);
}

int vision_session_distance(struct vision_session *s, uint32_t cm)
{
    if (cm == 0 || cm > 1000000u) {
        return -1;
    }
    return send_line(s, "distance %u", cm);
}

int vision_session_range(struct vision_session *s, const char *word)
{
    if (!word || (strcmp(word, "near") != 0 && strcmp(word, "normal") != 0 && strcmp(word, "far") != 0)) {
        return -1;
    }
    return send_line(s, "range %s", word);
}

int vision_session_mode(struct vision_session *s, bool traffic)
{
    return send_line(s, "mode %s", traffic ? "traffic" : "detect");
}

int vision_session_mode_word(struct vision_session *s, const char *word)
{
    if (!word || !word_ok(word, 16)) {
        return -1;
    }
    return send_line(s, "mode %s", word);
}

int vision_session_enrol(struct vision_session *s, bool on)
{
    return on ? send_line(s, "enrol") : send_line(s, "enrol off");
}

int vision_session_forget(struct vision_session *s)
{
    return send_line(s, "forget");
}

int vision_session_color(struct vision_session *s, const uint8_t rgb[3])
{
    if (!rgb) {
        return send_line(s, "color off");
    }
    return send_line(s, "color %u %u %u", rgb[0], rgb[1], rgb[2]);
}

int vision_session_sample(struct vision_session *s, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= (int32_t)POCKETCAM_VIEW_MAX_W || y >= (int32_t)POCKETCAM_VIEW_MAX_H) {
        return -1;
    }
    return send_line(s, "sample %d %d", x, y);
}

int vision_session_tol(struct vision_session *s, uint32_t tol)
{
    if (tol > 765) {
        return -1;
    }
    return send_line(s, "tol %u", tol);
}

int vision_session_edge(struct vision_session *s, uint32_t threshold)
{
    if (threshold > 255) {
        return -1;
    }
    return send_line(s, "edge %u", threshold);
}

int vision_session_trace(struct vision_session *s, bool dark)
{
    return send_line(s, "trace %s", dark ? "dark" : "light");
}

int vision_session_reset(struct vision_session *s)
{
    return send_line(s, "reset");
}

int vision_session_take_frame(struct vision_session *s, uint16_t *dst, uint32_t w, uint32_t h)
{
    int slot = s->frame_slot;
    int r = 0;

    s->frame_queued = false;
    if (slot < 0) {
        return 0;
    }
    if (s->shm && dst && s->frame_w == w && s->frame_h == h) {
        memcpy(dst, s->shm + pocketcam_slot_offset((uint32_t)slot), (size_t)w * h * 2);
        r = 1;
    }
    s->frame_slot = -1;
    release_slot(s, slot);
    return r;
}

const struct vision_shown *vision_session_tracks(const struct vision_session *s, int *count,
                                                 uint32_t *seq)
{
    if (count) {
        *count = s->shown_count;
    }
    if (seq) {
        *seq = s->shown_seq;
    }
    return s->shown;
}

void vision_session_counts(const struct vision_session *s, uint32_t *ab, uint32_t *ba)
{
    if (ab) {
        *ab = s->count_ab;
    }
    if (ba) {
        *ba = s->count_ba;
    }
}

const struct vision_traffic_report *vision_session_traffic(const struct vision_session *s)
{
    return &s->traffic;
}

const struct vision_recent_report *vision_session_recent(const struct vision_session *s)
{
    return &s->recent;
}

const struct vision_text_report *vision_session_text(const struct vision_session *s)
{
    return &s->text;
}

const struct vision_who_report *vision_session_who(const struct vision_session *s)
{
    return &s->who;
}

const struct vision_pixel_report *vision_session_pixels(const struct vision_session *s)
{
    return &s->pixels;
}

const struct vision_stats *vision_session_stats(const struct vision_session *s)
{
    return &s->stats;
}

/* ---- leaving ------------------------------------------------------------------ */

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* What the helper says while it is waited for: `loading` and `bye` are
 * what matter here; the socket is kept empty so a helper saying its last
 * lines never blocks on a full one. */
static void drain(struct vision_session *s, int64_t now)
{
    char buf[1024];
    ssize_t n;

    while (s->fd >= 0 && !s->eof) {
        n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) {
            feed(s, buf, (size_t)n, now);
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            s->eof = true;
        } else if (errno != EINTR) {
            break;
        }
    }
}

/* Wait up to ms for the helper to leave; while it says it is opening a
 * model, until that load's deadline and ms after it, but never past cap. */
static bool reap_within(struct vision_session *s, int ms, int64_t cap)
{
    int64_t end = mono_ms() + ms;

    for (;;) {
        int status;
        int64_t now = mono_ms();
        pid_t r;

        drain(s, now);
        r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid || (r < 0 && errno == ECHILD)) {
            drain(s, now);
            s->pid = -1;
            s->running = false;
            return true;
        }
        if (s->loading && s->load_by + ms > end) {
            end = s->load_by + ms < cap ? s->load_by + ms : cap;
        }
        if (now >= end) {
            return false;
        }
        {
            struct timespec d = { 0, 5 * 1000000L };

            nanosleep(&d, NULL);
        }
    }
}

enum vision_leave vision_session_abandon(struct vision_session *s, int grace_ms)
{
    enum vision_leave left = VISION_LEFT_IDLE;

    if (s->running && s->pid > 0) {
        int grace = grace_ms < 0 ? 0 : grace_ms;
        /* However many models it says it is opening, the wait has an end. */
        int64_t cap = mono_ms() + grace + 2 * (int64_t)VISION_LOAD_MS;

        bool gone;

        send_line(s, "quit");
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        /* For a helper the watchdog killed already, the wait for it to die. */
        gone = reap_within(s, grace, cap);
        if (gone && !s->killed) {
            left = s->bye ? VISION_LEFT_BYE : VISION_LEFT_EXITED;
        } else {
            if (!gone) {
                kill(s->pid, SIGKILL);
                reap_within(s, VISION_KILL_REAP_MS, mono_ms() + VISION_KILL_REAP_MS);
            }
            left = VISION_LEFT_KILLED;
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    drop_shm(s);
    vision_session_init(s);
    return left;
}
