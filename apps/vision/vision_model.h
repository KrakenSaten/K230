/*
 * Vision's state on its own: what the screen is showing and what each
 * event and tap changes. No processes, no LVGL, no clock but the one passed
 * in (docs/apps/VISION.md).
 *
 * Five modes. DETECT shows everything the model knows, with one counting
 * line. TRAFFIC shows traffic only - car, truck, bus, motorcycle, bicycle,
 * person - counted per class and in total across the counting line (IN and
 * OUT), each track's direction on its box, and a speed for every track
 * that crosses the two speed lines, from the ground distance the user
 * sets and the time between the crossings. COLOR, EDGE and TRACE look at
 * the picture's pixels instead of the detector: a sampled colour and its
 * matches, the edges, the dominant line. The helper does the work; this
 * keeps the choices and the words.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef VISION_MODEL_H
#define VISION_MODEL_H

#include "vision_session.h"

#include <stdbool.h>
#include <stdint.h>

enum vision_state {
    VISION_INIT = 0,   /* the helper is starting the camera and the model */
    VISION_LIVE,       /* streaming; boxes when there are any */
    VISION_ERROR,      /* something the user can retry */
    VISION_NO_DEVICE,  /* no camera, or no model */
};

enum vision_mode {
    VISION_MODE_DETECT = 0,
    VISION_MODE_TRAFFIC,
    VISION_MODE_COLOR,
    VISION_MODE_EDGE,
    VISION_MODE_TRACE,
    VISION_MODES
};

/* The counting line's place: a choice the user cycles through. */
enum vision_line_mode {
    VISION_LINE_OFF = 0,
    VISION_LINE_ACROSS,   /* horizontal, mid-height: counts DOWN and UP */
    VISION_LINE_DOWN,     /* vertical, mid-width: counts LEFT and RIGHT */
    VISION_LINE_MODES
};

/* The speed lines: none, or a pair either side of the middle, parallel to
 * the counting line, closer or further apart. */
enum vision_speed_mode {
    VISION_SPEED_OFF = 0,
    VISION_SPEED_NARROW,  /* at 40 % and 60 % */
    VISION_SPEED_WIDE,    /* at 25 % and 75 % */
    VISION_SPEED_MODES
};

/* The ground distances the user cycles through, in centimetres. */
#define VISION_DISTANCES 8
#define VISION_DISTANCE_DEFAULT 3  /* 10 m */
/* The colour tolerances: LOW, MED, HIGH. */
#define VISION_TOLS 3
#define VISION_TOL_DEFAULT 1
/* EDGE's hard threshold. */
#define VISION_EDGE_HARD_THRESHOLD 40

/* What a tap or an event asks the screen to do (a bit set). */
#define VISION_ACT_OPEN      0x01u  /* start the helper */
#define VISION_ACT_CLOSE     0x02u  /* end it */
#define VISION_ACT_STREAM    0x04u  /* send view and start */
#define VISION_ACT_LINE      0x08u  /* send the line */
#define VISION_ACT_RESET     0x10u  /* send reset */
#define VISION_ACT_MODE      0x20u  /* send the mode (the layout changes too) */
#define VISION_ACT_SPEED     0x40u  /* send the speed lines */
#define VISION_ACT_DISTANCE  0x80u  /* send the distance */
#define VISION_ACT_PIXELS    0x100u /* send the pixel modes' settings (colour, tolerance, edge, trace) */
#define VISION_ACT_SAMPLE    0x200u /* send a colour sample at vision_model_sample_point */

/* The buttons, by role; the order on screen is per mode
 * (vision_model_buttons). */
enum vision_button {
    VISION_BTN_MODE = 0,
    VISION_BTN_LINE,
    VISION_BTN_SPEED,
    VISION_BTN_DISTANCE,
    VISION_BTN_RESET,
    VISION_BTN_SAMPLE,
    VISION_BTN_TOL,
    VISION_BTN_EDGE,
    VISION_BTN_TRACE,
    VISION_BUTTONS
};

struct vision_model {
    enum vision_state state;
    bool simulated;
    bool live;            /* a picture has arrived */
    bool stalled;
    char camera[VISION_NAME_MAX];
    char model_name[VISION_NAME_MAX];
    uint32_t preview_w;
    uint32_t preview_h;
    uint32_t classes;
    char error[VISION_EVENT_TEXT_MAX];
    enum vision_mode mode;
    enum vision_line_mode line;
    enum vision_line_mode orient;  /* the last line that was not OFF: the speed lines' way */
    enum vision_speed_mode speed;
    int distance_idx;
    int tol_idx;
    bool edge_hard;
    bool trace_dark;
    bool have_target;     /* COLOR: a colour has been sampled */
    int32_t sample_x;     /* a sample asked for, in view pixels; -1 none */
    int32_t sample_y;
    uint32_t count_a;     /* into side A: DOWN or LEFT... see vision_model_count_names */
    uint32_t count_b;
    struct vision_traffic_report traffic;
    bool traffic_valid;
    struct vision_pixel_report pixels;
    bool color_valid;
    bool edge_valid;
    bool trace_valid;
    int active_tracks;    /* confirmed tracks on the last det line */
    struct vision_stats stats;
    bool stats_valid;
    int64_t last_frame_ms;
    uint32_t bad_tensors;
};

