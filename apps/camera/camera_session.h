/*
 * Camera's link to its helper: one pos-camera process for as long as the
 * Camera screen is open, driven without ever blocking the LVGL thread.
 *
 * Why a helper process (docs/decisions/ADR-006-camera-ownership.md, PROPOSED):
 * apps do not touch hardware (ADR-002, ui/shell/app.h), and the K230 camera
 * sits on out-of-tree vendor modules and a closed ISP daemon whose failure
 * modes nobody here has seen yet. With the camera in a child process:
 *
 *   - nothing on the LVGL thread opens the camera, waits for a frame,
 *     converts pixels or encodes a photo;
 *   - a helper stuck in a driver is killed by the watchdog below instead of
 *     freezing the panel, and the kernel releases what it held;
 *   - if the shell dies, or restarts itself to rotate, the helper gets
 *     SIGTERM (PR_SET_PDEATHSIG) and the camera is released with it;
 *   - the bench tool and the app are one binary and one code path.
 *
 * Pictures come through shared memory, never through the socket, and never
 * as a pointer the app keeps: camera_session_take_frame() and
 * camera_session_take_review() copy the newest picture into the caller's
 * buffer and hand the slot straight back to the helper. There are no
 * callbacks: nothing here can call into an app that has been destroyed.
 *
 * WATCHDOG. Every wait on the helper has a deadline, checked by poll(): the
 * first line (CAMERA_HELLO_MS), the camera opening (CAMERA_OPEN_MS), silence
 * while streaming (CAMERA_SILENCE_MS - a streaming helper reports a frame or a
 * stall at least every POCKETCAM_STALL_MS), a capture (CAMERA_CAPTURE_MS) and
 * any other reply (CAMERA_REPLY_MS). A missed deadline kills the helper and
 * ends the session with CAMERA_EXIT_HUNG.
 *
 * Pure C, no LVGL, clock passed in: tested on a host against the real helper
 * with the fake backend (tests/camera_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef CAMERA_SESSION_H
#define CAMERA_SESSION_H

#include "pocketcam/pocketcam_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CAMERA_HELLO_MS 3000
/* Opening the camera. On the K230 this includes the ISP daemon bringing the
 * sensor up, which is unmeasured (CAMERA_PLATFORM_RESEARCH.md, U6). */
#define CAMERA_OPEN_MS 10000
#define CAMERA_SILENCE_MS 4000
/* A still: POCKETCAM_STILL_TIMEOUT_MS for the backend, then encoding and a
 * synced write, both unmeasured on the C908. */
#define CAMERA_CAPTURE_MS 20000
#define CAMERA_REPLY_MS 3000
/* abandon(): from quit to SIGKILL, and after SIGKILL how long to reap. */
#define CAMERA_KILL_REAP_MS 200
#define CAMERA_EVENT_QUEUE 32
#define CAMERA_EVENT_TEXT_MAX 96
#define CAMERA_NAME_MAX 48
#define CAMERA_HELPER_PATH_MAX 256
#define CAMERA_ARG_MAX 256

enum camera_ev_kind {
    CAMERA_EV_READY,     /* name, simulated, value = photos, text = newest photo or "" */
    CAMERA_EV_NODEVICE,  /* text */
    CAMERA_EV_ERROR,     /* text = "<what> <text>" */
    CAMERA_EV_FRAME,     /* a preview picture is waiting: camera_session_take_frame() */
    CAMERA_EV_STALL,     /* value = ms without a frame */
    CAMERA_EV_MALFORMED, /* value = damaged frames in a row */
    CAMERA_EV_STOPPED,
    CAMERA_EV_SAVING,
    CAMERA_EV_CAPTURED,  /* name, bytes, value = photos; w x h review (0 x 0: none) */
    CAMERA_EV_CAPFAIL,   /* reason, text */
    CAMERA_EV_DELETED,   /* name, value = photos */
    CAMERA_EV_DELFAIL,   /* text */
    CAMERA_EV_LOST,      /* text */
    CAMERA_EV_EXITED     /* reason; value = exit code, or 128 + signal */
};

enum camera_capfail {
    CAMERA_CAPFAIL_NOSPACE,
    CAMERA_CAPFAIL_QUOTA,
    CAMERA_CAPFAIL_DEVICE,
    CAMERA_CAPFAIL_IO
};

