/*
 * Vision's state. See vision_model.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_model.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VISION_STALL_AFTER_MS 2000

/* Sheet cell codes (vision_model_sheet_tap). */
#define CODE_MODE 100      /* + enum vision_mode */
#define CODE_LINE 200      /* + enum vision_line_mode */
#define CODE_SPEED 210     /* + enum vision_speed_mode */
#define CODE_DIST_PREV 220
#define CODE_DIST_SHOW 221
#define CODE_DIST_NEXT 222
#define CODE_LABELS 230
#define CODE_SPEEDS 231
#define CODE_TRAILS 232
#define CODE_RANGE 240     /* + enum vision_range */

static const char *const tol_names[VISION_TOLS] = { "TOL: LOW", "TOL: MED", "TOL: HIGH" };
static const char *const range_names[VISION_RANGES] = { "NEAR", "NORMAL", "FAR" };
static const char *const range_words[VISION_RANGES] = { "near", "normal", "far" };
static const char *const traffic_names[VISION_PROTO_TRAFFIC_CLASSES] = {
    "car", "truck", "bus", "moto", "bike", "person",
};
static const char *const line_names[VISION_LINE_MODES] = { "OFF", "ACROSS", "DOWN" };
static const char *const speed_names[VISION_SPEED_MODES] = { "OFF", "NARROW", "WIDE" };

/* What a helper with a detector runs, when it has not said otherwise. */
#define AVAIL_DETECTOR ((1u << VISION_MODE_DETECT) | (1u << VISION_MODE_TRACK) | (1u << VISION_MODE_TRAFFIC) | \
                        (1u << VISION_MODE_COLOR) | (1u << VISION_MODE_EDGE) | (1u << VISION_MODE_TRACE))

void vision_model_init(struct vision_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = VISION_INIT;
    vision_settings_defaults(&m->set);
    m->mode = m->set.mode;
    m->avail = AVAIL_DETECTOR;
    m->sample_x = -1;
    m->sample_y = -1;
}

void vision_model_load(struct vision_model *m, const struct vision_settings *s)
{
    m->set = *s;
    vision_settings_sanitize(&m->set);
    m->mode = m->set.mode;
}

unsigned vision_model_open(struct vision_model *m)
{
    struct vision_model keep = *m;

    vision_model_init(m);
    m->set = keep.set;
    m->mode = keep.set.mode;
    /* A sheet the owner opened stays open while Vision starts (an error
     * closes it). */
    m->sheet = keep.sheet;
    return VISION_ACT_OPEN;
}

static void fail(struct vision_model *m, enum vision_state st, const char *text)
{
    m->state = st;
    m->live = false;
    m->stalled = false;
    m->sheet = VISION_SHEET_NONE;
    snprintf(m->error, sizeof(m->error), "%s", text ? text : "");
}

