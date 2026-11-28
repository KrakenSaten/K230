/*
 * PacketShark's helper client. See psk_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "psk_session.h"

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

#define HELPER_DEFAULT "/usr/bin/pos-capture"
/* The simulator captures synthetic traffic unless told otherwise
 * (ui/shell/CMakeLists.txt); the device captures what the user picks. */
#ifndef PSK_SOURCE_DEFAULT
#define PSK_SOURCE_DEFAULT NULL
#endif
#define CHILD_FD_SCAN_MAX 1024

void psk_session_init(struct psk_session *s)
{
    uint32_t req = s ? s->next_req : 0;

    memset(s, 0, sizeof(*s));
    s->pid = -1;
    s->fd = -1;
    s->next_req = req;
}

const char *psk_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_CAPTURE_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

const char *psk_session_default_source(void)
{
    const char *s = getenv("POCKETOS_CAPTURE_SOURCE");

    if (s) {
        return *s ? s : NULL; /* set but empty: capture the chosen interface */
    }
    return PSK_SOURCE_DEFAULT;
}

bool psk_session_active(const struct psk_session *s)
{
    return s && s->running;
}

/* ---- sending ------------------------------------------------------------------ */

static int send_raw(struct psk_session *s, const char *line, int n)
{
    char buf[PPC_LINE_MAX + 1];

    if (!s->running || s->fd < 0 || n < 0 || n >= PPC_LINE_MAX) {
        return -1;
    }
    memcpy(buf, line, (size_t)n);
    buf[n++] = '\n';
    /* A short line into a socket the helper drains every loop turn: this
     * never waits. MSG_NOSIGNAL, so a helper that has just died cannot take
     * the shell down with SIGPIPE. */
    return send(s->fd, buf, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) == n ? 0 : -1;
}

static int send_word(struct psk_session *s, const char *word)
{
    return send_raw(s, word, (int)strlen(word));
}

int psk_session_capture(struct psk_session *s, const char *iface, enum ppc_keep keep, int promisc,
                        enum ppc_cfilter cf, uint32_t limit_s)
{
    char line[PPC_LINE_MAX];
    int n = ppc_cmd_format_start(line, sizeof(line), iface, keep, promisc, cf, limit_s);

    return n < 0 ? -1 : send_raw(s, line, n);
}

int psk_session_hold(struct psk_session *s, bool on)
{
    char line[32];
    int n = ppc_cmd_format_hold(line, sizeof(line), on);

    return n < 0 ? -1 : send_raw(s, line, n);
}

int psk_session_stop(struct psk_session *s)
{
    return send_word(s, "stop");
}

int psk_session_discard(struct psk_session *s)
{
    return send_word(s, "discard");
}

