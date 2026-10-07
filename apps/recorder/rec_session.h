/*
 * The Recorder's link to its audio helper: one pos-record child process per
 * recording, playback or repair, driven without ever blocking the LVGL
 * thread. The same shape as Wave's (apps/wave/wave_session.h), kept separate
 * because an app does not reach into another app's internals; a shared
 * helper-process client is a follow-up (docs/apps/RECORDER.md).
 *
 * The helper's stdin and stdout are one socketpair: events come back one per
 * line (rec_protocol.h), commands (pause, resume, stop) go out one per line,
 * always with MSG_NOSIGNAL | MSG_DONTWAIT, so a dead or slow helper can never
 * block or kill the shell. The helper gets SIGTERM if the shell dies
 * (PR_SET_PDEATHSIG) and treats a closed socket as a stop, so a recording is
 * finalized, not abandoned, when its app goes away.
 *
 * Every wait is bounded: rec_session_poll() never blocks; a stop sends "stop"
 * and SIGTERM and escalates to SIGKILL after REC_STOP_GRACE_MS (a recording
 * that is killed while finalizing is left as a .part, which the next repair
 * saves); rec_session_abandon() blocks for at most its grace plus
 * REC_KILL_REAP_MS. A helper that dies by a signal did not restore the audio
 * route, so `helper recover` is started detached, as Wave does.
 *
 * Bounded queue: REC_EVENT_QUEUE events between polls. When it is full a
 * level or progress reading is the first thing dropped - they are replaced
 * every 100 ms anyway - and a state event is never dropped for one.
 *
 * Pure C, no LVGL, clock passed in: tested with a scripted fake helper and
 * with the real pos-record over the file-backed fake sound card
 * (tests/rec_session_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETREC_SESSION_H
#define POCKETREC_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define REC_LINE_MAX 320
#define REC_EVENT_QUEUE 16
#define REC_EVENT_TEXT_MAX 256
/* From stop to SIGKILL while polling. Finishing a recording is an fsync. */
#define REC_STOP_GRACE_MS 5000
/* After SIGKILL, how long abandon() waits to reap. */
#define REC_KILL_REAP_MS 200
#define REC_HELPER_PATH_MAX 256

enum rec_session_state {
    REC_SESSION_IDLE,     /* no helper */
    REC_SESSION_RUNNING,  /* helper alive */
    REC_SESSION_STOPPING  /* stop sent, waiting for it to leave */
};

enum rec_event_kind {
    REC_EV_READY,     /* text = board */
    REC_EV_RECOVERED,
    REC_EV_RECORDING, /* a = rate, text = name */
    REC_EV_LEVEL,     /* a = peak, b = rms (0..32767) */
    REC_EV_PROGRESS,  /* a = ms, b = bytes */
    REC_EV_PAUSED,
    REC_EV_RESUMED,
    REC_EV_LIMIT,     /* text = space | length | time */
    REC_EV_SAVED,     /* a = ms, b = bytes, c = gaps, text = name */
    REC_EV_EMPTY,
    REC_EV_KEPT,      /* text = name */
    REC_EV_PLAYING,   /* a = total ms, b = rate, c = channels */
    REC_EV_PLAYED,
    REC_EV_STOPPED,
    REC_EV_REPAIRED,  /* text = name */
    REC_EV_DAMAGED,   /* text = name */
    REC_EV_ERROR,     /* text = "<code> <message>" */
    REC_EV_EXITED     /* c = exit code, or 128 + signal */
};

struct rec_event {
    enum rec_event_kind kind;
    int64_t a;
    int64_t b;
    int c;
    char text[REC_EVENT_TEXT_MAX];
};

struct rec_session {
    enum rec_session_state state;
    pid_t pid;
    char helper[REC_HELPER_PATH_MAX];
    unsigned recoveries;       /* detached recover runs started */
    unsigned dropped;          /* level/progress readings dropped by a full queue */
    int fd;
    char line[REC_LINE_MAX];
    size_t line_len;
    int overlong;
    int eof;
    int64_t stop_deadline_ms;
    int killed;
    struct rec_event queue[REC_EVENT_QUEUE];
    int q_head;
    int q_count;
};

void rec_session_init(struct rec_session *s);

/* Start `helper record --events --rate <rate> <path>`. 0, or -1 with a
 * reason (the session stays IDLE). */
int rec_session_start_record(struct rec_session *s, const char *helper, int rate, const char *path,
                             char *err, size_t errlen);
/* Start `helper play --events [--start-ms MS] [--volume-percent L] <path>`. */
int rec_session_start_play(struct rec_session *s, const char *helper, const char *path,
                           int64_t start_ms, int volume_percent, char *err, size_t errlen);
/* Start `helper recover --events --dir <dir>`. */
int rec_session_start_recover(struct rec_session *s, const char *helper, const char *dir, char *err,
                              size_t errlen);

/* "pause" or "resume" to a running helper. 0, or -1 when there is none or
 * the line could not be sent now. */
int rec_session_command(struct rec_session *s, const char *cmd);

/* Read what the helper wrote, reap it if it has gone, escalate a stop whose
 * grace ran out, and hand back one event. 1 with *ev filled, 0 when there is
 * nothing. Never blocks. */
int rec_session_poll(struct rec_session *s, struct rec_event *ev, int64_t now_ms);

/* "stop" and SIGTERM. The EXITED event follows from poll(). Repeating it is
 * harmless. */
void rec_session_stop(struct rec_session *s, int64_t now_ms);

/* For a destroyed app: stop, wait up to grace_ms, then SIGKILL and wait up
 * to REC_KILL_REAP_MS. IDLE afterwards whatever happened. */
void rec_session_abandon(struct rec_session *s, int grace_ms);

int rec_session_active(const struct rec_session *s);

/* $POCKETOS_RECORD_HELPER, else /usr/bin/pos-record. */
const char *rec_session_helper_path(void);

/* One event line (no newline). 1 when it is a known, well-formed event. */
int rec_session_parse_line(const char *line, struct rec_event *ev);

#endif
