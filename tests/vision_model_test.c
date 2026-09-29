/*
 * Vision's state machine and layout on their own: every state's words,
 * what each event does, the two modes, the line modes and their
 * directions, the speed lines and the distance, the traffic report's words,
 * and both shapes on the reference panel in both modes with every control
 * a usable, safe, non-overlapping target.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_layout.h"
#include "vision_model.h"

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

static void test_model(void)
{
    struct vision_model m;
    struct vision_view_text t;
    struct vision_event e;
    char buf[256];
    int32_t pm[8];
    const char *a;
    const char *b;

    vision_model_init(&m);
    check("opening starts the helper", vision_model_open(&m) == VISION_ACT_OPEN);
    check("and starts in DETECT with no speed lines and 10 m", m.mode == VISION_MODE_DETECT &&
                                                                   m.speed == VISION_SPEED_OFF && vision_model_distance_cm(&m) == 1000);
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
              (VISION_ACT_STREAM | VISION_ACT_MODE | VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS));
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

    check("the default line is ACROSS", m.line == VISION_LINE_ACROSS && vision_model_line_pm(&m, pm) &&
                                        pm[0] == 0 && pm[1] == 500 && pm[2] == 1000 && pm[3] == 500);
    vision_model_count_names(&m, &a, &b);
    check("counting DOWN and UP", strcmp(a, "DOWN") == 0 && strcmp(b, "UP") == 0);
    check("LINE cycles to DOWN and sends it, with the speed lines that follow it",
          vision_model_line_next(&m) == (VISION_ACT_LINE | VISION_ACT_SPEED) && m.line == VISION_LINE_DOWN);
    vision_model_count_names(&m, &a, &b);
    check("counting LEFT and RIGHT", strcmp(a, "LEFT") == 0 && strcmp(b, "RIGHT") == 0 &&
                                     vision_model_line_pm(&m, pm) && pm[0] == 500 && pm[2] == 500);
    vision_model_line_next(&m);
    check("then OFF", m.line == VISION_LINE_OFF && !vision_model_line_pm(&m, pm));
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("the button says so, and the counters show nothing",
          strcmp(t.line_btn, "LINE: OFF") == 0 && strcmp(t.count_a, "-") == 0);
    vision_model_line_next(&m);
    check("and round to ACROSS", m.line == VISION_LINE_ACROSS);
    m.count_a = 5;
    check("RESET clears the counts and sends it", vision_model_reset(&m) == VISION_ACT_RESET && m.count_a == 0);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("DETECT shows three buttons and one status line", !t.traffic && strcmp(t.mode_btn, "DETECT") == 0 &&
                                                                 vision_model_status_lines(&m) == 1);
    {
        enum vision_button order[VISION_BUTTONS];

        check("MODE, LINE, RESET", vision_model_buttons(&m, order) == 3 && order[0] == VISION_BTN_MODE &&
                                       order[1] == VISION_BTN_LINE && order[2] == VISION_BTN_RESET);
    }

    e = ev(VISION_EV_STATS);
    check("stats without a session change nothing", vision_model_event(&m, &e, NULL, 4000) == 0 && !m.stats_valid);
    {
        struct vision_session s;

        vision_session_init(&s);
        s.stats.fps_x10 = 123;
        s.stats.infer_ms = 31;
        s.stats.cpu_pct = 40;
        s.stats.rss_kb = 20480;
        vision_model_event(&m, &e, &s, 4000);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("stats become the status line", strstr(t.status, "12.3 fps") && strstr(t.status, "KPU 31 ms") &&
                                              strstr(t.status, "CPU 40%") && strstr(t.status, "20 MB"));
        s.count_ab = 3;
        s.count_ba = 1;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_COUNT }, &s, 4100);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("counts come from the session", m.count_a == 3 && m.count_b == 1 && strcmp(t.count_a, "DOWN 3") == 0 &&
                                                  strcmp(t.count_b, "UP 1") == 0);
        s.shown_count = 3;
        s.shown[0].id = 4;
        s.shown[1].id = 0;
        s.shown[2].id = 9;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_DET }, &s, 4200);
        check("a det line says how many confirmed tracks there are", m.active_tracks == 2);

        /* TRAFFIC. */
        check("MODE goes to TRAFFIC and sends the mode with the lines and the distance",
              vision_model_mode_next(&m) == (VISION_ACT_MODE | VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS) &&
                  m.mode == VISION_MODE_TRAFFIC && m.count_a == 0);
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_DET }, &s, 4300);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("TRAFFIC shows five buttons and three status lines",
              t.traffic && strcmp(t.mode_btn, "TRAFFIC") == 0 && vision_model_status_lines(&m) == 3);
        {
            enum vision_button order[VISION_BUTTONS];

            check("MODE, LINE, SPEED, DIST, RESET", vision_model_buttons(&m, order) == 5 && order[2] == VISION_BTN_SPEED &&
                                                        order[3] == VISION_BTN_DISTANCE && order[4] == VISION_BTN_RESET);
        }
        check("the counters are IN and OUT, with the direction", strcmp(t.count_a, "IN (DOWN) 0") == 0 &&
                                                                     strcmp(t.count_b, "OUT (UP) 0") == 0);
        check("the status: fps, KPU, tracks, total; the classes; the speed off",
              strstr(t.status, "12.3 fps") && strstr(t.status, "KPU 31 ms") && strstr(t.status, "2 tracks") &&
                  strstr(t.status, "0 total") && strstr(t.status, "car 0  truck 0  bus 0  moto 0  bike 0  person 0") &&
                  strstr(t.status, "SPEED off") && strstr(t.status, "(10 m)") && !strstr(t.status, "CPU"));
        check("SPEED: OFF, no speed lines", strcmp(t.speed_btn, "SPEED: OFF") == 0 && !vision_model_speed_pm(&m, pm));
        check("SPEED cycles to NARROW and sends it", vision_model_speed_next(&m) == VISION_ACT_SPEED &&
                                                         m.speed == VISION_SPEED_NARROW);
        check("two horizontal lines at 40 % and 60 %, left to right like the counting line",
              vision_model_speed_pm(&m, pm) && pm[0] == 0 && pm[1] == 400 && pm[2] == 1000 && pm[3] == 400 &&
                  pm[4] == 0 && pm[5] == 600 && pm[6] == 1000 && pm[7] == 600);
        vision_model_speed_next(&m);
        check("WIDE: 25 % and 75 %", m.speed == VISION_SPEED_WIDE && vision_model_speed_pm(&m, pm) && pm[1] == 250 && pm[5] == 750);
        vision_model_line_next(&m);
        check("with the counting line DOWN the speed lines stand up, top to bottom",
              m.line == VISION_LINE_DOWN && vision_model_speed_pm(&m, pm) && pm[0] == 250 && pm[1] == 0 &&
                  pm[2] == 250 && pm[3] == 1000 && pm[4] == 750 && pm[7] == 1000);
        vision_model_line_next(&m);
        check("and keep standing while the counting line is OFF", m.line == VISION_LINE_OFF && vision_model_speed_pm(&m, pm) &&
                                                                       pm[0] == 250 && pm[3] == 1000);
        vision_model_line_next(&m);
        vision_model_speed_next(&m);
        check("SPEED round to OFF", m.speed == VISION_SPEED_OFF);
        check("DIST cycles and sends it", vision_model_distance_next(&m) == VISION_ACT_DISTANCE && vision_model_distance_cm(&m) == 1500);
        {
            int i;
            int ok = 1;

            for (i = 0; i < VISION_DISTANCES; i++) {
                ok &= vision_model_distance_cm(&m) >= 100 && vision_model_distance_cm(&m) <= 5000;
                vision_model_distance_next(&m);
            }
            check("every distance is between 1 m and 50 m, and it comes round", ok && vision_model_distance_cm(&m) == 1500);
        }
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("the button says the distance", strcmp(t.dist_btn, "DIST: 15 m") == 0);
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
        vision_model_speed_next(&m);
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRAFFIC }, &s, 5000);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("the report becomes the counters and the status",
              m.traffic_valid && strcmp(t.count_a, "IN (DOWN) 4") == 0 && strcmp(t.count_b, "OUT (UP) 2") == 0 &&
                  strstr(t.status, "6 total") && strstr(t.status, "car 4") && strstr(t.status, "person 2") &&
                  strstr(t.status, "SPEED 43.2 km/h  last 43.2  max 51.0  (15 m)"));
        s.traffic.cur_kmh10 = 0;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRAFFIC }, &s, 5100);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("with no current speed the last one is shown", strstr(t.status, "SPEED --  last 43.2 km/h  max 51.0") != NULL);
        s.traffic.n = 0;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRAFFIC }, &s, 5200);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("with none yet, the lines' distance", strstr(t.status, "SPEED --  (15 m between lines)") != NULL);
        check("RESET in TRAFFIC clears the report too", vision_model_reset(&m) == VISION_ACT_RESET && m.traffic.total_ab == 0);
        /* The pixel modes. */
        check("MODE goes on to COLOR, with the pixel settings",
              vision_model_mode_next(&m) == (VISION_ACT_MODE | VISION_ACT_LINE | VISION_ACT_SPEED | VISION_ACT_DISTANCE | VISION_ACT_PIXELS) &&
                  m.mode == VISION_MODE_COLOR && vision_model_pixel_mode(&m) && strcmp(vision_model_mode_word(&m), "color") == 0);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("COLOR: MODE, SAMPLE, TOL; the picture takes taps; no lines; no counts",
              strcmp(t.mode_btn, "COLOR") == 0 && t.picture_tap && !t.lines && !t.traffic && strcmp(t.count_a, "-") == 0 &&
                  strcmp(t.tol_btn, "TOL: MED") == 0 && vision_model_tol(&m) == 96 && vision_model_status_lines(&m) == 1);
        {
            enum vision_button order[VISION_BUTTONS];

            check("in that order", vision_model_buttons(&m, order) == 3 && order[1] == VISION_BTN_SAMPLE && order[2] == VISION_BTN_TOL);
        }
        check("it asks for a colour", strstr(t.status, "Tap the picture or SAMPLE") != NULL);
        check("SAMPLE asks the helper for the middle", vision_model_sample_middle(&m, 400, 600) == VISION_ACT_SAMPLE &&
                                                           m.sample_x == 200 && m.sample_y == 300);
        check("a tap asks for that point", vision_model_sample_at(&m, 10, 20) == VISION_ACT_SAMPLE && m.sample_x == 10);
        check("a point off the picture does not", vision_model_sample_at(&m, -1, 20) == 0);
        check("TOL cycles and sends", vision_model_tol_next(&m) == VISION_ACT_PIXELS && vision_model_tol(&m) == 160);
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
        check("MODE to EDGE", vision_model_mode_next(&m) & VISION_ACT_MODE);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("EDGE: MODE, EDGE soft; no taps, no mark", m.mode == VISION_MODE_EDGE && strcmp(t.edge_btn, "EDGE: SOFT") == 0 &&
                                                            !t.picture_tap && !t.show_mark && vision_model_edge_threshold(&m) == 0 &&
                                                            strstr(t.status, "Finding edges") != NULL);
        check("EDGE hard sends the threshold", vision_model_edge_next(&m) == VISION_ACT_PIXELS && m.edge_hard &&
                                                   vision_model_edge_threshold(&m) == VISION_EDGE_HARD_THRESHOLD);
        s.pixels.edge_pm = 81;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_EDGE }, &s, 6100);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("the edge share is the status", strstr(t.status, "edges 8.1%") && strstr(t.status, "hard"));
        vision_model_mode_next(&m);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("TRACE: MODE, LINE: DARK", m.mode == VISION_MODE_TRACE && strcmp(t.trace_btn, "LINE: DARK") == 0 &&
                                             strstr(t.status, "Looking for a dark line") != NULL);
        s.pixels.trace.found = true;
        s.pixels.trace.offset_pm = -120;
        s.pixels.trace.slope_pm = 450;
        s.pixels.trace.rows = 300;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRACE }, &s, 6200);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("a line is said with its offset and lean", strstr(t.status, "line left 12%") && strstr(t.status, "leans right 45%") &&
                                                             strstr(t.status, "300 rows"));
        check("LINE: LIGHT sends and forgets the last answer", vision_model_trace_next(&m) == VISION_ACT_PIXELS && !m.trace_dark &&
                                                                   !m.trace_valid);
        s.pixels.trace.found = false;
        s.pixels.trace.rows = 3;
        vision_model_event(&m, &(struct vision_event) { .kind = VISION_EV_TRACE }, &s, 6300);
        vision_model_text(&m, &t, buf, sizeof(buf));
        check("no line is said too", strstr(t.status, "no light line") != NULL);
        check("MODE goes round to DETECT", (vision_model_mode_next(&m) & VISION_ACT_MODE) && m.mode == VISION_MODE_DETECT &&
                                                !vision_model_pixel_mode(&m));
        check("the class names", strcmp(vision_model_traffic_name(3), "moto") == 0 && strcmp(vision_model_traffic_name(6), "?") == 0);
    }

    e = ev(VISION_EV_LOST);
    vision_model_event(&m, &e, NULL, 5000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a lost camera is an error with a way back",
          m.state == VISION_ERROR && t.show_retry && strcmp(t.detail, "The camera went away") == 0);
    m.mode = VISION_MODE_TRAFFIC;
    m.speed = VISION_SPEED_WIDE;
    m.distance_idx = 2;
    m.tol_idx = 2;
    m.edge_hard = true;
    m.trace_dark = false;
    check("Try again keeps every choice and opens again",
          (m.line = VISION_LINE_DOWN, vision_model_open(&m) == VISION_ACT_OPEN) && m.state == VISION_INIT &&
              m.line == VISION_LINE_DOWN && m.mode == VISION_MODE_TRAFFIC && m.speed == VISION_SPEED_WIDE &&
              m.distance_idx == 2 && m.tol_idx == 2 && m.edge_hard && !m.trace_dark && !m.have_target);
    check("a tap while not live changes the choice and sends nothing",
          vision_model_speed_next(&m) == 0 && vision_model_distance_next(&m) == 0 && vision_model_line_next(&m) == 0 &&
              vision_model_mode_next(&m) == VISION_ACT_MODE);
    e = ev(VISION_EV_NOMODEL);
    snprintf(e.text, sizeof(e.text), "model file not found");
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

