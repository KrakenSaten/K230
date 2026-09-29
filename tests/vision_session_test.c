/*
 * Vision's helper client against the real helper (pos-vision) on the fake
 * camera and the fake detector: the protocol end to end, pictures through
 * the shared memory, boxes said in view pixels for the objects the fake
 * puts in, ids that persist, the line counting a crossing in its direction,
 * reset, a malformed tensor said and survived, a detector that keeps
 * giving nonsense ending the session, no camera, a busy camera, a missing
 * model, a hung helper (the watchdog), one that crashes, and thirty opens
 * and closes with no descriptor or child left behind.
 *
 * Usage: vision_session_test <path to pos-vision>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "vision_session.h"
#include "vision_settings.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static char helper[PATH_MAX];

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

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d))) {
        n += e->d_name[0] != '.';
    }
    closedir(d);
    return n - 1; /* the directory's own descriptor */
}

/* Wait up to ms for an event of this kind, pumping the session. Other
 * events are handled by `seen` (may be NULL). */
static int wait_for(struct vision_session *s, enum vision_ev_kind kind, int ms, struct vision_event *out,
                    void (*seen)(const struct vision_event *, void *), void *user)
{
    int64_t end = now_ms() + ms;

    while (now_ms() < end) {
        struct vision_event ev;

        while (vision_session_poll(s, &ev, now_ms())) {
            if (seen) {
                seen(&ev, user);
            }
            if (ev.kind == kind) {
                if (out) {
                    *out = ev;
                }
                return 1;
            }
        }
        nap(5);
    }
    return 0;
}

static int start(struct vision_session *s, const char *fake, const char *kpu, const char *model)
{
    struct vision_session_config cfg = { 0 };
    char err[96] = "";
    int r;

    cfg.helper = helper;
    cfg.backend = "fake";
    cfg.fake = fake;
    cfg.kpu = kpu;
    cfg.model = model;
    r = vision_session_start(s, &cfg, now_ms(), err, sizeof(err));
    if (r != 0) {
        printf("     start: %s\n", err);
    }
    return r;
}

