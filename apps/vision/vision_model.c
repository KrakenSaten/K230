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
static const uint32_t tols[VISION_TOLS] = { 48, 96, 160 };
static const char *const tol_names[VISION_TOLS] = { "TOL: LOW", "TOL: MED", "TOL: HIGH" };
static const char *const traffic_names[VISION_PROTO_TRAFFIC_CLASSES] = {
    "car", "truck", "bus", "moto", "bike", "person",
};
static const char *const mode_names[VISION_MODES] = { "DETECT", "TRAFFIC", "COLOR", "EDGE", "TRACE" };
static const char *const mode_words[VISION_MODES] = { "detect", "traffic", "color", "edge", "trace" };

void vision_model_init(struct vision_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = VISION_INIT;
    m->line = VISION_LINE_ACROSS;
    m->orient = VISION_LINE_ACROSS;
    m->speed = VISION_SPEED_OFF;
    m->distance_idx = VISION_DISTANCE_DEFAULT;
    m->tol_idx = VISION_TOL_DEFAULT;
    m->trace_dark = true;
    m->sample_x = -1;
    m->sample_y = -1;
}

unsigned vision_model_open(struct vision_model *m)
{
    struct vision_model keep = *m;

    vision_model_init(m);
    m->mode = keep.mode;
    m->line = keep.line;
    m->orient = keep.orient;
    m->speed = keep.speed;
    m->distance_idx = keep.distance_idx;
    m->tol_idx = keep.tol_idx;
    m->edge_hard = keep.edge_hard;
    m->trace_dark = keep.trace_dark;
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

static void clear_pixels(struct vision_model *m)
{
    memset(&m->pixels, 0, sizeof(m->pixels));
    m->color_valid = false;
    m->edge_valid = false;
    m->trace_valid = false;
}

bool vision_model_pixel_mode(const struct vision_model *m)
{
    return m->mode == VISION_MODE_COLOR || m->mode == VISION_MODE_EDGE || m->mode == VISION_MODE_TRACE;
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
                    VISION_ACT_DISTANCE | VISION_ACT_PIXELS;
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
    case VISION_EV_COLOR:
        if (s) {
            m->pixels.color = vision_session_pixels(s)->color;
            m->color_valid = true;
            m->have_target = true;
        }
        break;
    case VISION_EV_EDGE:
        if (s) {
            m->pixels.edge_pm = vision_session_pixels(s)->edge_pm;
            m->edge_valid = true;
        }
        break;
    case VISION_EV_TRACE:
        if (s) {
            m->pixels.trace = vision_session_pixels(s)->trace;
            m->trace_valid = true;
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
    clear_pixels(m);
    m->active_tracks = 0;
    m->have_target = false;
    m->sample_x = -1;
    m->sample_y = -1;
    return VISION_ACT_MODE |
           when_live(m, VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS);
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

unsigned vision_model_sample_at(struct vision_model *m, int32_t x, int32_t y)
{
    if (m->mode != VISION_MODE_COLOR || x < 0 || y < 0) {
        return 0;
    }
    m->sample_x = x;
    m->sample_y = y;
    return when_live(m, VISION_ACT_SAMPLE);
}

unsigned vision_model_sample_middle(struct vision_model *m, uint32_t view_w, uint32_t view_h)
{
    return vision_model_sample_at(m, (int32_t)(view_w / 2), (int32_t)(view_h / 2));
}

unsigned vision_model_tol_next(struct vision_model *m)
{
    m->tol_idx = (m->tol_idx + 1) % VISION_TOLS;
    return when_live(m, VISION_ACT_PIXELS);
}

unsigned vision_model_edge_next(struct vision_model *m)
{
    m->edge_hard = !m->edge_hard;
    return when_live(m, VISION_ACT_PIXELS);
}

unsigned vision_model_trace_next(struct vision_model *m)
{
    m->trace_dark = !m->trace_dark;
    m->trace_valid = false;
    return when_live(m, VISION_ACT_PIXELS);
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

uint32_t vision_model_tol(const struct vision_model *m)
{
    int i = m->tol_idx;

    if (i < 0 || i >= VISION_TOLS) {
        i = VISION_TOL_DEFAULT;
    }
    return tols[i];
}

uint32_t vision_model_edge_threshold(const struct vision_model *m)
{
    return m->edge_hard ? VISION_EDGE_HARD_THRESHOLD : 0;
}

const char *vision_model_mode_word(const struct vision_model *m)
{
    return m->mode >= 0 && m->mode < VISION_MODES ? mode_words[m->mode] : "detect";
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
    out[0] = VISION_BTN_MODE;
    switch (m->mode) {
    case VISION_MODE_TRAFFIC:
        out[1] = VISION_BTN_LINE;
        out[2] = VISION_BTN_SPEED;
        out[3] = VISION_BTN_DISTANCE;
        out[4] = VISION_BTN_RESET;
        return 5;
    case VISION_MODE_COLOR:
        out[1] = VISION_BTN_SAMPLE;
        out[2] = VISION_BTN_TOL;
        return 3;
    case VISION_MODE_EDGE:
        out[1] = VISION_BTN_EDGE;
        return 2;
    case VISION_MODE_TRACE:
        out[1] = VISION_BTN_TRACE;
        return 2;
    default:
        out[1] = VISION_BTN_LINE;
        out[2] = VISION_BTN_RESET;
        return 3;
    }
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

static void fps_prefix(const struct vision_model *m, char *buf, size_t len)
{
    if (m->stats_valid) {
        snprintf(buf, len, "%u.%u fps  %d ms  ", m->stats.fps_x10 / 10, m->stats.fps_x10 % 10, m->stats.post_ms);
    } else {
        buf[0] = '\0';
    }
}

static void pixel_status(const struct vision_model *m, char *buf, size_t len)
{
    char fps[32];

    fps_prefix(m, fps, sizeof(fps));
    switch (m->mode) {
    case VISION_MODE_COLOR:
        if (!m->have_target || !m->color_valid) {
            snprintf(buf, len, "%sTap the picture or SAMPLE to pick a colour", fps);
        } else {
            snprintf(buf, len, "%s#%02X%02X%02X  match %u.%u%%  at %d,%d", fps, m->pixels.color.r, m->pixels.color.g,
                     m->pixels.color.b, m->pixels.color.matched_pm / 10, m->pixels.color.matched_pm % 10,
                     m->pixels.color.cx, m->pixels.color.cy);
        }
        break;
    case VISION_MODE_EDGE:
        if (m->edge_valid) {
            snprintf(buf, len, "%sedges %u.%u%%  %s", fps, m->pixels.edge_pm / 10, m->pixels.edge_pm % 10,
                     m->edge_hard ? "hard" : "soft");
        } else {
            snprintf(buf, len, "%sFinding edges", fps);
        }
        break;
    default:
        if (!m->trace_valid) {
            snprintf(buf, len, "%sLooking for a %s line", fps, m->trace_dark ? "dark" : "light");
        } else if (!m->pixels.trace.found) {
            snprintf(buf, len, "%sno %s line  (%u rows)", fps, m->trace_dark ? "dark" : "light", m->pixels.trace.rows);
        } else {
            int32_t o = m->pixels.trace.offset_pm;
            int32_t sl = m->pixels.trace.slope_pm;

            snprintf(buf, len, "%sline %s %d%%  leans %s %d%%  %u rows", fps, o < 0 ? "left" : "right",
                     (o < 0 ? -o : o) / 10, sl < 0 ? "left" : "right", (sl < 0 ? -sl : sl) / 10,
                     m->pixels.trace.rows);
        }
        break;
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
    out->lines = m->mode == VISION_MODE_DETECT || m->mode == VISION_MODE_TRAFFIC;
    out->picture_tap = m->mode == VISION_MODE_COLOR && m->state == VISION_LIVE;
    out->mode_btn = mode_names[m->mode < VISION_MODES ? m->mode : 0];
    out->line_btn = m->line == VISION_LINE_OFF ? "LINE: OFF"
                    : m->line == VISION_LINE_ACROSS ? "LINE: ACROSS" : "LINE: DOWN";
    out->speed_btn = m->speed == VISION_SPEED_OFF ? "SPEED: OFF"
                     : m->speed == VISION_SPEED_NARROW ? "SPEED: NARROW" : "SPEED: WIDE";
    out->tol_btn = tol_names[m->tol_idx >= 0 && m->tol_idx < VISION_TOLS ? m->tol_idx : VISION_TOL_DEFAULT];
    out->edge_btn = m->edge_hard ? "EDGE: HARD" : "EDGE: SOFT";
    out->trace_btn = m->trace_dark ? "LINE: DARK" : "LINE: LIGHT";
    snprintf(out->dist_btn, sizeof(out->dist_btn), "DIST: %u m", vision_model_distance_cm(m) / 100);
    vision_model_count_names(m, &na, &nb);
    if (m->line == VISION_LINE_OFF || !out->lines) {
        snprintf(out->count_a, sizeof(out->count_a), "-");
        snprintf(out->count_b, sizeof(out->count_b), "-");
    } else if (out->traffic) {
        snprintf(out->count_a, sizeof(out->count_a), "IN (%s) %u", na, m->traffic.total_ab);
        snprintf(out->count_b, sizeof(out->count_b), "OUT (%s) %u", nb, m->traffic.total_ba);
    } else {
        snprintf(out->count_a, sizeof(out->count_a), "%s %u", na, m->count_a);
        snprintf(out->count_b, sizeof(out->count_b), "%s %u", nb, m->count_b);
    }
    if (m->mode == VISION_MODE_COLOR && m->color_valid && m->pixels.color.matched_pm > 0 &&
        m->pixels.color.cx >= 0 && m->pixels.color.cy >= 0) {
        out->show_mark = true;
        out->mark_x = m->pixels.color.cx;
        out->mark_y = m->pixels.color.cy;
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
        } else if (vision_model_pixel_mode(m)) {
            pixel_status(m, status_buf, status_len);
            out->status = status_buf;
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
