/*
 * Vision's state machine and layout on their own: every state's words,
 * what each event does, the modes and their groups, the picker and a mode's
 * setup, the settings each mode keeps apart, the line modes and their
 * directions, the speed lines and the distance, the traffic report's words,
 * the helper's caps, and both shapes on the reference panel in every mode
 * with every control a usable, safe, non-overlapping target - the sheet too.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_layout.h"
#include "vision_model.h"
#include "vision_trails.h"

#include <stdio.h>
#include <string.h>

#define TOUCH_MIN 64

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static struct vision_event ev(enum vision_ev_kind kind)
{
    struct vision_event e;

    memset(&e, 0, sizeof(e));
    e.kind = kind;
    return e;
}

static void go_live(struct vision_model *m)
{
    struct vision_event e = ev(VISION_EV_READY);

    snprintf(e.text, sizeof(e.text), "fake");
    snprintf(e.name, sizeof(e.name), "yolov8n.kmodel");
    e.w = 640;
    e.h = 360;
    e.value = 80;
    e.simulated = true;
    vision_model_event(m, &e, NULL, 1000);
    vision_model_event(m, &(struct vision_event) { .kind = VISION_EV_FRAME }, NULL, 1100);
}

/* The code of the cell whose text is `text` on the sheet, or -1. */
static int code_of(const struct vision_model *m, const char *text, bool *selected)
{
    struct vision_sheet_view v;
    int r;
    int c;

    vision_model_sheet(m, &v);
    for (r = 0; r < v.rows; r++) {
        for (c = 0; c < v.row[r].cells; c++) {
            if (strcmp(v.row[r].cell[c].text, text) == 0) {
                if (selected) {
                    *selected = v.row[r].cell[c].selected;
                }
                return v.row[r].cell[c].code;
            }
        }
    }
    return -1;
}

static void test_states(void)
{
    struct vision_model m;
    struct vision_view_text t;
    struct vision_event e;
    char buf[256];

    vision_model_init(&m);
    check("opening starts the helper", vision_model_open(&m) == VISION_ACT_OPEN);
    check("and starts in DETECT", m.mode == VISION_MODE_DETECT && m.set.mode == VISION_MODE_DETECT);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("INIT says it is starting", strcmp(t.title, "Starting") == 0 && !t.show_picture && !t.line_enabled);
    e = ev(VISION_EV_READY);
    snprintf(e.text, sizeof(e.text), "fake");
    snprintf(e.name, sizeof(e.name), "yolov8n.kmodel");
    e.w = 640;
    e.h = 360;
    e.value = 80;
    e.simulated = true;
    check("ready streams and sends the mode, the lines and the distance",
          vision_model_event(&m, &e, NULL, 1000) ==
              (VISION_ACT_STREAM | VISION_ACT_MODE | VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS |
               VISION_ACT_RANGE));
    check("and is LIVE with the camera's shape", m.state == VISION_LIVE && m.preview_w == 640 && m.classes == 80);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("LIVE before a picture waits for it, SIMULATED in the hint",
          strcmp(t.title, "Waiting for the picture") == 0 && strcmp(t.hint, "SIMULATED") == 0 &&
              !t.show_picture && t.line_enabled);
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_FRAME }, NULL, 1100);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a frame shows the picture", t.show_picture && t.title[0] == '\0');
    check("no stall right after a frame", !vision_model_tick(&m, 1500));
    check("a stall after two silent seconds", vision_model_tick(&m, 3200) && m.stalled);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("which the status says, as a warning", strcmp(t.status, "Waiting for the camera...") == 0 && t.status_warn);
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_FRAME }, NULL, 3300);
    check("a frame clears it", !m.stalled);
    {
        struct vision_session s;

        vision_session_init(&s);
        s.stats.fps_x10 = 123;
        s.stats.infer_ms = 31;
        s.stats.cpu_pct = 40;
        s.stats.rss_kb = 20480;
        check("stats without a session change nothing",
              vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_STATS }, NULL, 4000) == 0 && !m.stats_valid);
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_STATS }, &s, 4000);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("stats become the status line", strstr(t.status, "12.3 fps") && strstr(t.status, "KPU 31 ms") &&
                                                  strstr(t.status, "CPU 40%") && strstr(t.status, "20 MB"));
    }

    e = ev(VISION_EV_LOST);
    vision_model_event(&m, &e, NULL, 5000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a lost camera is an error with a way back",
          m.state == VISION_ERROR && t.show_retry && strcmp(t.detail, "The camera went away") == 0);
    e = ev(VISION_EV_NOMODEL);
    snprintf(e.text, sizeof(e.text), "model file not found");
    vision_model_open(&m);
    vision_model_event(&m, &e, NULL, 6000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("no model is NO_DEVICE with the reason", m.state == VISION_NO_DEVICE && strstr(t.detail, "not found") != NULL);
    vision_model_open(&m);
    e = ev(VISION_EV_EXITED);
    e.reason = VISION_EXIT_HUNG;
    vision_model_event(&m, &e, NULL, 7000);
    check("a hung helper is 'not responding'", m.state == VISION_ERROR && strstr(m.error, "not responding") != NULL);
    vision_model_open(&m);
    e = ev(VISION_EV_ERROR);
    snprintf(e.text, sizeof(e.text), "busy the camera is in use");
    vision_model_event(&m, &e, NULL, 8000);
    check("busy is said in words", strcmp(m.error, "The camera is in use") == 0);
    vision_model_open(&m);
    snprintf(e.text, sizeof(e.text), "infer the detector failed");
    vision_model_event(&m, &e, NULL, 8100);
    check("a failed run too", strcmp(m.error, "The detector failed on a frame") == 0);
}

