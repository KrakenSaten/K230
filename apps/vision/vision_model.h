/*
 * Vision's state on its own: what the screen is showing and what each
 * event and tap changes. No processes, no LVGL, no clock but the one passed
 * in (docs/apps/VISION.md).
 *
 * THE MODES come in groups (vision_settings.h): GENERAL (DETECT, TRACK),
 * ROAD (TRAFFIC), PEOPLE (FACE, RECOGNIZE), TEXT (READ) and TOOLS (COLOR,
 * EDGE, LINE TRACE). DETECT boxes every object the detector knows; TRACK
 * adds ids and a counting line; TRAFFIC looks for traffic only, counts it
 * per class and times it between two speed lines; the TOOLS look at the
 * picture's pixels instead of the detector. A mode is offered only when the
 * helper says it can run it (its `caps` line): a mode whose model is not on
 * the unit never appears, and a group with nothing to offer is not shown.
 *
 * THE SHEET is a panel over the picture with up to VISION_SHEET_ROWS rows of
 * up to VISION_SHEET_CELLS choices each. MODE opens it as the mode picker
 * (a row per group); SETUP opens it as a mode's own settings (Traffic's
 * line, speed lines and distance). The model says what the cells are and
 * takes a tap by the cell's code; the screen only draws them.
 *
 * The choices live in a struct vision_settings, per mode; a tap that
 * changes one asks the screen to store them (VISION_ACT_SAVE).
 *
 * THE DETECTOR FILES: the image carries R0 (from 0.3.6, an experimental
 * beta), and a bench unit may be given UPSTREAM by hand for an A/B
 * comparison (vision_settings.h). When either is there, the detector's modes
 * get a MODEL button that names the detector in force. It takes a tap only
 * when the other file is there too: a tap asks for the other one, which is
 * loaded by ending the helper and starting it again on that file
 * (VISION_ACT_OPEN). Only one detector is ever open. The new one is in force
 * - and stored - only once the helper says `ready` with it; a file that is
 * missing or does not load brings the previous one back and says why, and a
 * stored choice whose file has gone opens the one that is there. Without
 * either file nothing here changes: the helper opens its own default as
 * before.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef VISION_MODEL_H
#define VISION_MODEL_H

#include "vision_session.h"
#include "vision_settings.h"

#include <stdbool.h>
#include <stdint.h>

enum vision_state {
    VISION_INIT = 0,   /* the helper is starting the camera and the model */
    VISION_LIVE,       /* streaming; boxes when there are any */
    VISION_ERROR,      /* something the user can retry */
    VISION_NO_DEVICE,  /* no camera, or no model */
};

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
#define VISION_ACT_SAVE      0x400u /* the settings changed: store them */
#define VISION_ACT_RANGE     0x800u /* send TRAFFIC's detection range */
#define VISION_ACT_ENROL     0x1000u /* send enrol, or enrol off (vision_model.enrolling) */
#define VISION_ACT_FORGET    0x2000u /* send forget */

/* The buttons, by role; the order on screen is per mode
 * (vision_model_buttons). */
enum vision_button {
    VISION_BTN_MODE = 0,
    VISION_BTN_LINE,
    VISION_BTN_TRAILS,
    VISION_BTN_SETUP,
    VISION_BTN_RESET,
    VISION_BTN_SAMPLE,
    VISION_BTN_TOL,
    VISION_BTN_EDGE,
    VISION_BTN_TRACE,
    VISION_BTN_HOLD,
    VISION_BTN_ENROL,
    VISION_BTN_FORGET,
    VISION_BTN_MODEL,
    VISION_BUTTONS
};

enum vision_sheet {
    VISION_SHEET_NONE = 0,
    VISION_SHEET_MODES,   /* the mode picker */
    VISION_SHEET_SETUP,   /* the mode's own settings */
};

#define VISION_SHEET_ROWS 5
#define VISION_SHEET_CELLS 3
#define VISION_SHEET_TEXT 24

