/*
 * Vision's state. See vision_model.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_model.h"

#include <stdio.h>
#include <string.h>

#define VISION_STALL_AFTER_MS 2000

static const uint32_t distances_cm[VISION_DISTANCES] = { 100, 200, 500, 1000, 1500, 2000, 3000, 5000 };
static const char *const traffic_names[VISION_PROTO_TRAFFIC_CLASSES] = {
    "car", "truck", "bus", "moto", "bike", "person",
};

void vision_model_init(struct vision_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = VISION_INIT;
    m->line = VISION_LINE_ACROSS;
    m->orient = VISION_LINE_ACROSS;
    m->speed = VISION_SPEED_OFF;
    m->distance_idx = VISION_DISTANCE_DEFAULT;
}

unsigned vision_model_open(struct vision_model *m)
{
    enum vision_mode mode = m->mode;
    enum vision_line_mode line = m->line;
    enum vision_line_mode orient = m->orient;
    enum vision_speed_mode speed = m->speed;
    int distance_idx = m->distance_idx;

    vision_model_init(m);
    m->mode = mode;
    m->line = line;
    m->orient = orient;
    m->speed = speed;
    m->distance_idx = distance_idx;
    return VISION_ACT_OPEN;
}

static void fail(struct vision_model *m, enum vision_state st, const char *text)
{
    m->state = st;
    m->live = false;
    m->stalled = false;
    snprintf(m->error, sizeof(m->error), "%s", text ? text : "");
}

static void clear_counts(struct vision_model *m)
{
    m->count_a = 0;
    m->count_b = 0;
    memset(&m->traffic, 0, sizeof(m->traffic));
}

unsigned vision_model_event(struct vision_model *m, const struct vision_event *ev,
                            const struct vision_session *s, int64_t now_ms)
{
    unsigned acts = 0;

    switch (ev->kind) {
    case VISION_EV_READY:
        if (m->state == VISION_INIT) {
            m->state = VISION_LIVE;
            m->simulated = ev->simulated;
            snprintf(m->camera, sizeof(m->camera), "%.*s", (int)sizeof(m->camera) - 1, ev->text);
            snprintf(m->model_name, sizeof(m->model_name), "%.*s", (int)sizeof(m->model_name) - 1,
                     ev->name);
            m->preview_w = ev->w;
            m->preview_h = ev->h;
            m->classes = (uint32_t)ev->value;
            m->last_frame_ms = now_ms;
            /* Everything the helper has to know, whatever it defaults to. */
            acts |= VISION_ACT_STREAM | VISION_ACT_MODE | VISION_ACT_LINE | VISION_ACT_SPEED |
                    VISION_ACT_DISTANCE;
        }
        break;
    case VISION_EV_FRAME:
        if (m->state == VISION_LIVE) {
            m->live = true;
            m->stalled = false;
            m->last_frame_ms = now_ms;
        }
        break;
    case VISION_EV_DET:
        m->last_frame_ms = now_ms;
        if (s) {
            int n = 0;
            int i;
            int active = 0;
            const struct vision_shown *t = vision_session_tracks(s, &n, NULL);

            for (i = 0; i < n; i++) {
                active += t[i].id != 0;
            }
            m->active_tracks = active;
        }
        break;
    case VISION_EV_COUNT:
        if (s) {
            vision_session_counts(s, &m->count_a, &m->count_b);
        }
        break;
    case VISION_EV_TRAFFIC:
        if (s) {
            m->traffic = *vision_session_traffic(s);
            m->traffic_valid = true;
        }
        break;
    case VISION_EV_STATS:
        if (s) {
            m->stats = *vision_session_stats(s);
            m->stats_valid = true;
            m->bad_tensors = m->stats.bad;
        }
        break;
    case VISION_EV_STALL:
        if (m->state == VISION_LIVE) {
            m->stalled = true;
        }
        break;
    case VISION_EV_NODEVICE:
        fail(m, VISION_NO_DEVICE, ev->text[0] ? ev->text : "No camera was found");
        break;
    case VISION_EV_NOMODEL:
        fail(m, VISION_NO_DEVICE, ev->text[0] ? ev->text : "The detector could not be opened");
        break;
    case VISION_EV_ERROR:
        if (strncmp(ev->text, "busy", 4) == 0) {
            fail(m, VISION_ERROR, "The camera is in use");
        } else if (strncmp(ev->text, "exec", 4) == 0) {
            fail(m, VISION_ERROR, "The vision helper is missing");
        } else if (strncmp(ev->text, "model", 5) == 0) {
            fail(m, VISION_ERROR, "The detector stopped making sense");
        } else if (strncmp(ev->text, "infer", 5) == 0) {
            fail(m, VISION_ERROR, "The detector failed on a frame");
        } else {
            fail(m, VISION_ERROR, ev->text);
        }
        break;
    case VISION_EV_LOST:
        fail(m, VISION_ERROR, "The camera went away");
        break;
    case VISION_EV_EXITED:
        if (m->state == VISION_INIT || m->state == VISION_LIVE) {
            switch (ev->reason) {
            case VISION_EXIT_HUNG: fail(m, VISION_ERROR, "The camera is not responding"); break;
            case VISION_EXIT_CRASHED: fail(m, VISION_ERROR, "The vision helper crashed"); break;
            case VISION_EXIT_PROTOCOL: fail(m, VISION_ERROR, "The vision helper misbehaved"); break;
            default: fail(m, VISION_ERROR, "The vision helper ended"); break;
            }
        }
        break;
    default:
        break;
    }
    return acts;
}