int psk_session_window(struct psk_session *s, struct psk_model *m, uint32_t seq, uint32_t before,
                       uint32_t after)
{
    char line[96];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_window(line, sizeof(line), req, seq, before, after);

    if (n < 0 || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_req = req;
    return 0;
}

int psk_session_window_at(struct psk_session *s, struct psk_model *m, enum ppc_anchor anchor, uint64_t at,
                          uint32_t before, uint32_t after)
{
    char line[96];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_window_at(line, sizeof(line), req, anchor, at, before, after);

    if (n < 0 || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_req = req;
    return 0;
}

int psk_session_view(struct psk_session *s, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                     uint16_t port, bool pinned)
{
    char line[160];
    int n = ppc_cmd_format_view(line, sizeof(line), id, sort, classes, host, port, pinned);

    return n < 0 ? -1 : send_raw(s, line, n);
}

int psk_session_view_signals(struct psk_session *s, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                             uint16_t port, bool pinned, unsigned signals)
{
    char line[160];
    int n = ppc_cmd_format_view_signals(line, sizeof(line), id, sort, classes, host, port, pinned, signals);

    return n < 0 ? -1 : send_raw(s, line, n);
}

int psk_session_view_diag(struct psk_session *s, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                          uint16_t port, bool pinned, unsigned signals, unsigned diag)
{
    char line[160];
    int n = ppc_cmd_format_view_diag(line, sizeof(line), id, sort, classes, host, port, pinned, signals, diag);

    return n < 0 ? -1 : send_raw(s, line, n);
}

int psk_session_marks(struct psk_session *s, struct psk_model *m, uint32_t buckets)
{
    char line[48];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_marks(line, sizeof(line), req, buckets);

    if (n < 0 || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_marks = req;
    return 0;
}

int psk_session_net(struct psk_session *s, struct psk_model *m)
{
    char line[32];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_net(line, sizeof(line), req);

    if (n < 0 || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_net = req;
    return 0;
}

int psk_session_mark(struct psk_session *s)
{
    char line[16];
    int n = ppc_cmd_format_mark(line, sizeof(line));

    return n < 0 || send_raw(s, line, n) != 0 ? -1 : 0;
}

int psk_session_evid(struct psk_session *s, struct psk_model *m, uint32_t id)
{
    char line[48];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_evid(line, sizeof(line), req, id);

    if (n < 0 || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_ev = req;
    return 0;
}

int psk_session_intel(struct psk_session *s, struct psk_model *m, uint32_t id, unsigned pair)
{
    char line[48];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_intel(line, sizeof(line), req, id, pair);

    if (n < 0 || pair > 1u || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_in[pair] = req;
    return 0;
}

int psk_session_save(struct psk_session *s)
{
    return send_word(s, "save");
}

int psk_session_inspect(struct psk_session *s, struct psk_model *m, uint32_t seq, int step)
{
    char line[64];
    uint32_t req = ++s->next_req ? s->next_req : ++s->next_req;
    int n = ppc_cmd_format_inspect(line, sizeof(line), req, seq, step);

    if (n < 0 || send_raw(s, line, n) != 0) {
        return -1;
    }
    m->want_pkt = req;
    return 0;
}

int psk_session_pin(struct psk_session *s, uint32_t seq, bool on)
{
    char line[48];
    int n = ppc_cmd_format_pin(line, sizeof(line), seq, on);

    return n < 0 ? -1 : send_raw(s, line, n);
}

/* ---- the child ---------------------------------------------------------------- */

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

int psk_session_start(struct psk_session *s, const struct psk_session_config *cfg, int64_t now_ms,
                      char *err, size_t errlen)
{
    const char *helper = cfg && cfg->helper ? cfg->helper : psk_session_helper_path();
    const char *source = cfg && cfg->source ? cfg->source : psk_session_default_source();
    char *argv[5];
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
    psk_session_init(s);
    if (strlen(helper) >= PSK_HELPER_PATH_MAX || (source && strlen(source) >= PSK_SOURCE_MAX)) {
        if (err && errlen) {
            snprintf(err, errlen, "helper arguments too long");
        }
        return -1;
    }
    argv[argc++] = (char *)helper;
    argv[argc++] = "session";
    if (source && *source) {
        argv[argc++] = "--source";
        argv[argc++] = (char *)source;
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
        /* The environment is the shell's: the helper reads the platform's
         * directories (POCKETOS_RUNTIME_DIR, POCKETOS_LOG_DIR) from it. */
        execv(helper, argv);
        _exit(127);
    }
    close(sv[1]);
    s->fd = sv[0];
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    s->pid = pid;
    s->running = true;
    s->hello_by = now_ms + PSK_HELLO_MS;
    s->last_line_ms = now_ms;
    return 0;
}

/* ---- running ------------------------------------------------------------------ */

static void kill_helper(struct psk_session *s, enum psk_exit why)
{
    if (s->running && s->pid > 0 && !s->killed) {
        kill(s->pid, SIGKILL);
        s->killed = true;
        s->exit_reason = why;
    }
}

static unsigned feed(struct psk_session *s, struct psk_model *m, const char *buf, size_t n, int64_t now)
{
    unsigned changed = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                kill_helper(s, PSK_EXIT_PROTOCOL);
            } else if (!s->killed) {
                struct ppc_evt e;

                s->line[s->line_len] = '\0';
                s->last_line_ms = now;
                switch (ppc_evt_parse(s->line, &e)) {
                case PPC_EVT_HELLO:
                    s->hello_by = 0;
                    if (e.u.hello.proto != PPC_PROTO_VERSION) {
                        kill_helper(s, PSK_EXIT_PROTOCOL);
                        break;
                    }
                    changed |= psk_model_apply(m, &e, now);
                    break;
                case PPC_EVT_BAD:
                    if (++s->bad > PSK_BAD_LINES) {
                        kill_helper(s, PSK_EXIT_PROTOCOL);
                    }
                    break;
                default:
                    /* Nothing counts before the hello: a helper that talks
                     * before it says who it is is not ours. */
                    if (s->hello_by == 0) {
                        changed |= psk_model_apply(m, &e, now);
                    }
                    break;
                }
            }
            s->line_len = 0;
            s->overlong = false;
        } else if (s->overlong) {
            continue;
        } else if (s->line_len + 1 >= sizeof(s->line)) {
            /* Killed now, not at the newline: a helper that streams without
             * one would otherwise never be stopped. */
            s->overlong = true;
            s->line_len = 0;
            kill_helper(s, PSK_EXIT_PROTOCOL);
        } else {
            s->line[s->line_len++] = c;
        }
    }
    return changed;
}

static unsigned finish(struct psk_session *s, struct psk_model *m, int status, int64_t now)
{
    unsigned changed = 0;
    enum psk_exit reason;
    int value;

    if (s->fd >= 0) {
        char buf[512];
        ssize_t n;

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
        reason = PSK_EXIT_CRASHED;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        reason = PSK_EXIT_START;
    } else {
        reason = PSK_EXIT_NORMAL;
    }
    psk_model_helper_stopped(m, reason, value, now);
    s->pid = -1;
    s->running = false;
    s->hello_by = 0;
    return changed | PSK_CHANGED_EXITED | PSK_CHANGED_STATE;
}

unsigned psk_session_poll(struct psk_session *s, struct psk_model *m, int64_t now_ms)
{
    unsigned changed = 0;
    char buf[4096];
    int status;
    pid_t r;
    int reads = 0;

    if (!s->running) {
        return 0;
    }
    /* Bounded per poll, so a helper that floods cannot hold the LVGL
     * thread: what is left is read on the next timer tick. */
    while (s->fd >= 0 && !s->eof && reads++ < 16) {
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
        ((m->state == PPC_STATE_CAPTURING || m->state == PPC_STATE_HOLD) && m->hello &&
         now_ms - s->last_line_ms >= PSK_SILENCE_MS)) {
        kill_helper(s, PSK_EXIT_HUNG);
    }
    if (s->eof && !s->killed) {
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

static bool reap_within(struct psk_session *s, int ms)
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

void psk_session_abandon(struct psk_session *s, int grace_ms)
{
    if (s->running && s->pid > 0) {
        send_word(s, "quit");
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms)) {
            kill(s->pid, SIGKILL);
            /* A helper stuck in the kernel is left as a zombie rather than
             * holding the LVGL thread. */
            reap_within(s, PSK_KILL_REAP_MS);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    psk_session_init(s);
}