static void test_picker(void)
{
    struct vision_model m;
    struct vision_sheet_view v;
    struct vision_view_text t;
    char buf[256];
    bool sel = false;
    unsigned acts;
    int r;

    vision_model_init(&m);
    vision_model_open(&m);
    go_live(&m);
    vision_model_sheet(&m, &v);
    check("no sheet until asked", v.kind == VISION_SHEET_NONE && v.rows == 0);
    check("MODE opens the picker and sends nothing", vision_model_mode_button(&m) == 0 && m.sheet == VISION_SHEET_MODES);
    vision_model_sheet(&m, &v);
    check("before caps: GENERAL, ROAD and TOOLS - the detector's modes and the pixel modes",
          v.kind == VISION_SHEET_MODES && v.rows == 3 && strcmp(v.row[0].caption, "GENERAL") == 0 &&
              strcmp(v.row[1].caption, "ROAD") == 0 && strcmp(v.row[2].caption, "TOOLS") == 0);
    check("GENERAL: DETECT (in force) and TRACK", v.row[0].cells == 2 && strcmp(v.row[0].cell[0].text, "DETECT") == 0 &&
                                                      v.row[0].cell[0].selected && strcmp(v.row[0].cell[1].text, "TRACK") == 0 &&
                                                      !v.row[0].cell[1].selected);
    check("TOOLS: COLOR, EDGE, LINE TRACE", v.row[2].cells == 3 && strcmp(v.row[2].cell[2].text, "LINE TRACE") == 0);
    check("no FACE, RECOGNIZE or READ without a helper that says it can", code_of(&m, "FACE", NULL) < 0 &&
                                                                              code_of(&m, "RECOGNIZE", NULL) < 0 &&
                                                                              code_of(&m, "READ", NULL) < 0);
    check("MODE again closes it", vision_model_mode_button(&m) == 0 && m.sheet == VISION_SHEET_NONE);

    /* The helper's caps decide what is offered. */
    {
        struct vision_event e = ev(VISION_EV_CAPS);

        e.value = (1 << VISION_MODE_DETECT) | (1 << VISION_MODE_TRACK) | (1 << VISION_MODE_TRAFFIC) |
                  (1 << VISION_MODE_FACE) | (1 << VISION_MODE_READ) | (1 << VISION_MODE_COLOR);
        check("caps that keep the mode in force ask for nothing", vision_model_event(&m, &e, NULL, 2000) == 0);
        vision_model_mode_button(&m);
        vision_model_sheet(&m, &v);
        check("with FACE and READ offered: five groups, PEOPLE with FACE alone, TEXT with READ",
              v.rows == 5 && strcmp(v.row[2].caption, "PEOPLE") == 0 && v.row[2].cells == 1 &&
                  strcmp(v.row[2].cell[0].text, "FACE") == 0 && strcmp(v.row[3].caption, "TEXT") == 0 &&
                  strcmp(v.row[3].cell[0].text, "READ") == 0);
        check("TOOLS only lists what is offered", v.row[4].cells == 1 && strcmp(v.row[4].cell[0].text, "COLOR") == 0);
        check("EDGE cannot be chosen when not offered", vision_model_set_mode(&m, VISION_MODE_EDGE) == 0 &&
                                                            m.mode == VISION_MODE_DETECT);
    }
    for (r = 0; r < VISION_SHEET_ROWS; r++) {
        check("every picker row has at most three choices", v.row[r].cells <= VISION_SHEET_CELLS);
    }
    check("the picker is still open", m.sheet == VISION_SHEET_MODES);
    acts = vision_model_sheet_tap(&m, code_of(&m, "TRACK", NULL));
    check("TRACK from the picker: the mode, its line, stored; the picker closes",
          m.mode == VISION_MODE_TRACK && m.set.mode == VISION_MODE_TRACK && m.sheet == VISION_SHEET_NONE &&
              (acts & VISION_ACT_MODE) && (acts & VISION_ACT_LINE) && (acts & VISION_ACT_SAVE));
    vision_model_mode_button(&m);
    code_of(&m, "TRACK", &sel);
    check("the picker marks TRACK in force", sel);
    check("choosing the mode in force changes nothing and closes",
          vision_model_sheet_tap(&m, code_of(&m, "TRACK", NULL)) == 0 && m.sheet == VISION_SHEET_NONE);
    check("a picker code with no picker open does nothing", vision_model_sheet_tap(&m, 105) == 0 &&
                                                                m.mode == VISION_MODE_TRACK);
    vision_model_mode_button(&m);
    check("a code nobody made does nothing", vision_model_sheet_tap(&m, 9999) == 0 && m.mode == VISION_MODE_TRACK);
    vision_model_sheet_close(&m);

    /* A stored mode the helper cannot run falls back to DETECT and keeps
     * the wish. */
    {
        struct vision_settings s;
        struct vision_event e = ev(VISION_EV_CAPS);

        vision_settings_defaults(&s);
        s.mode = VISION_MODE_READ;
        vision_model_init(&m);
        vision_model_load(&m, &s);
        vision_model_open(&m);
        go_live(&m);
        e.value = (1 << VISION_MODE_DETECT) | (1 << VISION_MODE_TRACK);
        acts = vision_model_event(&m, &e, NULL, 2000);
        check("a stored READ on a helper without text runs DETECT and says so to the helper",
              m.mode == VISION_MODE_DETECT && m.set.mode == VISION_MODE_READ && (acts & VISION_ACT_MODE) &&
                  !(acts & VISION_ACT_SAVE));
        e.value |= 1 << VISION_MODE_READ;
        acts = vision_model_event(&m, &e, NULL, 2100);
        check("and READ comes back when the helper offers it", m.mode == VISION_MODE_READ && (acts & VISION_ACT_MODE));
        e.value = -1;
        vision_model_event(&m, &e, NULL, 2200);
        check("caps with bits beyond the modes are cut to the modes", m.avail == (1u << VISION_MODES) - 1u);
    }

    /* Each mode's controls and words. */
    vision_model_init(&m);
    vision_model_open(&m);
    go_live(&m);
    {
        enum vision_button order[VISION_BUTTONS];
        struct vision_session s;
        int32_t pm[4];

        vision_model_text(&m, &t, buf, sizeof(buf));
        check("DETECT: MODE alone, no lines, no ids, one status line",
              vision_model_buttons(&m, order) == 1 && order[0] == VISION_BTN_MODE && !t.lines && !t.ids &&
                  vision_model_status_lines(&m) == 1 && strcmp(t.mode_btn, "DETECT") == 0);
        check("DETECT sends no counting line", !vision_model_line_pm(&m, pm));
        vision_session_init(&s);
        s.shown_count = 3;
        s.shown[0].cls = 2;
        s.shown[1].cls = 0;
        s.shown[2].cls = 2;
        s.shown[1].id = 7;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_DET }, &s, 1200);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("DETECT's counters: the objects and their classes", strcmp(t.count_a, "OBJECTS 3") == 0 &&
                                                                     strcmp(t.count_b, "CLASSES 2") == 0 &&
                                                                     m.active_tracks == 1);
        vision_model_set_mode(&m, VISION_MODE_TRACK);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("TRACK: MODE, LINE, TRAILS, RESET; ids, trails and the line",
              vision_model_buttons(&m, order) == 4 && order[1] == VISION_BTN_LINE && order[2] == VISION_BTN_TRAILS &&
                  order[3] == VISION_BTN_RESET && t.lines && t.ids && t.trails && !t.speeds &&
                  strcmp(t.trails_btn, "TRAILS: ON") == 0 && strcmp(t.count_a, "DOWN 0") == 0);
        check("TRAILS off: stored, nothing sent", vision_model_trails_next(&m) == VISION_ACT_SAVE && !m.set.track.trails &&
                                                      (vision_model_text(&m, &t, buf, sizeof(buf)), !t.trails) &&
                                                      strcmp(t.trails_btn, "TRAILS: OFF") == 0);
        vision_model_trails_next(&m);
        vision_model_set_mode(&m, VISION_MODE_TRAFFIC);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("TRAFFIC: MODE, SETUP, RESET; three status lines", vision_model_buttons(&m, order) == 3 &&
                                                                     order[1] == VISION_BTN_SETUP &&
                                                                     order[2] == VISION_BTN_RESET &&
                                                                     vision_model_status_lines(&m) == 3 && t.traffic);
        vision_model_set_mode(&m, VISION_MODE_COLOR);
        check("COLOR: MODE, SAMPLE, TOL", vision_model_buttons(&m, order) == 3 && order[1] == VISION_BTN_SAMPLE &&
                                              order[2] == VISION_BTN_TOL);
        vision_model_set_mode(&m, VISION_MODE_EDGE);
        check("EDGE: MODE, EDGE", vision_model_buttons(&m, order) == 2 && order[1] == VISION_BTN_EDGE);
        vision_model_set_mode(&m, VISION_MODE_TRACE);
        check("TRACE: MODE, TRACE", vision_model_buttons(&m, order) == 2 && order[1] == VISION_BTN_TRACE);
        check("SETUP does nothing outside TRAFFIC", vision_model_setup_button(&m) == 0 && m.sheet == VISION_SHEET_NONE);
    }
}

