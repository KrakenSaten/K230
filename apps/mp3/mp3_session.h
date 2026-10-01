/*
 * The MP3 app's link to its audio helper: one pos-mp3 child process per
 * track, driven without ever blocking the LVGL thread. The same shape as
 * Recorder's and Wave's (apps/recorder/rec_session.h), kept separate because
 * an app does not reach into another app's internals.
 *
 * The helper's stdin and stdout are one socketpair: events come back one per
 * line (mp3_protocol.h), commands (pause, resume, seek, volume) go out one
 * per line, always with MSG_NOSIGNAL | MSG_DONTWAIT, so a dead or slow
 * helper can never block or kill the shell. The helper gets SIGTERM if the
 * shell dies (PR_SET_PDEATHSIG) and treats a closed socket as a stop.
 *
 * Every wait is bounded: mp3_session_poll() never blocks; a stop sends
 * "stop" and SIGTERM and escalates to SIGKILL after MP3_STOP_GRACE_MS;
 * mp3_session_abandon() blocks for at most its grace plus MP3_KILL_REAP_MS.
 * A helper that dies by a signal did not restore the audio route, so
 * `helper recover` is started detached, as Wave and Recorder do.
 *
 * Bounded queue: MP3_EVENT_QUEUE events between polls. When it is full a
 * progress reading is the first thing dropped - they are replaced every
 * 250 ms anyway - and a state event is never dropped for one.
 *
 * Pure C, no LVGL, clock passed in: tested with a scripted fake helper and
 * with the real pos-mp3 over the file-backed fake sound card
 * (tests/mp3_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETMP3_SESSION_H
#define POCKETMP3_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define MP3_LINE_MAX 320
#define MP3_EVENT_QUEUE 16
#define MP3_EVENT_TEXT_MAX 256
/* From stop to SIGKILL while polling. A stop is one audio wait (200 ms). */
#define MP3_STOP_GRACE_MS 2000
/* After SIGKILL, how long abandon() waits to reap. */
#define MP3_KILL_REAP_MS 200
#define MP3_HELPER_PATH_MAX 256

enum mp3_session_state {
    MP3_SESSION_IDLE,     /* no helper */
    MP3_SESSION_RUNNING,  /* helper alive */
    MP3_SESSION_STOPPING  /* stop sent, waiting for it to leave */
};

enum mp3_event_kind {
    MP3_EV_READY,       /* text = board */
    MP3_EV_RECOVERED,
    MP3_EV_TITLE,       /* text = title */
    MP3_EV_ARTIST,      /* text = artist */
    MP3_EV_PLAYING,     /* a = total ms (0 unknown), b = seekable, c = rate, d = channels, text = codec */
    MP3_EV_PROGRESS,    /* a = ms */
    MP3_EV_PAUSED,
    MP3_EV_RESUMED,
    MP3_EV_PLAYED,
    MP3_EV_STOPPED,
    MP3_EV_ERROR,       /* text = "<code> <message>" */
    MP3_EV_EXITED       /* c = exit code, or 128 + signal */
};

struct mp3_event {
    enum mp3_event_kind kind;
    int64_t a;
    int64_t b;
    int c;
    int d;
    char text[MP3_EVENT_TEXT_MAX];
};

struct mp3_session {
    enum mp3_session_state state;
    pid_t pid;
    char helper[MP3_HELPER_PATH_MAX];
    unsigned recoveries;       /* detached recover runs started */
    unsigned dropped;          /* progress readings dropped by a full queue */
    int fd;
    char line[MP3_LINE_MAX];
    size_t line_len;
    int overlong;
    int eof;
    int64_t stop_deadline_ms;
    int killed;
    struct mp3_event queue[MP3_EVENT_QUEUE];
    int q_head;
    int q_count;
};

void mp3_session_init(struct mp3_session *s);

/* Start `helper play --events [--start-ms MS] [--volume-percent V] <path>`.
 * 0, or -1 with a reason (the session stays IDLE). */
int mp3_session_start_play(struct mp3_session *s, const char *helper, const char *path,
                           int64_t start_ms, int volume_percent, char *err, size_t errlen);

/* "pause" or "resume" to a running helper. 0, or -1 when there is none or
 * the line could not be sent now. */
int mp3_session_command(struct mp3_session *s, const char *cmd);
/* "seek <ms>" and "volume <percent>" (1..100). Same return. */
int mp3_session_seek(struct mp3_session *s, int64_t ms);
int mp3_session_volume(struct mp3_session *s, int percent);

/* Read what the helper wrote, reap it if it has gone, escalate a stop whose
 * grace ran out, and hand back one event. 1 with *ev filled, 0 when there is
 * nothing. Never blocks. */
int mp3_session_poll(struct mp3_session *s, struct mp3_event *ev, int64_t now_ms);

/* "stop" and SIGTERM. The EXITED event follows from poll(). Repeating it is
 * harmless. */
void mp3_session_stop(struct mp3_session *s, int64_t now_ms);

/* For a destroyed app: stop, wait up to grace_ms, then SIGKILL and wait up
 * to MP3_KILL_REAP_MS. IDLE afterwards whatever happened. */
void mp3_session_abandon(struct mp3_session *s, int grace_ms);

int mp3_session_active(const struct mp3_session *s);

/* $POCKETOS_MP3_HELPER, else /usr/bin/pos-mp3. */
const char *mp3_session_helper_path(void);

/* One event line (no newline). 1 when it is a known, well-formed event. */
int mp3_session_parse_line(const char *line, struct mp3_event *ev);

#endif