static void test_parse(void)
{
    struct vision_session s;
    struct vision_event ev;

    vision_session_init(&s);
    check("ready parses", vision_session_parse_line(&s, "ready fake 640 360 1 yolov8n.kmodel 320 320 80", &ev) &&
                              ev.kind == VISION_EV_READY && ev.w == 640 && ev.simulated && ev.value == 80 &&
                              strcmp(ev.name, "yolov8n.kmodel") == 0);
    check("ready with a missing field does not", !vision_session_parse_line(&s, "ready fake 640 360 1", &ev));
    check("caps parse into a bit per mode", vision_session_parse_line(&s, "caps detect track traffic color", &ev) &&
                                                ev.kind == VISION_EV_CAPS &&
                                                ev.value == ((1 << VISION_MODE_DETECT) | (1 << VISION_MODE_TRACK) |
                                                             (1 << VISION_MODE_TRAFFIC) | (1 << VISION_MODE_COLOR)));
    check("a mode word this build does not know is skipped, a word too long for any too",
          vision_session_parse_line(&s, "caps detect hologram readreadreadreadreadread  face", &ev) &&
              ev.value == ((1 << VISION_MODE_DETECT) | (1 << VISION_MODE_FACE)));
    check("caps with nothing offered is an empty set", vision_session_parse_line(&s, "caps", &ev) &&
                                                           ev.kind == VISION_EV_CAPS && ev.value == 0);
    check("capsule is not caps", !vision_session_parse_line(&s, "capsule detect", &ev));
    check("frame parses", vision_session_parse_line(&s, "frame 2 17 528 938", &ev) && ev.kind == VISION_EV_FRAME &&
                              ev.value == 2 && ev.w == 528 && ev.h == 938);
    check("a frame in the review slot does not", !vision_session_parse_line(&s, "frame 3 17 528 938", &ev));
    check("a det line parses into the session",
          vision_session_parse_line(&s, "det 40 2 1:0:900:10:20:30:40:4:0 0:2:600:100:200:50:60:0:432", &ev) &&
              ev.kind == VISION_EV_DET && ev.value == 40 && s.shown_count == 2 && s.shown[0].id == 1 &&
              s.shown[0].cls == 0 && s.shown[0].conf == 900 && s.shown[0].w == 30 && s.shown[0].dir == 4 &&
              s.shown[0].kmh10 == 0 && s.shown[1].id == 0 && s.shown[1].x == 100 && s.shown[1].dir == 0 &&
              s.shown[1].kmh10 == 432 && s.shown_seq == 40);
    check("an empty det line parses", vision_session_parse_line(&s, "det 41 0", &ev) && s.shown_count == 0);
    check("a det line with fewer boxes than it says does not",
          !vision_session_parse_line(&s, "det 42 2 1:0:900:10:20:30:40:0:0", &ev));
    check("nor one with more", !vision_session_parse_line(&s, "det 42 1 1:0:900:10:20:30:40:0:0 2:0:900:1:2:3:4:0:0", &ev));
    check("nor a box outside any view", !vision_session_parse_line(&s, "det 42 1 1:0:900:1000:20:30:40:0:0", &ev));
    check("nor an impossible confidence", !vision_session_parse_line(&s, "det 42 1 1:0:1900:10:20:30:40:0:0", &ev));
    check("nor a direction that is not one", !vision_session_parse_line(&s, "det 42 1 1:0:900:10:20:30:40:5:0", &ev));
    check("nor the old seven-field box", !vision_session_parse_line(&s, "det 42 1 1:0:900:10:20:30:40", &ev));
    check("count parses", vision_session_parse_line(&s, "count 3 1", &ev) && s.count_ab == 3 && s.count_ba == 1);
    check("a traffic line parses into the report",
          vision_session_parse_line(&s, "traffic 4 2 432 432 510 400 3 1 3:1 0:0 0:0 0:0 0:0 1:1", &ev) &&
              ev.kind == VISION_EV_TRAFFIC && s.traffic.total_ab == 4 && s.traffic.total_ba == 2 &&
              s.traffic.cur_kmh10 == 432 && s.traffic.max_kmh10 == 510 && s.traffic.mean_kmh10 == 400 &&
              s.traffic.n == 3 && s.traffic.rejected == 1 && s.traffic.cls_ab[0] == 3 && s.traffic.cls_ba[0] == 1 &&
              s.traffic.cls_ab[5] == 1 && s.traffic.cls_ba[5] == 1);
    check("a color line parses into the report",
          vision_session_parse_line(&s, "color 200 30 30 123 150 80", &ev) && ev.kind == VISION_EV_COLOR &&
              s.pixels.color.r == 200 && s.pixels.color.b == 30 && s.pixels.color.matched_pm == 123 &&
              s.pixels.color.cx == 150 && s.pixels.color.cy == 80);
    check("a colour channel over 255 does not", !vision_session_parse_line(&s, "color 300 0 0 1 1 1", &ev));
    check("an edge line parses", vision_session_parse_line(&s, "edge 81", &ev) && ev.kind == VISION_EV_EDGE &&
                                     s.pixels.edge_pm == 81);
    check("a share over 1000 does not", !vision_session_parse_line(&s, "edge 1001", &ev));
    check("a trace line parses", vision_session_parse_line(&s, "trace 1 -120 450 300", &ev) &&
                                     ev.kind == VISION_EV_TRACE && s.pixels.trace.found && s.pixels.trace.offset_pm == -120 &&
                                     s.pixels.trace.slope_pm == 450 && s.pixels.trace.rows == 300);
    check("a trace with an impossible offset does not", !vision_session_parse_line(&s, "trace 1 -2000 0 1", &ev));
    check("a traffic line short of a class does not",
          !vision_session_parse_line(&s, "traffic 4 2 432 432 510 400 3 1 3:1 0:0 0:0 0:0 0:0", &ev));
    check("nor one with junk after", !vision_session_parse_line(&s, "traffic 4 2 432 432 510 400 3 1 3:1 0:0 0:0 0:0 0:0 1:1 x", &ev));
    check("stats parses", vision_session_parse_line(&s, "stats 95 31 4 2 40 20480 1 0", &ev) &&
                              s.stats.fps_x10 == 95 && s.stats.infer_ms == 31 && s.stats.rss_kb == 20480 &&
                              s.stats.bad == 1);
    check("nodevice carries its text", vision_session_parse_line(&s, "nodevice no camera", &ev) &&
                                           ev.kind == VISION_EV_NODEVICE && strcmp(ev.text, "no camera") == 0);
    check("nomodel too", vision_session_parse_line(&s, "nomodel model file not found", &ev) &&
                             ev.kind == VISION_EV_NOMODEL && strcmp(ev.text, "model file not found") == 0);
    check("an unknown word is not an event", !vision_session_parse_line(&s, "weather sunny", &ev));
}

