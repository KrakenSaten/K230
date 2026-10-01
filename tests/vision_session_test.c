/*
 * Vision's helper client against the real helper (pos-vision) on the fake
 * camera and the fake detector: the protocol end to end, pictures through
 * the shared memory, boxes said in view pixels for the objects the fake
 * puts in, ids that persist, the line counting a crossing in its direction,
 * reset, a malformed tensor said and survived, a detector that keeps
 * giving nonsense ending the session, no camera, a busy camera, a missing
 * model, a hung helper (the watchdog), one that crashes, and thirty opens
 * and closes with no descriptor or child left behind - each one ending
 * with `bye`. A model that is slow to open is waited for, not killed, and
 * so is a close in the middle of it; one that never opens is killed all
 * the same. A turn of the display while tracks are held counts nothing.
 *
 * Usage: vision_session_test <path to pos-vision>
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
#include <sys/stat.h>
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
    check("a recent line parses into the report",
          vision_session_parse_line(&s, "recent 300 5 3 2 4 0 0 0 0 1 3 432 0", &ev) && ev.kind == VISION_EV_RECENT &&
              s.recent.window_s == 300 && s.recent.crossed == 5 && s.recent.ab == 3 && s.recent.ba == 2 &&
              s.recent.cls[0] == 4 && s.recent.cls[5] == 1 && s.recent.speeds == 3 && s.recent.mean_kmh10 == 432 &&
              !s.recent.saturated);
    check("a saturated one", vision_session_parse_line(&s, "recent 300 1 1 0 1 0 0 0 0 0 0 0 1", &ev) &&
                                 s.recent.saturated);
    check("not one whose directions do not add up", !vision_session_parse_line(&s, "recent 300 5 3 1 4 0 0 0 0 1 0 0 0", &ev));
    check("nor a mean with no speeds", !vision_session_parse_line(&s, "recent 300 0 0 0 0 0 0 0 0 0 0 432 0", &ev));
    check("nor a field short, or one too many", !vision_session_parse_line(&s, "recent 300 0 0 0 0 0 0 0 0 0 0 0", &ev) &&
                                                   !vision_session_parse_line(&s, "recent 300 0 0 0 0 0 0 0 0 0 0 0 0 9", &ev));
    check("nor a saturation that is not 0 or 1", !vision_session_parse_line(&s, "recent 300 0 0 0 0 0 0 0 0 0 0 0 2", &ev));
    check("a text line parses, its text decoded (UTF-8 and escapes)",
          vision_session_parse_line(&s, "text 7 2 10:20:100:30:990:EXIT%2012 5:60:80:20:950:%E5%85%B6%3A%25", &ev) &&
              ev.kind == VISION_EV_TEXT && s.text.n == 2 && s.text.seq == 7 && s.text.line[0].x == 10 &&
              s.text.line[0].conf == 990 && strcmp(s.text.line[0].text, "EXIT 12") == 0 &&
              strcmp(s.text.line[1].text, "\xe5\x85\xb6:%") == 0);
    check("an empty read parses", vision_session_parse_line(&s, "text 8 0", &ev) && s.text.n == 0);
    check("not a broken escape", !vision_session_parse_line(&s, "text 9 1 10:20:100:30:990:A%2", &ev) &&
                                     !vision_session_parse_line(&s, "text 9 1 10:20:100:30:990:A%ZZ", &ev));
    check("nor an escaped control byte", !vision_session_parse_line(&s, "text 9 1 10:20:100:30:990:A%0AB", &ev));
    check("nor a line too long for the screen's copy",
          !vision_session_parse_line(&s, "text 9 1 1:2:3:4:5:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", &ev));
    check("nor fewer lines than it says, or more", !vision_session_parse_line(&s, "text 9 2 1:2:3:4:5:A", &ev) &&
                                                      !vision_session_parse_line(&s, "text 9 1 1:2:3:4:5:A 1:2:3:4:5:B", &ev));
    check("nor a box outside any view, or a confidence over 1000",
          !vision_session_parse_line(&s, "text 9 1 1000:2:300:4:5:A", &ev) &&
              !vision_session_parse_line(&s, "text 9 1 1:2:3:4:5000:A", &ev));
    check("nor nine lines", !vision_session_parse_line(&s, "text 9 9", &ev));
    check("a read that cannot be done is said with why", vision_session_parse_line(&s, "readfail the text models are missing", &ev) &&
                                                             ev.kind == VISION_EV_READFAIL &&
                                                             strcmp(ev.text, "the text models are missing") == 0);
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
    for (i = 0; i < 3 && !(s.recent.crossed == 1 && s.recent.speeds == 1); i++) {
        wait_for(&s, VISION_EV_RECENT, 1500, &ev, seen, &w);
    }
    check("the last five minutes hold the car: one crossing IN, a car, its speed",
          s.recent.window_s == 300 && s.recent.crossed == 1 && s.recent.ab == 1 && s.recent.cls[0] == 1 &&
              s.recent.speeds == 1 && s.recent.mean_kmh10 == s.traffic.last_kmh10 && !s.recent.saturated);
    vision_session_reset(&s);
    for (i = 0; i < 4; i++) {
        if (wait_for(&s, VISION_EV_TRAFFIC, 2000, &ev, seen, &w) && s.traffic.n == 0) {
            break;
        }
    }
    check("reset zeroes the report", s.traffic.total_ab == 0 && s.traffic.n == 0 && s.traffic.last_kmh10 == 0);
    for (i = 0; i < 3 && s.recent.crossed != 0; i++) {
        wait_for(&s, VISION_EV_RECENT, 1500, &ev, seen, &w);
    }
    check("and the recent window", s.recent.crossed == 0 && s.recent.speeds == 0);
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

/* Let the det lines already on their way when a command went out pass. */
static void settle(struct vision_session *s, struct watch *w)
{
    struct vision_event ev;
    int i;

    for (i = 0; i < 3; i++) {
        wait_for(s, VISION_EV_DET, 1000, &ev, seen, w);
    }
}