static void clear_counts(struct vision_model *m)
{
    m->count_a = 0;
    m->count_b = 0;
    memset(&m->traffic, 0, sizeof(m->traffic));
    memset(&m->recent, 0, sizeof(m->recent));
    m->recent_valid = false;
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

bool vision_model_offered(const struct vision_model *m, enum vision_mode mode)
{
    return (int)mode >= 0 && mode < VISION_MODES && (m->avail & (1u << mode)) != 0;
}

/* The mode in force follows the setting when the helper offers it, and
 * falls back to DETECT when it does not (a unit without the text model):
 * the setting itself stays, so the owner's choice comes back with the
 * model. */
static void resolve_mode(struct vision_model *m)
{
    m->mode = vision_model_offered(m, m->set.mode) ? m->set.mode : VISION_MODE_DETECT;
}

/* Everything a new mode leaves behind. */
static void mode_changed(struct vision_model *m)
{
    clear_counts(m);
    clear_pixels(m);
    memset(&m->text, 0, sizeof(m->text));
    m->text_valid = false;
    m->hold = false;
    m->readfail[0] = '\0';
    m->active_tracks = 0;
    m->shown_objects = 0;
    m->shown_classes = 0;
    m->have_target = false;
    m->sample_x = -1;
    m->sample_y = -1;
}

static unsigned when_live(const struct vision_model *m, unsigned acts)
{
    return m->state == VISION_LIVE ? acts : 0;
}

/* The mode and everything that goes with it, for the helper. */
static unsigned mode_acts(const struct vision_model *m)
{
    return VISION_ACT_MODE |
           when_live(m, VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS | VISION_ACT_RANGE);
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
                    VISION_ACT_DISTANCE | VISION_ACT_PIXELS | VISION_ACT_RANGE;
        }
        break;
    case VISION_EV_CAPS:
        if (m->state == VISION_INIT || m->state == VISION_LIVE) {
            enum vision_mode was = m->mode;

            m->avail = (uint32_t)ev->value & ((1u << VISION_MODES) - 1u);
            resolve_mode(m);
            if (m->mode != was) {
                mode_changed(m);
                acts |= mode_acts(m);
            }
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
            int j;
            int active = 0;
            int classes = 0;
            const struct vision_shown *t = vision_session_tracks(s, &n, NULL);

            for (i = 0; i < n; i++) {
                active += t[i].id != 0;
                for (j = 0; j < i && t[j].cls != t[i].cls; j++) {
                }
                classes += j == i;
            }
            m->active_tracks = active;
            m->shown_objects = n;
            m->shown_classes = classes;
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
    case VISION_EV_RECENT:
        if (s) {
            m->recent = *vision_session_recent(s);
            m->recent_valid = true;
        }
        break;
    case VISION_EV_TEXT:
        /* A held result stays: the owner is reading it. */
        if (s && m->mode == VISION_MODE_READ && !m->hold) {
            m->text = *vision_session_text(s);
            m->text_valid = true;
            m->readfail[0] = '\0';
        }
        break;
    case VISION_EV_READFAIL:
        snprintf(m->readfail, sizeof(m->readfail), "%s", ev->text[0] ? ev->text : "the text models failed");
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

unsigned vision_model_set_mode(struct vision_model *m, enum vision_mode mode)
{
    if (!vision_model_offered(m, mode)) {
        return 0;
    }
    m->sheet = VISION_SHEET_NONE;
    if (mode == m->mode && mode == m->set.mode) {
        return 0;
    }
    m->set.mode = mode;
    m->mode = mode;
    /* A new way of looking is a new count, on the helper too. */
    mode_changed(m);
    return mode_acts(m) | VISION_ACT_SAVE;
}

unsigned vision_model_mode_button(struct vision_model *m)
{
    m->sheet = m->sheet == VISION_SHEET_MODES ? VISION_SHEET_NONE : VISION_SHEET_MODES;
    return 0;
}

unsigned vision_model_setup_button(struct vision_model *m)
{
    if (m->mode != VISION_MODE_TRAFFIC) {
        return 0;
    }
    m->sheet = m->sheet == VISION_SHEET_SETUP ? VISION_SHEET_NONE : VISION_SHEET_SETUP;
    return 0;
}

unsigned vision_model_sheet_close(struct vision_model *m)
{
    m->sheet = VISION_SHEET_NONE;
    return 0;
}

enum vision_line_mode vision_model_line(const struct vision_model *m)
{
    switch (m->mode) {
    case VISION_MODE_TRACK:
        return m->set.track.line;
    case VISION_MODE_TRAFFIC:
        return m->set.traffic.line;
    default:
        return VISION_LINE_OFF;
    }
}

static unsigned set_line(struct vision_model *m, enum vision_line_mode line)
{
    if (m->mode == VISION_MODE_TRACK) {
        m->set.track.line = line;
    } else if (m->mode == VISION_MODE_TRAFFIC) {
        m->set.traffic.line = line;
        if (line != VISION_LINE_OFF) {
            m->set.traffic.orient = line;
        }
    } else {
        return 0;
    }
    clear_counts(m);
    /* The speed lines lie the way the counting line does. */
    return when_live(m, VISION_ACT_LINE | VISION_ACT_SPEED) | VISION_ACT_SAVE;
}

unsigned vision_model_line_next(struct vision_model *m)
{
    return set_line(m, (enum vision_line_mode)((vision_model_line(m) + 1) % VISION_LINE_MODES));
}

unsigned vision_model_trails_next(struct vision_model *m)
{
    if (m->mode == VISION_MODE_TRACK) {
        m->set.track.trails = !m->set.track.trails;
    } else if (m->mode == VISION_MODE_TRAFFIC) {
        m->set.traffic.trails = !m->set.traffic.trails;
    } else {
        return 0;
    }
    /* The screen's alone: nothing to tell the helper. */
    return VISION_ACT_SAVE;
}

unsigned vision_model_hold_next(struct vision_model *m)
{
    if (m->mode != VISION_MODE_READ) {
        return 0;
    }
    m->hold = !m->hold;
    return 0;
}

void vision_model_text_ascii(const char *utf8, char *out, size_t len)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)utf8;

    if (!out || len == 0) {
        return;
    }
    while (p && *p && o + 1 < len) {
        if (*p < 0x80) {
            out[o++] = *p >= 0x20 && *p < 0x7f ? (char)*p : '?';
            p++;
            continue;
        }
        /* One character of several bytes: a lead byte and its
         * continuations. */
        out[o++] = '?';
        p++;
        while ((*p & 0xc0) == 0x80) {
            p++;
        }
    }
    out[o] = '\0';
}