struct watch {
    int frames;
    int dets;
    int dets_with_boxes;
    uint32_t last_id;
    int malformed;
    int counts;
    struct vision_session *s; /* frames are taken as they come, so they keep coming */
};

static uint16_t taken[360 * 640];

static void seen(const struct vision_event *ev, void *user)
{
    struct watch *w = user;

    switch (ev->kind) {
    case VISION_EV_FRAME:
        w->frames++;
        if (w->s) {
            vision_session_take_frame(w->s, taken, 360, 640);
        }
        break;
    case VISION_EV_DET: w->dets++; break;
    case VISION_EV_MALFORMED: w->malformed++; break;
    case VISION_EV_COUNT: w->counts++; break;
    default: break;
    }
}

static void test_happy(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    static uint16_t px[360 * 640];
    int32_t pm[4] = { 0, 500, 1000, 500 };
    int n = 0;
    const struct vision_shown *t;
    int i;
    int fds = open_fds();
    int ok;

    vision_session_init(&s);
    /* A person walking down the frame (in sensor space, x grows with the
     * frame count), a car standing still. The fake camera turns 90 like
     * unit A's, so the view is 360 x 640 portrait for a 640 x 360 frame,
     * and the sensor's x is the picture's y: the walk goes down the screen,
     * across the ACROSS line at mid-height. */
    check("the helper starts",
          start(&s, "period=20", "box=0:900:40:100:60:120:12:0,box=2:700:400:200:120:60", NULL) == 0);
    check("ready arrives with the camera and the model",
          wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w) && strcmp(ev.text, "fake") == 0 &&
              strcmp(ev.name, "fake-detector") == 0 && ev.w == 640 && ev.h == 360 && ev.simulated &&
              ev.value == 80);
    vision_session_view(&s, 360, 640, 0);
    vision_session_line(&s, pm);
    check("the line answers with zero counts", wait_for(&s, VISION_EV_COUNT, 2000, &ev, seen, &w));
    vision_session_stream(&s, true, now_ms());
    check("a picture arrives", wait_for(&s, VISION_EV_FRAME, 3000, &ev, seen, &w) && ev.w == 360 && ev.h == 640);
    check("and is copied out of the shared memory", vision_session_take_frame(&s, px, 360, 640) == 1);
    w.s = &s;
    check("a det line arrives", wait_for(&s, VISION_EV_DET, 3000, &ev, seen, &w));
    /* Let the tracker confirm both objects. */
    for (i = 0; i < 4; i++) {
        wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w);
    }
    t = vision_session_tracks(&s, &n, NULL);
    check("two objects are tracked, in view pixels", n == 2);
    ok = n == 2;
    for (i = 0; ok && i < n; i++) {
        ok &= t[i].id != 0 && t[i].x >= 0 && t[i].y >= 0 && t[i].x + t[i].w <= 360 && t[i].y + t[i].h <= 640 &&
              t[i].conf > 0;
    }
    check("both confirmed with ids, inside the view", ok);
    {
        uint32_t id_person = 0;
        uint32_t id_car = 0;

        for (i = 0; i < n; i++) {
            if (t[i].cls == 0) {
                id_person = t[i].id;
            } else if (t[i].cls == 2) {
                id_car = t[i].id;
            }
        }
        check("the person and the car each have an id", id_person && id_car && id_person != id_car);
        /* Twenty frames later the same ids. The person moves in the
         * sensor's x, which after the quarter turn is the view's y: it
         * walks down the picture and crosses the mid-height line. */
        for (i = 0; i < 25; i++) {
            wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w);
        }
        t = vision_session_tracks(&s, &n, NULL);
        ok = n == 2;
        for (i = 0; ok && i < n; i++) {
            ok &= (t[i].cls == 0 && t[i].id == id_person) || (t[i].cls == 2 && t[i].id == id_car);
        }
        check("the ids persist across thirty frames", ok);
    }
    check("the crossing was counted", wait_for(&s, VISION_EV_COUNT, 3000, &ev, seen, &w) || w.counts >= 2);
    {
        uint32_t ab;
        uint32_t ba;

        vision_session_counts(&s, &ab, &ba);
        check("once, downward", ab == 1 && ba == 0);
    }
    check("stats arrive once a second", wait_for(&s, VISION_EV_STATS, 2500, &ev, seen, &w) &&
                                            vision_session_stats(&s)->fps_x10 > 0);
    vision_session_reset(&s);
    check("reset answers with zero counts", wait_for(&s, VISION_EV_COUNT, 2000, &ev, seen, &w) && s.count_ab == 0);
    check("pictures keep coming", w.frames >= 3);
    vision_session_stream(&s, false, now_ms());
    check("stop is answered", wait_for(&s, VISION_EV_STOPPED, 2000, &ev, seen, &w));
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));
    check("no descriptor is left behind", open_fds() == fds);
}