/* How many confirmed tracks of class cls the next det lines show at most. */
static int confirmed_of(struct vision_session *s, struct watch *w, uint16_t cls, int lines)
{
    struct vision_event ev;
    int best = 0;
    int i;

    for (i = 0; i < lines; i++) {
        const struct vision_shown *t;
        int n = 0;
        int j;
        int c = 0;

        if (!wait_for(s, VISION_EV_DET, 1000, &ev, seen, w)) {
            break;
        }
        t = vision_session_tracks(s, &n, NULL);
        for (j = 0; j < n; j++) {
            c += t[j].cls == cls && t[j].id != 0;
        }
        best = c > best ? c : best;
    }
    return best;
}

/* The detection range against the real helper: a car too small for the
 * whole picture (the fake detector sees nothing under 12 of the model's
 * pixels) but large enough through FAR's zoom window; a large one every
 * range keeps but NEAR's size floor. */
static void test_range(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };

    vision_session_init(&s);
    /* Sensor 640 x 360, turned a quarter for the detector: a 360 x 640
     * picture at half size in the 320 x 320 model. The far car, 16 x 14,
     * is 7 x 8 model pixels there and 14 x 16 through the 320 x 320 window
     * at the picture's centre, where it stands. The chair is large and is
     * not traffic. */
    check("the helper starts", start(&s, "period=20", "minpx=12,box=2:800:300:170:16:14", NULL) == 0);
    check("ready", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w));
    vision_session_view(&s, 360, 640, 0);
    vision_session_mode(&s, true);
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    check("NORMAL: the far car is too small to be seen", confirmed_of(&s, &w, 2, 12) == 0);
    vision_session_range(&s, "far");
    settle(&s, &w);
    check("FAR: the zoom window finds it and a track confirms", confirmed_of(&s, &w, 2, 12) == 1);
    vision_session_range(&s, "near");
    settle(&s, &w);
    check("NEAR: gone again - under NEAR's smallest box", confirmed_of(&s, &w, 2, 12) == 0);
    check("an unknown range is not sent", vision_session_range(&s, "medium") == -1 && vision_session_range(&s, NULL) == -1);
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));

    vision_session_init(&s);
    check("again with a car of every range's size", start(&s, "period=20", "minpx=12,box=2:800:200:100:120:90", NULL) == 0);
    check("ready", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w));
    vision_session_view(&s, 360, 640, 0);
    vision_session_mode(&s, true);
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    check("NORMAL keeps it", confirmed_of(&s, &w, 2, 8) == 1);
    vision_session_range(&s, "near");
    settle(&s, &w);
    check("NEAR keeps it", confirmed_of(&s, &w, 2, 8) == 1);
    vision_session_range(&s, "far");
    settle(&s, &w);
    check("FAR keeps it once, not twice (the two passes merged)", confirmed_of(&s, &w, 2, 12) == 1);
    vision_session_abandon(&s, 1000);
}

/* READ against the real helper: the fake text models (vision_kpu_fake.c)
 * behind files that exist or not, a dictionary of '!' to '~' and the blank
 * last; the lines read, in view pixels; rapid reads; one that does not fit
 * its dictionary. */
