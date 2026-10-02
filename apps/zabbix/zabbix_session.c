/*
 * The Zabbix app's helper client. See zabbix_session.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "zabbix_session.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_DEFAULT "/usr/bin/pos-zabbix"
/* The simulator runs the fake unless told otherwise (ui/shell/CMakeLists.txt);
 * the device talks to the configured server. */
#ifndef ZABBIX_FAKE_DEFAULT
#define ZABBIX_FAKE_DEFAULT NULL
#endif
/* Descriptors above stderr the shell may hold without close-on-exec (the
 * display, input devices, IPC sockets). The child closes them all before
 * exec; this is the highest it looks at. */
#define CHILD_FD_SCAN_MAX 1024

void zabbix_session_init(struct zabbix_session *s)
{
    memset(s, 0, sizeof(*s));
    s->pid = -1;
    s->fd = -1;
    zbx_rx_init(&s->rx);
}

const char *zabbix_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_ZABBIX_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

const char *zabbix_session_default_fake(void)
{
    const char *b = getenv("POCKETOS_ZABBIX_BACKEND");
    const char *f = getenv("POCKETOS_ZABBIX_FAKE");

    if (b && strcmp(b, "fake") == 0) {
        return f && *f ? f : "demo";
    }
    if (b && *b) {
        return NULL; /* "live": the real backend, even in the simulator */
    }
    return ZABBIX_FAKE_DEFAULT;
}

bool zabbix_session_active(const struct zabbix_session *s)
{
    return s && s->running;
}

/* ---- sending ------------------------------------------------------------------ */

static int send_line(struct zabbix_session *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int send_line(struct zabbix_session *s, const char *fmt, ...)
{
    char line[ZBX_LINE_MAX];
    va_list ap;
    int n;

    if (!s->running || s->fd < 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line) - 1) {
        return -1;
    }
    line[n++] = '\n';
    /* A few bytes into a socket the helper reads whenever it is not waiting
     * on the server: this never waits. MSG_NOSIGNAL, so a helper that has
     * just died cannot take the shell down with SIGPIPE. */
    return send(s->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) == n ? 0 : -1;
}

static bool word_ok(const char *w, size_t max)
{
    size_t i;

    if (!w || !*w || strlen(w) >= max) {
        return false;
    }
    for (i = 0; w[i]; i++) {
        if ((unsigned char)w[i] <= ' ' || w[i] == 0x7f) {
            return false;
        }
    }
    return true;
}

int zabbix_session_refresh(struct zabbix_session *s)
{
    return send_line(s, "refresh");
}

int zabbix_session_detail(struct zabbix_session *s, const char *hostid)
{
    if (!hostid || !*hostid) {
        return send_line(s, "detail\t-");
    }
    if (!word_ok(hostid, ZBX_ID_MAX)) {
        return -1;
    }
    return send_line(s, "detail\t%s", hostid);
}

int zabbix_session_scenario(struct zabbix_session *s, const char *name)
{
    if (!word_ok(name, ZBX_TEXT_MAX)) {
        return -1;
    }
    return send_line(s, "scenario\t%s", name);
}

int zabbix_session_settings(struct zabbix_session *s, bool save, const char *url, const char *auth,
                            const char *user, const char *secret)
{
    char line[ZBX_CMD_LINE_MAX];
    size_t n;
    int rc = -1;

    if (!s->running || s->fd < 0 ||
        zbx_proto_cmd_settings(line, sizeof(line), save, url, auth, user, secret) != 0) {
        return -1;
    }
    n = strlen(line);
    /* Under 2 KB into a socket the helper drains whenever it is not waiting
     * on a server: it fits the socket buffer, so this never waits either. */
    if (send(s->fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)n) {
        rc = 0;
    }
    /* The line holds the typed secret, in hex. */
    explicit_bzero(line, sizeof(line));
    return rc;
}

/* ---- the child ---------------------------------------------------------------- */

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

int zabbix_session_start(struct zabbix_session *s, const struct zabbix_session_config *cfg,
                         int64_t now_ms, char *err, size_t errlen)
{
    const char *helper = cfg && cfg->helper ? cfg->helper : zabbix_session_helper_path();
    const char *fake = cfg ? cfg->fake : NULL;
    char *argv[6];
    int argc = 0;
    int sv[2];
    pid_t parent = getpid();
    pid_t pid;

    if (s->running) {
        if (err && errlen) {
            snprintf(err, errlen, "the helper is already running");
        }
        return -1;
    }
    zabbix_session_init(s);
    if (strlen(helper) >= ZABBIX_HELPER_PATH_MAX || (fake && strlen(fake) >= ZABBIX_ARG_MAX)) {
        if (err && errlen) {
            snprintf(err, errlen, "helper arguments too long");
        }
        return -1;
    }
    argv[argc++] = (char *)helper;
    argv[argc++] = "session";
    if (fake && *fake) {
        argv[argc++] = "--fake";
        argv[argc++] = (char *)fake;
    }
    argv[argc] = NULL;

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
        int null;
        int fd;

        /* Leave with the shell: if it dies - or execs itself to rotate -
         * this gets SIGTERM and its connection goes with it. The check after
         * closes the race where the shell died before the prctl. */
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
        _exit(127);
    }
    close(sv[1]);
    s->fd = sv[0];
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    s->pid = pid;
    s->running = true;
    s->hello_by = now_ms + ZABBIX_HELLO_MS;
    s->last_line_ms = now_ms;
    return 0;
}