bool vision_model_tick(struct vision_model *m, int64_t now_ms)
{
    if (m->state == VISION_LIVE && m->live && !m->stalled &&
        now_ms - m->last_frame_ms >= VISION_STALL_AFTER_MS) {
        m->stalled = true;
        return true;
    }
    return false;
}

static unsigned when_live(const struct vision_model *m, unsigned acts)
{
    return m->state == VISION_LIVE ? acts : 0;
}

unsigned vision_model_mode_next(struct vision_model *m)
{
    m->mode = (enum vision_mode)((m->mode + 1) % VISION_MODES);
    /* A new way of looking is a new count, on the helper too. */
    clear_counts(m);
    m->active_tracks = 0;
    return VISION_ACT_MODE | when_live(m, VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE);
}

unsigned vision_model_line_next(struct vision_model *m)
{
    m->line = (enum vision_line_mode)((m->line + 1) % VISION_LINE_MODES);
    if (m->line != VISION_LINE_OFF) {
        m->orient = m->line;
    }
    clear_counts(m);
    /* The speed lines lie the way the counting line does. */
    return when_live(m, VISION_ACT_LINE | VISION_ACT_SPEED);
}

unsigned vision_model_speed_next(struct vision_model *m)
{
    m->speed = (enum vision_speed_mode)((m->speed + 1) % VISION_SPEED_MODES);
    m->traffic.cur_kmh10 = 0;
    return when_live(m, VISION_ACT_SPEED);
}

unsigned vision_model_distance_next(struct vision_model *m)
{
    m->distance_idx = (m->distance_idx + 1) % VISION_DISTANCES;
    m->traffic.cur_kmh10 = 0;
    return when_live(m, VISION_ACT_DISTANCE);
}

unsigned vision_model_reset(struct vision_model *m)
{
    clear_counts(m);
    return when_live(m, VISION_ACT_RESET);
}

bool vision_model_line_pm(const struct vision_model *m, int32_t pm[4])
{
    switch (m->line) {
    case VISION_LINE_ACROSS:
        /* Left to right at mid-height: side B (positive) is below, so
         * ab = DOWN, ba = UP. */
        pm[0] = 0;
        pm[1] = 500;
        pm[2] = 1000;
        pm[3] = 500;
        return true;
    case VISION_LINE_DOWN:
        /* Top to bottom at mid-width: side B is to the left, so ab = LEFT,
         * ba = RIGHT. */
        pm[0] = 500;
        pm[1] = 0;
        pm[2] = 500;
        pm[3] = 1000;
        return true;
    default:
        return false;
    }
}