static void test_read(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    char dir[] = "/tmp/vision-read-XXXXXX";
    char det[128];
    char rec[128];
    char dict[128];
    FILE *f;
    int c;
    int i;
    int reads = 0;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    snprintf(det, sizeof(det), "%s/text_det.kmodel", dir);
    snprintf(rec, sizeof(rec), "%s/text_rec.kmodel", dir);
    snprintf(dict, sizeof(dict), "%s/text_dict.txt", dir);
    setenv("POCKETOS_VISION_TEXT_DET", det, 1);
    setenv("POCKETOS_VISION_TEXT_REC", rec, 1);
    setenv("POCKETOS_VISION_TEXT_DICT", dict, 1);

    /* No models on the unit: no READ. */
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "text=100:40:120:40:EXIT12", NULL) == 0);
    check("without the text models READ is not offered", wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w) &&
                                                             !(ev.value & (1 << VISION_MODE_READ)));
    vision_session_abandon(&s, 1000);

    f = fopen(det, "w");
    if (f) {
        fclose(f);
    }
    f = fopen(rec, "w");
    if (f) {
        fclose(f);
    }
    f = fopen(dict, "w");
    if (f) {
        for (c = '!'; c <= '~'; c++) {
            fprintf(f, "%c\r\n", c);
        }
        fprintf(f, "BLANK\r\n");
        fclose(f);
    }
    vision_session_init(&s);
    /* Two lines on the sensor; the fake camera turns them upright with the
     * picture. */
    check("the helper starts with the models there",
          start(&s, "period=20", "text=100:40:40:120:EXIT12,text=300:120:30:140:SN-4711", NULL) == 0);
    check("READ is offered", wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w) && (ev.value & (1 << VISION_MODE_READ)));
    vision_session_view(&s, 360, 640, 0);
    vision_session_mode_word(&s, "read");
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    check("a read arrives", wait_for(&s, VISION_EV_TEXT, 3000, &ev, seen, &w));
    check("two lines, read as written, sure",
          s.text.n == 2 && ((strcmp(s.text.line[0].text, "EXIT12") == 0 && strcmp(s.text.line[1].text, "SN-4711") == 0) ||
                            (strcmp(s.text.line[1].text, "EXIT12") == 0 && strcmp(s.text.line[0].text, "SN-4711") == 0)) &&
              s.text.line[0].conf == 1000);
    printf("     read \"%s\" and \"%s\"\n", s.text.line[0].text, s.text.n > 1 ? s.text.line[1].text : "");
    {
        int ok = s.text.n > 0;

        for (i = 0; i < s.text.n; i++) {
            ok &= s.text.line[i].x >= 0 && s.text.line[i].y >= 0 && s.text.line[i].x + s.text.line[i].w <= 360 &&
                  s.text.line[i].y + s.text.line[i].h <= 640;
        }
        check("inside the view", ok);
    }
    for (i = 0; i < 30; i++) {
        if (wait_for(&s, VISION_EV_TEXT, 1500, &ev, seen, &w)) {
            reads++;
        }
        if (reads >= 4) {
            break;
        }
    }
    check("reads keep coming, and so do pictures between them", reads >= 4 && w.frames >= 4);
    check("no boxes of the detector while reading", s.shown_count == 0);
    vision_session_mode_word(&s, "detect");
    check("back to DETECT", wait_for(&s, VISION_EV_DET, 2000, &ev, seen, &w));
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));

    /* A dictionary that does not fit the recogniser's classes. */
    f = fopen(dict, "w");
    if (f) {
        fputs("a\nb\nc\n", f);
        fclose(f);
    }
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "text=100:40:120:40:EXIT12", NULL) == 0);
    wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w);
    vision_session_view(&s, 360, 640, 0);
    vision_session_mode_word(&s, "read");
    check("a dictionary that does not fit is said, not read wrong",
          wait_for(&s, VISION_EV_READFAIL, 3000, &ev, seen, &w) && strstr(ev.text, "do not fit") != NULL);
    vision_session_stream(&s, true, now_ms());
    check("and the helper goes on (pictures)", wait_for(&s, VISION_EV_FRAME, 3000, &ev, seen, &w));
    vision_session_abandon(&s, 1000);
    unsetenv("POCKETOS_VISION_TEXT_DET");
    unsetenv("POCKETOS_VISION_TEXT_REC");
    unsetenv("POCKETOS_VISION_TEXT_DICT");
    unlink(det);
    unlink(rec);
    unlink(dict);
    rmdir(dir);
}