static void test_track_and_traffic(void)
{
    struct vision_model m;
    struct vision_view_text t;
    struct vision_session s;
    struct vision_sheet_view v;
    char buf[256];
    int32_t pm[8];
    const char *a;
    const char *b;
    unsigned acts;
    bool sel = false;

    vision_model_init(&m);
    vision_model_open(&m);
    go_live(&m);
    vision_model_set_mode(&m, VISION_MODE_TRACK);
    check("TRACK's line is ACROSS", vision_model_line(&m) == VISION_LINE_ACROSS && vision_model_line_pm(&m, pm) &&
                                        pm[0] == 0 && pm[1] == 500 && pm[2] == 1000 && pm[3] == 500);
    vision_model_count_names(&m, &a, &b);
    check("counting DOWN and UP", strcmp(a, "DOWN") == 0 && strcmp(b, "UP") == 0);
    acts = vision_model_line_next(&m);
    check("LINE cycles to DOWN, sends it and stores it", acts == (VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_SAVE) &&
                                                             m.set.track.line == VISION_LINE_DOWN);
    vision_model_count_names(&m, &a, &b);
    check("counting LEFT and RIGHT", strcmp(a, "LEFT") == 0 && strcmp(b, "RIGHT") == 0 &&
                                         vision_model_line_pm(&m, pm) && pm[0] == 500 && pm[2] == 500);
    vision_model_line_next(&m);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("then OFF: the button says so, the counters count tracks", m.set.track.line == VISION_LINE_OFF &&
                                                                        strcmp(t.line_btn, "LINE: OFF") == 0 &&
                                                                        strcmp(t.count_a, "TRACKS 0") == 0);
    vision_model_line_next(&m);
    vision_session_init(&s);
    s.count_ab = 3;
    s.count_ba = 1;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_COUNT }, &s, 4100);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("counts come from the session", strcmp(t.count_a, "DOWN 3") == 0 && strcmp(t.count_b, "UP 1") == 0);
    check("RESET clears the counts and sends it, storing nothing", vision_model_reset(&m) == VISION_ACT_RESET && m.count_a == 0);
    check("TRACK has no speed lines", !vision_model_speed_pm(&m, pm));

    /* TRAFFIC keeps its own line. */
    vision_model_line_next(&m); /* TRACK: DOWN */
    vision_model_set_mode(&m, VISION_MODE_TRAFFIC);
    check("TRAFFIC's line is its own: still ACROSS while TRACK's is DOWN",
          vision_model_line(&m) == VISION_LINE_ACROSS && m.set.track.line == VISION_LINE_DOWN);
    check("SETUP opens TRAFFIC's setup", vision_model_setup_button(&m) == 0 && m.sheet == VISION_SHEET_SETUP);
    vision_model_sheet(&m, &v);
    check("the setup: DETECTION RANGE, COUNT LINE, SPEED LINES, DISTANCE, SHOW",
          v.kind == VISION_SHEET_SETUP && v.rows == 5 && strcmp(v.row[0].caption, "DETECTION RANGE") == 0 &&
              strcmp(v.row[1].caption, "COUNT LINE") == 0 && strcmp(v.row[2].caption, "SPEED LINES") == 0 &&
              strcmp(v.row[3].caption, "DISTANCE") == 0 && strcmp(v.row[4].caption, "SHOW") == 0);
    check("the range is NEAR NORMAL FAR, NORMAL in force", strcmp(v.row[0].cell[0].text, "NEAR") == 0 &&
                                                               strcmp(v.row[0].cell[2].text, "FAR") == 0 &&
                                                               v.row[0].cell[1].selected && !v.row[0].cell[2].selected);
    check("the choices in force are marked", v.row[1].cell[1].selected && !v.row[1].cell[0].selected &&
                                                 v.row[2].cell[0].selected && strcmp(v.row[3].cell[1].text, "10 m") == 0 &&
                                                 !v.row[3].cell[1].enabled);
    check("SHOW: labels, speeds and trails, all on", strcmp(v.row[4].cell[0].text, "LABELS") == 0 &&
                                                         v.row[4].cell[0].selected && v.row[4].cell[1].selected &&
                                                         v.row[4].cell[2].selected);
    acts = vision_model_sheet_tap(&m, v.row[0].cell[2].code);
    check("FAR: sent and stored", m.set.traffic.range == VISION_RANGE_FAR && acts == (VISION_ACT_RANGE | VISION_ACT_SAVE) &&
                                      strcmp(vision_model_range_word(&m), "far") == 0);
    check("FAR again: nothing", vision_model_sheet_tap(&m, v.row[0].cell[2].code) == 0);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("the status leads with the range", strncmp(t.status, "FAR  ", 5) == 0);
    acts = vision_model_sheet_tap(&m, v.row[1].cell[2].code);
    check("COUNT LINE DOWN: sent and stored, TRACK's untouched, the sheet stays open",
          vision_model_line(&m) == VISION_LINE_DOWN && m.set.track.line == VISION_LINE_DOWN &&
              acts == (VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_SAVE) && m.sheet == VISION_SHEET_SETUP);
    acts = vision_model_sheet_tap(&m, v.row[2].cell[1].code);
    check("SPEED LINES NARROW: sent and stored", m.set.traffic.speed == VISION_SPEED_NARROW &&
                                                     acts == (VISION_ACT_SPEED | VISION_ACT_SAVE));
    check("two vertical lines at 40 % and 60 %, top to bottom like the counting line",
          vision_model_speed_pm(&m, pm) && pm[0] == 400 && pm[1] == 0 && pm[2] == 400 && pm[3] == 1000 && pm[4] == 600 &&
              pm[7] == 1000);
    vision_model_sheet_tap(&m, v.row[1].cell[0].code);
    check("the count line OFF keeps the speed lines standing", vision_model_line(&m) == VISION_LINE_OFF &&
                                                                   vision_model_speed_pm(&m, pm) && pm[0] == 400 && pm[3] == 1000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("and the counters show nothing", strcmp(t.count_a, "-") == 0 && strcmp(t.count_b, "-") == 0);
    vision_model_sheet_tap(&m, v.row[1].cell[1].code);
    vision_model_sheet_tap(&m, v.row[2].cell[2].code);
    check("ACROSS and WIDE: horizontal at 25 % and 75 %", vision_model_speed_pm(&m, pm) && pm[0] == 0 && pm[1] == 250 &&
                                                              pm[2] == 1000 && pm[5] == 750);
    acts = vision_model_sheet_tap(&m, v.row[3].cell[2].code);
    check("DISTANCE > sends and stores 15 m", vision_model_distance_cm(&m) == 1500 &&
                                                  acts == (VISION_ACT_DISTANCE | VISION_ACT_SAVE));
    vision_model_sheet_tap(&m, v.row[3].cell[0].code);
    vision_model_sheet_tap(&m, v.row[3].cell[0].code);
    check("< twice: 5 m", vision_model_distance_cm(&m) == 500);
    check("the distance itself is no button", vision_model_sheet_tap(&m, v.row[3].cell[1].code) == 0);
    check("LABELS off: stored only (the screen's own)", vision_model_sheet_tap(&m, v.row[4].cell[0].code) == VISION_ACT_SAVE &&
                                                            !m.set.traffic.labels &&
                                                            (vision_model_text(&m, &t, buf, sizeof(buf)), !t.ids) && t.speeds);
    check("SPEEDS off", vision_model_sheet_tap(&m, v.row[4].cell[1].code) == VISION_ACT_SAVE && !m.set.traffic.speeds &&
                            (vision_model_text(&m, &t, buf, sizeof(buf)), !t.speeds));
    check("TRAILS off: TRAFFIC's own, TRACK's stays", vision_model_sheet_tap(&m, v.row[4].cell[2].code) == VISION_ACT_SAVE &&
                                                          !m.set.traffic.trails && m.set.track.trails &&
                                                          (vision_model_text(&m, &t, buf, sizeof(buf)), !t.trails));
    vision_model_sheet_tap(&m, v.row[4].cell[0].code);
    vision_model_sheet_tap(&m, v.row[4].cell[1].code);
    vision_model_sheet_tap(&m, v.row[4].cell[2].code);
    {
        int i;
        int ok = 1;

        for (i = 0; i < VISION_DISTANCES; i++) {
            ok &= vision_model_distance_cm(&m) >= 100 && vision_model_distance_cm(&m) <= 5000;
            vision_model_distance_prev(&m);
        }
        check("every distance is between 1 m and 50 m, and < comes round", ok && vision_model_distance_cm(&m) == 500);
    }
    vision_model_sheet(&m, &v);
    code_of(&m, "WIDE", &sel);
    check("the sheet shows the new choices", sel && strcmp(v.row[3].cell[1].text, "5 m") == 0);
    check("SETUP again closes it", vision_model_setup_button(&m) == 0 && m.sheet == VISION_SHEET_NONE);
    check("a setup code with the setup closed does nothing", vision_model_sheet_tap(&m, 211) == 0 &&
                                                                 m.set.traffic.speed == VISION_SPEED_WIDE);

    /* The report. */
    s.traffic.total_ab = 4;
    s.traffic.total_ba = 2;
    s.traffic.cur_kmh10 = 432;
    s.traffic.last_kmh10 = 432;
    s.traffic.max_kmh10 = 510;
    s.traffic.mean_kmh10 = 400;
    s.traffic.n = 3;
    s.traffic.cls_ab[0] = 3;
    s.traffic.cls_ba[0] = 1;
    s.traffic.cls_ab[5] = 1;
    s.traffic.cls_ba[5] = 1;
    s.stats.fps_x10 = 123;
    s.stats.infer_ms = 31;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_STATS }, &s, 4900);
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRAFFIC }, &s, 5000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("the totals since RESET are the counters", m.traffic_valid && strcmp(t.count_a, "IN (DOWN) 4") == 0 &&
                                                         strcmp(t.count_b, "OUT (UP) 2") == 0);
    check("before the first recent line the status says so, with the rate",
          strstr(t.status, "FAR  12.3 fps  KPU 31 ms") && strstr(t.status, "5 min: -") &&
              strstr(t.status, "car 0  truck 0  bus 0  moto 0  bike 0  person 0"));
    s.recent.window_s = 300;
    s.recent.crossed = 5;
    s.recent.ab = 3;
    s.recent.ba = 2;
    s.recent.cls[0] = 4;
    s.recent.cls[5] = 1;
    s.recent.speeds = 3;
    s.recent.mean_kmh10 = 432;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_RECENT }, &s, 5100);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("the last five minutes: crossings each way, their mean speed and how many, by class",
          m.recent_valid && strstr(t.status, "5 min: 5  IN 3  OUT 2  avg 43.2 km/h (3)") &&
              strstr(t.status, "car 4  truck 0  bus 0  moto 0  bike 0  person 1"));
    s.recent.speeds = 0;
    s.recent.mean_kmh10 = 0;
    s.recent.saturated = true;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_RECENT }, &s, 5200);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("no speed yet, with the distance; a window that overflowed says it holds the latest only",
          strstr(t.status, "5 min: 5  IN 3  OUT 2 (latest only)  no speed yet (5 m)") != NULL);
    vision_model_sheet_tap(&m, (vision_model_setup_button(&m), v.row[2].cell[0].code));
    vision_model_setup_button(&m);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("speed lines off: no speed words at all", strstr(t.status, "speed") == NULL && strstr(t.status, "km/h") == NULL);
    check("RESET in TRAFFIC clears the report and the window", vision_model_reset(&m) == VISION_ACT_RESET &&
                                                                   m.traffic.total_ab == 0 && !m.recent_valid);

    /* An error closes the sheet; Try again keeps every choice. */
    vision_model_setup_button(&m);
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_LOST }, NULL, 6000);
    vision_model_sheet(&m, &v);
    check("an error closes the sheet", m.sheet == VISION_SHEET_NONE && v.kind == VISION_SHEET_NONE);
    {
        struct vision_settings before = m.set;

        check("Try again keeps every setting and opens again", vision_model_open(&m) == VISION_ACT_OPEN &&
                                                                   m.state == VISION_INIT &&
                                                                   memcmp(&before, &m.set, sizeof(before)) == 0 &&
                                                                   m.mode == VISION_MODE_TRAFFIC);
    }
    check("a tap while not live changes the choice, stores it and sends nothing",
          vision_model_speed_next(&m) == VISION_ACT_SAVE && vision_model_distance_next(&m) == VISION_ACT_SAVE &&
              vision_model_line_next(&m) == VISION_ACT_SAVE);
}