struct vision_view_text {
    const char *title;    /* the panel in the picture's place, or "" */
    const char *detail;
    const char *status;   /* the line(s) under the picture */
    const char *hint;     /* the header's right end: SIMULATED, or "" */
    const char *mode_btn; /* DETECT / TRAFFIC / COLOR / EDGE / TRACE */
    const char *line_btn; /* LINE: OFF / ACROSS / DOWN */
    const char *speed_btn;
    const char *tol_btn;
    const char *edge_btn;
    const char *trace_btn;
    char dist_btn[24];
    char count_a[32];     /* "DOWN 3", "IN (DOWN) 3", or "-" */
    char count_b[32];
    bool status_warn;
    bool show_picture;
    bool show_retry;      /* TRY AGAIN (ERROR) or CHECK AGAIN (NO_DEVICE) */
    bool line_enabled;    /* the buttons take taps */
    bool traffic;         /* TRAFFIC mode: SPEED and DIST are shown */
    bool lines;           /* the counting and speed lines are drawn (DETECT, TRAFFIC) */
    bool picture_tap;     /* COLOR: a tap on the picture samples a colour */
    bool show_mark;       /* COLOR: the mark at the matches' centroid */
    int32_t mark_x;
    int32_t mark_y;
};

void vision_model_init(struct vision_model *m);

/* Opening the screen, or Try again: what to do. */
unsigned vision_model_open(struct vision_model *m);
unsigned vision_model_event(struct vision_model *m, const struct vision_event *ev,
                            const struct vision_session *s, int64_t now_ms);
/* The once-a-tick check: a stalled preview. Returns true when the text
 * changed. */
bool vision_model_tick(struct vision_model *m, int64_t now_ms);
unsigned vision_model_mode_next(struct vision_model *m);
unsigned vision_model_line_next(struct vision_model *m);
unsigned vision_model_speed_next(struct vision_model *m);
unsigned vision_model_distance_next(struct vision_model *m);
unsigned vision_model_reset(struct vision_model *m);
/* COLOR: sample at a view point (the SAMPLE button samples the middle,
 * vision_model_sample_middle), cycle the tolerance; EDGE: soft / hard;
 * TRACE: dark / light. */
unsigned vision_model_sample_at(struct vision_model *m, int32_t x, int32_t y);
unsigned vision_model_sample_middle(struct vision_model *m, uint32_t view_w, uint32_t view_h);
unsigned vision_model_tol_next(struct vision_model *m);
unsigned vision_model_edge_next(struct vision_model *m);
unsigned vision_model_trace_next(struct vision_model *m);

/* The line's endpoints for its mode, per-mille of the view; false for OFF. */
bool vision_model_line_pm(const struct vision_model *m, int32_t pm[4]);
/* The speed lines' endpoints, A then B, per-mille; false for OFF. */
bool vision_model_speed_pm(const struct vision_model *m, int32_t pm[8]);
/* The distance chosen, in centimetres. */
uint32_t vision_model_distance_cm(const struct vision_model *m);
/* The colour tolerance chosen (the sum of three 8-bit differences). */
uint32_t vision_model_tol(const struct vision_model *m);
/* EDGE's threshold to send: 0 for grey, VISION_EDGE_HARD_THRESHOLD for hard. */
uint32_t vision_model_edge_threshold(const struct vision_model *m);
/* The helper's word for the mode. */
const char *vision_model_mode_word(const struct vision_model *m);
/* Whether the mode looks at pixels (COLOR, EDGE, TRACE). */
bool vision_model_pixel_mode(const struct vision_model *m);
/* The names of the two directions counted in this mode ("DOWN"/"UP" or
 * "LEFT"/"RIGHT"), and which count is which. */
void vision_model_count_names(const struct vision_model *m, const char **a, const char **b);
/* The buttons shown in this mode, in order; returns how many. */
int vision_model_buttons(const struct vision_model *m, enum vision_button out[VISION_BUTTONS]);
/* How many lines the status takes in this mode. */
int vision_model_status_lines(const struct vision_model *m);
/* A short name for traffic class i (car truck bus moto bike person). */
const char *vision_model_traffic_name(int i);

void vision_model_text(const struct vision_model *m, struct vision_view_text *out, char *status_buf,
                       size_t status_len);

#endif