bool vision_model_speed_pm(const struct vision_model *m, int32_t pm[8])
{
    int32_t a;
    int32_t b;

    switch (m->speed) {
    case VISION_SPEED_NARROW:
        a = 400;
        b = 600;
        break;
    case VISION_SPEED_WIDE:
        a = 250;
        b = 750;
        break;
    default:
        return false;
    }
    if (m->orient == VISION_LINE_DOWN) {
        /* Two vertical lines, each top to bottom like the counting line. */
        pm[0] = a;
        pm[1] = 0;
        pm[2] = a;
        pm[3] = 1000;
        pm[4] = b;
        pm[5] = 0;
        pm[6] = b;
        pm[7] = 1000;
    } else {
        pm[0] = 0;
        pm[1] = a;
        pm[2] = 1000;
        pm[3] = a;
        pm[4] = 0;
        pm[5] = b;
        pm[6] = 1000;
        pm[7] = b;
    }
    return true;
}

uint32_t vision_model_distance_cm(const struct vision_model *m)
{
    int i = m->distance_idx;

    if (i < 0 || i >= VISION_DISTANCES) {
        i = VISION_DISTANCE_DEFAULT;
    }
    return distances_cm[i];
}

void vision_model_count_names(const struct vision_model *m, const char **a, const char **b)
{
    if (m->line == VISION_LINE_DOWN) {
        *a = "LEFT";
        *b = "RIGHT";
    } else {
        *a = "DOWN";
        *b = "UP";
    }
}

int vision_model_buttons(const struct vision_model *m, enum vision_button out[VISION_BUTTONS])
{
    if (m->mode == VISION_MODE_TRAFFIC) {
        out[0] = VISION_BTN_MODE;
        out[1] = VISION_BTN_LINE;
        out[2] = VISION_BTN_SPEED;
        out[3] = VISION_BTN_DISTANCE;
        out[4] = VISION_BTN_RESET;
        return 5;
    }
    out[0] = VISION_BTN_MODE;
    out[1] = VISION_BTN_LINE;
    out[2] = VISION_BTN_RESET;
    return 3;
}

int vision_model_status_lines(const struct vision_model *m)
{
    return m->mode == VISION_MODE_TRAFFIC ? 3 : 1;
}

const char *vision_model_traffic_name(int i)
{
    return i >= 0 && i < VISION_PROTO_TRAFFIC_CLASSES ? traffic_names[i] : "?";
}

static void kmh(char *buf, size_t len, uint32_t kmh10)
{
    snprintf(buf, len, "%u.%u", kmh10 / 10, kmh10 % 10);
}

static void traffic_status(const struct vision_model *m, char *buf, size_t len)
{
    size_t off = 0;
    int n;
    int i;
    char cur[16];
    char last[16];
    char max[16];
    uint32_t total = m->traffic.total_ab + m->traffic.total_ba;

    if (m->stats_valid) {
        n = snprintf(buf, len, "%u.%u fps  KPU %d ms  %d tracks  %u total\n", m->stats.fps_x10 / 10,
                     m->stats.fps_x10 % 10, m->stats.infer_ms, m->active_tracks, total);
    } else {
        n = snprintf(buf, len, "%s  %d tracks  %u total\n", m->live ? "Detecting" : "Starting the stream",
                     m->active_tracks, total);
    }
    if (n < 0) {
        return;
    }
    off = (size_t)n < len ? (size_t)n : len;
    for (i = 0; i < VISION_PROTO_TRAFFIC_CLASSES && off < len; i++) {
        n = snprintf(buf + off, len - off, "%s%s %u", i ? "  " : "", traffic_names[i],
                     m->traffic.cls_ab[i] + m->traffic.cls_ba[i]);
        if (n < 0) {
            return;
        }
        off += (size_t)n < len - off ? (size_t)n : len - off;
    }
    if (off >= len) {
        return;
    }
    if (m->speed == VISION_SPEED_OFF) {
        snprintf(buf + off, len - off, "\nSPEED off  (%u m)", vision_model_distance_cm(m) / 100);
        return;
    }
    if (m->traffic.n == 0) {
        snprintf(buf + off, len - off, "\nSPEED --  (%u m between lines)", vision_model_distance_cm(m) / 100);
        return;
    }
    kmh(cur, sizeof(cur), m->traffic.cur_kmh10);
    kmh(last, sizeof(last), m->traffic.last_kmh10);
    kmh(max, sizeof(max), m->traffic.max_kmh10);
    if (m->traffic.cur_kmh10) {
        snprintf(buf + off, len - off, "\nSPEED %s km/h  last %s  max %s  (%u m)", cur, last, max,
                 vision_model_distance_cm(m) / 100);
    } else {
        snprintf(buf + off, len - off, "\nSPEED --  last %s km/h  max %s  (%u m)", last, max,
                 vision_model_distance_cm(m) / 100);
    }
}