static void test_tools(void)
{
    struct vision_model m;
    struct vision_view_text t;
    struct vision_session s;
    char buf[256];

    vision_model_init(&m);
    vision_model_open(&m);
    go_live(&m);
    vision_session_init(&s);
    check("COLOR from DETECT sends the mode and the pixel settings",
          vision_model_set_mode(&m, VISION_MODE_COLOR) ==
                  (VISION_ACT_MODE | VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS |
                   VISION_ACT_RANGE | VISION_ACT_SAVE) &&
              vision_model_pixel_mode(&m) && strcmp(vision_model_mode_word(&m), "color") == 0);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("COLOR: the picture takes taps; no lines; no counts",
          strcmp(t.mode_btn, "COLOR") == 0 && t.picture_tap && !t.lines && !t.traffic && strcmp(t.count_a, "-") == 0 &&
              strcmp(t.tol_btn, "TOL: MED") == 0 && vision_model_tol(&m) == 96 && vision_model_status_lines(&m) == 1);
    check("it asks for a colour", strstr(t.status, "Tap the picture or SAMPLE") != NULL);
    vision_model_mode_button(&m);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("with the picker open a tap on the picture picks nothing", !t.picture_tap);
    vision_model_sheet_close(&m);
    check("SAMPLE asks the helper for the middle", vision_model_sample_middle(&m, 400, 600) == VISION_ACT_SAMPLE &&
                                                       m.sample_x == 200 && m.sample_y == 300);
    check("a tap asks for that point", vision_model_sample_at(&m, 10, 20) == VISION_ACT_SAMPLE && m.sample_x == 10);
    check("a point off the picture does not", vision_model_sample_at(&m, -1, 20) == 0);
    check("TOL cycles, sends and stores", vision_model_tol_next(&m) == (VISION_ACT_PIXELS | VISION_ACT_SAVE) &&
                                              vision_model_tol(&m) == 160);
    vision_model_tol_next(&m);
    check("round to LOW", vision_model_tol(&m) == 48);
    s.pixels.color.r = 200;
    s.pixels.color.g = 30;
    s.pixels.color.b = 30;
    s.pixels.color.matched_pm = 123;
    s.pixels.color.cx = 150;
    s.pixels.color.cy = 80;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_COLOR }, &s, 6000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a colour report is the status and the mark", m.have_target && strstr(t.status, "#C81E1E") &&
                                                            strstr(t.status, "match 12.3%") && strstr(t.status, "at 150,80") &&
                                                            t.show_mark && t.mark_x == 150 && t.mark_y == 80);
    vision_model_set_mode(&m, VISION_MODE_EDGE);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("EDGE: soft; no taps, no mark", strcmp(t.edge_btn, "EDGE: SOFT") == 0 && !t.picture_tap && !t.show_mark &&
                                              vision_model_edge_threshold(&m) == 0 && strstr(t.status, "Finding edges") != NULL);
    check("EDGE hard sends the threshold and stores it", vision_model_edge_next(&m) == (VISION_ACT_PIXELS | VISION_ACT_SAVE) &&
                                                             m.set.edge.hard &&
                                                             vision_model_edge_threshold(&m) == VISION_EDGE_HARD_THRESHOLD);
    s.pixels.edge_pm = 81;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_EDGE }, &s, 6100);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("the edge share is the status", strstr(t.status, "edges 8.1%") && strstr(t.status, "hard"));
    vision_model_set_mode(&m, VISION_MODE_TRACE);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("TRACE: LINE: DARK", strcmp(t.trace_btn, "LINE: DARK") == 0 && strcmp(t.mode_btn, "LINE TRACE") == 0 &&
                                   strstr(t.status, "Looking for a dark line") != NULL);
    s.pixels.trace.found = true;
    s.pixels.trace.offset_pm = -120;
    s.pixels.trace.slope_pm = 450;
    s.pixels.trace.rows = 300;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRACE }, &s, 6200);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a line is said with its offset and lean", strstr(t.status, "line left 12%") && strstr(t.status, "leans right 45%") &&
                                                         strstr(t.status, "300 rows"));
    check("LINE: LIGHT sends, stores, and forgets the last answer",
          vision_model_trace_next(&m) == (VISION_ACT_PIXELS | VISION_ACT_SAVE) && !m.set.trace.dark && !m.trace_valid);
    s.pixels.trace.found = false;
    s.pixels.trace.rows = 3;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRACE }, &s, 6300);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("no line is said too", strstr(t.status, "no light line") != NULL);
    check("the class names", strcmp(vision_model_traffic_name(3), "moto") == 0 && strcmp(vision_model_traffic_name(6), "?") == 0);
}