/* REPLAY: the helper plays saved pictures instead of the camera. */
static void test_replay(void)
{
    struct vision_session s;
    struct vision_session_config cfg = { 0 };
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    char dir[] = "/tmp/vision-replay-XXXXXX";
    char a[96];
    char b[96];
    char list[200];
    char err[96];
    FILE *f;
    int i;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    snprintf(a, sizeof(a), "%s/a.ppm", dir);
    snprintf(b, sizeof(b), "%s/b.ppm", dir);
    for (i = 0; i < 2; i++) {
        f = fopen(i ? b : a, "wb");
        if (f) {
            int k;

            fprintf(f, "P6\n320 180\n255\n");
            for (k = 0; k < 320 * 180; k++) {
                fputc(i ? 200 : 20, f);
                fputc(100, f);
                fputc(50, f);
            }
            fclose(f);
        }
    }
    snprintf(list, sizeof(list), "%s,%s", a, b);
    vision_session_init(&s);
    cfg.helper = helper;
    cfg.backend = "image";
    cfg.config = list;
    cfg.kpu = "box=2:800:10:10:60:40";
    check("the helper starts on a replay", vision_session_start(&s, &cfg, now_ms(), err, sizeof(err)) == 0);
    check("ready: the replay, its pictures' size, SIMULATED", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w) &&
                                                                  strcmp(ev.text, "replay") == 0 && ev.w == 320 &&
                                                                  ev.h == 180 && ev.simulated);
    vision_session_view(&s, 320, 180, 90);
    vision_session_stream(&s, true, now_ms());
    w.s = NULL;
    check("pictures arrive", wait_for(&s, VISION_EV_FRAME, 3000, &ev, seen, &w) && ev.w == 320 && ev.h == 180);
    {
        static uint16_t px[320 * 180];

        vision_session_take_frame(&s, px, 320, 180);
        /* RGB565 of (20, 100, 50) or (200, 100, 50): shown as it is,
         * whatever the display's rotation - never turned. */
        check("as they are, not turned", px[0] == px[319] && ((px[0] >> 11) == 2 || (px[0] >> 11) == 25));
    }
    for (i = 0; i < 4; i++) {
        wait_for(&s, VISION_EV_DET, 2000, &ev, seen, &w);
    }
    check("the detector runs on them", s.shown_count == 1 && s.shown[0].cls == 2);
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));

    vision_session_init(&s);
    snprintf(list, sizeof(list), "%s/none.ppm", dir);
    check("a replay of nothing starts", vision_session_start(&s, &cfg, now_ms(), err, sizeof(err)) == 0);
    check("and says there is no camera", wait_for(&s, VISION_EV_NODEVICE, 3000, &ev, seen, &w) &&
                                             strstr(ev.text, "replay") != NULL);
    vision_session_abandon(&s, 1000);
    unlink(a);
    unlink(b);
    rmdir(dir);
}

/* RECOGNIZE against the real helper and the fake face models: offered only
 * with both models; enrolment of the one face in view, kept in the state
 * directory (0600 in 0700); that face then the OWNER on the next open,
 * another face unknown, both at once told apart; enrolment refusing two
 * faces; FORGET removing the owner from the unit; a profile of another
 * model not used. */
static int wait_who(struct vision_session *s, int n, struct watch *w)
{
    struct vision_event ev;
    int i;

    for (i = 0; i < 40; i++) {
        if (wait_for(s, VISION_EV_WHO, 1500, &ev, seen, w) && s->who.n == n) {
            return 1;
        }
    }
    return 0;
}

