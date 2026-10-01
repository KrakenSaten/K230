/*
 * Video's helper client against the real helper (argv[1]: the test build of
 * pos-video, fake backend, no sound card): starting and the hello, a valid
 * file (opened, the first picture copied out of shared memory), play, pause,
 * seek and stop answered, an invalid file, a buffer too small for a picture,
 * a helper that hangs while playing (the watchdog kills it), one that crashes
 * (and `recover` is started for it), one that never says hello, one that
 * speaks another protocol version, one that sends an impossible picture, one
 * that cannot be started at all, leaving while playing, and starting again
 * many times without leaking a descriptor.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "video_session.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static const char *helper;
static char dir[128];

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
    fflush(stdout);
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static int fds_open(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    closedir(d);
    return n;
}

static void write_file(const char *name, const char *text, int mode)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    chmod(path, (mode_t)mode);
}

static void path_of(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dir, name);
}

static struct video_session s;
static uint16_t pixels[VIDEO_VIEW_MAX_PIXELS];

/* Poll until an event of kind arrives (taking pictures on the way). */
static int wait_ev(enum video_ev_kind kind, struct video_event *out, int ms)
{
    int64_t end = mono_ms() + ms;

    for (;;) {
        struct video_event ev;
        int64_t now = mono_ms();

        while (video_session_poll(&s, &ev, now)) {
            if (ev.kind == VIDEO_EV_FRAME && kind != VIDEO_EV_FRAME) {
                video_session_take_frame(&s, pixels, VIDEO_VIEW_MAX_PIXELS, NULL, NULL);
            }
            if (ev.kind == kind) {
                if (out) {
                    *out = ev;
                }
                return 1;
            }
        }
        if (now >= end) {
            return 0;
        }
        sleep_ms(5);
    }
}

