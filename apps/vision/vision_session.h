/*
 * Vision's link to its helper: one pos-vision process for as long as the
 * Vision screen is open, driven without ever blocking the LVGL thread.
 *
 * The same shape as Camera's (apps/camera/camera_session.h, ADR-006): the
 * helper is the only thing that opens the camera or the detector; pictures
 * come through sealed shared memory and are copied out, never pointed at;
 * every wait on the helper has a deadline, and a missed one kills it; the
 * helper leaves with the shell (PR_SET_PDEATHSIG). What Vision adds are the
 * detection, count and statistics lines (pocketvision_proto.h), read into
 * plain structs the screen draws from.
 *
 * Pure C, no LVGL, clock passed in: tested on a host against the real
 * helper with the fake camera and the fake detector
 * (tests/vision_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef VISION_SESSION_H
#define VISION_SESSION_H

#include "pocketvision/pocketvision.h"
#include "pocketvision/pocketvision_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VISION_HELLO_MS 3000
/* Opening the camera and loading the model (a 3.5 MB kmodel read and
 * checked by the runtime; unmeasured on the C908 until the gate). */
#define VISION_OPEN_MS 15000
#define VISION_SILENCE_MS 4000
#define VISION_REPLY_MS 3000
#define VISION_KILL_REAP_MS 200
#define VISION_EVENT_QUEUE 32
#define VISION_EVENT_TEXT_MAX 96
#define VISION_HELPER_PATH_MAX 256
#define VISION_ARG_MAX 256
#define VISION_NAME_MAX 48

enum vision_ev_kind {
    VISION_EV_READY,     /* text = camera name, name = model, w x h = preview, value = classes */
    VISION_EV_NODEVICE,  /* text */
    VISION_EV_NOMODEL,   /* text */
    VISION_EV_ERROR,     /* text = "<what> <text>" */
    VISION_EV_FRAME,     /* a preview picture is waiting: vision_session_take_frame() */
    VISION_EV_DET,       /* the tracks after frame value=seq: vision_session_tracks() */
    VISION_EV_COUNT,     /* counts changed: vision_session_counts() */
    VISION_EV_TRAFFIC,   /* the traffic report changed: vision_session_traffic() */
    VISION_EV_COLOR,     /* a COLOR pass: vision_session_pixels()->color */
    VISION_EV_EDGE,      /* an EDGE pass: ->edge_pm */
    VISION_EV_TRACE,     /* a TRACE pass: ->trace */
    VISION_EV_STATS,     /* vision_session_stats() */
    VISION_EV_MALFORMED, /* value = damaged frames or bad tensors in a row */
    VISION_EV_STALL,     /* value = ms without a frame */
    VISION_EV_STOPPED,
    VISION_EV_LOST,      /* text */
    VISION_EV_EXITED     /* reason; value = exit code, or 128 + signal */
};

enum vision_exit {
    VISION_EXIT_NORMAL,
    VISION_EXIT_HUNG,
    VISION_EXIT_PROTOCOL,
    VISION_EXIT_CRASHED
};

struct vision_event {
    enum vision_ev_kind kind;
    int value;
    int reason;           /* enum vision_exit */
    bool simulated;
    uint32_t w;
    uint32_t h;
    char name[VISION_NAME_MAX];
    char text[VISION_EVENT_TEXT_MAX];
};

/* One tracked object, in view pixels. */
struct vision_shown {
    uint32_t id;          /* 0: not yet confirmed */
    uint16_t cls;
    uint16_t conf;        /* per-mille */
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    uint8_t dir;          /* VISION_DIR_*: the way it has gone on the picture */
    uint32_t kmh10;       /* the speed measured on it, x10; 0 for none */
};

/* What the helper's `traffic` line said. */
struct vision_traffic_report {
    uint32_t total_ab;    /* IN */
    uint32_t total_ba;    /* OUT */
    uint32_t cur_kmh10;   /* the newest speed while its track lives, or 0 */
    uint32_t last_kmh10;
    uint32_t max_kmh10;
    uint32_t mean_kmh10;
    uint32_t n;           /* measurements */
    uint32_t rejected;
    uint32_t cls_ab[VISION_PROTO_TRAFFIC_CLASSES];
    uint32_t cls_ba[VISION_PROTO_TRAFFIC_CLASSES];
};

/* What the pixel modes' lines said (color, edge, trace). */
struct vision_pixel_report {
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint32_t matched_pm;  /* of the picture */
        int32_t cx;           /* the matches' centroid in view pixels, -1 none */
        int32_t cy;
    } color;
    uint32_t edge_pm;         /* strong edges, per-mille of the picture */
    struct {
        bool found;
        int32_t offset_pm;
        int32_t slope_pm;
        uint32_t rows;
    } trace;
};

struct vision_stats {
    uint32_t fps_x10;
    int infer_ms;
    int pre_ms;
    int post_ms;
    int cpu_pct;
    long rss_kb;
    uint32_t bad;
    uint32_t dropped;
};

