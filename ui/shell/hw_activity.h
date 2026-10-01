/*
 * Whether the microphone and the camera are in use, from the kernel.
 *
 * The indicator LEDs (kbd_leds.h) must say what the hardware is doing, not
 * which app is on screen: Wave with its session idle does not listen, a
 * recorder helper that outlived its app still does, and a crashed helper
 * stops listening the moment the kernel closes its files. So both answers
 * are read from what the kernel reports about the devices themselves,
 * whoever holds them:
 *
 *   microphone  an ALSA capture substream that is open:
 *               <proc>/asound/card*<n>/pcm*c/sub*<n>/status reads anything
 *               but "closed" (Wave's and Recorder's pocketaudio capture,
 *               arecord, anything else)
 *   camera      a process holding a camera capture node open: the video
 *               devices whose V4L2 name starts "vvcam" (the ISP's capture
 *               nodes, /dev/video1..3 on the K230 - the VPU and the 2-D
 *               engine are video devices too, and are not the camera),
 *               found by <proc>/<pid>/fd links (pos-camera, pos-vision -
 *               and so Camera, Vision and DeskBuddy's vision provider).
 *               The vendor's isp_media_server holds every node for as long
 *               as the board runs and is not counted
 *
 * No app has to report anything, and nothing can leave an LED on after the
 * holder is gone: the answer is recomputed from scratch on every call.
 *
 * Pure C, no LVGL; the /proc and /sys roots are parameters, so the tests run
 * against a fake tree (tests/hw_activity_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_HW_ACTIVITY_H
#define POCKETOS_HW_ACTIVITY_H

#include <stdbool.h>

#define HW_ACTIVITY_MAX_CAMERA_NODES 8
/* The vendor ISP daemon's comm (15 characters, as the kernel keeps it): it
 * holds every capture node from boot on and is not a camera user. */
#define HW_ACTIVITY_ISP_DAEMON "isp_media_serve"

struct hw_activity {
    char proc_root[128];
    /* The camera's device nodes, e.g. "/dev/video2", found once. */
    char camera_node[HW_ACTIVITY_MAX_CAMERA_NODES][32];
    int camera_nodes;
    /* The pid holding the camera at the last look, or 0: checked first next
     * time, so a busy camera costs one process's fd table, not every one. */
    int camera_pid;
};

/* Find the camera's nodes under <sys_root>/class/video4linux. Returns how
 * many (0 on a board without a camera, which then never reports one). */
int hw_activity_init(struct hw_activity *a, const char *proc_root, const char *sys_root);

/* 1 when a capture substream is open, 0 when none is. */
int hw_activity_mic(const struct hw_activity *a);
/* 1 when some process holds a camera node open, 0 when none does. */
int hw_activity_camera(struct hw_activity *a);

#endif
