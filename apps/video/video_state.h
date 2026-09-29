/*
 * The Video app's state machine: what the screen is showing, what each tap
 * does, and what each helper event changes. Pure C: no LVGL, no processes,
 * no clock (tests/video_state_test.c).
 *
 * Every entry point returns a set of VIDEO_ACT_* bits the screen carries out
 * (start or end the helper, send a command, lay out again); the model never
 * does anything itself.
 *
 * INTENT AND TRUTH. The play button follows the user's intent (want_play),
 * so it answers a tap at once; the status line follows what the helper last
 * said. Rapid taps each send their command and the helper applies them in
 * order, so the last tap wins.
 *
 * SEEKS are coalesced: while one is being answered, a newer target replaces
 * any waiting one, and only that is sent when the answer comes. Dragging the
 * progress bar shows the target and seeks once, on release.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VIDEO_STATE_H
#define POCKETOS_VIDEO_STATE_H

#include "video_files.h"
#include "video_session.h"

#include <stdbool.h>
#include <stdint.h>

enum video_status {
    VIDEO_ST_LIST,    /* choosing a file; no helper */
    VIDEO_ST_OPENING, /* helper started, file being opened */
    VIDEO_ST_PAUSED,
    VIDEO_ST_PLAYING,
    VIDEO_ST_STOPPED, /* paused at the start */
    VIDEO_ST_ENDED,
    VIDEO_ST_ERROR    /* the player stays on screen with a message; BACK leaves */
};

enum video_act {
    VIDEO_ACT_START = 1u << 0,   /* start a helper for model.name (view, then open) */
    VIDEO_ACT_ABANDON = 1u << 1, /* end the helper */
    VIDEO_ACT_PLAY = 1u << 2,
    VIDEO_ACT_PAUSE = 1u << 3,
    VIDEO_ACT_STOP = 1u << 4,
    VIDEO_ACT_SEEK = 1u << 5,    /* to model.seek_sent */
    VIDEO_ACT_LAYOUT = 1u << 6,  /* the screen's shape changed (player, list, fullscreen) */
    VIDEO_ACT_RESCAN = 1u << 7,  /* read the folder again */
    VIDEO_ACT_HOME = 1u << 8     /* leave the app */
};

#define VIDEO_TITLE_MAX 48

struct video_model {
    enum video_status status;
    bool fullscreen;
    bool want_play;
    char name[VIDEO_FILES_NAME_MAX];

    int64_t duration_ms;
    int64_t pos_ms;
    uint32_t src_w;
    uint32_t src_h;
    uint32_t fps_x100;
    char audio[VIDEO_WORD_MAX];   /* the sound's word (video_proto.h), "" before open */
    uint64_t frames;              /* pictures put on screen */

    bool dragging;
    int64_t drag_ms;
    bool seek_inflight;
    bool seek_waiting;
    int64_t seek_waiting_ms;
    int64_t seek_sent;

    struct video_stats stats;
    bool have_stats;

    char error_title[VIDEO_TITLE_MAX];
    char error_detail[VIDEO_EVENT_TEXT_MAX];
};

void video_model_init(struct video_model *m);

/* ---- taps --------------------------------------------------------------------- */

/* A file from the list. */
unsigned video_model_choose(struct video_model *m, const char *name);
/* BACK: the player to the list (ending the helper), the list to home. */
unsigned video_model_back(struct video_model *m);
/* PLAY / PAUSE. */
unsigned video_model_toggle(struct video_model *m);
unsigned video_model_stop(struct video_model *m);
unsigned video_model_fullscreen(struct video_model *m);
unsigned video_model_rescan(struct video_model *m);
/* The progress bar: pressed or moved (shows ms), released (seeks there). */
unsigned video_model_drag(struct video_model *m, int64_t ms);
unsigned video_model_drag_end(struct video_model *m, int64_t ms);
/* Seek to ms at once (coalesced). */
unsigned video_model_seek(struct video_model *m, int64_t ms);

/* ---- the helper --------------------------------------------------------------- */

unsigned video_model_event(struct video_model *m, const struct video_event *ev);
/* The helper could not be started (fork, memory). */
unsigned video_model_start_failed(struct video_model *m, const char *why);

/* ---- what to show ------------------------------------------------------------- */

/* The position the screen shows: the drag target while dragging. */
int64_t video_model_shown_pos(const struct video_model *m);
/* Whether the transport controls do anything now. */
bool video_model_can_control(const struct video_model *m);
/* "Playing", "Paused", "Opening...", ... */
const char *video_model_status_text(const struct video_model *m);
/* The sound in a word for the status line, or "" when there is nothing to say. */
const char *video_model_audio_text(const struct video_model *m);

#endif
