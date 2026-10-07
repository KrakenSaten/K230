/*
 * The MP3 app's helper process client. See mp3_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "mp3_session.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_DEFAULT "/usr/bin/pos-mp3"
/* Descriptors above stderr the shell may hold without close-on-exec. The
 * child closes them all before exec. */
#define CHILD_FD_SCAN_MAX 1024

void mp3_session_init(struct mp3_session *s)
{
    memset(s, 0, sizeof(*s));
    s->state = MP3_SESSION_IDLE;
    s->pid = -1;
    s->fd = -1;
}

const char *mp3_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_MP3_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

int mp3_session_active(const struct mp3_session *s)
{
    return s && s->state != MP3_SESSION_IDLE;
}

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s", what);
    }
}

/* ---- the queue ---------------------------------------------------------- */

static int droppable(enum mp3_event_kind k)
{
    return k == MP3_EV_PROGRESS;
}

static void push(struct mp3_session *s, const struct mp3_event *ev)
{
    int i;

    if (s->q_count == MP3_EVENT_QUEUE) {
        if (droppable(ev->kind)) {
            s->dropped++;
            return;
        }
        for (i = 0; i < s->q_count; i++) {
            if (droppable(s->queue[(s->q_head + i) % MP3_EVENT_QUEUE].kind)) {
                for (; i < s->q_count - 1; i++) {
                    s->queue[(s->q_head + i) % MP3_EVENT_QUEUE] =
                        s->queue[(s->q_head + i + 1) % MP3_EVENT_QUEUE];
                }
                s->q_count--;
                s->dropped++;
                break;
            }
        }
        if (s->q_count == MP3_EVENT_QUEUE) {
            /* Sixteen state events unread: the oldest goes. The app polls
             * every 50 ms and the helper says a handful per track, so this
             * is a helper gone wrong, and EXITED still follows. */
            s->q_head = (s->q_head + 1) % MP3_EVENT_QUEUE;
            s->q_count--;
        }
    }
    s->queue[(s->q_head + s->q_count) % MP3_EVENT_QUEUE] = *ev;
    s->q_count++;
}

static int pop(struct mp3_session *s, struct mp3_event *ev)
{
    if (s->q_count == 0) {
        return 0;
    }
    *ev = s->queue[s->q_head];
    s->q_head = (s->q_head + 1) % MP3_EVENT_QUEUE;
    s->q_count--;
    return 1;
}

/* ---- parsing ------------------------------------------------------------ */

static const char *after(const char *line, const char *word)
{
    size_t n = strlen(word);

    if (strncmp(line, word, n) != 0) {
        return NULL;
    }
    if (line[n] == '\0') {
        return line + n;
    }
    return line[n] == ' ' ? line + n + 1 : NULL;
}

/* A non-negative decimal of at most 15 digits, then a space or the end. */
static const char *number(const char *p, int64_t *out)
{
    int64_t v = 0;
    int n = 0;

    if (!p) {
        return NULL;
    }
    for (; *p >= '0' && *p <= '9'; p++, n++) {
        if (n >= 15) {
            return NULL;
        }
        v = v * 10 + (*p - '0');
    }
    if (n == 0 || (*p != '\0' && *p != ' ')) {
        return NULL;
    }
    *out = v;
    return *p == ' ' ? p + 1 : p;
}

/* Tag text: not empty, no control characters (UTF-8 bytes are fine). */
static int text_ok(const char *p)
{
    size_t n = strlen(p);
    size_t i;

    if (n == 0 || n >= MP3_EVENT_TEXT_MAX) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if ((unsigned char)p[i] < 0x20 || p[i] == 0x7f) {
            return 0;
        }
    }
    return 1;
}