static int start(const char *h)
{
    struct video_session_config cfg;
    char err[128];

    memset(&cfg, 0, sizeof(cfg));
    cfg.helper = h;
    cfg.backend = "fake";
    cfg.volume_percent = 50;
    return video_session_start(&s, &cfg, mono_ms(), err, sizeof(err));
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/video_session_test.XXXXXX";
    char path[256];
    struct video_event ev;
    uint32_t w = 0;
    uint32_t h = 0;
    int fds0;
    int i;

    if (argc < 2 || !mkdtemp(tmpl)) {
        fprintf(stderr, "usage: video_session_test HELPER\n");
        return 2;
    }
    helper = argv[1];
    snprintf(dir, sizeof(dir), "%s", tmpl);
    /* No sound card for these: the helper's sound is the player test's. */
    setenv("POS_VIDEO_NO_AUDIO", "1", 1);
    write_file("good.mp4", "DOORS-FAKE-VIDEO w=320 h=180 fps=25 ms=3000\n", 0644);
    write_file("broken.mp4", "not a video\n", 0644);
    write_file("hang.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=5000 hang_at=10\n", 0644);
    write_file("crash.mp4", "DOORS-FAKE-VIDEO w=160 h=90 fps=25 ms=5000 crash_at=5\n", 0644);
    write_file("mute.sh", "#!/bin/sh\nexec sleep 20\n", 0755);
    write_file("oldproto.sh", "#!/bin/sh\necho 'hello 99 fake'\nexec sleep 20\n", 0755);
    write_file("badframe.sh", "#!/bin/sh\necho 'hello 1 fake'\necho 'frame 9 1 10 10 0'\nexec sleep 20\n",
               0755);
    write_file("hugeframe.sh",
               "#!/bin/sh\necho 'hello 1 fake'\necho 'frame 0 1 1280 1280 0'\nexec sleep 20\n", 0755);

    video_session_init(&s);
    check("idle at first", !video_session_active(&s) && video_session_play(&s, 0) == -1 &&
                               video_session_view(&s, 10, 10) == -1);
    check("the helper path defaults to /usr/bin/pos-video",
          (unsetenv("POCKETOS_VIDEO_HELPER"), strcmp(video_session_helper_path(), "/usr/bin/pos-video") == 0));
    setenv("POCKETOS_VIDEO_BACKEND", "fake", 1);
    check("the backend from the environment", strcmp(video_session_backend(), "fake") == 0);

    /* ---- a valid file ---- */
    fds0 = fds_open();
    check("the helper starts", start(helper) == 0 && video_session_active(&s));
    check("a second start is refused", start(helper) == -1);
    check("a view too big for a slot is not sent", video_session_view(&s, 5000, 10) == -1 &&
                                                       video_session_view(&s, 0, 10) == -1);
    check("view", video_session_view(&s, 640, 360) == 0);
    check("a relative path is refused", video_session_open(&s, "good.mp4", mono_ms()) == -1);
    check("a path with a newline is refused", video_session_open(&s, "/tmp/a\nplay", mono_ms()) == -1);
    path_of("good.mp4", path, sizeof(path));
    check("open", video_session_open(&s, path, mono_ms()) == 0);
    check("opened", wait_ev(VIDEO_EV_OPENED, &ev, 3000) && ev.ms == 3000 && ev.w == 320 &&
                        ev.h == 180 && ev.fps_x100 == 2500 && strcmp(ev.word, "none") == 0 &&
                        strcmp(ev.codec, "h264") == 0);
    check("the first picture", wait_ev(VIDEO_EV_FRAME, &ev, 3000) && ev.ms == 0 && ev.w == 320);
    check("a buffer too small takes nothing (and gives the slot back)",
          video_session_take_frame(&s, pixels, 100, &w, &h) == 0);
    check("nothing is waiting now", video_session_take_frame(&s, pixels, VIDEO_VIEW_MAX_PIXELS, &w, &h) == 0);
    check("play", video_session_play(&s, mono_ms()) == 0);
    check("play answered", wait_ev(VIDEO_EV_STATE, &ev, 2000) && ev.value == VIDEO_PLAY_PLAYING);
    check("a picture while playing", wait_ev(VIDEO_EV_FRAME, &ev, 1000));
    {
        /* The newest picture is the one taken: a later one may have come in
         * the same read as the event. */
        int64_t number = s.frame_ms * 25 / 1000;

        check("... copied out at its size, its number in the first pixel",
              video_session_take_frame(&s, pixels, VIDEO_VIEW_MAX_PIXELS, &w, &h) == 1 && w == 320 &&
                  h == 180 && pixels[0] == number);
    }
    check("progress", wait_ev(VIDEO_EV_POS, &ev, 1000) && ev.ms > 0);
    check("statistics", wait_ev(VIDEO_EV_STATS, &ev, 2000) && ev.stats.fps_x10 >= 150 &&
                            ev.stats.shown >= 10);
    check("pause", video_session_pause(&s, mono_ms()) == 0 && wait_ev(VIDEO_EV_STATE, &ev, 2000) &&
                       ev.value == VIDEO_PLAY_PAUSED);
    check("seek", video_session_seek(&s, 2000, mono_ms()) == 0 && wait_ev(VIDEO_EV_SEEKED, &ev, 2000) &&
                      ev.ms >= 1960 && ev.ms <= 2000);
    check("a negative seek is not sent", video_session_seek(&s, -1, mono_ms()) == -1);
    check("stop", video_session_stop(&s, mono_ms()) == 0 && wait_ev(VIDEO_EV_STATE, &ev, 2000) &&
                      ev.value == VIDEO_PLAY_STOPPED && ev.ms == 0);
    sleep_ms(4500);
    check("paused, a quiet helper is not a hung one", wait_ev(VIDEO_EV_EXITED, &ev, 10) == 0 &&
                                                          video_session_active(&s));

    /* ---- an invalid file, then a valid one in the same helper ---- */
    path_of("broken.mp4", path, sizeof(path));
    video_session_open(&s, path, mono_ms());
    check("an invalid file: openfail", wait_ev(VIDEO_EV_OPENFAIL, &ev, 2000) &&
                                           strcmp(ev.word, "corrupt") == 0);
    path_of("good.mp4", path, sizeof(path));
    video_session_open(&s, path, mono_ms());
    check("... and a good one after it", wait_ev(VIDEO_EV_OPENED, &ev, 2000));
    video_session_play(&s, mono_ms());
    wait_ev(VIDEO_EV_FRAME, NULL, 1000);
    {
        int64_t t0 = mono_ms();
        unsigned rec = s.recoveries;

        video_session_abandon(&s, 300);
        check("leaving while playing is quick and clean",
              mono_ms() - t0 < 400 && !video_session_active(&s) && s.recoveries == rec);
    }

    /* ---- a helper that hangs while playing ---- */
    start(helper);
    video_session_view(&s, 160, 90);
    path_of("hang.mp4", path, sizeof(path));
    video_session_open(&s, path, mono_ms());
    wait_ev(VIDEO_EV_OPENED, NULL, 2000);
    video_session_play(&s, mono_ms());
    {
        int64_t t0 = mono_ms();
        unsigned rec = s.recoveries;

        check("a hung decoder: the watchdog kills the helper",
              wait_ev(VIDEO_EV_EXITED, &ev, 8000) && ev.reason == VIDEO_EXIT_HUNG &&
                  mono_ms() - t0 >= VIDEO_SILENCE_MS - 100);
        check("... and recovers the sound card for it", s.recoveries == rec + 1);
    }

    /* ---- a helper that crashes ---- */
    start(helper);
    video_session_view(&s, 160, 90);
    path_of("crash.mp4", path, sizeof(path));
    video_session_open(&s, path, mono_ms());
    wait_ev(VIDEO_EV_OPENED, NULL, 2000);
    video_session_play(&s, mono_ms());
    check("a crash is reported as one", wait_ev(VIDEO_EV_EXITED, &ev, 3000) &&
                                            ev.reason == VIDEO_EXIT_CRASHED && ev.value == 128 + 6);

    /* ---- helpers that break the protocol ---- */
    path_of("mute.sh", path, sizeof(path));
    start(path);
    {
        int64_t t0 = mono_ms();

        check("no hello: killed as hung", wait_ev(VIDEO_EV_EXITED, &ev, 6000) &&
                                              ev.reason == VIDEO_EXIT_HUNG &&
                                              mono_ms() - t0 >= VIDEO_HELLO_MS - 100);
    }
    path_of("oldproto.sh", path, sizeof(path));
    start(path);
    check("another protocol version: killed", wait_ev(VIDEO_EV_EXITED, &ev, 3000) &&
                                                  ev.reason == VIDEO_EXIT_PROTOCOL);
    path_of("badframe.sh", path, sizeof(path));
    start(path);
    check("a picture in a slot that does not exist: killed",
          wait_ev(VIDEO_EV_EXITED, &ev, 3000) && ev.reason == VIDEO_EXIT_PROTOCOL);
    path_of("hugeframe.sh", path, sizeof(path));
    start(path);
    check("a picture bigger than a slot: killed",
          wait_ev(VIDEO_EV_EXITED, &ev, 3000) && ev.reason == VIDEO_EXIT_PROTOCOL);
    path_of("nothing-here", path, sizeof(path));
    start(path);
    check("a helper that cannot be started says so",
          wait_ev(VIDEO_EV_ERROR, &ev, 3000) && strcmp(ev.word, "device") == 0 &&
              strstr(ev.text, "exec") != NULL);
    check("... and is gone", wait_ev(VIDEO_EV_EXITED, &ev, 3000) && ev.value == 127);

    /* ---- reopening, many times ---- */
    for (i = 0; i < 20; i++) {
        start(helper);
        video_session_view(&s, 320, 180);
        path_of("good.mp4", path, sizeof(path));
        video_session_open(&s, path, mono_ms());
        wait_ev(VIDEO_EV_FRAME, NULL, 2000);
        video_session_take_frame(&s, pixels, VIDEO_VIEW_MAX_PIXELS, NULL, NULL);
        if (i % 2) {
            video_session_play(&s, mono_ms());
            wait_ev(VIDEO_EV_POS, NULL, 1000);
        }
        video_session_abandon(&s, 300);
    }
    check("twenty opens and closes: no descriptor left behind", fds_open() == fds0);
    check("... and nothing running", !video_session_active(&s));

    {
        char c[256];

        snprintf(c, sizeof(c), "rm -rf '%s'", dir);
        if (system(c) != 0) {
            fprintf(stderr, "cleanup failed\n");
        }
    }
    printf("video_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