static void test_recognize(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    char dir[] = "/tmp/vision-recog-XXXXXX";
    char det[128];
    char emb[128];
    char state[128];
    char owner[160];
    struct stat st;
    FILE *f;
    int i;
    int k;
    int ok;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    snprintf(det, sizeof(det), "%s/face_det.kmodel", dir);
    snprintf(emb, sizeof(emb), "%s/face_embed.kmodel", dir);
    snprintf(state, sizeof(state), "%s/state", dir);
    snprintf(owner, sizeof(owner), "%s/vision/owner.v1", state);
    setenv("POCKETOS_VISION_FACE_DET", det, 1);
    setenv("POCKETOS_VISION_FACE_EMBED", emb, 1);
    setenv("POCKETOS_STATE_DIR", state, 1);
    f = fopen(det, "w");
    if (f) {
        fclose(f);
    }

    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "face=200:100:80:100:1", NULL) == 0);
    check("with the face detector alone RECOGNIZE is not offered, FACE is",
          wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w) && (ev.value & (1 << VISION_MODE_FACE)) &&
              !(ev.value & (1 << VISION_MODE_RECOGNIZE)));
    vision_session_abandon(&s, 1000);

    f = fopen(emb, "w");
    if (f) {
        fputs("fake", f);
        fclose(f);
    }
    vision_session_init(&s);
    check("the helper starts with both models", start(&s, "period=20", "face=200:100:80:100:1", NULL) == 0);
    check("RECOGNIZE is offered", wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w) &&
                                      (ev.value & (1 << VISION_MODE_RECOGNIZE)));
    vision_session_view(&s, 640, 360, 90);
    vision_session_mode_word(&s, "recognize");
    check("no owner yet", wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w) && ev.value == 0);
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    check("faces are found, none scored without an owner", wait_who(&s, 0, &w) && s.who.faces == 1);
    vision_session_enrol(&s, true);
    ok = wait_for(&s, VISION_EV_ENROL, 3000, &ev, seen, &w) && ev.w == 0 && ev.h == 5;
    for (i = 0, k = 0; i < 40 && k < 5; i++) {
        if (wait_for(&s, VISION_EV_ENROL, 1500, &ev, seen, &w)) {
            ok &= (int)ev.w == k + 1;
            k = (int)ev.w;
        }
    }
    check("enrolment takes five views, one at a time", ok && k == 5);
    check("and the owner is kept", wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w) && ev.value == 1 && ev.w == 5);
    check("on the unit, private: 0600 in a 0700 directory",
          stat(owner, &st) == 0 && (st.st_mode & 0777) == 0600 &&
              (snprintf(owner, sizeof(owner), "%s/vision", state), stat(owner, &st) == 0) &&
              (st.st_mode & 0777) == 0700);
    snprintf(owner, sizeof(owner), "%s/vision/owner.v1", state);
    ok = wait_who(&s, 1, &w);
    check("the enrolled face is the owner, sure", ok && s.who.t[0].owner && s.who.t[0].score >= 990);
    vision_session_abandon(&s, 1000);

    /* The next open: another person where the owner was. */
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "face=200:100:80:100:2", NULL) == 0);
    wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w);
    vision_session_view(&s, 640, 360, 90);
    vision_session_mode_word(&s, "recognize");
    check("the owner is still known", wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w) && ev.value == 1 && ev.w == 5);
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    ok = wait_who(&s, 1, &w);
    printf("     someone else scores %u\n", s.who.t[0].score);
    check("someone else is unknown", ok && !s.who.t[0].owner && s.who.t[0].score < 750);
    vision_session_abandon(&s, 1000);

    /* A group of six, more than a round looks at: every face is scored in
     * turn, not the largest three again and again. */
    vision_session_init(&s);
    check("the helper starts",
          start(&s, "period=20",
                "face=10:100:60:80:2,face=110:100:64:84:3,face=210:100:68:88:4,face=310:100:72:92:5,"
                "face=410:100:76:96:6,face=510:100:80:100:7",
                NULL) == 0);
    wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w);
    vision_session_view(&s, 640, 360, 90);
    vision_session_mode_word(&s, "recognize");
    wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w);
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    ok = wait_who(&s, 6, &w);
    for (i = 0; ok && i < 6; i++) {
        ok &= !s.who.t[i].owner;
    }
    check("a group of six: all six scored, none the owner", ok && s.who.faces == 6);
    vision_session_abandon(&s, 1000);

    /* Both at once; then an enrolment with two faces in view. */
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "face=100:100:80:100:1,face=420:120:80:100:2", NULL) == 0);
    wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w);
    vision_session_view(&s, 640, 360, 90);
    vision_session_mode_word(&s, "recognize");
    wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w);
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    ok = wait_who(&s, 2, &w);
    check("two faces: the owner and someone else, told apart",
          ok && s.who.t[0].owner != s.who.t[1].owner && s.who.faces == 2);
    {
        /* The owner is the one on the left (x 100 on the sensor). */
        int own = s.who.t[0].owner ? 0 : 1;
        int n = 0;
        const struct vision_shown *t = vision_session_tracks(&s, &n, NULL);
        int left = -1;

        for (i = 0; i < n; i++) {
            if (t[i].id == s.who.t[own].id) {
                left = t[i].x < 300;
            }
        }
        check("the owner's label is on the owner's face", left == 1);
    }
    vision_session_enrol(&s, true);
    wait_for(&s, VISION_EV_ENROL, 3000, &ev, seen, &w);
    ok = !wait_for(&s, VISION_EV_ENROL, 1500, &ev, seen, &w);
    check("an enrolment takes no view while two faces are in view", ok);
    vision_session_enrol(&s, false);
    vision_session_forget(&s);
    check("FORGET: no owner", wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w) && ev.value == 0);
    check("and nothing of it left on the unit", stat(owner, &st) != 0 && errno == ENOENT);
    ok = wait_who(&s, 0, &w);
    check("faces are no longer scored", ok && s.who.faces == 2);
    vision_session_abandon(&s, 1000);

    /* A profile of another model is not used. */
    f = fopen(owner, "w");
    if (f) {
        fprintf(f, "doors-vision-owner 1\nmodel other.kmodel 4\ndim 4\nsamples 1\nv 1 0 0 0\n");
        fclose(f);
    }
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "face=200:100:80:100:1", NULL) == 0);
    wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w);
    vision_session_view(&s, 640, 360, 90);
    vision_session_mode_word(&s, "recognize");
    check("an owner made with another model is no owner", wait_for(&s, VISION_EV_OWNER, 3000, &ev, seen, &w) &&
                                                              ev.value == 0);
    vision_session_abandon(&s, 1000);
    unsetenv("POCKETOS_VISION_FACE_DET");
    unsetenv("POCKETOS_VISION_FACE_EMBED");
    unsetenv("POCKETOS_STATE_DIR");
    unlink(owner);
    snprintf(owner, sizeof(owner), "%s/vision", state);
    rmdir(owner);
    rmdir(state);
    unlink(det);
    unlink(emb);
    rmdir(dir);
}

/* FACE: the face model's outputs decoded, tracked and said as boxes with
 * ids; offered only when the model is there; a model that is not a face
 * detector said, not run. */
