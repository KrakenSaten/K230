/*
 * Camera's helper client against the real helper (pos-camera-testhooks) on
 * the fake backend: the protocol end to end, pictures through the shared
 * memory in the right orientation, capture, review, delete, a full disk and a
 * disk that fills up mid-photo, no camera, a busy camera, a missing helper,
 * the camera going away, a late frame, a damaged frame, a helper that hangs
 * in the driver (the watchdog), one that crashes, one killed from outside, a
 * shell that dies under its helper, and fifty opens and closes with no
 * descriptor or child left behind.
 *
 * The fake proves nothing about a real camera's speed; the timings checked
 * here are the session's own bounds.
 *
 * Usage: camera_session_test <path to pos-camera-testhooks>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_session.h"

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
static char photos[PATH_MAX];

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

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static int files_in(const char *dir, int *tmp)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    *tmp = 0;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            *tmp += strstr(e->d_name, ".tmp") != NULL;
        } else {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* The picture buffer a real app would own. */
static uint16_t pic[64 * 64];
static uint32_t pic_w = 36;
static uint32_t pic_h = 64;
static int frames_taken;

/* Poll until an event of kind arrives or timeout_ms passes; frames are taken
 * as an app would take them. 1 with *out, 0 on timeout. */
static int wait_for(struct camera_session *s, enum camera_ev_kind kind, int timeout_ms,
                    struct camera_event *out)
{
    int64_t end = now_ms() + timeout_ms;
    struct camera_event ev;

