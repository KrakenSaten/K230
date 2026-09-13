/*
 * Wave's link to its audio helper: one pos-wave child process per send or
 * listen, driven without ever blocking the LVGL thread.
 *
 * Why a child process and not a thread or an in-shell stream. PocketOS
 * first-party code has no threads (KEYBOARD_DRIVER_DESIGN_2026-09-12 §3) and
 * apps do not touch hardware (ui/shell/app.h, ADR-002). The helper owns the
 * sound device and the modem for exactly as long as one operation lasts, so:
 *
 *   - nothing on the LVGL thread opens, reads or writes audio, or decodes;
 *   - a helper that hangs in a driver is killed, and the kernel closes its
 *     PCM, instead of freezing the shell;
 *   - if the shell dies, the helper gets SIGTERM (PR_SET_PDEATHSIG), cleans
 *     up and exits: no audio outlives the app, and there is no background
 *     microphone to forget about;
 *   - the helper is the same binary the bench uses (pos-wave), so the first
 *     hardware test and the app exercise one code path.
 *
 * The text to send goes over the helper's stdin, not argv, so it never shows
 * in a process list. The helper answers with one event per line on stdout
 * (tools/wave/pos_wave.c, "EVENTS"). stdin and stdout are one socketpair, so
 * the shell writes with MSG_NOSIGNAL and a helper that has already died
 * cannot take the shell down with SIGPIPE.
 *
 * Every wait is bounded: wave_session_poll() never blocks, a stop escalates
 * from SIGTERM to SIGKILL after WAVE_STOP_GRACE_MS, and wave_session_abandon()
 * (for an app being destroyed) waits at most its grace plus
 * WAVE_KILL_REAP_MS. Pure C, no LVGL, clock passed in: tested on a host with
 * a scripted fake helper (tests/wave_session_test.c).
 *
 * RECOVERY. A helper that dies by a signal - the SIGKILL this session sends
 * after the grace, a crash, the OOM killer - never ran its cleanup, so it may
 * have left the route switched and the amplifier enabled. The session is the
 * process that sees that happen, so it starts `helper recover` right away
 * (pocketaudio.h, "Recovery"). That process is detached (double fork, its own
 * session, no death signal): it finishes even if the shell is on its way out,
 * init reaps it, and nothing here waits for it. A helper that exits normally,
 * with any exit code, has cleaned up itself.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETWAVE_SESSION_H
#define POCKETWAVE_SESSION_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* One event line from the helper, terminator included. Longer lines are
 * discarded whole and reported as a protocol error. */
#define WAVE_LINE_MAX 512
/* Events queued between polls. The helper's output is a handful of lines per
 * operation plus a level every 250 ms; a full queue drops levels first. */
#define WAVE_EVENT_QUEUE 16
/* Received text, in bytes, after hex decoding (matches WAVE_MAX_PAYLOAD). */
#define WAVE_EVENT_TEXT_MAX 160
/* From SIGTERM to SIGKILL. The helper's own worst case is one audio wait
 * (POCKETAUDIO_MAX_WAIT_MS, 200 ms) plus closing the device. */
#define WAVE_STOP_GRACE_MS 1000
/* After SIGKILL, how long abandon() waits to reap. */
#define WAVE_KILL_REAP_MS 200

enum wave_session_state {
    WAVE_SESSION_IDLE,      /* no helper */
    WAVE_SESSION_RUNNING,   /* helper alive, operation in progress */
    WAVE_SESSION_STOPPING   /* SIGTERM sent, waiting for it to leave */
};

enum wave_event_kind {
    WAVE_EV_READY,     /* helper started; text = board name */
    WAVE_EV_SENDING,   /* playback started; value = expected duration in ms */
    WAVE_EV_LISTENING, /* microphone open */
    WAVE_EV_LEVEL,     /* value = microphone peak, 0..100 % of full scale */
    WAVE_EV_RECEIVED,  /* text/len = a decoded message */
    WAVE_EV_MISSED,    /* a transmission was heard and could not be decoded */
    WAVE_EV_SENT,      /* playback finished */
    WAVE_EV_STOPPED,   /* the helper acknowledged a stop */
    WAVE_EV_ERROR,     /* text = "<code> <message>" */
    WAVE_EV_EXITED     /* helper gone; value = exit code, or 128 + signal */
};

struct wave_event {
    enum wave_event_kind kind;
    int value;
    size_t len;
    char text[WAVE_EVENT_TEXT_MAX + 1];
};

/* The longest helper path a session accepts. */
#define WAVE_HELPER_PATH_MAX 256

struct wave_session {
    enum wave_session_state state;
    pid_t pid;
    char helper[WAVE_HELPER_PATH_MAX]; /* the running helper, for its recovery */
    unsigned recoveries;               /* recover runs started, for tests and logs */
    int fd;                           /* our end of the socketpair, or -1 */
    char line[WAVE_LINE_MAX];
    size_t line_len;
    int overlong;                     /* discarding until the next newline */
    int eof;
    int64_t stop_deadline_ms;         /* when STOPPING escalates to SIGKILL */
    int killed;
    struct wave_event queue[WAVE_EVENT_QUEUE];
    int q_head;
    int q_count;
};

void wave_session_init(struct wave_session *s);

/* Start `helper send --events --protocol <protocol> --volume <volume>` with
 * text on its stdin. 0, or -1 with a reason in err (the session stays IDLE). */
int wave_session_start_send(struct wave_session *s, const char *helper, const char *protocol,
                            int volume, const char *text, size_t len, char *err, size_t errlen);

/* Start `helper listen --events --seconds <seconds>`. */
int wave_session_start_listen(struct wave_session *s, const char *helper, int seconds, char *err,
                              size_t errlen);

/* Read whatever the helper has written, reap it if it has gone, escalate a
 * stop whose grace has run out, and hand back one event. Returns 1 with *ev
 * filled, 0 when there is nothing (yet). Never blocks. */
int wave_session_poll(struct wave_session *s, struct wave_event *ev, int64_t now_ms);

/* Ask the helper to stop (SIGTERM). The EXITED event follows from poll(). */
void wave_session_stop(struct wave_session *s, int64_t now_ms);

/* For a destroyed app: SIGTERM, wait up to grace_ms for it to leave, then
 * SIGKILL and wait up to WAVE_KILL_REAP_MS. The session is IDLE afterwards
 * whatever happened. Blocks for at most grace_ms + WAVE_KILL_REAP_MS. */
void wave_session_abandon(struct wave_session *s, int grace_ms);

int wave_session_active(const struct wave_session *s);

/* The helper to run: $POCKETOS_WAVE_HELPER, else /usr/bin/pos-wave. */
const char *wave_session_helper_path(void);

/* Parse one event line (no newline). 1 when it is a known event. Exposed for
 * the test. */
int wave_session_parse_line(const char *line, struct wave_event *ev);

#endif