enum camera_exit {
    CAMERA_EXIT_NORMAL,   /* it left by itself (after quit, nodevice, error or lost) */
    CAMERA_EXIT_HUNG,     /* a deadline passed and it was killed */
    CAMERA_EXIT_PROTOCOL, /* it said something impossible and was killed */
    CAMERA_EXIT_CRASHED   /* a signal nobody here sent */
};

struct camera_event {
    enum camera_ev_kind kind;
    int value;
    int reason;           /* enum camera_capfail or enum camera_exit */
    bool simulated;
    uint32_t w;
    uint32_t h;
    uint64_t bytes;
    char name[CAMERA_NAME_MAX];
    char text[CAMERA_EVENT_TEXT_MAX];
};

struct camera_session_config {
    const char *helper;   /* NULL: camera_session_helper_path() */
    const char *backend;  /* NULL: camera_session_backend() */
    const char *fake;     /* the fake backend's script, or NULL */
    const char *dir;      /* the photo folder, or NULL for the helper's default */
};

struct camera_session {
    pid_t pid;
    int fd;               /* our end of the socketpair, or -1 */
    int shm_fd;
    const uint8_t *shm;   /* read-only mapping, or NULL */
    bool running;
    bool eof;
    bool killed;
    enum camera_exit exit_reason;
    char line[POCKETCAM_LINE_MAX];
    size_t line_len;
    bool overlong;

    /* deadlines, 0 when not waiting */
    int64_t hello_by;
    int64_t open_by;
    int64_t silence_by;
    int64_t capture_by;
    int64_t reply_by;
    bool streaming;

    /* the newest preview picture not taken yet */
    int frame_slot;       /* -1: none */
    uint32_t frame_w;
    uint32_t frame_h;
    bool frame_queued;    /* an EV_FRAME for it is in the queue */
    int review_slot;
    uint32_t review_w;
    uint32_t review_h;

    struct camera_event queue[CAMERA_EVENT_QUEUE];
    int q_head;
    int q_count;
    unsigned dropped;     /* events lost to a full queue, for tests and logs */
};

void camera_session_init(struct camera_session *s);

/* Start the helper. 0, or -1 with a reason in err (the session stays idle). */
int camera_session_start(struct camera_session *s, const struct camera_session_config *cfg,
                         int64_t now_ms, char *err, size_t errlen);

/* Read what the helper wrote, enforce the deadlines, reap it when it has
 * gone, and hand back one event. 1 with *ev filled, 0 when there is nothing.
 * Never blocks. */
int camera_session_poll(struct camera_session *s, struct camera_event *ev, int64_t now_ms);

/* Commands. Each returns 0, or -1 when there is no helper to send it to. */
/* display_rotation: the display's rotation in degrees (0, 90, 180, 270), from
 * which the helper works out how to turn the sensor's picture upright. */
int camera_session_view(struct camera_session *s, uint32_t w, uint32_t h, int display_rotation);
int camera_session_preview(struct camera_session *s, bool on, int64_t now_ms);
int camera_session_capture(struct camera_session *s, int display_rotation, int64_t now_ms);
int camera_session_delete(struct camera_session *s, const char *name, int64_t now_ms);

/* Copy the newest preview picture into dst (w x h RGB565, tightly packed)
 * and give its slot back. 1 when a picture of exactly that size was copied;
 * 0 when there was none (a picture of another size - sent before a resize -
 * is dropped). */
int camera_session_take_frame(struct camera_session *s, uint16_t *dst, uint32_t w, uint32_t h);
/* The same for the review picture of the last capture. */
int camera_session_take_review(struct camera_session *s, uint16_t *dst, uint32_t w, uint32_t h);

/* For a destroyed app or a retry: ask the helper to quit, wait up to
 * grace_ms, then SIGKILL and wait up to CAMERA_KILL_REAP_MS. Idle afterwards
 * whatever happened; blocks at most grace_ms + CAMERA_KILL_REAP_MS. */
void camera_session_abandon(struct camera_session *s, int grace_ms);

bool camera_session_active(const struct camera_session *s);

/* $POCKETOS_CAMERA_HELPER, else /usr/bin/pos-camera. */
const char *camera_session_helper_path(void);
/* $POCKETOS_CAMERA_BACKEND, else the build's default: "v4l2" on the device,
 * "fake" in the simulator (CAMERA_BACKEND_DEFAULT). */
const char *camera_session_backend(void);

/* Parse one event line (no newline). 1 when it is a known, well-formed
 * event. Exposed for the test. */
int camera_session_parse_line(const char *line, struct camera_event *ev);

#endif