unsigned vision_model_set_range(struct vision_model *m, enum vision_range r)
{
    if ((int)r < 0 || r >= VISION_RANGES || r == m->set.traffic.range) {
        return 0;
    }
    m->set.traffic.range = r;
    m->traffic.cur_kmh10 = 0;
    return when_live(m, VISION_ACT_RANGE) | VISION_ACT_SAVE;
}

const char *vision_model_range_word(const struct vision_model *m)
{
    enum vision_range r = m->set.traffic.range;

    return range_words[(int)r >= 0 && r < VISION_RANGES ? r : VISION_RANGE_NORMAL];
}

const char *vision_model_range_name(enum vision_range r)
{
    return range_names[(int)r >= 0 && r < VISION_RANGES ? r : VISION_RANGE_NORMAL];
}

static unsigned set_speed(struct vision_model *m, enum vision_speed_mode speed)
{
    m->set.traffic.speed = speed;
    m->traffic.cur_kmh10 = 0;
    return when_live(m, VISION_ACT_SPEED) | VISION_ACT_SAVE;
}

unsigned vision_model_speed_next(struct vision_model *m)
{
    return set_speed(m, (enum vision_speed_mode)((m->set.traffic.speed + 1) % VISION_SPEED_MODES));
}

static unsigned set_distance(struct vision_model *m, int idx)
{
    m->set.traffic.distance_idx = (idx + VISION_DISTANCES) % VISION_DISTANCES;
    m->traffic.cur_kmh10 = 0;
    return when_live(m, VISION_ACT_DISTANCE) | VISION_ACT_SAVE;
}

unsigned vision_model_distance_next(struct vision_model *m)
{
    return set_distance(m, m->set.traffic.distance_idx + 1);
}

unsigned vision_model_distance_prev(struct vision_model *m)
{
    return set_distance(m, m->set.traffic.distance_idx - 1);
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
    m->set.color.tol_idx = (m->set.color.tol_idx + 1) % VISION_TOLS;
    return when_live(m, VISION_ACT_PIXELS) | VISION_ACT_SAVE;
}

unsigned vision_model_edge_next(struct vision_model *m)
{
    m->set.edge.hard = !m->set.edge.hard;
    return when_live(m, VISION_ACT_PIXELS) | VISION_ACT_SAVE;
}