static void test_face(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    char dir[] = "/tmp/vision-face-XXXXXX";
    char det[128];
    char other[128];
    FILE *f;
    int i;
    int ok = 0;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    snprintf(det, sizeof(det), "%s/face_det.kmodel", dir);
    snprintf(other, sizeof(other), "%s/other.kmodel", dir);
    setenv("POCKETOS_VISION_FACE_DET", det, 1);

    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "face=200:100:60:80", NULL) == 0);
    check("without the face model FACE is not offered", wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w) &&
                                                            !(ev.value & (1 << VISION_MODE_FACE)));
    vision_session_abandon(&s, 1000);

    f = fopen(det, "w");
    if (f) {
        fclose(f);
    }
    vision_session_init(&s);
    /* Two faces on the sensor, and a car the object detector would see
     * exactly where the first face is: FACE must not show the car, and
     * DETECT's car and FACE's face must land on the same place. */
    check("the helper starts with the face model there",
          start(&s, "period=20", "face=200:100:60:80,face=420:140:80:100,box=2:900:200:100:60:80", NULL) == 0);
    check("FACE is offered", wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w) && (ev.value & (1 << VISION_MODE_FACE)));
    /* The fake camera is mounted at 90: a display at 90 sees the sensor as
     * it is, pixel for pixel. */
    vision_session_view(&s, 640, 360, 90);
    vision_session_mode_word(&s, "face");
    vision_session_stream(&s, true, now_ms());
    w.s = &s;
    for (i = 0; i < 40 && !ok; i++) {
        if (wait_for(&s, VISION_EV_DET, 1500, &ev, seen, &w) && s.shown_count == 2 && s.shown[0].id &&
            s.shown[1].id) {
            ok = 1;
        }
    }
    check("two faces, each with an id", ok);
    {
        int a = s.shown[0].x < s.shown[1].x ? 0 : 1;
        int b = 1 - a;

        printf("     faces %d,%d %dx%d %u and %d,%d %dx%d %u\n", s.shown[a].x, s.shown[a].y, s.shown[a].w,
               s.shown[a].h, s.shown[a].conf, s.shown[b].x, s.shown[b].y, s.shown[b].w, s.shown[b].h, s.shown[b].conf);
        check("where they are on the sensor (a view of the same size), within a few pixels",
              ok && abs(s.shown[a].x - 200) <= 4 && abs(s.shown[a].y - 100) <= 4 && abs(s.shown[a].w - 60) <= 6 &&
                  abs(s.shown[a].h - 80) <= 6 && abs(s.shown[b].x - 420) <= 4 && abs(s.shown[b].w - 80) <= 6);
        check("as sure as the model says (95 %)", ok && s.shown[a].conf >= 940 && s.shown[a].conf <= 960);
    }
    {
        uint32_t ids[2] = { s.shown[0].id, s.shown[1].id };
        int same = 1;

        for (i = 0; i < 5; i++) {
            wait_for(&s, VISION_EV_DET, 1500, &ev, seen, &w);
            same &= s.shown_count == 2 && ((s.shown[0].id == ids[0] && s.shown[1].id == ids[1]) ||
                                           (s.shown[0].id == ids[1] && s.shown[1].id == ids[0]));
        }
        check("the ids hold from frame to frame", ok && same);
    }
    /* Upright on a portrait display: the picture is turned for the model
     * and the faces turned back. */
    {
        struct vision_shown face[2];
        int near = 0;

        vision_session_view(&s, 360, 640, 0);
        for (i = 0; i < 6; i++) {
            wait_for(&s, VISION_EV_DET, 1500, &ev, seen, &w);
        }
        ok = s.shown_count == 2;
        face[0] = s.shown[0];
        face[1] = s.shown[1];
        vision_session_mode_word(&s, "detect");
        near = 0;
        for (i = 0; i < 20 && !near; i++) {
            if (wait_for(&s, VISION_EV_DET, 1500, &ev, seen, &w) && s.shown_count == 1 && s.shown[0].cls == 2) {
                int k;

                for (k = 0; k < 2; k++) {
                    near |= abs(face[k].x - s.shown[0].x) <= 6 && abs(face[k].y - s.shown[0].y) <= 6 &&
                            abs(face[k].w - s.shown[0].w) <= 8 && abs(face[k].h - s.shown[0].h) <= 8;
                }
                printf("     portrait: car %d,%d %dx%d; faces %d,%d %dx%d and %d,%d %dx%d\n", s.shown[0].x,
                       s.shown[0].y, s.shown[0].w, s.shown[0].h, face[0].x, face[0].y, face[0].w, face[0].h,
                       face[1].x, face[1].y, face[1].w, face[1].h);
                break;
            }
        }
        check("portrait: two faces still", ok);
        check("back to DETECT: the car, not the faces - where the face on it was", near);
    }
    vision_session_abandon(&s, 1000);
    check("the helper is gone", !vision_session_active(&s));

    /* A file that is there but no face detector. */
    f = fopen(other, "w");
    if (f) {
        fclose(f);
    }
    setenv("POCKETOS_VISION_FACE_DET", other, 1);
    vision_session_init(&s);
    check("the helper starts", start(&s, "period=20", "face=200:100:60:80", NULL) == 0);
    wait_for(&s, VISION_EV_CAPS, 3000, &ev, seen, &w);
    vision_session_view(&s, 640, 360, 0);
    vision_session_mode_word(&s, "face");
    check("a model that is not a face detector is said", wait_for(&s, VISION_EV_FACEFAIL, 3000, &ev, seen, &w) &&
                                                             ev.text[0] != '\0');
    vision_session_stream(&s, true, now_ms());
    check("and the helper goes on (pictures)", wait_for(&s, VISION_EV_FRAME, 3000, &ev, seen, &w));
    vision_session_abandon(&s, 1000);
    unsetenv("POCKETOS_VISION_FACE_DET");
    unlink(det);
    unlink(other);
    rmdir(dir);
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