/* A codec word: [a-z0-9_]+, short. */
static int word_ok(const char *p)
{
    size_t n = strlen(p);
    size_t i;

    if (n == 0 || n >= 24) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= '0' && p[i] <= '9') || p[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

static int bare(const char *line, const char *word)
{
    const char *a = after(line, word);

    return a && *a == '\0';
}

int mp3_session_parse_line(const char *line, struct mp3_event *ev)
{
    const char *a;
    int64_t c = 0;
    int64_t d = 0;

    memset(ev, 0, sizeof(*ev));
    if ((a = after(line, "ready")) != NULL) {
        ev->kind = MP3_EV_READY;
        snprintf(ev->text, sizeof(ev->text), "%s", a);
        return 1;
    }
    if (bare(line, "recovered")) {
        ev->kind = MP3_EV_RECOVERED;
        return 1;
    }
    if ((a = after(line, "meta")) != NULL) {
        const char *t;

        if ((t = after(a, "title")) != NULL) {
            ev->kind = MP3_EV_TITLE;
        } else if ((t = after(a, "artist")) != NULL) {
            ev->kind = MP3_EV_ARTIST;
        } else {
            return 0;
        }
        if (!text_ok(t)) {
            return 0;
        }
        snprintf(ev->text, sizeof(ev->text), "%s", t);
        return 1;
    }
    if ((a = after(line, "playing")) != NULL) {
        ev->kind = MP3_EV_PLAYING;
        a = number(a, &ev->a);
        a = number(a, &ev->b);
        a = number(a, &c);
        a = number(a, &d);
        if (!a || !word_ok(a) || ev->b > 1 || c < 1 || c > 1000000 || d < 1 || d > 64) {
            return 0;
        }
        ev->c = (int)c;
        ev->d = (int)d;
        snprintf(ev->text, sizeof(ev->text), "%s", a);
        return 1;
    }
    if ((a = after(line, "progress")) != NULL) {
        ev->kind = MP3_EV_PROGRESS;
        a = number(a, &ev->a);
        return a && *a == '\0';
    }
    if (bare(line, "paused")) {
        ev->kind = MP3_EV_PAUSED;
        return 1;
    }
    if (bare(line, "resumed")) {
        ev->kind = MP3_EV_RESUMED;
        return 1;
    }
    if (bare(line, "played")) {
        ev->kind = MP3_EV_PLAYED;
        return 1;
    }
    if (bare(line, "stopped")) {
        ev->kind = MP3_EV_STOPPED;
        return 1;
    }
    if ((a = after(line, "error")) != NULL && *a) {
        ev->kind = MP3_EV_ERROR;
        snprintf(ev->text, sizeof(ev->text), "%s", a);
        return 1;
    }
    return 0;
}

static void feed(struct mp3_session *s, const char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            struct mp3_event ev;

            if (s->overlong) {
                memset(&ev, 0, sizeof(ev));
                ev.kind = MP3_EV_ERROR;
                snprintf(ev.text, sizeof(ev.text), "protocol helper line too long");
                push(s, &ev);
            } else {
                s->line[s->line_len] = '\0';
                /* Unknown or malformed lines are ignored: a newer helper may
                 * say more, and a broken one ends with EXITED anyway. */
                if (mp3_session_parse_line(s->line, &ev)) {
                    push(s, &ev);
                }
            }
            s->line_len = 0;
            s->overlong = 0;
        } else if (s->overlong) {
            continue;
        } else if (s->line_len + 1 >= sizeof(s->line)) {
            s->overlong = 1;
            s->line_len = 0;
        } else {
            s->line[s->line_len++] = c;
        }
    }
}

/* ---- the child ---------------------------------------------------------- */

static void close_inherited(void)
{
    int fd;

    for (fd = 3; fd < CHILD_FD_SCAN_MAX; fd++) {
        close(fd);
    }
}

/* `helper recover`, detached (double fork, own session, no death signal):
 * it completes even while the shell exits, and init reaps it. */
static void recover_detached(struct mp3_session *s)
{
    pid_t mid;
    int status;

    if (!s->helper[0]) {
        return;
    }
    mid = fork();
    if (mid < 0) {
        return;
    }
    if (mid == 0) {
        pid_t pid = fork();

        if (pid == 0) {
            char *argv[] = { s->helper, "recover", NULL };
            sigset_t none;
            int null = open("/dev/null", O_RDWR);

            setsid();
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, NULL);
            signal(SIGPIPE, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            if (null >= 0) {
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
            }
            close_inherited();
            execv(s->helper, argv);
            _exit(127);
        }
        _exit(0);
    }
    while (waitpid(mid, &status, 0) < 0 && errno == EINTR) {
    }
    s->recoveries++;
}

static int spawn(struct mp3_session *s, const char *helper, char *const argv[], char *err,
                 size_t errlen)
{
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (s->state != MP3_SESSION_IDLE) {
        say(err, errlen, "a track is already playing");
        return -1;
    }
    if (!helper || strlen(helper) >= sizeof(s->helper)) {
        say(err, errlen, "helper path too long");
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        say(err, errlen, "cannot create the helper's channel");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        say(err, errlen, "cannot start the helper");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int null;

        /* Leave with the shell; the check closes the race where it died
         * before the prctl. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) {
            _exit(0);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(sv[1], 0) < 0 || dup2(sv[1], 1) < 0) {
            _exit(127);
        }
        null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, 2);
        }
        close_inherited();
        execv(helper, argv);
        {
            static const char line[] = "error audio the player helper could not be started\n";

            if (write(1, line, sizeof(line) - 1) < 0) {
                /* nothing more can be said */
            }
        }
        _exit(127);
    }
    close(sv[1]);
    snprintf(s->helper, sizeof(s->helper), "%s", helper);
    s->fd = sv[0];
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    s->pid = pid;
    s->state = MP3_SESSION_RUNNING;
    s->line_len = 0;
    s->overlong = 0;
    s->eof = 0;
    s->killed = 0;
    s->q_head = 0;
    s->q_count = 0;
    return 0;
}

/* An absolute path: no newline, not too long. */
static int valid_path(const char *path)
{
    return path && path[0] == '/' && strlen(path) < 1024 && !strchr(path, '\n');
}

