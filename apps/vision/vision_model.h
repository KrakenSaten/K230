/*
 * Vision's state on its own: what the screen is showing and what each
 * event and tap changes. No processes, no LVGL, no clock but the one passed
 * in (docs/apps/VISION.md).
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

/* The counting line's place: a choice the user cycles through. */
enum vision_line_mode {
    VISION_LINE_OFF = 0,
    VISION_LINE_ACROSS,   /* horizontal, mid-height: counts DOWN and UP */
    VISION_LINE_DOWN,     /* vertical, mid-width: counts LEFT and RIGHT */
    VISION_LINE_MODES
};

/* What a tap or an event asks the screen to do (a bit set). */
#define VISION_ACT_OPEN      0x01u  /* start the helper */
#define VISION_ACT_CLOSE     0x02u  /* end it */
#define VISION_ACT_STREAM    0x04u  /* send view and start */
#define VISION_ACT_LINE      0x08u  /* send the line */
#define VISION_ACT_RESET     0x10u  /* send reset */

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
    enum vision_line_mode line;
    uint32_t count_a;     /* into side A: DOWN or LEFT... see vision_model_count_names */
    uint32_t count_b;
    struct vision_stats stats;
    bool stats_valid;
    int64_t last_frame_ms;
    uint32_t bad_tensors;
};

struct vision_view_text {
    const char *title;    /* the panel in the picture's place, or "" */
    const char *detail;
    const char *status;   /* the line under the picture */
    const char *hint;     /* the header's right end: SIMULATED, or "" */
    const char *line_btn; /* LINE: OFF / ACROSS / DOWN */
    bool status_warn;
    bool show_picture;
    bool show_retry;      /* TRY AGAIN (ERROR) or CHECK AGAIN (NO_DEVICE) */
    bool line_enabled;    /* LINE and RESET buttons take taps */
};

void vision_model_init(struct vision_model *m);

/* Opening the screen, or Try again: what to do. */
unsigned vision_model_open(struct vision_model *m);
unsigned vision_model_event(struct vision_model *m, const struct vision_event *ev,
                            const struct vision_session *s, int64_t now_ms);
/* The once-a-tick check: a stalled preview. Returns true when the text
 * changed. */
bool vision_model_tick(struct vision_model *m, int64_t now_ms);
unsigned vision_model_line_next(struct vision_model *m);
unsigned vision_model_reset(struct vision_model *m);

/* The line's endpoints for its mode, per-mille of the view; false for OFF. */
bool vision_model_line_pm(const struct vision_model *m, int32_t pm[4]);
/* The names of the two directions counted in this mode ("DOWN"/"UP" or
 * "LEFT"/"RIGHT"), and which count is which. */
void vision_model_count_names(const struct vision_model *m, const char **a, const char **b);

void vision_model_text(const struct vision_model *m, struct vision_view_text *out, char *status_buf,
                       size_t status_len);

#endif