/* ---- running ------------------------------------------------------------------ */

static void kill_helper(struct zabbix_session *s, enum zabbix_exit why)
{
    if (s->running && s->pid > 0 && !s->killed) {
        kill(s->pid, SIGKILL);
        s->killed = true;
        s->exit_reason = why;
    }
}

static unsigned feed(struct zabbix_session *s, struct zabbix_model *m, const char *buf, size_t n,
                     int64_t now)
{
    unsigned changed = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                kill_helper(s, ZABBIX_EXIT_PROTOCOL);
            } else {
                struct zbx_rx_msg msg;

                s->line[s->line_len] = '\0';
                s->last_line_ms = now;
                switch (zbx_rx_line(&s->rx, s->line, &msg)) {
                case ZBX_RX_HELLO:
                    s->hello_by = 0;
                    if (msg.proto != ZBX_PROTO_VERSION) {
                        kill_helper(s, ZABBIX_EXIT_PROTOCOL);
                        break;
                    }
                    changed |= zabbix_model_apply(m, &msg, &s->rx, now);
                    break;
                case ZBX_RX_BAD:
                    /* A damaged set is dropped by the reader and the last
                     * whole one stays; a helper that keeps sending damage is
                     * not worth keeping. */
                    if (s->rx.bad > 16) {
                        kill_helper(s, ZABBIX_EXIT_PROTOCOL);
                    }
                    break;
                default:
                    if (s->hello_by == 0) {
                        changed |= zabbix_model_apply(m, &msg, &s->rx, now);
                    }
                    break;
                }
            }
            s->line_len = 0;
            s->overlong = false;
        } else if (s->overlong) {
            continue;
        } else if (s->line_len + 1 >= sizeof(s->line)) {
            s->overlong = true;
            s->line_len = 0;
        } else {
            s->line[s->line_len++] = c;
        }
    }
    return changed;
}

static unsigned finish(struct zabbix_session *s, struct zabbix_model *m, int status, int64_t now)
{
    unsigned changed = 0;
    enum zabbix_exit reason;
    int value;

    if (s->fd >= 0) {
        char buf[512];
        ssize_t n;

        /* Anything the helper wrote just before it left. */
        while ((n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            changed |= feed(s, m, buf, (size_t)n, now);
        }
        close(s->fd);
        s->fd = -1;
    }
    if (WIFEXITED(status)) {
        value = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        value = 128 + WTERMSIG(status);
    } else {
        value = 255;
    }
    if (s->killed) {
        reason = s->exit_reason;
    } else if (WIFSIGNALED(status)) {
        reason = ZABBIX_EXIT_CRASHED;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        reason = ZABBIX_EXIT_START; /* exec failed: no helper at that path */
    } else {
        reason = ZABBIX_EXIT_NORMAL;
    }
    zabbix_model_helper_stopped(m, reason, value, now);
    s->pid = -1;
    s->running = false;
    s->hello_by = 0;
    return changed | ZABBIX_CHANGED_EXITED | ZABBIX_CHANGED_STATE;
}

unsigned zabbix_session_poll(struct zabbix_session *s, struct zabbix_model *m, int64_t now_ms)
{
    unsigned changed = 0;
    char buf[2048];
    int status;
    pid_t r;

    if (!s->running) {
        return 0;
    }
    while (s->fd >= 0 && !s->eof) {
        ssize_t n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);

        if (n > 0) {
            changed |= feed(s, m, buf, (size_t)n, now_ms);
        } else if (n == 0) {
            s->eof = true;
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                s->eof = true;
            }
            break;
        }
    }
    if ((s->hello_by && now_ms >= s->hello_by) ||
        (m->busy && now_ms - s->last_line_ms >= ZABBIX_BUSY_SILENCE_MS)) {
        kill_helper(s, ZABBIX_EXIT_HUNG);
    }
    if (s->eof && !s->killed) {
        /* It closed its end: it is leaving, or it will be made to. */
        kill(s->pid, SIGTERM);
    }
    r = waitpid(s->pid, &status, WNOHANG);
    if (r == s->pid) {
        changed |= finish(s, m, status, now_ms);
    } else if (r < 0 && errno == ECHILD) {
        changed |= finish(s, m, 255 << 8, now_ms);
    }
    return changed;
}

/* ---- leaving ------------------------------------------------------------------- */

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* True when the helper is gone within ms. */
static bool reap_within(struct zabbix_session *s, int ms)
{
    int64_t end = mono_ms() + ms;
    int status;

    for (;;) {
        pid_t r = waitpid(s->pid, &status, WNOHANG);

        if (r == s->pid || (r < 0 && errno == ECHILD)) {
            return true;
        }
        if (mono_ms() >= end) {
            return false;
        }
        {
            struct timespec d = { 0, 5 * 1000000L };

            nanosleep(&d, NULL);
        }
    }
}

void zabbix_session_abandon(struct zabbix_session *s, int grace_ms)
{
    if (s->running && s->pid > 0) {
        /* quit ends a helper that is waiting for work; SIGTERM ends one that
         * is in the middle of a request (its loop sees the flag as soon as
         * curl returns, and curl returns on its own timeout at worst). */
        send_line(s, "quit");
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms)) {
            kill(s->pid, SIGKILL);
            /* If even this is not enough the helper is stuck in the kernel;
             * it is left as a zombie rather than holding the LVGL thread. */
            reap_within(s, ZABBIX_KILL_REAP_MS);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    zabbix_session_init(s);
}