struct vision_sheet_cell {
    char text[VISION_SHEET_TEXT];
    bool selected;        /* the choice in force: drawn as the primary button */
    bool enabled;
    int code;             /* what vision_model_sheet_tap is given for it */
};

struct vision_sheet_row {
    const char *caption;
    int cells;
    struct vision_sheet_cell cell[VISION_SHEET_CELLS];
};

struct vision_sheet_view {
    enum vision_sheet kind;
    const char *title;
    int rows;
    struct vision_sheet_row row[VISION_SHEET_ROWS];
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
    struct vision_settings set;
    enum vision_mode mode;  /* the mode in force: set.mode when it is offered, else DETECT */
    uint32_t avail;         /* bit per mode the helper can run */
    enum vision_sheet sheet;
    bool have_target;     /* COLOR: a colour has been sampled */
    int32_t sample_x;     /* a sample asked for, in view pixels; -1 none */
    int32_t sample_y;
    uint32_t count_a;     /* into side A: DOWN or LEFT... see vision_model_count_names */
    uint32_t count_b;
    struct vision_traffic_report traffic;
    bool traffic_valid;
    struct vision_recent_report recent;
    bool recent_valid;
    struct vision_text_report text;   /* READ: the last read (kept while held) */
    bool text_valid;
    bool hold;                        /* READ: the result stays as it is */
    char readfail[VISION_EVENT_TEXT_MAX]; /* READ cannot read: why; "" when it can */
    char facefail[VISION_EVENT_TEXT_MAX]; /* FACE cannot look: why; "" when it can */
    uint32_t faces_seen;  /* FACE: faces that got an id since the mode began or RESET */
    uint32_t face_top_id; /* the highest id counted; the helper's ids only grow */
    /* RECOGNIZE */
    struct vision_who_report who;     /* the last scores */
    bool who_valid;
    bool owner_known;                 /* the helper has said whether there is an owner */
    bool have_owner;
    uint32_t owner_views;
    bool enrolling;
    uint32_t enrol_k;
    uint32_t enrol_n;
    char enrolfail[VISION_EVENT_TEXT_MAX];
    char recogfail[VISION_EVENT_TEXT_MAX];
    bool forget_armed;                /* FORGET asked once: the next tap forgets */
    int64_t forget_armed_ms;
    struct vision_pixel_report pixels;
    bool color_valid;
    bool edge_valid;
    bool trace_valid;
    int active_tracks;    /* confirmed tracks on the last det line */
    int shown_objects;    /* boxes on the last det line */
    int shown_classes;    /* distinct classes among them */
    struct vision_stats stats;
    bool stats_valid;
    int64_t last_frame_ms;
    uint32_t bad_tensors;
    /* THE DETECTOR A/B (none of it used when det_files is 0) */
    uint32_t det_files;   /* bit per enum vision_detector whose file is on the unit */
    int det_active;       /* the detector the running helper said ready with; -1 none */
    int det_loading;      /* the detector the helper was started on; -1 its default */
    int det_fallback;     /* where to go back when det_loading fails; -1 nowhere */
    char det_fail[VISION_EVENT_TEXT_MAX + 32]; /* why the last one asked for is not in force */
    int64_t det_fail_ms;  /* when det_fail was shown first; 0 not yet */
};

