/*
 * The Video app's state machine. See video_state.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "video_state.h"

#include <stdio.h>
#include <string.h>

void video_model_init(struct video_model *m)
{
    memset(m, 0, sizeof(*m));
    m->status = VIDEO_ST_LIST;
}

static void reset_player(struct video_model *m)
{
    m->duration_ms = 0;
    m->pos_ms = 0;
    m->src_w = m->src_h = 0;
    m->fps_x100 = 0;
    m->audio[0] = '\0';
    m->frames = 0;
    m->dragging = false;
    m->seek_inflight = false;
    m->seek_waiting = false;
    m->have_stats = false;
    memset(&m->stats, 0, sizeof(m->stats));
    m->error_title[0] = '\0';
    m->error_detail[0] = '\0';
}

static void set_error(struct video_model *m, const char *title, const char *detail)
{
    m->status = VIDEO_ST_ERROR;
    m->want_play = false;
    m->dragging = false;
    m->seek_inflight = false;
    m->seek_waiting = false;
    snprintf(m->error_title, sizeof(m->error_title), "%s", title);
    snprintf(m->error_detail, sizeof(m->error_detail), "%s", detail ? detail : "");
}

static bool in_player(const struct video_model *m)
{
    return m->status != VIDEO_ST_LIST;
}

bool video_model_can_control(const struct video_model *m)
{
    return m->status == VIDEO_ST_PAUSED || m->status == VIDEO_ST_PLAYING ||
           m->status == VIDEO_ST_STOPPED || m->status == VIDEO_ST_ENDED;
}

/* ---- taps --------------------------------------------------------------------- */

unsigned video_model_choose(struct video_model *m, const char *name)
{
    if (m->status != VIDEO_ST_LIST || !name || !video_files_playable_name(name)) {
        return 0;
    }
    reset_player(m);
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->status = VIDEO_ST_OPENING;
    /* Choosing a file means watching it. */
    m->want_play = true;
    return VIDEO_ACT_START | VIDEO_ACT_LAYOUT;
}

unsigned video_model_back(struct video_model *m)
{
    if (!in_player(m)) {
        return VIDEO_ACT_HOME;
    }
    m->status = VIDEO_ST_LIST;
    m->fullscreen = false;
    m->want_play = false;
    reset_player(m);
    return VIDEO_ACT_ABANDON | VIDEO_ACT_LAYOUT | VIDEO_ACT_RESCAN;
}

unsigned video_model_toggle(struct video_model *m)
{
    if (m->status == VIDEO_ST_OPENING) {
        m->want_play = !m->want_play;
        return 0;
    }
    if (!video_model_can_control(m)) {
        return 0;
    }
    m->want_play = !m->want_play;
    return m->want_play ? VIDEO_ACT_PLAY : VIDEO_ACT_PAUSE;
}

unsigned video_model_stop(struct video_model *m)
{
    if (!video_model_can_control(m)) {
        return 0;
    }
    m->want_play = false;
    m->dragging = false;
    m->seek_waiting = false;
    return VIDEO_ACT_STOP;
}

unsigned video_model_fullscreen(struct video_model *m)
{
    if (!in_player(m)) {
        return 0;
    }
    m->fullscreen = !m->fullscreen;
    return VIDEO_ACT_LAYOUT;
}

unsigned video_model_rescan(struct video_model *m)
{
    return m->status == VIDEO_ST_LIST ? VIDEO_ACT_RESCAN : 0;
}

static int64_t clamp(const struct video_model *m, int64_t ms)
{
    if (ms < 0) {
        return 0;
    }
    if (m->duration_ms > 0 && ms > m->duration_ms) {
        return m->duration_ms;
    }
    return ms;
}

unsigned video_model_seek(struct video_model *m, int64_t ms)
{
    if (!video_model_can_control(m) || m->duration_ms <= 0) {
        return 0;
    }
    ms = clamp(m, ms);
    m->pos_ms = ms;
    if (m->seek_inflight) {
        m->seek_waiting = true;
        m->seek_waiting_ms = ms;
        return 0;
    }
    m->seek_inflight = true;
    m->seek_sent = ms;
    return VIDEO_ACT_SEEK;
}

unsigned video_model_drag(struct video_model *m, int64_t ms)
{
    if (!video_model_can_control(m) || m->duration_ms <= 0) {
        return 0;
    }
    m->dragging = true;
    m->drag_ms = clamp(m, ms);
    return 0;
}

unsigned video_model_drag_end(struct video_model *m, int64_t ms)
{
    if (!m->dragging) {
        return 0;
    }
    m->dragging = false;
    return video_model_seek(m, ms);
}

/* ---- the helper --------------------------------------------------------------- */

static unsigned seek_answered(struct video_model *m)
{
    m->seek_inflight = false;
    if (m->seek_waiting) {
        m->seek_waiting = false;
        m->seek_inflight = true;
        m->seek_sent = m->seek_waiting_ms;
        m->pos_ms = m->seek_waiting_ms;
        return VIDEO_ACT_SEEK;
    }
    return 0;
}

