/*
 * Microphone and camera activity test (ui/shell/hw_activity.c) against a
 * fake /proc and /sys: which video nodes are the camera, a capture stream
 * opening and closing, a process holding a camera node and letting go -
 * including one that simply disappears, as a crashed helper does - and the
 * VPU, which is a video device and not the camera.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "hw_activity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks;
static int failed;
static char root[256];

static void check(const char *name, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void sh(const char *fmt, const char *arg)
{
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), fmt, root, arg ? arg : "");
    if (system(cmd) != 0) {
        fprintf(stderr, "setup failed: %s\n", cmd);
        exit(2);
    }
}

static void put(const char *rel, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/* A process <pid> with fd <fd> pointing at <target>. */
static void fd_link(const char *pid, const char *fd, const char *target)
{
    char dir[512];
    char path[600];

    snprintf(dir, sizeof(dir), "%s/proc/%s/fd", root, pid);
    sh("mkdir -p %s/proc/%s/fd", pid);
    snprintf(path, sizeof(path), "%s/%s", dir, fd);
    unlink(path);
    if (symlink(target, path) != 0) {
        perror(path);
        exit(2);
    }
}

int main(void)
{
    struct hw_activity a;
    char proc[300];
    char sys[300];
    char path[600];

    snprintf(root, sizeof(root), "/tmp/hw_activity_test.%d", (int)getpid());
    sh("rm -rf %s%s", NULL);
    sh("mkdir -p %s/sys/class/video4linux/video0 %s/sys/class/video4linux/video1", root);
    sh("mkdir -p %s/sys/class/video4linux/video2 %s/sys/class/video4linux/video4", root);
    sh("mkdir -p %s/sys/class/video4linux/v4l-subdev0%s", NULL);
    sh("mkdir -p %s/proc/asound/card0/pcm0c/sub0 %s/proc/asound/card0/pcm0p/sub0", root);
    /* The K230's own names (unit A, 2026-09-30). */
    put("sys/class/video4linux/video0/name", "mvx\n");
    put("sys/class/video4linux/video1/name", "vvcam-video.0.0\n");
    put("sys/class/video4linux/video2/name", "vvcam-video.0.1\n");
    put("sys/class/video4linux/video4/name", "canaan-non-ai-2d\n");
    put("sys/class/video4linux/v4l-subdev0/name", "vvcam-isp-subdev.0\n");
    put("proc/asound/card0/pcm0c/sub0/status", "closed\n");
    put("proc/asound/card0/pcm0p/sub0/status", "closed\n");
    put("proc/asound/cards", " 0 [K230I2SINNO    ]: K230_I2S_INNO - K230_I2S_INNO\n");
    snprintf(proc, sizeof(proc), "%s/proc", root);
    snprintf(sys, sizeof(sys), "%s/sys", root);

    /* Some ordinary processes: a shell holding the DRM card and a tty, a
     * video player holding the VPU. */
    fd_link("1", "0", "/dev/console");
    fd_link("812", "0", "/dev/null");
    fd_link("812", "7", "/dev/dri/card0");
    fd_link("900", "5", "/dev/video0"); /* the VPU: not the camera */
    fd_link("901", "3", "/dev/video4"); /* the 2-D engine: not the camera */
    sh("mkdir -p %s/proc/self%s", NULL); /* not a pid */

    check("two camera nodes found (vvcam, not mvx or the 2-D engine)",
          hw_activity_init(&a, proc, sys) == 2);
    check("the VPU and the 2-D engine held: no camera", hw_activity_camera(&a) == 0);
    check("capture closed: no microphone", hw_activity_mic(&a) == 0);

    /* Wave or Recorder opens the capture stream. */
    put("proc/asound/card0/pcm0c/sub0/status", "state: RUNNING\nowner_pid   : 1200\n");
    check("capture running: microphone", hw_activity_mic(&a) == 1);
    put("proc/asound/card0/pcm0c/sub0/status", "state: SETUP\n");
    check("capture open but not started: still the microphone", hw_activity_mic(&a) == 1);
    put("proc/asound/card0/pcm0c/sub0/status", "closed\n");
    check("capture closed again: released", hw_activity_mic(&a) == 0);
    put("proc/asound/card0/pcm0p/sub0/status", "state: RUNNING\n");
    check("playback running is not the microphone", hw_activity_mic(&a) == 0);

    /* pos-camera holds /dev/video2. */
    fd_link("1300", "4", "/dev/video2");
    check("a helper holding video2: camera", hw_activity_camera(&a) == 1);
    check("and it is remembered", a.camera_pid == 1300);
    check("asked again: still the camera", hw_activity_camera(&a) == 1);
    /* It closes the node but lives on. */
    snprintf(path, sizeof(path), "%s/proc/1300/fd/4", root);
    unlink(path);
    check("node closed, process alive: released", hw_activity_camera(&a) == 0 && a.camera_pid == 0);
    /* Vision's helper holds video1 - and then crashes: its /proc entry goes. */
    fd_link("1400", "9", "/dev/video1");
    check("another helper on video1: camera", hw_activity_camera(&a) == 1);
    sh("rm -rf %s/proc/1400%s", NULL);
    check("the helper vanished: released on the next look", hw_activity_camera(&a) == 0);
    /* Two holders; the remembered one lets go, the other still holds. */
    fd_link("1500", "3", "/dev/video2");
    fd_link("1501", "3", "/dev/video1");
    check("two holders: camera", hw_activity_camera(&a) == 1);
    snprintf(path, sizeof(path), "%s/proc/%d", root, a.camera_pid);
    sh("rm -rf %s%s", path + strlen(root));
    check("the first gone, the second still holds: camera", hw_activity_camera(&a) == 1);

    /* A board without a camera or a sound card. */
    sh("rm -rf %s/sys/class/video4linux %s/proc/asound", root);
    check("no video4linux: no camera nodes", hw_activity_init(&a, proc, sys) == 0);
    check("no camera nodes: never the camera", hw_activity_camera(&a) == 0);
    check("no asound: never the microphone", hw_activity_mic(&a) == 0);
    check("NULL is harmless", hw_activity_mic(NULL) == 0 && hw_activity_camera(NULL) == 0);

    sh("rm -rf %s%s", NULL);
    printf("hw_activity_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