unsigned vision_model_trace_next(struct vision_model *m)
{
    m->set.trace.dark = !m->set.trace.dark;
    m->trace_valid = false;
    return when_live(m, VISION_ACT_PIXELS) | VISION_ACT_SAVE;
}

/* ---- the sheet -------------------------------------------------------------------- */

static void cell(struct vision_sheet_row *r, const char *text, bool selected, bool enabled, int code)
{
    struct vision_sheet_cell *c;

    if (r->cells >= VISION_SHEET_CELLS) {
        return;
    }
    c = &r->cell[r->cells++];
    snprintf(c->text, sizeof(c->text), "%s", text);
    c->selected = selected;
    c->enabled = enabled;
    c->code = code;
}

static struct vision_sheet_row *row(struct vision_sheet_view *v, const char *caption)
{
    struct vision_sheet_row *r;

    if (v->rows >= VISION_SHEET_ROWS) {
        return NULL;
    }
    r = &v->row[v->rows++];
    memset(r, 0, sizeof(*r));
    r->caption = caption;
    return r;
}

static void sheet_modes(const struct vision_model *m, struct vision_sheet_view *v)
{
    int g;

    v->title = "MODE";
    for (g = 0; g < VISION_GROUPS; g++) {
        struct vision_sheet_row *r = NULL;
        int i;

        for (i = 0; i < VISION_MODES; i++) {
            if (vision_mode_group((enum vision_mode)i) != (enum vision_group)g ||
                !vision_model_offered(m, (enum vision_mode)i)) {
                continue;
            }
            if (!r) {
                r = row(v, vision_group_name((enum vision_group)g));
                if (!r) {
                    return;
                }
            }
            cell(r, vision_mode_name((enum vision_mode)i), m->mode == (enum vision_mode)i, true, CODE_MODE + i);
        }
    }
}

static void sheet_traffic(const struct vision_model *m, struct vision_sheet_view *v)
{
    struct vision_sheet_row *r;
    char dist[VISION_SHEET_TEXT];
    int i;

    v->title = "TRAFFIC SETUP";
    r = row(v, "DETECTION RANGE");
    for (i = 0; i < VISION_RANGES; i++) {
        cell(r, range_names[i], m->set.traffic.range == (enum vision_range)i, true, CODE_RANGE + i);
    }
    r = row(v, "COUNT LINE");
    for (i = 0; i < VISION_LINE_MODES; i++) {
        cell(r, line_names[i], m->set.traffic.line == (enum vision_line_mode)i, true, CODE_LINE + i);
    }
    r = row(v, "SPEED LINES");
    for (i = 0; i < VISION_SPEED_MODES; i++) {
        cell(r, speed_names[i], m->set.traffic.speed == (enum vision_speed_mode)i, true, CODE_SPEED + i);
    }
    r = row(v, "DISTANCE");
    snprintf(dist, sizeof(dist), "%u m", vision_model_distance_cm(m) / 100);
    cell(r, "<", false, true, CODE_DIST_PREV);
    cell(r, dist, true, false, CODE_DIST_SHOW);
    cell(r, ">", false, true, CODE_DIST_NEXT);
    /* Toggles: the choice is on when it is the primary button. */
    r = row(v, "SHOW");
    cell(r, "LABELS", m->set.traffic.labels, true, CODE_LABELS);
    cell(r, "SPEEDS", m->set.traffic.speeds, true, CODE_SPEEDS);
    cell(r, "TRAILS", m->set.traffic.trails, true, CODE_TRAILS);
}

void vision_model_sheet(const struct vision_model *m, struct vision_sheet_view *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = m->state == VISION_LIVE || m->state == VISION_INIT ? m->sheet : VISION_SHEET_NONE;
    out->title = "";
    if (out->kind == VISION_SHEET_MODES) {
        sheet_modes(m, out);
    } else if (out->kind == VISION_SHEET_SETUP && m->mode == VISION_MODE_TRAFFIC) {
        sheet_traffic(m, out);
    } else {
        out->kind = VISION_SHEET_NONE;
    }
}