/* TRAFFIC against the real helper: a car driving down the picture (the
 * sensor's x, after the quarter turn) through the two speed lines and the
 * counting line; a chair that is not traffic; the speed from the fed
 * distance and the helper's clock; the report; reset. */
static void test_traffic(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    int32_t line[4] = { 0, 500, 1000, 500 };
    int32_t speed[8] = { 0, 400, 1000, 400, 0, 600, 1000, 600 };
    int i;
    int n = 0;
    const struct vision_shown *t;
    bool saw_dir = false;
    bool saw_chair = false;
    bool saw_speed = false;
    uint32_t car_id = 0;
    bool one_id = true;
    int64_t at_a = 0;      /* when this test first saw the car's centre past line A (view y 256) */
    int64_t at_b = 0;      /* ... and past line B (384) */
    uint32_t expected = 0; /* km/h x10 from 5 m and that time */

    vision_session_init(&s);
    /* The car: 80 x 60 at 16 px a frame along the sensor's x, from the
     * top of the picture off the bottom. The chair stands still. Frames
     * every 20 ms: 128 px between the speed lines is 8 frames, 160 ms; at
     * 5 m that is 31.25 m/s, 112.5 km/h - when the helper keeps up with
     * the fake camera, which under a sanitizer it does not. So the
     * expectation is measured here too: the time this test sees between
     * the car's centre passing the two lines, over the same 5 m. */
    check("the helper starts",
          start(&s, "period=20", "box=2:800:0:150:80:60:16:0,box=56:700:400:40:60:60", NULL) == 0);
    check("ready", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w));
    check("then the modes it can run: the detector's and the pixel modes, no model-less ones",
          wait_for(&s, VISION_EV_CAPS, 1000, &ev, seen, &w) &&
              ev.value == ((1 << VISION_MODE_DETECT) | (1 << VISION_MODE_TRACK) | (1 << VISION_MODE_TRAFFIC) |
                           (1 << VISION_MODE_COLOR) | (1 << VISION_MODE_EDGE) | (1 << VISION_MODE_TRACE)));
    vision_session_view(&s, 360, 640, 0);
    vision_session_mode(&s, true);
    check("traffic mode answers with an empty report",
          wait_for(&s, VISION_EV_TRAFFIC, 2000, &ev, seen, &w) && s.traffic.total_ab == 0 && s.traffic.n == 0);
    vision_session_line(&s, line);
    vision_session_speed_lines(&s, speed);
    vision_session_distance(&s, 500);
    vision_session_stream(&s, true, now_ms());
    for (i = 0; i < 60; i++) {
        if (!wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w)) {
            break;
        }
        t = vision_session_tracks(&s, &n, NULL);
        {
            int j;

            for (j = 0; j < n; j++) {
                saw_chair |= t[j].cls == 56;
                if (t[j].cls == 2 && t[j].id) {
                    if (car_id == 0) {
                        car_id = t[j].id;
                    } else if (t[j].id != car_id) {
                        one_id = false;
                    }
                    saw_dir |= t[j].dir == VISION_DIR_DOWN;
                    saw_speed |= t[j].kmh10 > 0;
                    if (!at_a && t[j].y + t[j].h / 2 > 256) {
                        at_a = now_ms();
                    }
                    if (!at_b && t[j].y + t[j].h / 2 > 384) {
                        at_b = now_ms();
                    }
                }
            }
        }
    }
    check("the chair is never tracked in traffic mode", !saw_chair);
    check("the car keeps one id all the way", car_id != 0 && one_id);
    check("and is said to be going down the picture", saw_dir);
    check("the counting line counted it IN, as a car",
          s.traffic.total_ab == 1 && s.traffic.total_ba == 0 && s.traffic.cls_ab[0] == 1 && s.count_ab == 1);
    if (at_a && at_b > at_a) {
        expected = (uint32_t)((500u * 360u) / (uint64_t)(at_b - at_a));
    }
    check("the speed lines measured it, within 40 % of 5 m over the time this test saw between the lines",
          s.traffic.n == 1 && expected > 0 && s.traffic.last_kmh10 * 10 >= expected * 6 &&
              s.traffic.last_kmh10 * 10 <= expected * 14 && s.traffic.last_kmh10 >= 200 &&
              s.traffic.last_kmh10 <= 3000 && s.traffic.max_kmh10 == s.traffic.last_kmh10 &&
              s.traffic.mean_kmh10 == s.traffic.last_kmh10);
    printf("     measured %u.%u km/h, this test expected %u.%u\n", s.traffic.last_kmh10 / 10,
           s.traffic.last_kmh10 % 10, expected / 10, expected % 10);
    check("the speed was shown on the car's box while it was tracked", saw_speed);
    check("and retired with the track", s.traffic.cur_kmh10 == 0);
    vision_session_reset(&s);
    for (i = 0; i < 4; i++) {
        if (wait_for(&s, VISION_EV_TRAFFIC, 2000, &ev, seen, &w) && s.traffic.n == 0) {
            break;
        }
    }
    check("reset zeroes the report", s.traffic.total_ab == 0 && s.traffic.n == 0 && s.traffic.last_kmh10 == 0);
    vision_session_mode(&s, false);
    for (i = 0; i < 20; i++) {
        wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w);
    }
    t = vision_session_tracks(&s, &n, NULL);
    saw_chair = false;
    for (i = 0; i < n; i++) {
        saw_chair |= t[i].cls == 56;
    }
    check("back in detect mode the chair is tracked again", saw_chair);
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));
}