void vision_model_text(const struct vision_model *m, struct vision_view_text *out, char *status_buf,
                       size_t status_len)
{
    const char *na;
    const char *nb;

    memset(out, 0, sizeof(*out));
    out->hint = m->simulated ? "SIMULATED" : "";
    out->traffic = m->mode == VISION_MODE_TRAFFIC;
    out->mode_btn = out->traffic ? "TRAFFIC" : "DETECT";
    out->line_btn = m->line == VISION_LINE_OFF ? "LINE: OFF"
                    : m->line == VISION_LINE_ACROSS ? "LINE: ACROSS" : "LINE: DOWN";
    out->speed_btn = m->speed == VISION_SPEED_OFF ? "SPEED: OFF"
                     : m->speed == VISION_SPEED_NARROW ? "SPEED: NARROW" : "SPEED: WIDE";
    snprintf(out->dist_btn, sizeof(out->dist_btn), "DIST: %u m", vision_model_distance_cm(m) / 100);
    vision_model_count_names(m, &na, &nb);
    if (m->line == VISION_LINE_OFF) {
        snprintf(out->count_a, sizeof(out->count_a), "-");
        snprintf(out->count_b, sizeof(out->count_b), "-");
    } else if (out->traffic) {
        snprintf(out->count_a, sizeof(out->count_a), "IN (%s) %u", na, m->traffic.total_ab);
        snprintf(out->count_b, sizeof(out->count_b), "OUT (%s) %u", nb, m->traffic.total_ba);
    } else {
        snprintf(out->count_a, sizeof(out->count_a), "%s %u", na, m->count_a);
        snprintf(out->count_b, sizeof(out->count_b), "%s %u", nb, m->count_b);
    }
    out->title = "";
    out->detail = "";
    out->status = "";
    switch (m->state) {
    case VISION_INIT:
        out->title = "Starting";
        out->detail = "Opening the camera and the detector";
        break;
    case VISION_LIVE:
        out->show_picture = m->live;
        out->line_enabled = true;
        if (!m->live) {
            out->title = "Waiting for the picture";
        }
        if (m->stalled) {
            out->status = "Waiting for the camera...";
            out->status_warn = true;
        } else if (out->traffic) {
            traffic_status(m, status_buf, status_len);
            out->status = status_buf;
            out->status_warn = m->stats_valid && m->stats.bad > 0;
        } else if (m->stats_valid) {
            snprintf(status_buf, status_len, "%u.%u fps  KPU %d ms  pre %d  post %d  CPU %d%%  %ld MB",
                     m->stats.fps_x10 / 10, m->stats.fps_x10 % 10, m->stats.infer_ms,
                     m->stats.pre_ms, m->stats.post_ms, m->stats.cpu_pct,
                     (m->stats.rss_kb + 512) / 1024);
            out->status = status_buf;
            out->status_warn = m->stats.bad > 0;
        } else {
            out->status = m->live ? "Detecting" : "Starting the stream";
        }
        break;
    case VISION_ERROR:
        out->title = "Vision stopped";
        out->detail = m->error;
        out->show_retry = true;
        break;
    case VISION_NO_DEVICE:
        out->title = "No camera or detector";
        out->detail = m->error;
        out->show_retry = true;
        break;
    }
}