unsigned vision_model_sheet_tap(struct vision_model *m, int code)
{
    if (code >= CODE_MODE && code < CODE_MODE + VISION_MODES && m->sheet == VISION_SHEET_MODES) {
        return vision_model_set_mode(m, (enum vision_mode)(code - CODE_MODE));
    }
    if (m->sheet != VISION_SHEET_SETUP || m->mode != VISION_MODE_TRAFFIC) {
        return 0;
    }
    if (code >= CODE_LINE && code < CODE_LINE + VISION_LINE_MODES) {
        return set_line(m, (enum vision_line_mode)(code - CODE_LINE));
    }
    if (code >= CODE_SPEED && code < CODE_SPEED + VISION_SPEED_MODES) {
        return set_speed(m, (enum vision_speed_mode)(code - CODE_SPEED));
    }
    if (code == CODE_DIST_PREV) {
        return vision_model_distance_prev(m);
    }
    if (code == CODE_DIST_NEXT) {
        return vision_model_distance_next(m);
    }
    if (code >= CODE_RANGE && code < CODE_RANGE + VISION_RANGES) {
        return vision_model_set_range(m, (enum vision_range)(code - CODE_RANGE));
    }
    if (code == CODE_LABELS) {
        m->set.traffic.labels = !m->set.traffic.labels;
        return VISION_ACT_SAVE;
    }
    if (code == CODE_SPEEDS) {
        m->set.traffic.speeds = !m->set.traffic.speeds;
        return VISION_ACT_SAVE;
    }
    if (code == CODE_TRAILS) {
        return vision_model_trails_next(m);
    }
    return 0;
}

/* ---- what the helper is sent ----------------------------------------------------------- */

