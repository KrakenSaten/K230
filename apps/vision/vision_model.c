/*
 * Vision's state. See vision_model.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_model.h"

#include <stdio.h>
#include <string.h>

#define VISION_STALL_AFTER_MS 2000

void vision_model_init(struct vision_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = VISION_INIT;
    m->line = VISION_LINE_ACROSS;
}

unsigned vision_model_open(struct vision_model *m)
{
    enum vision_line_mode line = m->line;

    vision_model_init(m);
    m->line = line;
    return VISION_ACT_OPEN;
}

static void fail(struct vision_model *m, enum vision_state st, const char *text)
{
    m->state = st;
    m->live = false;
    m->stalled = false;
    snprintf(m->error, sizeof(m->error), "%s", text ? text : "");
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
            acts |= VISION_ACT_STREAM | VISION_ACT_LINE;
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
        break;
    case VISION_EV_COUNT:
        if (s) {
            vision_session_counts(s, &m->count_a, &m->count_b);
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

unsigned vision_model_line_next(struct vision_model *m)
{
    m->line = (enum vision_line_mode)((m->line + 1) % VISION_LINE_MODES);
    m->count_a = 0;
    m->count_b = 0;
    return m->state == VISION_LIVE ? VISION_ACT_LINE : 0;
}

unsigned vision_model_reset(struct vision_model *m)
{
    m->count_a = 0;
    m->count_b = 0;
    return m->state == VISION_LIVE ? VISION_ACT_RESET : 0;
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

void vision_model_text(const struct vision_model *m, struct vision_view_text *out, char *status_buf,
                       size_t status_len)
{
    memset(out, 0, sizeof(*out));
    out->hint = m->simulated ? "SIMULATED" : "";
    out->line_btn = m->line == VISION_LINE_OFF ? "LINE: OFF"
                    : m->line == VISION_LINE_ACROSS ? "LINE: ACROSS" : "LINE: DOWN";
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