int mp3_session_start_play(struct mp3_session *s, const char *helper, const char *path,
                           int64_t start_ms, int volume_percent, char *err, size_t errlen)
{
    char ms[24];
    char pct[16];
    char *argv[] = { (char *)helper, "play", "--events", NULL, NULL, NULL, NULL, NULL, NULL };
    int n = 3;

    if (!valid_path(path) || start_ms < 0 || volume_percent < 1 || volume_percent > 100) {
        say(err, errlen, "invalid playback request");
        return -1;
    }
    if (start_ms > 0) {
        snprintf(ms, sizeof(ms), "%lld", (long long)start_ms);
        argv[n++] = "--start-ms";
        argv[n++] = ms;
    }
    snprintf(pct, sizeof(pct), "%d", volume_percent);
    argv[n++] = "--volume-percent";
    argv[n++] = pct;
    argv[n] = (char *)path;
    return spawn(s, helper, argv, err, errlen);
}

static int send_line(struct mp3_session *s, const char *line)
{
    size_t n = strlen(line);
    ssize_t w;

    if (s->fd < 0) {
        return -1;
    }
    /* A few bytes into a socket buffer that the helper drains every 20 ms:
     * never waits, and a dead helper makes it fail instead of SIGPIPE. */
    w = send(s->fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    return w == (ssize_t)n ? 0 : -1;
}

int mp3_session_command(struct mp3_session *s, const char *cmd)
{
    char line[16];

    if (s->state != MP3_SESSION_RUNNING || (strcmp(cmd, "pause") != 0 && strcmp(cmd, "resume") != 0)) {
        return -1;
    }
    snprintf(line, sizeof(line), "%s\n", cmd);
    return send_line(s, line);
}

int mp3_session_seek(struct mp3_session *s, int64_t ms)
{
    char line[40];

    if (s->state != MP3_SESSION_RUNNING || ms < 0) {
        return -1;
    }
    snprintf(line, sizeof(line), "seek %lld\n", (long long)ms);
    return send_line(s, line);
}

int mp3_session_volume(struct mp3_session *s, int percent)
{
    char line[24];

    if (s->state != MP3_SESSION_RUNNING || percent < 1 || percent > 100) {
        return -1;
    }
    snprintf(line, sizeof(line), "volume %d\n", percent);
    return send_line(s, line);
}

/* ---- running ------------------------------------------------------------ */

static void finish(struct mp3_session *s, int status)
{
    struct mp3_event ev;

    if (s->fd >= 0) {
        char buf[512];
        ssize_t n;

        while ((n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            feed(s, buf, (size_t)n);
        }
        close(s->fd);
        s->fd = -1;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = MP3_EV_EXITED;
    if (WIFEXITED(status)) {
        ev.c = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ev.c = 128 + WTERMSIG(status);
        recover_detached(s);
    } else {
        ev.c = 255;
    }
    push(s, &ev);
    s->pid = -1;
    s->state = MP3_SESSION_IDLE;
}

int mp3_session_poll(struct mp3_session *s, struct mp3_event *ev, int64_t now_ms)
{
    if (s->state != MP3_SESSION_IDLE) {
        char buf[512];
        int status;
        pid_t r;

        while (s->fd >= 0 && !s->eof) {
            ssize_t n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);

            if (n > 0) {
                feed(s, buf, (size_t)n);
            } else if (n == 0) {
                s->eof = 1;
            } else if (errno == EINTR) {
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    s->eof = 1;
                }
                break;
            }
        }
        r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) {
            finish(s, status);
        } else if (r < 0 && errno == ECHILD) {
            finish(s, 255 << 8);
        } else if (s->state == MP3_SESSION_STOPPING && !s->killed && now_ms >= s->stop_deadline_ms) {
            kill(s->pid, SIGKILL);
            s->killed = 1;
        }
    }
    return pop(s, ev);
}

void mp3_session_stop(struct mp3_session *s, int64_t now_ms)
{
    if (s->state != MP3_SESSION_RUNNING) {
        return;
    }
    send_line(s, "stop\n");
    kill(s->pid, SIGTERM);
    s->state = MP3_SESSION_STOPPING;
    s->stop_deadline_ms = now_ms + MP3_STOP_GRACE_MS;
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int reap_within(struct mp3_session *s, int ms, int *signaled)
{
    int64_t end = mono_ms() + ms;
    int status;

    for (;;) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);

        if (r == s->pid) {
            *signaled = WIFSIGNALED(status);
            return 1;
        }
        if (r < 0 && errno == ECHILD) {
            return 1;
        }
        if (mono_ms() >= end) {
            return 0;
        }
        {
            struct timespec d = { 0, 10 * 1000000L };

            nanosleep(&d, NULL);
        }
    }
}

void mp3_session_abandon(struct mp3_session *s, int grace_ms)
{
    if (s->state != MP3_SESSION_IDLE && s->pid > 0) {
        int signaled = 0;

        if (!s->killed) {
            send_line(s, "stop\n");
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms, &signaled)) {
            kill(s->pid, SIGKILL);
            reap_within(s, MP3_KILL_REAP_MS, &signaled);
            signaled = 1;
        }
        if (signaled) {
            recover_detached(s);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    {
        unsigned recoveries = s->recoveries;

        mp3_session_init(s);
        s->recoveries = recoveries;
    }
}
