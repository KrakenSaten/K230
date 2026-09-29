/*
 * The Video app's link to its helper: one pos-video process per played file,
 * driven without ever blocking the LVGL thread. The shape is Camera's
 * (apps/camera/camera_session.h), kept separate because an app does not reach
 * into another app's internals.
 *
 * Why a helper process (docs/decisions/ADR-012-video-playback.md): apps do
 * not touch hardware (ADR-002), and a video file is untrusted input to a
 * large decoder library and a vendor hardware decoder. In a child process:
 *
 *   - nothing on the LVGL thread reads a file, decodes, scales or converts;
 *   - a damaged file that crashes the decoder crashes the helper, not the
 *     shell; one stuck in the hardware decoder is killed by the watchdog
 *     below, and the kernel releases the decoder and the sound card;
 *   - if the shell dies, or restarts itself to rotate, the helper gets
 *     SIGTERM (PR_SET_PDEATHSIG) and closes the sound card on its way out;
 *   - the bench tool and the app are one binary and one code path.
 *
 * Pictures come through shared memory, never through the socket, and never as
 * a pointer the app keeps: video_session_take_frame() copies the newest
 * picture into the caller's buffer and hands the slot straight back. There
 * are no callbacks.
 *
 * WATCHDOG. Every wait on the helper has a deadline, checked by poll(): the
 * first line (VIDEO_HELLO_MS), an open and its first picture (VIDEO_OPEN_MS),
 * the answer to play, pause, stop and seek (VIDEO_REPLY_MS), and silence while
 * playing (VIDEO_SILENCE_MS - a playing helper says `pos` every
 * VIDEO_POS_EVERY_MS). A missed deadline kills the helper and ends the
 * session with VIDEO_EXIT_HUNG. A helper that ended by a signal may have left
 * the sound card's route and amplifier switched: `pos-video recover` is
 * started for it, detached, as the Recorder does for pos-record.
 *
 * Pure C, no LVGL, clock passed in: tested on a host against the real helper
 * with the fake backend (tests/video_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_VIDEO_SESSION_H
#define POCKETOS_VIDEO_SESSION_H

#include "video_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define VIDEO_HELLO_MS 3000
/* Opening a file: the container read, the hardware decoder opened, the first
 * picture decoded. */
#define VIDEO_OPEN_MS 10000
/* A seek reopens the hardware decoder and decodes up to one key interval. */
#define VIDEO_REPLY_MS 5000
#define VIDEO_SILENCE_MS 4000
#define VIDEO_KILL_REAP_MS 200
#define VIDEO_EVENT_QUEUE 32
#define VIDEO_EVENT_TEXT_MAX 128
#define VIDEO_WORD_MAX 16
#define VIDEO_HELPER_PATH_MAX 256

enum video_ev_kind {
    VIDEO_EV_OPENED,   /* ms = duration, w x h = stored size, fps_x100, word = audio, codec */
    VIDEO_EV_OPENFAIL, /* word = reason, text */
    VIDEO_EV_FRAME,    /* a picture is waiting: video_session_take_frame(); ms = its time */
    VIDEO_EV_STATE,    /* value = enum video_play, ms = position */
    VIDEO_EV_POS,      /* ms */
    VIDEO_EV_SEEKED,   /* ms */
    VIDEO_EV_AUDIO,    /* word: the sound changed while playing */
    VIDEO_EV_STATS,    /* stats */
    VIDEO_EV_ERROR,    /* word = decode | device | io, text */
    VIDEO_EV_EXITED    /* reason = enum video_exit; value = exit code, or 128 + signal */
};

enum video_play {
    VIDEO_PLAY_PLAYING,
    VIDEO_PLAY_PAUSED,
    VIDEO_PLAY_STOPPED,
    VIDEO_PLAY_ENDED
};

enum video_exit {
    VIDEO_EXIT_NORMAL,   /* it left by itself (after quit, or an error before it started) */
    VIDEO_EXIT_HUNG,     /* a deadline passed and it was killed */
    VIDEO_EXIT_PROTOCOL, /* it said something impossible and was killed */
    VIDEO_EXIT_CRASHED   /* a signal nobody here sent */
};