static int inside(const struct vision_rect *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return r->x >= x0 && r->y >= y0 && r->x + r->w <= x1 && r->y + r->h <= y1;
}

static int overlap(const struct vision_rect *a, const struct vision_rect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

/* The sheet on the picture of a shape: every row, inside the panel,
 * usable, apart. */
static void sheet_on(const char *what, const struct vision_rect *pic, int rows, const int cells[])
{
    struct vision_sheet_layout sl;
    const struct vision_rect *all[1 + VISION_SHEET_ROWS_MAX * (1 + VISION_SHEET_CELLS_MAX)];
    int n = 0;
    int ok = 1;
    int r;
    int c;
    int i;
    int j;
    char name[160];

    snprintf(name, sizeof(name), "%s: a sheet of %d rows fits the picture", what, rows);
    check(name, vision_layout_sheet(&sl, pic->w, pic->h, rows, cells) == 0);
    all[n++] = &sl.title;
    for (r = 0; r < rows; r++) {
        all[n++] = &sl.caption[r];
        for (c = 0; c < cells[r]; c++) {
            all[n++] = &sl.cell[r][c];
            ok &= sl.cell[r][c].h >= TOUCH_MIN && sl.cell[r][c].w >= 100;
        }
    }
    snprintf(name, sizeof(name), "%s: the sheet's choices are usable targets", what);
    check(name, ok);
    ok = 1;
    for (i = 0; i < n; i++) {
        ok &= inside(all[i], 0, 0, pic->w, pic->h);
        for (j = i + 1; j < n; j++) {
            ok &= !overlap(all[i], all[j]);
        }
    }
    snprintf(name, sizeof(name), "%s: the sheet stays on the picture and nothing overlaps", what);
    check(name, ok);
}

static void shape(const char *what, int32_t w, int32_t h, int32_t il, int32_t it, int32_t ir,
                  int32_t ib, uint32_t fw, uint32_t fh, bool wide, int buttons, int status_lines)
{
    struct vision_layout l;
    const struct vision_rect *all[4 + VISION_LAYOUT_BUTTONS];
    int count = 4 + buttons;
    char name[160];
    int ok = 1;
    int i;
    int j;
    int64_t err;

    snprintf(name, sizeof(name), "%s: lays out", what);
    check(name, vision_layout_compute(&l, w, h, il, it, ir, ib, fw, fh, buttons, status_lines) == 0);
    snprintf(name, sizeof(name), "%s: the shape is %s", what, wide ? "wide" : "tall");
    check(name, l.wide == wide && l.buttons == buttons);
    all[0] = &l.picture;
    all[1] = &l.status;
    all[2] = &l.count_a;
    all[3] = &l.count_b;
    for (i = 0; i < buttons; i++) {
        all[4 + i] = &l.btn[i];
    }
    for (i = 0; i < count; i++) {
        ok &= inside(all[i], il, it, w - ir, h - ib);
    }
    snprintf(name, sizeof(name), "%s: everything inside the safe box", what);
    check(name, ok);
    ok = 1;
    for (i = 0; i < buttons; i++) {
        ok &= l.btn[i].h >= TOUCH_MIN && l.btn[i].w >= 100;
    }
    snprintf(name, sizeof(name), "%s: the buttons are usable targets", what);
    check(name, ok);
    ok = 1;
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            ok &= !overlap(all[i], all[j]);
        }
    }
    snprintf(name, sizeof(name), "%s: nothing overlaps", what);
    check(name, ok);
    snprintf(name, sizeof(name), "%s: the status holds its lines", what);
    check(name, l.status.h >= status_lines * VISION_STATUS_H);
    err = (int64_t)l.picture.w * fh - (int64_t)l.picture.h * fw;
    snprintf(name, sizeof(name), "%s: the picture has the frame's shape (%dx%d)", what, (int)l.picture.w,
             (int)l.picture.h);
    check(name, (err < 0 ? -err : err) <= (int64_t)(fw > fh ? fw : fh) && l.picture.w <= VISION_PICTURE_MAX &&
                    l.picture.h <= VISION_PICTURE_MAX);
    snprintf(name, sizeof(name), "%s: the picture takes most of the body", what);
    check(name, (int64_t)l.picture.w * l.picture.h * 2 > (int64_t)w * h);
    {
        /* The largest sheets there are: the full picker, and five rows of
         * three. */
        const int picker[5] = { 2, 1, 2, 1, 3 };
        const int full[5] = { 3, 3, 3, 3, 3 };

        sheet_on(what, &l.picture, 5, picker);
        sheet_on(what, &l.picture, 5, full);
    }
}