struct vision_view_text {
    const char *title;    /* the panel in the picture's place, or "" */
    const char *detail;
    const char *status;   /* the line(s) under the picture */
    const char *hint;     /* the header's right end: SIMULATED, or "" */
    const char *mode_btn; /* the mode's name */
    const char *line_btn; /* LINE: OFF / ACROSS / DOWN */
    const char *trails_btn; /* TRAILS: ON / OFF */
    const char *hold_btn;   /* HOLD / HELD */
    bool read;              /* READ: the boxes are the text lines */
    bool faces;             /* FACE: every box is a face */
    bool recog;             /* RECOGNIZE: the boxes say owner or unknown */
    const char *enrol_btn;  /* ENROL / STOP */
    const char *forget_btn; /* FORGET / SURE? */
    const char *model_btn;  /* R0 · Beta / UPSTREAM / LOADING R0 */
    bool model_enabled;     /* MODEL takes taps: live, and a detector to go to */
    bool forget_enabled;    /* there is an owner to forget */
    bool hold;              /* READ held: HOLD is the primary button */
    const char *tol_btn;
    const char *edge_btn;
    const char *trace_btn;
    char count_a[32];     /* "DOWN 3", "IN (DOWN) 3", "OBJECTS 3", or "-" */
    char count_b[32];
    bool status_warn;
    bool show_picture;
    bool show_retry;      /* TRY AGAIN (ERROR) or CHECK AGAIN (NO_DEVICE) */
    bool line_enabled;    /* the buttons take taps */
    bool traffic;         /* TRAFFIC mode */
    bool lines;           /* the counting (and speed) lines are drawn: TRACK, TRAFFIC */
    bool ids;             /* boxes carry their track ids and class */
    bool speeds;          /* boxes carry a measured speed */
    bool trails;          /* where each track has been is drawn */
    bool picture_tap;     /* COLOR: a tap on the picture samples a colour */
    bool show_mark;       /* COLOR: the mark at the matches' centroid */
    int32_t mark_x;
    int32_t mark_y;
};

void vision_model_init(struct vision_model *m);
/* The stored settings, before the screen opens. */
void vision_model_load(struct vision_model *m, const struct vision_settings *s);

/* The A/B detector files on the unit (bit per enum vision_detector), before
 * vision_model_open. 0: no A/B, the helper's own default detector. */
void vision_model_set_detector_files(struct vision_model *m, uint32_t present);
/* The detector the helper is to be started on (VISION_ACT_OPEN), or -1 for
 * the helper's own default. */
int vision_model_detector_to_load(const struct vision_model *m);
/* MODEL: ask for the other detector. The files are looked at again first
 * (present, as for vision_model_set_detector_files). */
unsigned vision_model_model_button(struct vision_model *m, uint32_t present);
/* How long a detector that could not be loaded is said under the picture. */
#define VISION_DET_FAIL_SHOW_MS 10000

/* Opening the screen, or Try again: what to do. */
unsigned vision_model_open(struct vision_model *m);
unsigned vision_model_event(struct vision_model *m, const struct vision_event *ev,
                            const struct vision_session *s, int64_t now_ms);
/* The once-a-tick check: a stalled preview. Returns true when the text
 * changed. */
bool vision_model_tick(struct vision_model *m, int64_t now_ms);

/* Whether the helper can run this mode. */
bool vision_model_offered(const struct vision_model *m, enum vision_mode mode);
/* Switch to a mode (the picker's taps). A mode not offered is refused (0). */
unsigned vision_model_set_mode(struct vision_model *m, enum vision_mode mode);
/* MODE: the picker opens, or closes when it is open. SETUP: the mode's
 * settings, the same. */
unsigned vision_model_mode_button(struct vision_model *m);
unsigned vision_model_setup_button(struct vision_model *m);
unsigned vision_model_sheet_close(struct vision_model *m);
/* The sheet's rows as the screen draws them; kind NONE when closed. */
void vision_model_sheet(const struct vision_model *m, struct vision_sheet_view *out);
/* A tap on a sheet cell, by its code. */
unsigned vision_model_sheet_tap(struct vision_model *m, int code);

unsigned vision_model_line_next(struct vision_model *m);
/* TRACK: trails on or off. */
unsigned vision_model_trails_next(struct vision_model *m);
/* READ: hold the result, or let it follow the picture again. */
unsigned vision_model_hold_next(struct vision_model *m);
/* RECOGNIZE: ENROL starts an enrolment of the one face in view, or stops
 * one under way. FORGET asks first: the first tap arms it (SURE?), a
 * second within VISION_FORGET_ARM_MS forgets the owner. */