/* The pixel modes against the real helper: EDGE finds the fake camera's
 * edges; COLOR samples the picture's middle and paints its matches; TRACE
 * answers; the boxes go while a pixel mode is on and come back after. */
static void test_pixels(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    static uint16_t px[360 * 640];
    int i;
    int n = 0;

    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "box=0:900:100:100:60:120", NULL) == 0);
    check("ready", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w));
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    w.s = &s; /* frames are taken as they come, so the slots come back */
    for (i = 0; i < 4; i++) {
        wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w);
    }
    vision_session_tracks(&s, &n, NULL);
    check("a box is tracked in DETECT", n == 1);
    vision_session_mode_word(&s, "edge");
    for (i = 0; i < 5; i++) {
        if (wait_for(&s, VISION_EV_DET, 2000, &ev, seen, &w) && s.shown_count == 0) {
            break;
        }
    }
    check("EDGE empties the boxes", s.shown_count == 0);
    check("and says its edges", wait_for(&s, VISION_EV_EDGE, 2000, &ev, seen, &w));
    /* The fake camera.s bars differ little in luma: few strong edges. */
    check("the fake camera.s picture has few strong edges", s.pixels.edge_pm <= 100);
    vision_session_edge(&s, 40);
    check("a hard threshold still answers", wait_for(&s, VISION_EV_EDGE, 2000, &ev, seen, &w));
    w.frames = 0;
    check("pictures keep coming in EDGE", wait_for(&s, VISION_EV_FRAME, 2000, &ev, seen, &w) && w.frames >= 1);
    vision_session_mode_word(&s, "color");
    vision_session_tol(&s, 96);
    for (i = 0; i < 3; i++) {
        wait_for(&s, VISION_EV_FRAME, 1000, &ev, seen, &w);
    }
    (void)px;
    check("without a target COLOR says nothing", !wait_for(&s, VISION_EV_COLOR, 400, &ev, seen, &w));
    vision_session_sample(&s, 180, 320);
    check("a sample in the middle brings a colour report", wait_for(&s, VISION_EV_COLOR, 2000, &ev, seen, &w));
    /* The fake camera's bars move: a 5 x 5 sample that straddles two of
     * them is a mean no pixel has, and matches nothing on that frame. The
     * target stays, and the bars bring it back within a few frames. */
    for (i = 0; i < 20 && s.pixels.color.matched_pm == 0; i++) {
        wait_for(&s, VISION_EV_COLOR, 1000, &ev, seen, &w);
    }
    check("and its colour is found on the picture", s.pixels.color.matched_pm > 0);
    printf("     sampled #%02x%02x%02x, %u.%u%% matched\n", s.pixels.color.r, s.pixels.color.g, s.pixels.color.b,
           s.pixels.color.matched_pm / 10, s.pixels.color.matched_pm % 10);
    vision_session_mode_word(&s, "trace");
    vision_session_trace(&s, false);
    check("TRACE answers", wait_for(&s, VISION_EV_TRACE, 2000, &ev, seen, &w));
    vision_session_mode_word(&s, "detect");
    for (i = 0; i < 6; i++) {
        wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w);
    }
    vision_session_tracks(&s, &n, NULL);
    check("back in DETECT the box is tracked again", n == 1);
    check("stats still come", wait_for(&s, VISION_EV_STATS, 2500, &ev, seen, &w));
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));
}

