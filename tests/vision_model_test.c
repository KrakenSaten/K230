/*
 * Vision's state machine and layout on their own: every state's words,
 * what each event does, the line modes and their directions, and both
 * shapes on the reference panel with every control a usable, safe,
 * non-overlapping target.
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
    char buf[128];
    int32_t pm[4];
    const char *a;
    const char *b;

    vision_model_init(&m);
    check("opening starts the helper", vision_model_open(&m) == VISION_ACT_OPEN);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("INIT says it is starting", strcmp(t.title, "Starting") == 0 && !t.show_picture && !t.line_enabled);
    e = ev(VISION_EV_READY);
    snprintf(e.text, sizeof(e.text), "fake");
    snprintf(e.name, sizeof(e.name), "yolov8n.kmodel");
    e.w = 640;
    e.h = 360;
    e.value = 80;
    e.simulated = true;
    check("ready streams and sends the line",
          vision_model_event(&m, &e, NULL, 1000) == (VISION_ACT_STREAM | VISION_ACT_LINE));
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
    check("LINE cycles to DOWN and sends it", vision_model_line_next(&m) == VISION_ACT_LINE && m.line == VISION_LINE_DOWN);
    vision_model_count_names(&m, &a, &b);
    check("counting LEFT and RIGHT", strcmp(a, "LEFT") == 0 && strcmp(b, "RIGHT") == 0 &&
                                     vision_model_line_pm(&m, pm) && pm[0] == 500 && pm[2] == 500);
    vision_model_line_next(&m);
    check("then OFF", m.line == VISION_LINE_OFF && !vision_model_line_pm(&m, pm));
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("the button says so", strcmp(t.line_btn, "LINE: OFF") == 0);
    vision_model_line_next(&m);
    check("and round to ACROSS", m.line == VISION_LINE_ACROSS);
    m.count_a = 5;
    check("RESET clears the counts and sends it", vision_model_reset(&m) == VISION_ACT_RESET && m.count_a == 0);

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
        check("counts come from the session", m.count_a == 3 && m.count_b == 1);
    }

    e = ev(VISION_EV_LOST);
    vision_model_event(&m, &e, NULL, 5000);
    vision_model_text(&m, &t, buf, sizeof(buf));
    check("a lost camera is an error with a way back",
          m.state == VISION_ERROR && t.show_retry && strcmp(t.detail, "The camera went away") == 0);
    check("Try again keeps the line choice and opens again",
          (m.line = VISION_LINE_DOWN, vision_model_open(&m) == VISION_ACT_OPEN) && m.state == VISION_INIT &&
              m.line == VISION_LINE_DOWN);
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
                  int32_t ib, uint32_t fw, uint32_t fh, bool wide)
{
    struct vision_layout l;
    const struct vision_rect *all[6];
    char name[160];
    int ok = 1;
    int i;
    int j;
    int64_t err;

    snprintf(name, sizeof(name), "%s: lays out", what);
    check(name, vision_layout_compute(&l, w, h, il, it, ir, ib, fw, fh) == 0);
    snprintf(name, sizeof(name), "%s: the shape is %s", what, wide ? "wide" : "tall");
    check(name, l.wide == wide);
    all[0] = &l.picture;
    all[1] = &l.status;
    all[2] = &l.count_a;
    all[3] = &l.count_b;
    all[4] = &l.line_btn;
    all[5] = &l.reset_btn;
    for (i = 0; i < 6; i++) {
        ok &= inside(all[i], il, it, w - ir, h - ib);
    }
    snprintf(name, sizeof(name), "%s: everything inside the safe box", what);
    check(name, ok);
    ok = l.line_btn.h >= TOUCH_MIN && l.reset_btn.h >= TOUCH_MIN && l.line_btn.w >= 100 && l.reset_btn.w >= 100;
    snprintf(name, sizeof(name), "%s: the buttons are usable targets", what);
    check(name, ok);
    ok = 1;
    for (i = 0; i < 6; i++) {
        for (j = i + 1; j < 6; j++) {
            ok &= !overlap(all[i], all[j]);
        }
    }
    snprintf(name, sizeof(name), "%s: nothing overlaps", what);
    check(name, ok);
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
     * 1116 portrait, 1192 x 452 landscape), with the corner clearance. */
    shape("portrait", 528, 1116, 0, 0, 0, 30, 360, 640, false);
    shape("landscape", 1192, 452, 30, 0, 30, 0, 640, 360, true);
    check("a body too small is refused", vision_layout_compute(&l, 200, 150, 0, 0, 0, 0, 640, 360) == -1);
    check("a frame with no size is refused", vision_layout_compute(&l, 528, 1116, 0, 0, 0, 0, 0, 360) == -1);
}

int main(void)
{
    test_model();
    test_layout();
    printf("vision_model_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