struct vision_session_config {
    const char *helper;   /* NULL: vision_session_helper_path() */
    const char *backend;  /* NULL: vision_session_backend() */
    const char *fake;     /* the fake camera's script, or NULL */
    const char *config;   /* the camera config, or NULL for the helper's default */
    const char *model;    /* the model file, or NULL for the helper's default */
    const char *kpu;      /* the fake detector's script, or NULL */
};

struct vision_session {
    pid_t pid;
    int fd;
    int shm_fd;
    const uint8_t *shm;
    bool running;
    bool eof;
    bool killed;
    enum vision_exit exit_reason;
    char line[VISION_LINE_MAX];
    size_t line_len;
    bool overlong;

    int64_t hello_by;
    int64_t open_by;
    int64_t silence_by;
    int64_t reply_by;
    bool streaming;

    int frame_slot;       /* -1: none */
    uint32_t frame_w;
    uint32_t frame_h;
    bool frame_queued;

    /* the newest det line, counts and stats */
    struct vision_shown shown[VISION_MAX_SHOWN];
    int shown_count;
    uint32_t shown_seq;
    uint32_t count_ab;
    uint32_t count_ba;
    struct vision_traffic_report traffic;
    struct vision_pixel_report pixels;
    struct vision_stats stats;

    struct vision_event queue[VISION_EVENT_QUEUE];
    int q_head;
    int q_count;
    unsigned dropped;
};

void vision_session_init(struct vision_session *s);

int vision_session_start(struct vision_session *s, const struct vision_session_config *cfg,
                         int64_t now_ms, char *err, size_t errlen);

/* Read what the helper wrote, enforce the deadlines, reap it when it has
 * gone, and hand back one event. 1 with *ev filled, 0 when there is nothing.
 * Never blocks. */
int vision_session_poll(struct vision_session *s, struct vision_event *ev, int64_t now_ms);

int vision_session_view(struct vision_session *s, uint32_t w, uint32_t h, int display_rotation);
int vision_session_stream(struct vision_session *s, bool on, int64_t now_ms);
/* The counting line in per-mille of the view; a NULL pm turns it off. */
int vision_session_line(struct vision_session *s, const int32_t pm[4]);
/* The two speed lines, A then B, per-mille of the view; NULL turns them off. */
int vision_session_speed_lines(struct vision_session *s, const int32_t pm[8]);
/* TRAFFIC's region of interest, two opposite corners in per-mille of the
 * view; NULL turns it off (the whole frame). The helper answers with the
 * region it gives the detector, in frame pixels (`roi`). */
int vision_session_roi(struct vision_session *s, const int32_t pm[4]);
/* The ground distance between the speed lines. */
int vision_session_distance(struct vision_session *s, uint32_t cm);
/* Traffic mode on or off. */
int vision_session_mode(struct vision_session *s, bool traffic);
/* The mode by its protocol word: detect, traffic, color, edge, trace. */
int vision_session_mode_word(struct vision_session *s, const char *word);
/* COLOR: the target colour (NULL: none), a sample at a view point, the
 * tolerance; EDGE: the threshold (0 grey); TRACE: a dark or a light line. */
int vision_session_color(struct vision_session *s, const uint8_t rgb[3]);
int vision_session_sample(struct vision_session *s, int32_t x, int32_t y);
int vision_session_tol(struct vision_session *s, uint32_t tol);
int vision_session_edge(struct vision_session *s, uint32_t threshold);
int vision_session_trace(struct vision_session *s, bool dark);
int vision_session_reset(struct vision_session *s);

/* Copy the newest preview picture into dst (w x h RGB565, tightly packed)
 * and give its slot back. 1 when copied, 0 when there was none or it was of
 * another size. */
int vision_session_take_frame(struct vision_session *s, uint16_t *dst, uint32_t w, uint32_t h);

/* What the last det line said: a pointer into the session, valid until the
 * next poll. */
const struct vision_shown *vision_session_tracks(const struct vision_session *s, int *count,
                                                 uint32_t *seq);
void vision_session_counts(const struct vision_session *s, uint32_t *ab, uint32_t *ba);
const struct vision_traffic_report *vision_session_traffic(const struct vision_session *s);
const struct vision_pixel_report *vision_session_pixels(const struct vision_session *s);
const struct vision_stats *vision_session_stats(const struct vision_session *s);

/* Ask the helper to quit, wait up to grace_ms, then SIGKILL and reap for
 * VISION_KILL_REAP_MS. Idle afterwards whatever happened. */
void vision_session_abandon(struct vision_session *s, int grace_ms);

bool vision_session_active(const struct vision_session *s);

/* $POCKETOS_VISION_HELPER, else /usr/bin/pos-vision. */
const char *vision_session_helper_path(void);
/* $POCKETOS_CAMERA_BACKEND, else the build's default: "v4l2" on the device,
 * "fake" in the simulator (CAMERA_BACKEND_DEFAULT, the same as Camera's). */
const char *vision_session_backend(void);

/* Parse one event line (no newline). 1 when it is a known, well-formed
 * event. det, count and stats lines are parsed into `s` when it is given
 * (NULL: only checked). Exposed for the test. */
int vision_session_parse_line(struct vision_session *s, const char *line, struct vision_event *ev);

#endif