static void test_malformed(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    int i;

    /* Frame 3's tensor is NaN, frame 5's claims the wrong shape: each is
     * said as malformed and detection goes on. */
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "box=0:900:100:100:60:120,malformed_at=3,shape_at=5", NULL) == 0);
    check("ready", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w));
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    for (i = 0; i < 10; i++) {
        wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w);
    }
    check("two malformed tensors were said", w.malformed == 2);
    check("and detection went on around them", w.dets >= 6 && vision_session_active(&s));
    check("the stats count them", wait_for(&s, VISION_EV_STATS, 2500, &ev, seen, &w) && s.stats.bad == 2);
    vision_session_abandon(&s, 1000);

    /* A detector that gives nonsense every time is declared broken. */
    vision_session_init(&s);
    memset(&w, 0, sizeof(w));
    check("the helper starts", start(&s, "period=10", "box=0:900:100:100:60:120,bad_from=1", NULL) == 0);
    wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w);
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    check("after enough bad tensors the helper gives up with an error",
          wait_for(&s, VISION_EV_ERROR, 5000, &ev, seen, &w) && strncmp(ev.text, "model", 5) == 0);
    check("and leaves with 5", wait_for(&s, VISION_EV_EXITED, 3000, &ev, seen, &w) && ev.value == 5);
    vision_session_abandon(&s, 100);

    /* A run that fails outright. */
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=10", "box=0:900:100:100:60:120,fail_at=2", NULL) == 0);
    wait_for(&s, VISION_EV_READY, 3000, &ev, NULL, NULL);
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    check("a failed run is an error", wait_for(&s, VISION_EV_ERROR, 3000, &ev, NULL, NULL) &&
                                          strncmp(ev.text, "infer", 5) == 0);
    vision_session_abandon(&s, 500);
}