struct video_stats {
    uint32_t fps_x10;
    uint64_t shown;
    uint64_t dropped;
    uint64_t late;
    uint32_t cpu_pct;
    uint32_t rss_kb;
    uint32_t xruns;
};

struct video_event {
    enum video_ev_kind kind;
    int64_t ms;
    uint32_t w;
    uint32_t h;
    uint32_t fps_x100;
    int value;
    int reason;
    char word[VIDEO_WORD_MAX];
    char codec[VIDEO_WORD_MAX];
    char text[VIDEO_EVENT_TEXT_MAX];
    struct video_stats stats;
};

struct video_session_config {
    const char *helper;   /* NULL: video_session_helper_path() */
    const char *backend;  /* NULL: video_session_backend() */
    int volume_percent;   /* 0: no sound */
};

struct video_session {
    pid_t pid;
    int fd;               /* our end of the socketpair, or -1 */
    int shm_fd;
    const uint8_t *shm;   /* read-only mapping, or NULL */
    bool running;
    bool eof;
    bool killed;
    enum video_exit exit_reason;
    char helper[VIDEO_HELPER_PATH_MAX];
    char line[VIDEO_LINE_MAX];
    size_t line_len;
    bool overlong;
    unsigned recoveries;  /* detached `recover` runs started */

    /* deadlines, 0 when not waiting */
    int64_t hello_by;
    int64_t open_by;      /* open sent: opened/openfail */
    int64_t picture_by;   /* opened: the first picture */
    int64_t reply_by;
    int64_t silence_by;
    bool playing;

    /* the newest picture not taken yet */
    int frame_slot;       /* -1: none */
    uint32_t frame_w;
    uint32_t frame_h;
    int64_t frame_ms;
    bool frame_queued;    /* an EV_FRAME for it is in the queue */

    struct video_event queue[VIDEO_EVENT_QUEUE];
    int q_head;
    int q_count;
    unsigned dropped;     /* events lost to a full queue */
};

void video_session_init(struct video_session *s);

/* Start the helper. 0, or -1 with a reason in err (the session stays idle). */
int video_session_start(struct video_session *s, const struct video_session_config *cfg,
                        int64_t now_ms, char *err, size_t errlen);

/* Read what the helper wrote, enforce the deadlines, reap it when it has
 * gone, and hand back one event. 1 with *ev filled, 0 when there is nothing.
 * Never blocks. */
int video_session_poll(struct video_session *s, struct video_event *ev, int64_t now_ms);

/* Commands. Each returns 0, or -1 when there is no helper to send it to or
 * the argument is not one the helper could take. */
int video_session_view(struct video_session *s, uint32_t w, uint32_t h);
int video_session_open(struct video_session *s, const char *path, int64_t now_ms);
int video_session_play(struct video_session *s, int64_t now_ms);
int video_session_pause(struct video_session *s, int64_t now_ms);
int video_session_stop(struct video_session *s, int64_t now_ms);
int video_session_seek(struct video_session *s, int64_t ms, int64_t now_ms);

/* Copy the newest picture into dst (room for max_pixels RGB565 pixels,
 * tightly packed) and give its slot back. 1 with *w, *h set when copied; 0
 * when there was none or it did not fit (the slot is given back either way). */
int video_session_take_frame(struct video_session *s, uint16_t *dst, size_t max_pixels, uint32_t *w,
                             uint32_t *h);

/* For a destroyed app or a new file: quit, wait up to grace_ms, then SIGKILL
 * and wait up to VIDEO_KILL_REAP_MS. Idle afterwards whatever happened;
 * blocks at most grace_ms + VIDEO_KILL_REAP_MS. */
void video_session_abandon(struct video_session *s, int grace_ms);

bool video_session_active(const struct video_session *s);

/* $POCKETOS_VIDEO_HELPER, else /usr/bin/pos-video. */
const char *video_session_helper_path(void);
/* $POCKETOS_VIDEO_BACKEND, else the build's default: "ffmpeg" on the device,
 * "fake" in the simulator (VIDEO_BACKEND_DEFAULT). */
const char *video_session_backend(void);

/* Parse one event line (no newline). 1 when it is a known, well-formed
 * event. Exposed for the test. */
int video_session_parse_line(const char *line, struct video_event *ev);

#endif