#define VISION_FORGET_ARM_MS 4000
unsigned vision_model_enrol_button(struct vision_model *m);
unsigned vision_model_forget_button(struct vision_model *m);
/* RECOGNIZE: what the box of track `id` says - "OWNER 91%", "unknown
 * 43%", or "face" before it is compared. */
void vision_model_who_label(const struct vision_model *m, uint32_t id, char *out, size_t len);
/* A line of read text as the screen can show it: printable ASCII, every
 * other character (the fonts have Latin only) a '?'. */
void vision_model_text_ascii(const char *utf8, char *out, size_t len);
/* TRAFFIC's detection range. */
unsigned vision_model_set_range(struct vision_model *m, enum vision_range r);
/* The helper's word for TRAFFIC's range, and its name on the screen. */
const char *vision_model_range_word(const struct vision_model *m);
const char *vision_model_range_name(enum vision_range r);
unsigned vision_model_speed_next(struct vision_model *m);
unsigned vision_model_distance_next(struct vision_model *m);
unsigned vision_model_distance_prev(struct vision_model *m);
unsigned vision_model_reset(struct vision_model *m);
/* COLOR: sample at a view point (the SAMPLE button samples the middle,
 * vision_model_sample_middle), cycle the tolerance; EDGE: soft / hard;
 * TRACE: dark / light. */
unsigned vision_model_sample_at(struct vision_model *m, int32_t x, int32_t y);
unsigned vision_model_sample_middle(struct vision_model *m, uint32_t view_w, uint32_t view_h);
unsigned vision_model_tol_next(struct vision_model *m);
unsigned vision_model_edge_next(struct vision_model *m);
unsigned vision_model_trace_next(struct vision_model *m);

/* The counting line in force: TRACK's or TRAFFIC's own, OFF elsewhere. */
enum vision_line_mode vision_model_line(const struct vision_model *m);
/* The line's endpoints for its mode, per-mille of the view; false for OFF. */
bool vision_model_line_pm(const struct vision_model *m, int32_t pm[4]);
/* The speed lines' endpoints, A then B, per-mille; false for OFF or outside
 * TRAFFIC. */
bool vision_model_speed_pm(const struct vision_model *m, int32_t pm[8]);
/* The distance chosen, in centimetres. */
uint32_t vision_model_distance_cm(const struct vision_model *m);
/* The colour tolerance chosen (the sum of three 8-bit differences). */
uint32_t vision_model_tol(const struct vision_model *m);
/* EDGE's threshold to send: 0 for grey, VISION_EDGE_HARD_THRESHOLD for hard. */
uint32_t vision_model_edge_threshold(const struct vision_model *m);
/* The helper's word for the mode in force. */
const char *vision_model_mode_word(const struct vision_model *m);
/* Whether the mode looks at pixels (COLOR, EDGE, TRACE). */
bool vision_model_pixel_mode(const struct vision_model *m);
/* The names of the two directions counted in this mode ("DOWN"/"UP" or
 * "LEFT"/"RIGHT"), and which count is which. */
void vision_model_count_names(const struct vision_model *m, const char **a, const char **b);
/* The buttons shown in this mode, in order; returns how many. */
int vision_model_buttons(const struct vision_model *m, enum vision_button out[VISION_BUTTONS]);
/* The detector's modes (DETECT, TRACK, TRAFFIC) are offered: false when the
 * unit has no detector model (docs/apps/VISION.md "The model"). */
bool vision_model_detector(const struct vision_model *m);
#define VISION_NO_DETECTOR_TEXT "No detector model on this unit: DETECT, TRACK and TRAFFIC are off"
/* How many lines the status takes in this mode. */
int vision_model_status_lines(const struct vision_model *m);
/* A short name for traffic class i (car truck bus moto bike person). */
const char *vision_model_traffic_name(int i);

void vision_model_text(const struct vision_model *m, struct vision_view_text *out, char *status_buf,
                       size_t status_len);

#endif