static int inside(const struct vision_rect *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    return r->x >= x0 && r->y >= y0 && r->x + r->w <= x1 && r->y + r->h <= y1;
}

static int overlap(const struct vision_rect *a, const struct vision_rect *b)
{
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
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
}

static void test_layout(void)
{
    struct vision_layout l;

    /* The reference panel's bodies under the NONE chrome (as Camera: 528 x
     * 1116 portrait, 1192 x 452 landscape), with the corner clearance, in
     * both modes. */
    shape("portrait DETECT", 528, 1116, 0, 0, 0, 30, 360, 640, false, 3, 1);
    shape("landscape DETECT", 1192, 452, 30, 0, 30, 0, 640, 360, true, 3, 1);
    shape("portrait TRAFFIC", 528, 1116, 0, 0, 0, 30, 360, 640, false, 5, 3);
    shape("landscape TRAFFIC", 1192, 452, 30, 0, 30, 0, 640, 360, true, 5, 3);
    shape("portrait EDGE", 528, 1116, 0, 0, 0, 30, 360, 640, false, 2, 1);
    shape("landscape EDGE", 1192, 452, 30, 0, 30, 0, 640, 360, true, 2, 1);
    check("a body too small is refused", vision_layout_compute(&l, 200, 150, 0, 0, 0, 0, 640, 360, 3, 1) == -1);
    check("a frame with no size is refused", vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 0, 360, 3, 1) == -1);
    check("too many buttons or status lines are refused",
          vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 360, 640, 6, 1) == -1 &&
              vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 360, 640, 3, 5) == -1 &&
              vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 360, 640, 0, 1) == -1);
}

int main(void)
{
    test_model();
    test_layout();
    printf("vision_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
