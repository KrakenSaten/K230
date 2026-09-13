/*
 * Wave's helper process client. See wave_session.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "wave_session.h"

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

#define HELPER_DEFAULT "/usr/bin/pos-wave"
/* Descriptors above stderr the shell may hold without close-on-exec (the
 * display, input devices, IPC sockets). The child closes them all before
 * exec; this is the highest it looks at. */
#define CHILD_FD_SCAN_MAX 1024

void wave_session_init(struct wave_session *s)
{
    memset(s, 0, sizeof(*s));
    s->state = WAVE_SESSION_IDLE;
    s->pid = -1;
    s->fd = -1;
}

const char *wave_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_WAVE_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

int wave_session_active(const struct wave_session *s)
{
    return s && s->state != WAVE_SESSION_IDLE;
}

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

/* ---- the event queue ---------------------------------------------------- */

static void push(struct wave_session *s, const struct wave_event *ev)
{
    int i;

    if (s->q_count == WAVE_EVENT_QUEUE) {
        if (ev->kind == WAVE_EV_LEVEL) {
            return; /* a meter reading is the first thing worth losing */
        }
        /* Make room by dropping the oldest level reading, or failing that
         * the oldest event. */
        for (i = 0; i < s->q_count; i++) {
            int at = (s->q_head + i) % WAVE_EVENT_QUEUE;

            if (s->queue[at].kind == WAVE_EV_LEVEL) {
                for (; i < s->q_count - 1; i++) {
                    int from = (s->q_head + i + 1) % WAVE_EVENT_QUEUE;
                    int to = (s->q_head + i) % WAVE_EVENT_QUEUE;

                    s->queue[to] = s->queue[from];
                }
                s->q_count--;
                break;
            }
        }
        if (s->q_count == WAVE_EVENT_QUEUE) {
            s->q_head = (s->q_head + 1) % WAVE_EVENT_QUEUE;
            s->q_count--;
        }
    }
    s->queue[(s->q_head + s->q_count) % WAVE_EVENT_QUEUE] = *ev;
    s->q_count++;
}

static int pop(struct wave_session *s, struct wave_event *ev)
{
    if (s->q_count == 0) {
        return 0;
    }
    *ev = s->queue[s->q_head];
    s->q_head = (s->q_head + 1) % WAVE_EVENT_QUEUE;
    s->q_count--;
    return 1;
}

/* ---- parsing ------------------------------------------------------------ */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* A non-negative decimal of at most 9 digits and nothing else. */
static int parse_uint(const char *p, int *out)
{
    int v = 0;
    int n = 0;

    for (; *p; p++, n++) {
        if (*p < '0' || *p > '9' || n >= 9) {
            return 0;
        }
        v = v * 10 + (*p - '0');
    }
    if (n == 0) {
        return 0;
    }
    *out = v;
    return 1;
}

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

int wave_session_parse_line(const char *line, struct wave_event *ev)
{
    const char *arg;

    memset(ev, 0, sizeof(*ev));
    if ((arg = after(line, "ready")) != NULL) {
        ev->kind = WAVE_EV_READY;
        snprintf(ev->text, sizeof(ev->text), "%s", arg);
        ev->len = strlen(ev->text);
        return 1;
    }
    if ((arg = after(line, "sending")) != NULL) {
        ev->kind = WAVE_EV_SENDING;
        return parse_uint(arg, &ev->value);
    }
    if ((arg = after(line, "listening")) != NULL && *arg == '\0') {
        ev->kind = WAVE_EV_LISTENING;
        return 1;
    }
    if ((arg = after(line, "level")) != NULL) {
        ev->kind = WAVE_EV_LEVEL;
        return parse_uint(arg, &ev->value) && ev->value <= 100;
    }
    if ((arg = after(line, "sent")) != NULL && *arg == '\0') {
        ev->kind = WAVE_EV_SENT;
        return 1;
    }
    if ((arg = after(line, "missed")) != NULL && *arg == '\0') {
        ev->kind = WAVE_EV_MISSED;
        return 1;
    }
    if ((arg = after(line, "stopped")) != NULL && *arg == '\0') {
        ev->kind = WAVE_EV_STOPPED;
        return 1;
    }
    if ((arg = after(line, "received")) != NULL) {
        size_t n = strlen(arg);
        size_t i;

        if (n == 0 || n % 2 != 0 || n / 2 > WAVE_EVENT_TEXT_MAX) {
            return 0;
        }
        for (i = 0; i < n; i += 2) {
            int hi = hexval(arg[i]);
            int lo = hexval(arg[i + 1]);

            if (hi < 0 || lo < 0) {
                return 0;
            }
            ev->text[i / 2] = (char)(hi * 16 + lo);
        }
        ev->len = n / 2;
        ev->text[ev->len] = '\0';
        ev->kind = WAVE_EV_RECEIVED;
        return 1;
    }
    if ((arg = after(line, "error")) != NULL && *arg) {
        ev->kind = WAVE_EV_ERROR;
        snprintf(ev->text, sizeof(ev->text), "%s", arg);
        ev->len = strlen(ev->text);
        return 1;
    }
    return 0;
}