bool vision_model_line_pm(const struct vision_model *m, int32_t pm[4])
{
    switch (vision_model_line(m)) {
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

    if (m->mode != VISION_MODE_TRAFFIC) {
        return false;
    }
    switch (m->set.traffic.speed) {
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
    if (m->set.traffic.orient == VISION_LINE_DOWN) {
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
    return vision_distance_cm(m->set.traffic.distance_idx);
}

uint32_t vision_model_tol(const struct vision_model *m)
{
    return vision_tol_value(m->set.color.tol_idx);
}

uint32_t vision_model_edge_threshold(const struct vision_model *m)
{
    return m->set.edge.hard ? VISION_EDGE_HARD_THRESHOLD : 0;
}

const char *vision_model_mode_word(const struct vision_model *m)
{
    return vision_mode_word(m->mode);
}

void vision_model_count_names(const struct vision_model *m, const char **a, const char **b)
{
    if (vision_model_line(m) == VISION_LINE_DOWN) {
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
    case VISION_MODE_TRACK:
        out[1] = VISION_BTN_LINE;
        out[2] = VISION_BTN_TRAILS;
        out[3] = VISION_BTN_RESET;
        return 4;
    case VISION_MODE_TRAFFIC:
        out[1] = VISION_BTN_SETUP;
        out[2] = VISION_BTN_RESET;
        return 3;
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
    case VISION_MODE_READ:
        out[1] = VISION_BTN_HOLD;
        return 2;
    default:
        return 1;
    }
}

int vision_model_status_lines(const struct vision_model *m)
{
    return m->mode == VISION_MODE_TRAFFIC || m->mode == VISION_MODE_READ ? 3 : 1;
}

/* READ's words: what was read, line after line, or why nothing is. */
static void read_status(const struct vision_model *m, char *buf, size_t len)
{
    size_t off = 0;
    int i;

    buf[0] = '\0';
    if (m->readfail[0]) {
        snprintf(buf, len, "Cannot read: %s", m->readfail);
        return;
    }
    if (!m->text_valid) {
        snprintf(buf, len, "%s", m->live ? "Reading..." : "Starting the stream");
        return;
    }
    if (m->text.n == 0) {
        snprintf(buf, len, "No text found. Point at printed text, a sign or a label.");
        return;
    }
    for (i = 0; i < m->text.n && off + 4 < len; i++) {
        char a[VISION_TEXT_BYTES];
        int n;

        vision_model_text_ascii(m->text.line[i].text, a, sizeof(a));
        n = snprintf(buf + off, len - off, "%s%s", i ? "  |  " : "", a);
        if (n < 0) {
            break;
        }
        off += (size_t)n < len - off ? (size_t)n : len - off - 1;
    }
}

const char *vision_model_traffic_name(int i)
{
    return i >= 0 && i < VISION_PROTO_TRAFFIC_CLASSES ? traffic_names[i] : "?";
}

/* ---- the words ---------------------------------------------------------------------- */

static void kmh(char *buf, size_t len, uint32_t kmh10)
{
    snprintf(buf, len, "%u.%u", kmh10 / 10, kmh10 % 10);
}

/* Append to buf at *off, never past len. */
static void add(char *buf, size_t len, size_t *off, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*off >= len) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, len - *off, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *off += (size_t)n < len - *off ? (size_t)n : len - *off - 1;
    }
}

/* TRAFFIC's three lines: the range, the rate and the tracks; the last five
 * minutes' crossings by direction, and their mean speed when the speed lines
 * are on; the same minutes by class. The counters under it are the totals
 * since RESET. */
static void traffic_status(const struct vision_model *m, char *buf, size_t len)
{
    const struct vision_recent_report *r = &m->recent;
    size_t off = 0;
    int i;

    buf[0] = '\0';
    add(buf, len, &off, "%s  ", range_names[m->set.traffic.range < VISION_RANGES ? m->set.traffic.range : 1]);
    if (m->stats_valid) {
        add(buf, len, &off, "%u.%u fps  KPU %d ms  ", m->stats.fps_x10 / 10, m->stats.fps_x10 % 10,
            m->stats.infer_ms);
    } else {
        add(buf, len, &off, "%s  ", m->live ? "Detecting" : "Starting the stream");
    }
    add(buf, len, &off, "%d tracks\n", m->active_tracks);
    if (!m->recent_valid) {
        add(buf, len, &off, "5 min: -\n");
    } else {
        add(buf, len, &off, "%u min: %u  IN %u  OUT %u", r->window_s / 60, r->crossed, r->ab, r->ba);
        if (r->saturated) {
            /* More crossed than the window keeps: it says so. */
            add(buf, len, &off, " (latest only)");
        }
        if (m->set.traffic.speed != VISION_SPEED_OFF) {
            if (r->speeds > 0) {
                char mean[16];

                kmh(mean, sizeof(mean), r->mean_kmh10);
                add(buf, len, &off, "  avg %s km/h (%u)", mean, r->speeds);
            } else {
                add(buf, len, &off, "  no speed yet (%u m)", vision_model_distance_cm(m) / 100);
            }
        }
        add(buf, len, &off, "\n");
    }
    for (i = 0; i < VISION_PROTO_TRAFFIC_CLASSES; i++) {
        add(buf, len, &off, "%s%s %u", i ? "  " : "", traffic_names[i], m->recent_valid ? r->cls[i] : 0u);
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
                     m->set.edge.hard ? "hard" : "soft");
        } else {
            snprintf(buf, len, "%sFinding edges", fps);
        }
        break;
    default:
        if (!m->trace_valid) {
            snprintf(buf, len, "%sLooking for a %s line", fps, m->set.trace.dark ? "dark" : "light");
        } else if (!m->pixels.trace.found) {
            snprintf(buf, len, "%sno %s line  (%u rows)", fps, m->set.trace.dark ? "dark" : "light",
                     m->pixels.trace.rows);
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

static void counters(const struct vision_model *m, struct vision_view_text *out)
{
    const char *na;
    const char *nb;

    vision_model_count_names(m, &na, &nb);
    snprintf(out->count_a, sizeof(out->count_a), "-");
    snprintf(out->count_b, sizeof(out->count_b), "-");
    switch (m->mode) {
    case VISION_MODE_DETECT:
        snprintf(out->count_a, sizeof(out->count_a), "OBJECTS %d", m->shown_objects);
        snprintf(out->count_b, sizeof(out->count_b), "CLASSES %d", m->shown_classes);
        break;
    case VISION_MODE_TRACK:
        if (vision_model_line(m) != VISION_LINE_OFF) {
            snprintf(out->count_a, sizeof(out->count_a), "%s %u", na, m->count_a);
            snprintf(out->count_b, sizeof(out->count_b), "%s %u", nb, m->count_b);
        } else {
            snprintf(out->count_a, sizeof(out->count_a), "TRACKS %d", m->active_tracks);
        }
        break;
    case VISION_MODE_TRAFFIC:
        if (vision_model_line(m) != VISION_LINE_OFF) {
            snprintf(out->count_a, sizeof(out->count_a), "IN (%s) %u", na, m->traffic.total_ab);
            snprintf(out->count_b, sizeof(out->count_b), "OUT (%s) %u", nb, m->traffic.total_ba);
        }
        break;
    case VISION_MODE_READ:
        if (m->text_valid) {
            uint32_t sum = 0;
            int i;

            for (i = 0; i < m->text.n; i++) {
                sum += m->text.line[i].conf;
            }
            snprintf(out->count_a, sizeof(out->count_a), "LINES %d", m->text.n);
            if (m->text.n > 0) {
                snprintf(out->count_b, sizeof(out->count_b), "SURE %u%%", (sum / (uint32_t)m->text.n) / 10);
            }
        }
        break;
    default:
        break;
    }
}

void vision_model_text(const struct vision_model *m, struct vision_view_text *out, char *status_buf,
                       size_t status_len)
{
    enum vision_line_mode line = vision_model_line(m);

    memset(out, 0, sizeof(*out));
    out->hint = m->simulated ? "SIMULATED" : "";
    out->traffic = m->mode == VISION_MODE_TRAFFIC;
    out->lines = m->mode == VISION_MODE_TRACK || m->mode == VISION_MODE_TRAFFIC;
    out->ids = m->mode == VISION_MODE_TRACK || (m->mode == VISION_MODE_TRAFFIC && m->set.traffic.labels);
    out->speeds = m->mode == VISION_MODE_TRAFFIC && m->set.traffic.speeds;
    out->trails = (m->mode == VISION_MODE_TRACK && m->set.track.trails) ||
                  (m->mode == VISION_MODE_TRAFFIC && m->set.traffic.trails);
    out->trails_btn = m->set.track.trails ? "TRAILS: ON" : "TRAILS: OFF";
    out->read = m->mode == VISION_MODE_READ;
    out->hold = out->read && m->hold;
    out->hold_btn = out->hold ? "HELD" : "HOLD";
    out->picture_tap = m->mode == VISION_MODE_COLOR && m->state == VISION_LIVE && m->sheet == VISION_SHEET_NONE;
    out->mode_btn = vision_mode_name(m->mode);
    out->line_btn = line == VISION_LINE_OFF ? "LINE: OFF" : line == VISION_LINE_ACROSS ? "LINE: ACROSS" : "LINE: DOWN";
    out->tol_btn = tol_names[m->set.color.tol_idx >= 0 && m->set.color.tol_idx < VISION_TOLS ? m->set.color.tol_idx
                                                                                             : VISION_TOL_DEFAULT];
    out->edge_btn = m->set.edge.hard ? "EDGE: HARD" : "EDGE: SOFT";
    out->trace_btn = m->set.trace.dark ? "LINE: DARK" : "LINE: LIGHT";
    counters(m, out);
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
        } else if (out->read) {
            read_status(m, status_buf, status_len);
            out->status = status_buf;
            out->status_warn = m->readfail[0] != '\0';
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