static void test_layout(void)
{
    struct vision_layout l;
    struct vision_sheet_layout sl;
    const int one[1] = { 1 };
    const int bad[2] = { 1, 4 };

    /* The reference panel's bodies under the NONE chrome (as Camera: 528 x
     * 1116 portrait, 1192 x 452 landscape), with the corner clearance, in
     * every mode's button count. */
    shape("portrait DETECT", 528, 1116, 0, 0, 0, 30, 360, 640, false, 1, 1);
    shape("landscape DETECT", 1192, 452, 30, 0, 30, 0, 640, 360, true, 1, 1);
    shape("portrait TRACK", 528, 1116, 0, 0, 0, 30, 360, 640, false, 4, 1);
    shape("landscape TRACK", 1192, 452, 30, 0, 30, 0, 640, 360, true, 4, 1);
    shape("portrait TRAFFIC", 528, 1116, 0, 0, 0, 30, 360, 640, false, 3, 3);
    shape("landscape TRAFFIC", 1192, 452, 30, 0, 30, 0, 640, 360, true, 3, 3);
    shape("portrait EDGE", 528, 1116, 0, 0, 0, 30, 360, 640, false, 2, 1);
    shape("landscape EDGE", 1192, 452, 30, 0, 30, 0, 640, 360, true, 2, 1);
    check("a body too small is refused", vision_layout_compute(&l, 200, 150, 0, 0, 0, 0, 640, 360, 3, 1) == -1);
    check("a frame with no size is refused", vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 0, 360, 3, 1) == -1);
    check("too many buttons or status lines are refused",
          vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 360, 640, 6, 1) == -1 &&
              vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 360, 640, 3, 5) == -1 &&
              vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 360, 640, 0, 1) == -1);
    vision_layout_compute(&l, 1192, 452, 30, 0, 30, 0, 640, 360, 3, 3);
    check("on the landscape picture captions stand beside the choices",
          vision_layout_sheet(&sl, l.picture.w, l.picture.h, 1, one) == 0 && sl.side_by_side);
    vision_layout_compute(&l, 528, 1116, 0, 0, 0, 30, 360, 640, 3, 3);
    check("on the portrait picture above them", vision_layout_sheet(&sl, l.picture.w, l.picture.h, 1, one) == 0 &&
                                                   !sl.side_by_side && sl.cell[0][0].y > sl.caption[0].y);
    check("a sheet that cannot fit is refused", vision_layout_sheet(&sl, 400, 200, 5, (int[]) { 1, 1, 1, 1, 1 }) == -1);
    check("a row of four is refused", vision_layout_sheet(&sl, 800, 450, 2, bad) == -1);
    check("no rows, or six, are refused", vision_layout_sheet(&sl, 800, 450, 0, one) == -1 &&
                                              vision_layout_sheet(&sl, 800, 450, 6, (int[]) { 1, 1, 1, 1, 1, 1 }) == -1);
}

