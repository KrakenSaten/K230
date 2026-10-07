/*
 * Camera's link to its helper: one pos-camera process for as long as the
 * Camera screen is open, driven without ever blocking the LVGL thread.
 *
 * Why a helper process (docs/decisions/ADR-006-camera-ownership.md):
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
 * THE LIBRARY. The same client runs `pos-camera library` for the gallery
 * (cfg.library): the same process boundary, shared memory, slot ownership and
 * watchdog, and no camera. It lists the photos, draws them into the three
 * picture slots at the size asked for, exports and deletes; every file the
 * gallery touches is touched by that helper, never on the LVGL thread. Only
 * one list, export or delete is outstanding at a time (the gallery's state
 * machine waits for each answer); up to three pictures may be.
 *
 * Pure C, no LVGL, clock passed in: tested on a host against the real helper
 * with the fake backend (tests/camera_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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
/* The library: a picture (a JPEG decode, scaled in the DCT, unmeasured on the
 * C908), a listing of up to POCKETCAM_LIBRARY_MAX files, a synced copy. */
#define CAMERA_DECODE_MS 8000
#define CAMERA_LIST_MS 5000
#define CAMERA_EXPORT_MS 15000
#define CAMERA_PICTURE_SLOTS POCKETCAM_PREVIEW_SLOTS
/* The longest list the helper sends (POCKETCAM_LIBRARY_MAX; the lint keeps
 * the two equal). */
#define CAMERA_LIBRARY_MAX 1000
/* A date in a photo name is believed from this year (POCKETCAM_EXIF_YEAR_MIN,
 * which the helper applies to EXIF dates; the lint keeps the two equal). */
#define CAMERA_DATE_YEAR_MIN 1995
/* abandon(): from quit to SIGKILL, and after SIGKILL how long to reap. */
#define CAMERA_KILL_REAP_MS 200
#define CAMERA_EVENT_QUEUE 32
#define CAMERA_EVENT_TEXT_MAX 96
#define CAMERA_NAME_MAX 48
#define CAMERA_HELPER_PATH_MAX 256
#define CAMERA_ARG_MAX 256
#define CAMERA_PATH_TEXT_MAX 160
#define CAMERA_TAKEN_MAX 20

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
    CAMERA_EV_EXITED,    /* reason; value = exit code, or 128 + signal */
    /* the library */
    CAMERA_EV_LISTED,    /* value = names waiting (camera_session_take_list), bytes = total */
    CAMERA_EV_LISTFAIL,  /* text */
    CAMERA_EV_IMAGE,     /* value = slot, w x h, name, image, text = description */
    CAMERA_EV_IMGFAIL,   /* value = slot, name, reason = enum camera_imgfail, text */
    CAMERA_EV_EXPORTED,  /* name, value = 1 when it was already there, path */
    CAMERA_EV_EXPFAIL    /* name, reason = enum camera_expfail, text */
};

enum camera_imgfail {
    CAMERA_IMGFAIL_MISSING,
    CAMERA_IMGFAIL_CORRUPT,
    CAMERA_IMGFAIL_UNSUPPORTED,
    CAMERA_IMGFAIL_TOOLARGE,
    CAMERA_IMGFAIL_IO
};

enum camera_expfail {
    CAMERA_EXPFAIL_EXISTS,
    CAMERA_EXPFAIL_NOSPACE,
    CAMERA_EXPFAIL_MISSING,
    CAMERA_EXPFAIL_IO
};

/* What the library helper says about a photo it drew. */
struct camera_image_meta {
    uint32_t shown_w;     /* the photo upright, at full size */
    uint32_t shown_h;
    uint64_t bytes;       /* the file */
    int64_t mtime;
    bool damaged;         /* drawn, but the file is damaged or cut short */
    bool jpeg;            /* else PPM */
    char taken[CAMERA_TAKEN_MAX]; /* "YYYY:MM:DD HH:MM:SS" from the file, or "" */
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
    struct camera_image_meta image; /* CAMERA_EV_IMAGE */
    char path[CAMERA_PATH_TEXT_MAX]; /* CAMERA_EV_EXPORTED */
};

struct camera_session_config {
    const char *helper;   /* NULL: camera_session_helper_path() */
    const char *backend;  /* NULL: camera_session_backend() */
    const char *fake;     /* the fake backend's script, or NULL */
    const char *dir;      /* the photo folder, or NULL for the helper's default */
    bool library;         /* `pos-camera library`: the gallery, no camera */
    const char *export_dir; /* library: where exports go, or NULL for the default */
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
    int64_t reply_window; /* what reply_by was set to: a queued reply keeps it (handle()) */
    bool streaming;

    /* the newest preview picture not taken yet */
    int frame_slot;       /* -1: none */
    uint32_t frame_w;
    uint32_t frame_h;
    bool frame_queued;    /* an EV_FRAME for it is in the queue */
    int review_slot;
    uint32_t review_w;
    uint32_t review_h;

    /* the library */
    bool library;
    int64_t decode_by;
    bool picture_asked[CAMERA_PICTURE_SLOTS];   /* requested, not answered */
    bool picture_ready[CAMERA_PICTURE_SLOTS];   /* answered, not taken */
    uint32_t picture_w[CAMERA_PICTURE_SLOTS];
    uint32_t picture_h[CAMERA_PICTURE_SLOTS];
    bool list_ready;
    int list_count;

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

/* ---- the library (cfg.library) --------------------------------------------- */

/* Ask for the list of photos; answered by LISTED or LISTFAIL. */
int camera_session_list(struct camera_session *s, int64_t now_ms);
/* Copy up to max names of the waiting list, newest first, into names and give
 * its slot back. Returns how many; 0 when no list is waiting. A name that is
 * not one the helper could have sent ends the list there. */
int camera_session_take_list(struct camera_session *s, char (*names)[CAMERA_NAME_MAX], int max);
/* Ask for photo name drawn into a w x h box (cover: filled and cut; else the
 * whole photo, fitted). Returns the slot it will come back in - IMAGE or
 * IMGFAIL with that slot - or -1 when all three are in use, the arguments are
 * bad or there is no helper. */
int camera_session_request_picture(struct camera_session *s, const char *name, uint32_t w,
                                   uint32_t h, bool cover, int64_t now_ms);
/* Copy the picture that came back in slot into dst (at least max_w x max_h
 * pixels; it arrives tightly packed at *w x *h) and give the slot back.
 * 1 when copied; 0 when there was none or it did not fit (the slot is given
 * back either way). dst NULL only gives it back. */
int camera_session_take_picture(struct camera_session *s, int slot, uint16_t *dst, uint32_t max_w,
                                uint32_t max_h, uint32_t *w, uint32_t *h);
/* How many picture slots are free for a request. */
int camera_session_pictures_free(const struct camera_session *s);
/* Copy photo name to the Files export folder; EXPORTED or EXPFAIL. */
int camera_session_export(struct camera_session *s, const char *name, int64_t now_ms);

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