static void feed(struct wave_session *s, const char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                struct wave_event ev;

                memset(&ev, 0, sizeof(ev));
                ev.kind = WAVE_EV_ERROR;
                snprintf(ev.text, sizeof(ev.text), "protocol helper line too long");
                ev.len = strlen(ev.text);
                push(s, &ev);
            } else {
                struct wave_event ev;

                s->line[s->line_len] = '\0';
                /* Unknown lines are ignored: a newer helper may say more. */
                if (wave_session_parse_line(s->line, &ev)) {
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

static int valid_word(const char *w)
{
    size_t n = 0;

    for (; w && *w; w++, n++) {
        if (!((*w >= 'a' && *w <= 'z') || *w == '_' || (*w >= '0' && *w <= '9')) || n >= 24) {
            return 0;
        }
    }
    return w && n > 0;
}

static int spawn(struct wave_session *s, const char *helper, char *const argv[], const char *text,
                 size_t len, char *err, size_t errlen)
{
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (s->state != WAVE_SESSION_IDLE) {
        if (err && errlen) {
            snprintf(err, errlen, "an operation is already running");
        }
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        say(err, errlen, "socketpair");
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        say(err, errlen, "fork");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int fd;
        int null;

        /* Leave with the shell: if it dies, this gets SIGTERM and cleans up
         * the audio device on its way out. The check after closes the race
         * where the shell died before the prctl. */
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
        for (fd = 3; fd < CHILD_FD_SCAN_MAX; fd++) {
            close(fd);
        }
        execv(helper, argv);
        {
            char line[160];
            int n = snprintf(line, sizeof(line), "error exec %s: %s\n", helper, strerror(errno));

            if (n > 0 && write(1, line, (size_t)n) < 0) {
                /* nothing more can be said */
            }
        }
        _exit(127);
    }
    close(sv[1]);
    s->fd = sv[0];
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    s->pid = pid;
    s->state = WAVE_SESSION_RUNNING;
    s->line_len = 0;
    s->overlong = 0;
    s->eof = 0;
    s->killed = 0;
    s->q_head = 0;
    s->q_count = 0;
    if (text && len) {
        size_t off = 0;

        /* At most WAVE_EVENT_TEXT_MAX bytes into an empty socket buffer, so
         * this never waits; a helper that already died makes it fail, and
         * that is reported by the EXITED event rather than here. */
        while (off < len) {
            ssize_t w = send(s->fd, text + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);

            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
    }
    shutdown(s->fd, SHUT_WR);
    return 0;
}

int wave_session_start_send(struct wave_session *s, const char *helper, const char *protocol,
                            int volume, const char *text, size_t len, char *err, size_t errlen)
{
    char vol[16];
    char *argv[] = { (char *)helper, "send", "--events", "--protocol", (char *)protocol,
                     "--volume", vol, NULL };

    if (!helper || !valid_word(protocol) || volume < 1 || volume > 100 || !text || len == 0 ||
        len > WAVE_EVENT_TEXT_MAX) {
        if (err && errlen) {
            snprintf(err, errlen, "invalid send request");
        }
        return -1;
    }
    snprintf(vol, sizeof(vol), "%d", volume);
    return spawn(s, helper, argv, text, len, err, errlen);
}

int wave_session_start_listen(struct wave_session *s, const char *helper, int seconds, char *err,
                              size_t errlen)
{
    char secs[16];
    char *argv[] = { (char *)helper, "listen", "--events", "--seconds", secs, NULL };

    if (!helper || seconds < 1 || seconds > 3600) {
        if (err && errlen) {
            snprintf(err, errlen, "invalid listen request");
        }
        return -1;
    }
    snprintf(secs, sizeof(secs), "%d", seconds);
    return spawn(s, helper, argv, NULL, 0, err, errlen);
}

/* ---- running ------------------------------------------------------------ */

static void finish(struct wave_session *s, int status)
{
    struct wave_event ev;

    if (s->fd >= 0) {
        char buf[256];
        ssize_t n;

        /* Anything the helper wrote just before it left. */
        while ((n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            feed(s, buf, (size_t)n);
        }
        close(s->fd);
        s->fd = -1;
    }
    memset(&ev, 0, sizeof(ev));
    ev.kind = WAVE_EV_EXITED;
    if (WIFEXITED(status)) {
        ev.value = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ev.value = 128 + WTERMSIG(status);
    } else {
        ev.value = 255;
    }
    push(s, &ev);
    s->pid = -1;
    s->state = WAVE_SESSION_IDLE;
}

int wave_session_poll(struct wave_session *s, struct wave_event *ev, int64_t now_ms)
{
    if (s->state != WAVE_SESSION_IDLE) {
        char buf[256];
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
        } else if (s->state == WAVE_SESSION_STOPPING && !s->killed && now_ms >= s->stop_deadline_ms) {
            kill(s->pid, SIGKILL);
            s->killed = 1;
        }
    }
    return pop(s, ev);
}

void wave_session_stop(struct wave_session *s, int64_t now_ms)
{
    if (s->state != WAVE_SESSION_RUNNING) {
        return;
    }
    kill(s->pid, SIGTERM);
    s->state = WAVE_SESSION_STOPPING;
    s->stop_deadline_ms = now_ms + WAVE_STOP_GRACE_MS;
}

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int reap_within(struct wave_session *s, int ms)
{
    int64_t end = mono_ms() + ms;
    int status;

    for (;;) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);

        if (r == s->pid || (r < 0 && errno == ECHILD)) {
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

void wave_session_abandon(struct wave_session *s, int grace_ms)
{
    if (s->state != WAVE_SESSION_IDLE && s->pid > 0) {
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms)) {
            kill(s->pid, SIGKILL);
            /* If even this fails the child is stuck in the kernel; it is
             * left as a zombie rather than holding the LVGL thread. */
            reap_within(s, WAVE_KILL_REAP_MS);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    wave_session_init(s);
}