static void test_failures(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct vision_session_config cfg = { 0 };
    char err[96];

    vision_session_init(&s);
    check("no camera", start(&s, "open=nodev", NULL, NULL) == 0 && wait_for(&s, VISION_EV_NODEVICE, 3000, &ev, NULL, NULL));
    check("then the helper leaves with 3", wait_for(&s, VISION_EV_EXITED, 3000, &ev, NULL, NULL) && ev.value == 3);
    vision_session_abandon(&s, 100);

    vision_session_init(&s);
    check("a busy camera", start(&s, "open=busy", NULL, NULL) == 0 && wait_for(&s, VISION_EV_ERROR, 3000, &ev, NULL, NULL) &&
                               strncmp(ev.text, "busy", 4) == 0);
    vision_session_abandon(&s, 500);

    vision_session_init(&s);
    check("a bad detector script is no model",
          start(&s, "period=20", "nonsense=1", NULL) == 0 && wait_for(&s, VISION_EV_NOMODEL, 3000, &ev, NULL, NULL));
    check("and the helper leaves with 5", wait_for(&s, VISION_EV_EXITED, 3000, &ev, NULL, NULL) && ev.value == 5);
    vision_session_abandon(&s, 100);

    vision_session_init(&s);
    cfg.helper = "/nonexistent/pos-vision";
    cfg.backend = "fake";
    check("a missing helper starts (the child says so)", vision_session_start(&s, &cfg, now_ms(), err, sizeof(err)) == 0);
    check("and is an exec error", wait_for(&s, VISION_EV_ERROR, 3000, &ev, NULL, NULL) && strncmp(ev.text, "exec", 4) == 0);
    vision_session_abandon(&s, 500);

    /* The camera goes away mid-stream. */
    vision_session_init(&s);
    check("a camera that goes away", start(&s, "period=10,lost_after=5", "box=0:900:100:100:60:120", NULL) == 0);
    wait_for(&s, VISION_EV_READY, 3000, &ev, NULL, NULL);
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    check("is lost", wait_for(&s, VISION_EV_LOST, 3000, &ev, NULL, NULL));
    check("and the helper leaves with 4", wait_for(&s, VISION_EV_EXITED, 3000, &ev, NULL, NULL) && ev.value == 4);
    vision_session_abandon(&s, 100);

    /* A helper stuck in the driver is killed by the watchdog. */
    vision_session_init(&s);
    check("a helper that hangs on a frame", start(&s, "period=10,hang_at=3", "box=0:900:100:100:60:120", NULL) == 0);
    wait_for(&s, VISION_EV_READY, 3000, &ev, NULL, NULL);
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    check("is killed by the watchdog", wait_for(&s, VISION_EV_EXITED, VISION_SILENCE_MS + 3000, &ev, NULL, NULL) &&
                                           ev.reason == VISION_EXIT_HUNG);
    vision_session_abandon(&s, 100);

    /* One that crashes. */
    vision_session_init(&s);
    check("a helper that crashes", start(&s, "period=10,crash_at=3", "box=0:900:100:100:60:120", NULL) == 0);
    wait_for(&s, VISION_EV_READY, 3000, &ev, NULL, NULL);
    vision_session_view(&s, 360, 640, 0);
    vision_session_stream(&s, true, now_ms());
    check("is reported as crashed", wait_for(&s, VISION_EV_EXITED, 5000, &ev, NULL, NULL) && ev.reason == VISION_EXIT_CRASHED);
    vision_session_abandon(&s, 100);
}

static void test_lifetime(void)
{
    struct vision_session s;
    struct vision_event ev;
    int fds = open_fds();
    int i;
    int ok = 1;

    for (i = 0; i < 30; i++) {
        vision_session_init(&s);
        ok &= start(&s, "period=20", "box=0:900:100:100:60:120", NULL) == 0;
        ok &= wait_for(&s, VISION_EV_READY, 3000, &ev, NULL, NULL);
        vision_session_view(&s, 360, 640, 0);
        vision_session_stream(&s, true, now_ms());
        ok &= wait_for(&s, VISION_EV_DET, 3000, &ev, NULL, NULL);
        vision_session_abandon(&s, 500);
        ok &= !vision_session_active(&s);
    }
    check("thirty opens and closes", ok);
    check("leave no descriptor behind", open_fds() == fds);
    check("and no child", waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pos-vision>\n", argv[0]);
        return 2;
    }
    if (!realpath(argv[1], helper)) {
        snprintf(helper, sizeof(helper), "%s", argv[1]);
    }
    signal(SIGPIPE, SIG_IGN);
    test_parse();
    test_happy();
    test_traffic();
    test_pixels();
    test_malformed();
    test_failures();
    test_lifetime();
    printf("vision_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