/* What a leave test watches: frames taken so they keep coming, faces in
 * view, and whether the helper went. */
struct leave_watch {
    struct vision_session *s;
    int faces;
    int exited;
    int reason;
};

static void leave_seen(const struct vision_event *ev, void *user)
{
    struct leave_watch *w = user;

    if (ev->kind == VISION_EV_FRAME) {
        vision_session_take_frame(w->s, taken, 640, 360);
    } else if (ev->kind == VISION_EV_DET) {
        w->faces += w->s->shown_count > 0;
    } else if (ev->kind == VISION_EV_EXITED) {
        w->exited++;
        w->reason = ev->reason;
    }
}

/* Pump the session until it has seen `loading` (the helper is opening a
 * model), at most ms. */
static int until_loading(struct vision_session *s, struct leave_watch *w, int ms)
{
    int64_t end = now_ms() + ms;

    while (now_ms() < end && !s->loading) {
        struct vision_event ev;

        while (vision_session_poll(s, &ev, now_ms())) {
            leave_seen(&ev, w);
        }
        nap(5);
    }
    return s->loading;
}

/* A FACE session on the fake face model, whose every net open takes
 * open_ms: Vision's own order, the stream first and the mode after it. */
static int start_face(struct vision_session *s, struct leave_watch *w, int open_ms)
{
    char kpu[96];
    struct vision_event ev;

    snprintf(kpu, sizeof(kpu), "face=200:100:60:80,net_open_ms=%d", open_ms);
    vision_session_init(s);
    memset(w, 0, sizeof(*w));
    w->s = s;
    if (start(s, "period=20", kpu, NULL) != 0 || !wait_for(s, VISION_EV_CAPS, 3000, &ev, leave_seen, w) ||
        !(ev.value & (1 << VISION_MODE_FACE))) {
        return 0;
    }
    vision_session_view(s, 640, 360, 90);
    vision_session_stream(s, true, now_ms());
    vision_session_mode_word(s, "face");
    return 1;
}

