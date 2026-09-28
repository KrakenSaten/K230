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
    check("frame parses", vision_session_parse_line(&s, "frame 2 17 528 938", &ev) && ev.kind == VISION_EV_FRAME &&
                              ev.value == 2 && ev.w == 528 && ev.h == 938);
    check("a frame in the review slot does not", !vision_session_parse_line(&s, "frame 3 17 528 938", &ev));
    check("a det line parses into the session",
          vision_session_parse_line(&s, "det 40 2 1:0:900:10:20:30:40 0:2:600:100:200:50:60", &ev) &&
              ev.kind == VISION_EV_DET && ev.value == 40 && s.shown_count == 2 && s.shown[0].id == 1 &&
              s.shown[0].cls == 0 && s.shown[0].conf == 900 && s.shown[0].w == 30 && s.shown[1].id == 0 &&
              s.shown[1].x == 100 && s.shown_seq == 40);
    check("an empty det line parses", vision_session_parse_line(&s, "det 41 0", &ev) && s.shown_count == 0);
    check("a det line with fewer boxes than it says does not",
          !vision_session_parse_line(&s, "det 42 2 1:0:900:10:20:30:40", &ev));
    check("nor one with more", !vision_session_parse_line(&s, "det 42 1 1:0:900:10:20:30:40 2:0:900:1:2:3:4", &ev));
    check("nor a box outside any view", !vision_session_parse_line(&s, "det 42 1 1:0:900:1000:20:30:40", &ev));
    check("nor an impossible confidence", !vision_session_parse_line(&s, "det 42 1 1:0:1900:10:20:30:40", &ev));
    check("count parses", vision_session_parse_line(&s, "count 3 1", &ev) && s.count_ab == 3 && s.count_ba == 1);
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
    test_malformed();
    test_failures();
    test_lifetime();
    printf("vision_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