static void openfail_text(struct video_model *m, const char *reason, const char *text)
{
    if (strcmp(reason, VIDEO_OPENFAIL_MISSING) == 0) {
        set_error(m, "File not found", "It may have been moved or deleted.");
    } else if (strcmp(reason, VIDEO_OPENFAIL_UNSUPPORTED) == 0) {
        set_error(m, "Can't play this video", text && *text ? text : "Only MP4 with H.264 is supported.");
    } else if (strcmp(reason, VIDEO_OPENFAIL_CORRUPT) == 0) {
        set_error(m, "This file is damaged", text);
    } else if (strcmp(reason, VIDEO_OPENFAIL_DEVICE) == 0) {
        set_error(m, "Video decoder not available", text);
    } else {
        set_error(m, "The file could not be read", text);
    }
}

unsigned video_model_event(struct video_model *m, const struct video_event *ev)
{
    if (!in_player(m)) {
        return 0; /* a helper being abandoned says its last words */
    }
    if (m->status == VIDEO_ST_ERROR && ev->kind != VIDEO_EV_EXITED) {
        return 0;
    }
    switch (ev->kind) {
    case VIDEO_EV_OPENED:
        if (m->status != VIDEO_ST_OPENING) {
            return 0;
        }
        m->duration_ms = ev->ms;
        m->src_w = ev->w;
        m->src_h = ev->h;
        m->fps_x100 = ev->fps_x100;
        snprintf(m->audio, sizeof(m->audio), "%s", ev->word);
        m->status = VIDEO_ST_PAUSED;
        return m->want_play ? VIDEO_ACT_PLAY : 0;
    case VIDEO_EV_OPENFAIL:
        openfail_text(m, ev->word, ev->text);
        return 0;
    case VIDEO_EV_FRAME:
        m->frames++;
        if (!m->seek_inflight) {
            m->pos_ms = clamp(m, ev->ms);
        }
        return 0;
    case VIDEO_EV_STATE:
        if (m->status == VIDEO_ST_OPENING) {
            return 0;
        }
        m->status = ev->value == VIDEO_PLAY_PLAYING   ? VIDEO_ST_PLAYING
                    : ev->value == VIDEO_PLAY_STOPPED ? VIDEO_ST_STOPPED
                    : ev->value == VIDEO_PLAY_ENDED   ? VIDEO_ST_ENDED
                                                      : VIDEO_ST_PAUSED;
        if (ev->value == VIDEO_PLAY_ENDED || ev->value == VIDEO_PLAY_STOPPED) {
            m->want_play = false;
        }
        if (!m->seek_inflight) {
            m->pos_ms = clamp(m, ev->ms);
        }
        return 0;
    case VIDEO_EV_POS:
        if (!m->seek_inflight) {
            m->pos_ms = clamp(m, ev->ms);
        }
        return 0;
    case VIDEO_EV_SEEKED:
        if (!m->seek_waiting) {
            m->pos_ms = clamp(m, ev->ms);
        }
        return seek_answered(m);
    case VIDEO_EV_AUDIO:
        snprintf(m->audio, sizeof(m->audio), "%s", ev->word);
        return 0;
    case VIDEO_EV_STATS:
        m->stats = ev->stats;
        m->have_stats = true;
        return 0;
    case VIDEO_EV_ERROR:
        set_error(m, strcmp(ev->word, "device") == 0 ? "Video decoder failed" : "Playback failed",
                  ev->text);
        return 0;
    case VIDEO_EV_EXITED:
        if (m->status == VIDEO_ST_ERROR) {
            return 0; /* already said */
        }
        if (ev->reason == VIDEO_EXIT_HUNG) {
            set_error(m, "The player stopped responding", "It was stopped. Try the file again.");
        } else if (ev->reason == VIDEO_EXIT_CRASHED) {
            set_error(m, "The player crashed", "The file may be damaged.");
        } else if (ev->reason == VIDEO_EXIT_PROTOCOL) {
            set_error(m, "The player misbehaved", "It was stopped.");
        } else {
            set_error(m, "The player ended", "");
        }
        return 0;
    }
    return 0;
}

unsigned video_model_start_failed(struct video_model *m, const char *why)
{
    set_error(m, "The player could not start", why);
    return 0;
}

/* ---- what to show ------------------------------------------------------------- */

int64_t video_model_shown_pos(const struct video_model *m)
{
    return m->dragging ? m->drag_ms : m->pos_ms;
}

const char *video_model_status_text(const struct video_model *m)
{
    switch (m->status) {
    case VIDEO_ST_OPENING:
        return "Opening...";
    case VIDEO_ST_PLAYING:
        return "Playing";
    case VIDEO_ST_PAUSED:
        return "Paused";
    case VIDEO_ST_STOPPED:
        return "Stopped";
    case VIDEO_ST_ENDED:
        return "Ended";
    case VIDEO_ST_ERROR:
        return m->error_title;
    default:
        return "";
    }
}

const char *video_model_audio_text(const struct video_model *m)
{
    if (strcmp(m->audio, VIDEO_AUDIO_NONE) == 0) {
        return "No sound";
    }
    if (strcmp(m->audio, VIDEO_AUDIO_MUTED) == 0) {
        return "Muted";
    }
    if (strcmp(m->audio, VIDEO_AUDIO_BUSY) == 0) {
        return "Sound busy";
    }
    if (strcmp(m->audio, VIDEO_AUDIO_UNSUPPORTED) == 0) {
        return "Sound not supported";
    }
    if (strcmp(m->audio, VIDEO_AUDIO_ERROR) == 0) {
        return "Sound failed";
    }
    return "";
}