static void test_leave(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct leave_watch w;
    char dir[] = "/tmp/vision-leave-XXXXXX";
    char det[128];
    FILE *f;
    int64_t t0;
    int64_t took;
    enum vision_leave left;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    snprintf(det, sizeof(det), "%s/face_det.kmodel", dir);
    f = fopen(det, "w");
    if (f) {
        fclose(f);
    }
    setenv("POCKETOS_VISION_FACE_DET", det, 1);

    /* An ordinary close: the helper closes the camera, the nets and the
     * detector, says bye, and exits - no kill. */
    check("a face session starts", start_face(&s, &w, 0));
    check("and finds the face", wait_for(&s, VISION_EV_DET, 3000, &ev, leave_seen, &w) &&
                                    (w.faces > 0 || wait_for(&s, VISION_EV_DET, 3000, &ev, leave_seen, &w)));
    check("an ordinary close ends with bye, not a kill",
          vision_session_abandon(&s, VISION_LEAVE_GRACE_MS) == VISION_LEFT_BYE);

    /* A model slower to open than the silence watchdog allows a quiet
     * helper: it says loading, the watchdog waits for the load, and FACE
     * runs once it is open. */
    check("a slow model: the session starts", start_face(&s, &w, VISION_SILENCE_MS + 1500));
    check("the helper says it is loading", until_loading(&s, &w, 3000));
    t0 = now_ms();
    while (now_ms() - t0 < VISION_SILENCE_MS + 1500 + 3000 && w.faces == 0 && !w.exited) {
        wait_for(&s, VISION_EV_DET, 200, &ev, leave_seen, &w);
    }
    took = now_ms() - t0;
    check("a load longer than the silence watchdog is not killed", w.exited == 0 && vision_session_active(&s));
    check("and the face is found once the model is open", w.faces > 0 && took >= VISION_SILENCE_MS);
    printf("     first face %lld ms after loading\n", (long long)took);
    check("and it still leaves with bye", vision_session_abandon(&s, VISION_LEAVE_GRACE_MS) == VISION_LEFT_BYE);

    /* Closed in the middle of that load, with a grace far shorter than
     * the load: the leave waits for the load, then the close, and the
     * helper is never killed with the model half open. */
    check("a close mid-load: the session starts", start_face(&s, &w, 2500));
    check("and is loading", until_loading(&s, &w, 3000));
    t0 = now_ms();
    left = vision_session_abandon(&s, 300);
    took = now_ms() - t0;
    check("a close during a slow load waits for it and ends with bye", left == VISION_LEFT_BYE);
    check("having waited for the load, within its deadline", took >= 1500 && took < VISION_LOAD_MS);
    printf("     the close took %lld ms\n", (long long)took);
    check("no helper left", waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    /* A load that never ends is a helper stuck: the watchdog kills it at
     * the load's deadline, not before and not never. */
    check("a load that never ends: the session starts", start_face(&s, &w, 3600000));
    check("and is loading", until_loading(&s, &w, 3000));
    t0 = now_ms();
    wait_for(&s, VISION_EV_EXITED, VISION_LOAD_MS + 3000, &ev, leave_seen, &w);
    took = now_ms() - t0;
    check("is killed as hung at the load's deadline",
          w.exited == 1 && w.reason == VISION_EXIT_HUNG && took >= VISION_LOAD_MS - 500 && took <= VISION_LOAD_MS + 2000);
    vision_session_abandon(&s, 100);

    /* And a close while it is stuck so still ends, with a kill. */
    check("stuck again: the session starts", start_face(&s, &w, 3600000));
    check("and is loading", until_loading(&s, &w, 3000));
    t0 = now_ms();
    left = vision_session_abandon(&s, 300);
    took = now_ms() - t0;
    check("a close while a load never ends kills it in the end",
          left == VISION_LEFT_KILLED && took <= VISION_LOAD_MS + 300 + 1000);
    check("and reaps it", waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    unsetenv("POCKETOS_VISION_FACE_DET");
    unlink(det);
    rmdir(dir);
}

/* A turn of the display (or another picture size) moves the count and
 * speed lines among the tracks, which stay where they are in the frame. A
 * car parked above the count line is below it after a half turn: that is
 * not a crossing, and no speed may be timed across the old and the new
 * lines. What was counted before the turn stays. */
static void test_view_change(void)
{
    struct vision_session s;
    struct vision_event ev;
    struct watch w = { 0, 0, 0, 0, 0, 0, NULL };
    int32_t line[4] = { 0, 300, 1000, 300 };
    int32_t speed[8] = { 0, 100, 1000, 100, 0, 400, 1000, 400 };
    uint32_t n_before;
    uint32_t rejected_before;
    int parked = 0;
    int i;

    vision_session_init(&s);
    /* Mount 90, display 0: the sensor's x runs down the 360 x 640 picture.
     * One car drives down across the lines and off the picture; the other
     * is parked in another column at picture y ~130, above the count line
     * (192) and between the speed lines (64, 256). */
    check("view change: the helper starts",
          start(&s, "period=20", "box=2:800:0:200:80:60:16:0,box=2:800:100:20:60:60", NULL) == 0);
    check("ready", wait_for(&s, VISION_EV_READY, 3000, &ev, seen, &w));
    w.s = &s;
    vision_session_view(&s, 360, 640, 0);
    vision_session_mode(&s, true);
    vision_session_line(&s, line);
    vision_session_speed_lines(&s, speed);
    vision_session_distance(&s, 500);
    vision_session_stream(&s, true, now_ms());
    for (i = 0; i < 80; i++) {
        const struct vision_shown *t;
        int n = 0;
        int j;

        if (!wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w)) {
            break;
        }
        t = vision_session_tracks(&s, &n, NULL);
        parked = 0;
        for (j = 0; j < n; j++) {
            parked += t[j].id != 0 && t[j].y + t[j].h / 2 < 192;
        }
    }
    check("before the turn: the driving car counted IN once and timed, the parked one held above the line",
          s.count_ab == 1 && s.count_ba == 0 && s.traffic.total_ab == 1 && s.traffic.n == 1 && parked == 1);
    n_before = s.traffic.n;
    rejected_before = s.traffic.rejected;
    /* The display turns half way round: the same picture size. */
    vision_session_view(&s, 360, 640, 180);
    parked = 0;
    for (i = 0; i < 40; i++) {
        const struct vision_shown *t;
        int n = 0;
        int j;

        if (!wait_for(&s, VISION_EV_DET, 1000, &ev, seen, &w)) {
            break;
        }
        t = vision_session_tracks(&s, &n, NULL);
        parked = 0;
        for (j = 0; j < n; j++) {
            parked += t[j].id != 0 && t[j].y + t[j].h / 2 > 192;
        }
    }
    check("after the turn the parked car is shown below the count line, still tracked", parked == 1);
    check("and nothing crossed: no phantom count", s.count_ab == 1 && s.count_ba == 0 && s.traffic.total_ab == 1 &&
                                                         s.traffic.total_ba == 0);
    check("no speed timed across the old and new lines", s.traffic.n == n_before &&
                                                             s.traffic.rejected == rejected_before &&
                                                             s.traffic.cur_kmh10 == 0);
    vision_session_abandon(&s, VISION_LEAVE_GRACE_MS);
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
        ok &= vision_session_abandon(&s, VISION_LEAVE_GRACE_MS) == VISION_LEFT_BYE;
        ok &= !vision_session_active(&s);
    }
    check("thirty opens and closes, each ending with bye", ok);
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
    test_range();
    test_read();
    test_replay();
    test_face();
    test_recognize();
    test_pixels();
    test_malformed();
    test_failures();
    test_view_change();
    test_lifetime();
    test_leave();
    printf("vision_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