static void test_read(void)
{
    struct vision_model m;
    struct vision_view_text t;
    struct vision_session s;
    struct vision_event e = ev(VISION_EV_CAPS);
    enum vision_button order[VISION_BUTTONS];
    char buf[256];
    char a[32];

    vision_model_init(&m);
    vision_model_open(&m);
    go_live(&m);
    e.value = (1 << VISION_MODE_DETECT) | (1 << VISION_MODE_READ);
    vision_model_event(&m, &e, NULL, 1200);
    check("READ is chosen from TEXT when offered", (vision_model_set_mode(&m, VISION_MODE_READ) & VISION_ACT_MODE) &&
                                                        m.mode == VISION_MODE_READ &&
                                                        strcmp(vision_model_mode_word(&m), "read") == 0);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("READ: MODE and HOLD; three status lines; text boxes, no ids, no lines",
          vision_model_buttons(&m, order) == 2 && order[1] == VISION_BTN_HOLD && vision_model_status_lines(&m) == 3 &&
              t.read && !t.ids && !t.lines && strcmp(t.hold_btn, "HOLD") == 0 && strstr(t.status, "Reading") != NULL);
    vision_session_init(&s);
    s.text.n = 2;
    s.text.line[0].conf = 990;
    snprintf(s.text.line[0].text, sizeof(s.text.line[0].text), "EXIT12");
    s.text.line[1].conf = 950;
    snprintf(s.text.line[1].text, sizeof(s.text.line[1].text), "\xe5\x85\xb6" "AB");
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TEXT }, &s, 1300);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("what was read is the status, line after line, other scripts as ?", strstr(t.status, "EXIT12  |  ?AB") != NULL);
    check("the counters: the lines and how sure", strcmp(t.count_a, "LINES 2") == 0 && strcmp(t.count_b, "SURE 97%") == 0);
    check("HOLD holds, and is the primary button", vision_model_hold_next(&m) == 0 && m.hold &&
                                                       (vision_model_text(&m, &t, buf, sizeof(buf)), t.hold) &&
                                                       strcmp(t.hold_btn, "HELD") == 0);
    snprintf(s.text.line[0].text, sizeof(s.text.line[0].text), "OTHER");
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TEXT }, &s, 1400);
    check("a new read does not replace a held one", strcmp(m.text.line[0].text, "EXIT12") == 0);
    vision_model_hold_next(&m);
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TEXT }, &s, 1500);
    check("let go, it follows the picture again", !m.hold && strcmp(m.text.line[0].text, "OTHER") == 0);
    s.text.n = 0;
    vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TEXT }, &s, 1600);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("nothing read says where to point", strstr(t.status, "No text found") != NULL && strcmp(t.count_a, "LINES 0") == 0);
    e = ev(VISION_EV_READFAIL);
    snprintf(e.text, sizeof(e.text), "the text models and the dictionary do not fit together");
    vision_model_event(&m, &e, NULL, 1700);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a read that cannot be done is a warning with why", t.status_warn && strstr(t.status, "Cannot read") &&
                                                                  strstr(t.status, "do not fit"));
    vision_model_set_mode(&m, VISION_MODE_DETECT);
    check("leaving READ forgets its read and its trouble; HOLD then does nothing",
          vision_model_hold_next(&m) == 0 && !m.hold && !m.text_valid && m.readfail[0] == '\0');
    vision_model_text_ascii("a\x01" "b\xc3\xa6" "c\xe2\x82\xac" "d\xf0\x9f\x98\x80", a, sizeof(a));
    check("the ASCII view: controls and every multi-byte character as one ?", strcmp(a, "a?b?c?d?") == 0);
    vision_model_text_ascii("ABCDEFGH", a, 5);
    check("and cut to the room", strcmp(a, "ABCD") == 0);
}