    while (now_ms() < end) {
        while (camera_session_poll(s, &ev, now_ms())) {
            if (ev.kind == CAMERA_EV_FRAME) {
                frames_taken += camera_session_take_frame(s, pic, pic_w, pic_h);
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

static int start(struct camera_session *s, const char *fake, const char *hlp)
{
    struct camera_session_config cfg = { 0 };
    char err[160];
    int r;

    cfg.helper = hlp ? hlp : helper;
    cfg.backend = "fake";
    cfg.fake = fake;
    cfg.dir = photos;
    camera_session_init(s);
    r = camera_session_start(s, &cfg, now_ms(), err, sizeof(err));
    if (r != 0) {
        printf("     start: %s\n", err);
    }
    return r;
}

static int is_orange(uint16_t px)
{
    int r = ((px >> 11) & 0x1f) << 3;
    int g = ((px >> 5) & 0x3f) << 2;
    int b = (px & 0x1f) << 3;

    return r > 215 && g > 100 && g < 160 && b < 40;
}

#define FAST "size=64x36,still=128x72,period=5"

static void test_parse(void)
{
    struct camera_event ev;

    check("parse: ready", camera_session_parse_line("ready fake 1920 1080 1 3 IMG_0003.jpg", &ev) &&
                              ev.kind == CAMERA_EV_READY && ev.simulated && ev.value == 3 &&
                              strcmp(ev.text, "IMG_0003.jpg") == 0);
    check("parse: ready with no photo", camera_session_parse_line("ready fake 1920 1080 0 0 -", &ev) &&
                                            ev.text[0] == '\0' && !ev.simulated);
    check("parse: a frame", camera_session_parse_line("frame 2 17 528 938", &ev) &&
                                ev.value == 2 && ev.w == 528 && ev.h == 938);
    check("parse: a frame in the review slot is refused",
          !camera_session_parse_line("frame 3 17 528 938", &ev));
    check("parse: a frame past the slot size is refused",
          !camera_session_parse_line("frame 0 17 2000 938", &ev));
    check("parse: a frame of no size is refused", !camera_session_parse_line("frame 0 1 0 10", &ev));
    check("parse: a frame with junk is refused", !camera_session_parse_line("frame 0 x 10 10", &ev));
    check("parse: captured", camera_session_parse_line("captured 3 528 938 1234 IMG_0001.jpg 1",
                                                       &ev) &&
                                 ev.bytes == 1234 && strcmp(ev.name, "IMG_0001.jpg") == 0);
    check("parse: captured in a preview slot is refused",
          !camera_session_parse_line("captured 0 528 938 1234 IMG_0001.jpg 1", &ev));
    check("parse: captured with half a size is refused",
          !camera_session_parse_line("captured 3 528 0 1234 IMG_0001.jpg 1", &ev));
    check("parse: capfail nospace", camera_session_parse_line("capfail nospace storage is full", &ev) &&
                                        ev.reason == CAMERA_CAPFAIL_NOSPACE &&
                                        strcmp(ev.text, "storage is full") == 0);
    check("parse: an unknown word is not an event", !camera_session_parse_line("zoom 2", &ev));
}

static void test_happy(void)
{
    struct camera_session s;
    struct camera_event ev;
    int64_t t0;
    int n;
    int tmp;
    int baseline = open_fds();

    check("start", start(&s, FAST, NULL) == 0);
    check("ready", wait_for(&s, CAMERA_EV_READY, 3000, &ev));
    check("simulated, an empty folder", ev.simulated && ev.value == 0 && ev.text[0] == '\0' &&
                                            ev.w == 128 && ev.h == 72);
    camera_session_view(&s, pic_w, pic_h, 0);
    camera_session_preview(&s, true, now_ms());
    frames_taken = 0;
    check("a frame", wait_for(&s, CAMERA_EV_FRAME, 2000, NULL));
    wait_for(&s, CAMERA_EV_FRAME, 1000, NULL);
    check("taken into the app's buffer", frames_taken >= 1);
    /* Portrait (display 0): the fake is mounted like unit A's sensor (90), so
     * a quarter turn clockwise puts its marker top-right. */
    check("portrait: turned a quarter clockwise, the marker top-right",
          is_orange(pic[1 * pic_w + pic_w - 2]) && !is_orange(pic[1 * pic_w + 1]));
    t0 = now_ms();
    frames_taken = 0;
    while (now_ms() - t0 < 1000) {
        wait_for(&s, CAMERA_EV_STOPPED, 50, NULL);
    }
    printf("     %d frames in one second (the fake runs at 200)\n", frames_taken);
    check("the preview is capped at 10 a second", frames_taken >= 5 && frames_taken <= 11);

    /* A frame of the wrong size is dropped and its slot given back: frames
     * keep coming, so no slot leaked. */
    pic_w = 30;
    frames_taken = 0;
    for (n = 0; n < 10; n++) {
        wait_for(&s, CAMERA_EV_FRAME, 300, NULL);
    }
    check("pictures of another size are not copied", frames_taken == 0);
    pic_w = 36;
    frames_taken = 0;
    wait_for(&s, CAMERA_EV_FRAME, 1000, NULL);
    check("and no slot leaked: they still come", frames_taken == 1);

    /* Landscape. */
    pic_w = 64;
    pic_h = 36;
    camera_session_view(&s, pic_w, pic_h, 270);
    frames_taken = 0;
    while (frames_taken == 0 && wait_for(&s, CAMERA_EV_FRAME, 1000, NULL)) {
    }
    /* The shell's landscape (display 270): 90 - 270, a half turn. */
    check("landscape: turned a half turn, the marker bottom-right",
          is_orange(pic[(pic_h - 2) * pic_w + pic_w - 2]) && !is_orange(pic[1 * pic_w + 1]));

    camera_session_capture(&s, 270, now_ms());
    check("saving", wait_for(&s, CAMERA_EV_SAVING, 3000, NULL));
    check("captured", wait_for(&s, CAMERA_EV_CAPTURED, 5000, &ev));
    check("a photo of the view's size for review", ev.w == 64 && ev.h == 36 && ev.value == 1);
    check("named as a photo with no clock or a clock", strncmp(ev.name, "IMG_", 4) == 0);
    memset(pic, 0, sizeof(pic));
    check("the review picture is taken", camera_session_take_review(&s, pic, 64, 36) == 1 &&
                                             is_orange(pic[34 * 64 + 62]));
    check("and only once", camera_session_take_review(&s, pic, 64, 36) == 0);
    check("the photo is in the folder, whole", files_in(photos, &tmp) == 1 && tmp == 0);
    frames_taken = 0;
    wait_for(&s, CAMERA_EV_FRAME, 300, NULL);
    check("no preview after a capture until asked", frames_taken == 0);
    camera_session_preview(&s, true, now_ms());
    check("preview again", wait_for(&s, CAMERA_EV_FRAME, 2000, NULL));

    camera_session_delete(&s, ev.name, now_ms());
    check("deleted", wait_for(&s, CAMERA_EV_DELETED, 3000, &ev) && ev.value == 0);
    check("the folder is empty", files_in(photos, &tmp) == 0 && tmp == 0);
    camera_session_delete(&s, ev.name, now_ms());
    check("deleting it again fails cleanly", wait_for(&s, CAMERA_EV_DELFAIL, 3000, NULL));
    check("a name with a slash is not even sent", camera_session_delete(&s, "IMG_/x.jpg", 0) == -1);

    camera_session_preview(&s, false, now_ms());
    check("stopped", wait_for(&s, CAMERA_EV_STOPPED, 3000, NULL));
    frames_taken = 0;
    wait_for(&s, CAMERA_EV_FRAME, 400, NULL);
    check("nothing after the stop", frames_taken == 0);

    t0 = now_ms();
    camera_session_abandon(&s, 300);
    check("abandon is quick", now_ms() - t0 < 500);
    check("and leaves no child", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    check("and no descriptor", open_fds() == baseline);
    check("the session is idle", !camera_session_active(&s) &&
                                     camera_session_poll(&s, &ev, now_ms()) == 0);
    check("a command to no helper fails", camera_session_preview(&s, true, now_ms()) == -1);
}

static void test_open_failures(void)
{
    struct camera_session s;
    struct camera_event ev;
    struct camera_session_config cfg = { 0 };
    char err[160];
    int64_t t0;

    cfg.helper = helper;
    cfg.backend = "v4l2";
    cfg.dir = photos;
    camera_session_init(&s);
    camera_session_start(&s, &cfg, now_ms(), err, sizeof(err));
    check("v4l2 on a host with no camera: nodevice", wait_for(&s, CAMERA_EV_NODEVICE, 3000, &ev) &&
                                                         strstr(ev.text, "no camera") != NULL);
    check("then the helper leaves with 3", wait_for(&s, CAMERA_EV_EXITED, 3000, &ev) &&
                                               ev.value == 3 && ev.reason == CAMERA_EXIT_NORMAL);

    start(&s, "open=busy", NULL);
    check("busy: an error that says so", wait_for(&s, CAMERA_EV_ERROR, 3000, &ev) &&
                                             strncmp(ev.text, "busy", 4) == 0);
    check("and it leaves", wait_for(&s, CAMERA_EV_EXITED, 3000, NULL));

    start(&s, "frmaes=3", NULL);
    check("a bad script is an open error", wait_for(&s, CAMERA_EV_ERROR, 3000, &ev));
    wait_for(&s, CAMERA_EV_EXITED, 3000, NULL);

    start(&s, NULL, "/nonexistent/pos-camera");
    check("a missing helper: exec error", wait_for(&s, CAMERA_EV_ERROR, 3000, &ev) &&
                                              strncmp(ev.text, "exec", 4) == 0);
    check("exit 127", wait_for(&s, CAMERA_EV_EXITED, 3000, &ev) && ev.value == 127);

    /* A camera that never finishes opening: the watchdog. */
    t0 = now_ms();
    start(&s, "open=hang", NULL);
    check("a hung open is killed", wait_for(&s, CAMERA_EV_EXITED, CAMERA_OPEN_MS + 3000, &ev) &&
                                       ev.reason == CAMERA_EXIT_HUNG);
    check("after the open deadline", now_ms() - t0 >= CAMERA_OPEN_MS - 100);
    check("nothing left", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
}

static void test_stream_failures(void)
{
    struct camera_session s;
    struct camera_event ev;
    int64_t t0;

    start(&s, FAST ",lost_after=4", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_preview(&s, true, now_ms());
    check("the camera goes away: lost", wait_for(&s, CAMERA_EV_LOST, 3000, &ev));
    check("the helper leaves with 4", wait_for(&s, CAMERA_EV_EXITED, 3000, &ev) && ev.value == 4);

    start(&s, "size=64x36,period=50,delay_at=3:1600", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_preview(&s, true, now_ms());
    camera_session_view(&s, pic_w, pic_h, 270);
    check("a late frame: a stall is reported", wait_for(&s, CAMERA_EV_STALL, 3000, &ev) &&
                                                   ev.value >= 1000);
    check("then the frames come back", wait_for(&s, CAMERA_EV_FRAME, 3000, NULL));
    camera_session_abandon(&s, 300);

    start(&s, "size=64x36,period=5,malformed_at=3", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_preview(&s, true, now_ms());
    check("a damaged frame is reported", wait_for(&s, CAMERA_EV_MALFORMED, 3000, &ev));
    check("and the stream goes on", wait_for(&s, CAMERA_EV_FRAME, 3000, NULL));
    camera_session_abandon(&s, 300);

    t0 = now_ms();
    start(&s, "size=64x36,period=5,hang_at=5", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_preview(&s, true, now_ms());
    check("a helper stuck in the driver is killed by the watchdog",
          wait_for(&s, CAMERA_EV_EXITED, CAMERA_SILENCE_MS + 3000, &ev) &&
              ev.reason == CAMERA_EXIT_HUNG);
    check("within the silence bound", now_ms() - t0 < CAMERA_SILENCE_MS + 2000);

    start(&s, "size=64x36,period=5,crash_at=4", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_preview(&s, true, now_ms());
    check("a crash is a crash", wait_for(&s, CAMERA_EV_EXITED, 3000, &ev) &&
                                    ev.reason == CAMERA_EXIT_CRASHED && ev.value == 128 + SIGABRT);
    check("and nothing can be taken from it", camera_session_take_frame(&s, pic, pic_w, pic_h) == 0);

    start(&s, FAST, NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_preview(&s, true, now_ms());
    wait_for(&s, CAMERA_EV_FRAME, 2000, NULL);
    kill(s.pid, SIGKILL);
    check("killed from outside: crashed", wait_for(&s, CAMERA_EV_EXITED, 3000, &ev) &&
                                              ev.reason == CAMERA_EXIT_CRASHED);
    check("its shared memory is gone with it", s.shm == NULL && s.shm_fd < 0);
}

static void test_capture_failures(void)
{
    struct camera_session s;
    struct camera_event ev;
    int tmp;

    setenv("POCKETCAM_TEST_FREE_BYTES", "1000", 1);
    start(&s, FAST, NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_preview(&s, true, now_ms());
    wait_for(&s, CAMERA_EV_FRAME, 2000, NULL);
    camera_session_capture(&s, 270, now_ms());
    check("a full disk: capfail nospace", wait_for(&s, CAMERA_EV_CAPFAIL, 3000, &ev) &&
                                              ev.reason == CAMERA_CAPFAIL_NOSPACE);
    camera_session_preview(&s, true, now_ms());
    check("the preview goes on", wait_for(&s, CAMERA_EV_FRAME, 2000, NULL));
    camera_session_abandon(&s, 300);
    unsetenv("POCKETCAM_TEST_FREE_BYTES");

    setenv("POCKETCAM_TEST_FAIL_AFTER", "100", 1);
    start(&s, FAST, NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_capture(&s, 270, now_ms());
    check("a disk that fills mid-photo: capfail nospace",
          wait_for(&s, CAMERA_EV_CAPFAIL, 3000, &ev) && ev.reason == CAMERA_CAPFAIL_NOSPACE);
    check("no half photo and no temporary", files_in(photos, &tmp) == 0 && tmp == 0);
    camera_session_abandon(&s, 300);
    unsetenv("POCKETCAM_TEST_FAIL_AFTER");

    start(&s, FAST ",capture=fail", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_view(&s, pic_w, pic_h, 270);
    camera_session_capture(&s, 270, now_ms());
    check("the camera refuses a still: capfail device",
          wait_for(&s, CAMERA_EV_CAPFAIL, 3000, &ev) && ev.reason == CAMERA_CAPFAIL_DEVICE);
    camera_session_preview(&s, true, now_ms());
    check("the preview starts again after it", wait_for(&s, CAMERA_EV_FRAME, 2000, NULL));
    camera_session_abandon(&s, 300);

    start(&s, FAST ",capture=lost", NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_capture(&s, 270, now_ms());
    check("the camera goes away during a still: lost", wait_for(&s, CAMERA_EV_LOST, 3000, NULL));
    wait_for(&s, CAMERA_EV_EXITED, 3000, NULL);

    /* Capture without a view: saved, but no review picture. */
    start(&s, FAST, NULL);
    wait_for(&s, CAMERA_EV_READY, 3000, NULL);
    camera_session_capture(&s, 0, now_ms());
    check("no view: captured with no review picture",
          wait_for(&s, CAMERA_EV_CAPTURED, 5000, &ev) && ev.w == 0 && ev.h == 0);
    check("nothing to take", camera_session_take_review(&s, pic, 36, 64) == 0);
    camera_session_delete(&s, ev.name, now_ms());
    wait_for(&s, CAMERA_EV_DELETED, 3000, NULL);
    camera_session_abandon(&s, 300);
}

/* Fork a pretend shell that starts a session and dies without abandoning it;
 * with hold, a grandchild keeps every descriptor of the shell open for a
 * while. 1 when the helper is gone within three seconds. */
static int orphan(bool hold)
{
    struct camera_session s;
    int pfd[2];
    pid_t shell;
    pid_t ids[2] = { -1, -1 };
    int64_t end;
    int gone = 0;

    if (pipe(pfd) != 0) {
        return 0;
    }
    shell = fork();
    if (shell == 0) {
        close(pfd[0]);
        start(&s, FAST, NULL);
        wait_for(&s, CAMERA_EV_READY, 3000, NULL);
        camera_session_preview(&s, true, now_ms());
        ids[0] = s.pid;
        if (hold) {
            ids[1] = fork();
            if (ids[1] == 0) {
                nap(6000);
                _exit(0);
            }
        }
        if (write(pfd[1], ids, sizeof(ids)) != sizeof(ids)) {
            _exit(2);
        }
        _exit(0); /* no abandon: the shell just dies */
    }
    close(pfd[1]);
    if (read(pfd[0], ids, sizeof(ids)) != sizeof(ids)) {
        ids[0] = -1;
    }
    close(pfd[0]);
    waitpid(shell, NULL, 0);
    end = now_ms() + 3000;
    while (ids[0] > 0 && now_ms() < end) {
        if (kill(ids[0], 0) != 0 && errno == ESRCH) {
            gone = 1;
            break;
        }
        nap(20);
    }
    if (ids[1] > 0) {
        kill(ids[1], SIGKILL);
    }
    if (ids[0] > 0 && !gone) {
        kill(ids[0], SIGKILL);
    }
    return gone;
}

static void test_lifetime(void)
{
    struct camera_session s;
    struct camera_event ev;
    int baseline = open_fds();
    int64_t worst = 0;
    int ok = 1;
    int i;

    /* Fifty visits to the screen: open, a picture, leave - including leaving
     * before the helper has said anything at all. */
    for (i = 0; i < 50; i++) {
        int64_t t0;

        if (start(&s, FAST, NULL) != 0) {
            ok = 0;
            break;
        }
        if (i % 5 != 0) {
            ok &= wait_for(&s, CAMERA_EV_READY, 3000, NULL);
            camera_session_view(&s, pic_w, pic_h, 270);
            camera_session_preview(&s, true, now_ms());
            ok &= wait_for(&s, CAMERA_EV_FRAME, 2000, NULL);
        }
        t0 = now_ms();
        camera_session_abandon(&s, 300);
        if (now_ms() - t0 > worst) {
            worst = now_ms() - t0;
        }
    }
    printf("     slowest close: %lld ms\n", (long long)worst);
    check("fifty opens and closes", ok);
    check("each close within its bound", worst < 300 + CAMERA_KILL_REAP_MS + 100);
    check("no descriptor left", open_fds() == baseline);
    check("no child left", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);

    /* A shell that dies under a running helper: the helper leaves with it -
     * by end of file when the shell's end of the socket closes, and by the
     * death signal alone when something else still holds that end open
     * (a descriptor inherited by another child of the shell). */
    check("a helper whose shell died leaves too (its socket closed)", orphan(false));
    check("and by the death signal alone (its socket still held open)", orphan(true));
    (void)ev;
}

/* ---- the library ------------------------------------------------------------ */

static void test_library_parse(void)
{
    struct camera_event ev;

    check("parse: listed", camera_session_parse_line("listed 3 12 40", &ev) &&
                               ev.kind == CAMERA_EV_LISTED && ev.value == 12 && ev.bytes == 40);
    check("parse: a list in a picture slot is refused",
          !camera_session_parse_line("listed 0 12 40", &ev));
    check("parse: a list longer than the library is refused",
          !camera_session_parse_line("listed 3 5000 5000", &ev));
    check("parse: an image",
          camera_session_parse_line("image 1 170 170 1080 1920 412345 1790000000 0 jpg "
                                    "2026:09:26T10:15:30 IMG_20260926_101530_0003.jpg",
                                    &ev) &&
              ev.kind == CAMERA_EV_IMAGE && ev.value == 1 && ev.w == 170 &&
              ev.image.shown_w == 1080 && ev.image.shown_h == 1920 &&
              ev.image.bytes == 412345 && ev.image.jpeg && !ev.image.damaged &&
              strcmp(ev.image.taken, "2026:09:26 10:15:30") == 0 && ev.text[0] == '\0');
    check("parse: an undated, damaged, simulated image",
          camera_session_parse_line("image 0 10 10 64 36 99 0 1 ppm - IMG_0001.ppm Simulated "
                                    "picture",
                                    &ev) &&
              ev.image.damaged && !ev.image.jpeg && ev.image.taken[0] == '\0' &&
              strcmp(ev.text, "Simulated picture") == 0);
    check("parse: an image in the review slot is refused",
          !camera_session_parse_line("image 3 10 10 64 36 99 0 0 ppm - IMG_0001.ppm", &ev));
    check("parse: an image bigger than a slot is refused",
          !camera_session_parse_line("image 0 2000 10 64 36 99 0 0 ppm - IMG_0001.ppm", &ev));
    check("parse: an image named like a path is refused",
          !camera_session_parse_line("image 0 10 10 64 36 99 0 0 ppm - ../etc/passwd", &ev));
    check("parse: imgfail", camera_session_parse_line("imgfail 2 IMG_0009.jpg corrupt bad", &ev) &&
                                ev.kind == CAMERA_EV_IMGFAIL && ev.value == 2 &&
                                ev.reason == CAMERA_IMGFAIL_CORRUPT);
    check("parse: exported",
          camera_session_parse_line("exported IMG_0009.jpg 1 /root/Pictures/IMG_0009.jpg", &ev) &&
              ev.value == 1 && strcmp(ev.path, "/root/Pictures/IMG_0009.jpg") == 0);
    check("parse: expfail nospace", camera_session_parse_line("expfail IMG_0009.jpg nospace x", &ev) &&
                                        ev.reason == CAMERA_EXPFAIL_NOSPACE);
}

static int start_library(struct camera_session *s, const char *export_dir)
{
    struct camera_session_config cfg = { 0 };
    char err[160];

    cfg.helper = helper;
    cfg.library = true;
    cfg.dir = photos;
    cfg.export_dir = export_dir;
    camera_session_init(s);
    return camera_session_start(s, &cfg, now_ms(), err, sizeof(err));
}

/* Three photos taken with the camera, as the app takes them. */
static int take_photos(int n)
{
    struct camera_session s;
    int i;
    int ok = 1;

    if (start(&s, FAST, NULL) != 0 || !wait_for(&s, CAMERA_EV_READY, 3000, NULL)) {
        return 0;
    }
    camera_session_view(&s, 36, 64, 0);
    for (i = 0; i < n; i++) {
        camera_session_capture(&s, 0, now_ms());
        ok &= wait_for(&s, CAMERA_EV_CAPTURED, 5000, NULL);
        camera_session_take_review(&s, NULL, 0, 0);
        nap(1100); /* a second apart: dated names differ in more than their number */
    }
    camera_session_abandon(&s, 300);
    return ok;
}

static void test_library(const char *tmpl)
{
    static uint16_t big[240 * 240];
    struct camera_session s;
    struct camera_event ev;
    char names[16][CAMERA_NAME_MAX];
    char export_dir[PATH_MAX];
    char p[PATH_MAX + 32];
    char cmdline[256];
    int baseline = open_fds();
    int slots[4];
    uint32_t w = 0;
    uint32_t h = 0;
    int n;
    int i;
    int tmp;
    int ok;
    FILE *fp;
    int64_t t0;

    snprintf(export_dir, sizeof(export_dir), "%s/home/Pictures", tmpl);
    check("three photos are taken for the gallery", take_photos(3));
    snprintf(p, sizeof(p), "%s/IMG_0100.ppm", photos);
    fp = fopen(p, "wb");
    if (fp) {
        fputs("P6\n64 36\n", fp); /* no maximum, no pixels: damaged beyond reading */
        fclose(fp);
    }

    check("the library helper starts", start_library(&s, export_dir) == 0);
    check("and is ready", wait_for(&s, CAMERA_EV_READY, 3000, &ev));
    check("as the library, counting four photos",
          strcmp(ev.name, "library") == 0 && ev.value == 4 && strcmp(ev.text, "IMG_0100.ppm") == 0);
    snprintf(p, sizeof(p), "/proc/%d/cmdline", (int)s.pid);
    fp = fopen(p, "rb");
    memset(cmdline, 0, sizeof(cmdline));
    if (fp) {
        size_t got = fread(cmdline, 1, sizeof(cmdline) - 1, fp);

        for (i = 0; i < (int)got; i++) {
            cmdline[i] = cmdline[i] ? cmdline[i] : ' ';
        }
        fclose(fp);
    }
    check("it is `pos-camera library`, with no camera backend",
          strstr(cmdline, " library ") != NULL && strstr(cmdline, "--backend") == NULL);

    check("camera commands are not library ones", camera_session_list(&(struct camera_session){ 0 }, 0) == -1);
    camera_session_list(&s, now_ms());
    check("listed", wait_for(&s, CAMERA_EV_LISTED, 3000, &ev) && ev.value == 4 && ev.bytes == 4);
    n = camera_session_take_list(&s, names, 16);
    check("four names, newest first", n == 4 && strcmp(names[0], "IMG_0100.ppm") == 0 &&
                                          strncmp(names[1], "IMG_", 4) == 0 &&
                                          strcmp(names[1], names[2]) > 0);
    check("the list is taken once", camera_session_take_list(&s, names, 16) == 0);

    /* Three pictures at once; a fourth waits for a slot. */
    for (i = 0; i < 3; i++) {
        slots[i] = camera_session_request_picture(&s, names[i + 1], 60, 60, true, now_ms());
    }
    check("three pictures are asked for at once",
          slots[0] == 0 && slots[1] == 1 && slots[2] == 2 && camera_session_pictures_free(&s) == 0);
    check("a fourth has to wait",
          camera_session_request_picture(&s, names[0], 60, 60, true, now_ms()) == -1);
    ok = 1;
    for (i = 0; i < 3; i++) {
        memset(big, 0, sizeof(big));
        ok &= wait_for(&s, CAMERA_EV_IMAGE, 3000, &ev);
        ok &= camera_session_take_picture(&s, ev.value, big, 60, 60, &w, &h) == 1;
        ok &= w == 60 && h == 60 && ev.image.shown_w == 72 && ev.image.shown_h == 128;
        ok &= ev.image.bytes > 0 && ev.image.mtime > 0;
    }
    check("each comes back as a 60 x 60 thumbnail of the upright 72 x 128 photo", ok);
    check("marked simulated, and dated by the clock it was taken with",
          strcmp(ev.text, "Simulated picture") == 0 &&
              (ev.image.taken[0] != '\0') == (strncmp(ev.name, "IMG_2", 5) == 0));
    check("and every slot is free again", camera_session_pictures_free(&s) == 3);

    i = camera_session_request_picture(&s, names[0], 60, 60, false, now_ms());
    check("a damaged file is said to be corrupt",
          wait_for(&s, CAMERA_EV_IMGFAIL, 3000, &ev) && ev.value == i &&
              ev.reason == CAMERA_IMGFAIL_CORRUPT && strcmp(ev.name, "IMG_0100.ppm") == 0);
    camera_session_request_picture(&s, "IMG_9999.ppm", 60, 60, false, now_ms());
    check("a missing one missing", wait_for(&s, CAMERA_EV_IMGFAIL, 3000, &ev) &&
                                       ev.reason == CAMERA_IMGFAIL_MISSING);
    check("a name with a slash is not even sent",
          camera_session_request_picture(&s, "../x.jpg", 60, 60, false, now_ms()) == -1);
    check("nor a box bigger than a slot",
          camera_session_request_picture(&s, names[1], 2000, 60, false, now_ms()) == -1);
    i = camera_session_request_picture(&s, names[1], 240, 240, false, now_ms());
    check("contain: the whole photo, fitted",
          wait_for(&s, CAMERA_EV_IMAGE, 3000, &ev) && ev.w == 135 && ev.h == 240 &&
              camera_session_take_picture(&s, i, big, 240, 240, &w, &h) == 1 && w == 135);
    check("upright, as it was taken: the fake's marker top-right",
          is_orange(big[2 * 135 + 132]) && !is_orange(big[2 * 135 + 2]) &&
              !is_orange(big[237 * 135 + 132]));
    i = camera_session_request_picture(&s, names[1], 240, 240, false, now_ms());
    check("a picture larger than the buffer offered is not copied, but given back",
          wait_for(&s, CAMERA_EV_IMAGE, 3000, &ev) &&
              camera_session_take_picture(&s, i, big, 100, 100, &w, &h) == 0 &&
              camera_session_pictures_free(&s) == 3);

    /* Many pictures in a row: nothing leaks. */
    ok = 1;
    t0 = now_ms();
    for (i = 0; i < 60; i++) {
        int slot = camera_session_request_picture(&s, names[1 + i % 3], 120, 120, true, now_ms());

        ok &= slot >= 0 && wait_for(&s, CAMERA_EV_IMAGE, 3000, &ev) &&
              camera_session_take_picture(&s, slot, big, 120, 120, &w, &h) == 1;
    }
    printf("     sixty thumbnails in %lld ms\n", (long long)(now_ms() - t0));
    check("sixty pictures in a row, no slot lost", ok && camera_session_pictures_free(&s) == 3);

    camera_session_export(&s, names[1], now_ms());
    check("exported", wait_for(&s, CAMERA_EV_EXPORTED, 3000, &ev) && ev.value == 0 &&
                          strncmp(ev.path, export_dir, strlen(export_dir)) == 0);
    check("the copy is in the export folder", access(ev.path, R_OK) == 0);
    camera_session_export(&s, names[1], now_ms());
    check("again: already there", wait_for(&s, CAMERA_EV_EXPORTED, 3000, &ev) && ev.value == 1);
    camera_session_export(&s, "IMG_9999.jpg", now_ms());
    check("a photo that is gone cannot be exported",
          wait_for(&s, CAMERA_EV_EXPFAIL, 3000, &ev) && ev.reason == CAMERA_EXPFAIL_MISSING);

    camera_session_delete(&s, names[0], now_ms());
    check("deleted", wait_for(&s, CAMERA_EV_DELETED, 3000, &ev) && ev.value == 3);
    camera_session_delete(&s, names[0], now_ms());
    check("a photo already gone is deleted too, so its entry can go",
          wait_for(&s, CAMERA_EV_DELETED, 3000, &ev) && ev.value == 3 &&
              strcmp(ev.name, names[0]) == 0);
    camera_session_list(&s, now_ms());
    check("and listed no more", wait_for(&s, CAMERA_EV_LISTED, 3000, &ev) && ev.value == 3 &&
                                    camera_session_take_list(&s, names, 16) == 3 &&
                                    strcmp(names[0], "IMG_0100.ppm") != 0);
    check("the library itself is untouched by an export", files_in(photos, &tmp) == 3 && tmp == 0);

    t0 = now_ms();
    camera_session_abandon(&s, 300);
    check("the library closes quickly", now_ms() - t0 < 500);
    check("and leaves no child", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    check("and no descriptor", open_fds() == baseline);

    /* An empty or missing library. */
    {
        char keep[PATH_MAX];

        snprintf(keep, sizeof(keep), "%s", photos);
        snprintf(photos, sizeof(photos), "%s/empty-camera", tmpl);
        check("an empty library starts", start_library(&s, export_dir) == 0 &&
                                              wait_for(&s, CAMERA_EV_READY, 3000, &ev) &&
                                              ev.value == 0 && ev.text[0] == '\0');
        camera_session_list(&s, now_ms());
        check("and lists nothing", wait_for(&s, CAMERA_EV_LISTED, 3000, &ev) && ev.value == 0 &&
                                       camera_session_take_list(&s, names, 16) == 0);
        camera_session_abandon(&s, 300);
        snprintf(photos, sizeof(photos), "%s", keep);
    }
}

/* The helper answers one request after another. A delete sent behind three
 * slow pictures waits longer than its own window (CAMERA_REPLY_MS) in all;
 * each picture that comes back says the helper is alive, so it is not killed. */
static void test_library_queue(const char *tmpl)
{
    struct camera_session s;
    struct camera_event ev;
    char names[16][CAMERA_NAME_MAX];
    char export_dir[PATH_MAX];
    int n;
    int i;
    int ok = 1;
    int64_t t0;

    snprintf(export_dir, sizeof(export_dir), "%s/home/Pictures", tmpl);
    setenv("POCKETCAM_TEST_DECODE_MS", "1300", 1);
    check("a slow library helper starts", start_library(&s, export_dir) == 0 &&
                                              wait_for(&s, CAMERA_EV_READY, 3000, &ev));
    unsetenv("POCKETCAM_TEST_DECODE_MS");
    camera_session_list(&s, now_ms());
    n = wait_for(&s, CAMERA_EV_LISTED, 3000, &ev) ? camera_session_take_list(&s, names, 16) : 0;
    check("with photos in it", n >= 2);
    t0 = now_ms();
    for (i = 0; i < CAMERA_PICTURE_SLOTS && n > 0; i++) {
        ok &= camera_session_request_picture(&s, names[1 + i % (n - 1)], 60, 60, true, now_ms()) >= 0;
    }
    check("three slow pictures are asked for", ok);
    camera_session_delete(&s, names[0], now_ms());
    check("a delete behind them is answered, the helper not taken for hung",
          wait_for(&s, CAMERA_EV_DELETED, 3 * 1300 + CAMERA_REPLY_MS + 2000, &ev) &&
              now_ms() - t0 > CAMERA_REPLY_MS);
    for (i = 0; i < CAMERA_PICTURE_SLOTS; i++) {
        camera_session_take_picture(&s, i, NULL, 0, 0, NULL, NULL);
    }
    check("and every slot comes back", camera_session_pictures_free(&s) == CAMERA_PICTURE_SLOTS);
    camera_session_abandon(&s, 300);
    check("and leaves no child", waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/camera-session-XXXXXX";

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2 || !realpath(argv[1], helper)) {
        fprintf(stderr, "usage: camera_session_test <pos-camera-testhooks>\n");
        return 2;
    }
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(photos, sizeof(photos), "%s/camera", tmpl);
    unsetenv("POCKETCAM_TEST_FREE_BYTES");
    unsetenv("POCKETCAM_TEST_FAIL_AFTER");
    unsetenv("POCKETOS_CAMERA_FAKE");
    test_parse();
    test_happy();
    test_open_failures();
    test_stream_failures();
    test_capture_failures();
    test_lifetime();
    test_library_parse();
    test_library(tmpl);
    test_library_queue(tmpl);
    {
        char cmd[PATH_MAX + 16];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", tmpl);
        }
    }
    printf("camera_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