static struct vision_shown shown(uint32_t id, int32_t x, int32_t y)
{
    struct vision_shown s;

    memset(&s, 0, sizeof(s));
    s.id = id;
    s.x = x;
    s.y = y;
    s.w = 20;
    s.h = 10;
    return s;
}

static void test_trails(void)
{
    static struct vision_trails tr;
    struct vision_shown s[VISION_MAX_SHOWN + 1];
    int32_t xs[VISION_TRAIL_POINTS];
    int32_t ys[VISION_TRAIL_POINTS];
    int i;
    int n;

    vision_trails_clear(&tr);
    s[0] = shown(7, 0, 0);
    s[1] = shown(0, 50, 50);
    vision_trails_update(&tr, s, 2);
    check("a confirmed box starts a trail at its centre; an unconfirmed one none",
          vision_trails_points(&tr, 0, xs, ys, VISION_TRAIL_POINTS) == 1 && xs[0] == 10 && ys[0] == 5 &&
              vision_trails_points(&tr, 1, xs, ys, VISION_TRAIL_POINTS) == 0);
    s[0] = shown(7, 3, 0);
    vision_trails_update(&tr, s, 1);
    check("a move under the step adds nothing: a parked car leaves no smear",
          vision_trails_points(&tr, 0, xs, ys, VISION_TRAIL_POINTS) == 1);
    for (i = 1; i <= 20; i++) {
        s[0] = shown(7, 10 * i, 0);
        vision_trails_update(&tr, s, 1);
    }
    n = vision_trails_points(&tr, 0, xs, ys, VISION_TRAIL_POINTS);
    check("the trail keeps its newest points, oldest first", n == VISION_TRAIL_POINTS && xs[0] == 10 * 13 + 10 &&
                                                                 xs[n - 1] == 10 * 20 + 10);
    check("fewer asked for, fewer given", vision_trails_points(&tr, 0, xs, ys, 3) == 3);
    for (i = 0; i < VISION_TRAIL_KEEP - 1; i++) {
        vision_trails_update(&tr, s, 0);
    }
    check("a track missing from a few det lines keeps its trail", vision_trails_points(&tr, 0, xs, ys, VISION_TRAIL_POINTS) > 0);
    vision_trails_update(&tr, s, 0);
    check("then it is let go", vision_trails_points(&tr, 0, xs, ys, VISION_TRAIL_POINTS) == 0);
    s[0] = shown(8, 500, 500);
    vision_trails_update(&tr, s, 1);
    n = vision_trails_points(&tr, 0, xs, ys, VISION_TRAIL_POINTS);
    check("a new id never inherits an old trail", n == 1 && xs[0] == 510);
    for (i = 0; i <= VISION_MAX_SHOWN; i++) {
        s[i] = shown(100 + (uint32_t)i, i * 30, 0);
    }
    vision_trails_update(&tr, s, VISION_MAX_SHOWN + 1);
    n = 0;
    for (i = 0; i < VISION_TRAIL_TRACKS; i++) {
        n += vision_trails_points(&tr, i, xs, ys, VISION_TRAIL_POINTS) > 0;
    }
    check("more tracks than slots: every slot used, none overwritten, the rest wait", n == VISION_TRAIL_TRACKS);
    check("slots out of range are empty", vision_trails_points(&tr, -1, xs, ys, 8) == 0 &&
                                              vision_trails_points(&tr, VISION_TRAIL_TRACKS, xs, ys, 8) == 0);
}

int main(void)
{
    test_states();
    test_picker();
    test_track_and_traffic();
    test_tools();
    test_read();
    test_trails();
    test_layout();
    printf("vision_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
