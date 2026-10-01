/*
 * The Browser app's helper client. See browser_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "browser_session.h"

#include "pocketpaths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_DEFAULT "/usr/bin/pos-browser"
/* Descriptors above stderr the shell may hold without close-on-exec (the
 * display, input devices, IPC sockets). The child closes them all before
 * exec; this is the highest it looks at. */
#define CHILD_FD_SCAN_MAX 1024
/* Damaged lines tolerated before the helper is not worth keeping. */
#define BAD_LINES_MAX 16

void browser_session_init(struct browser_session *s)
{
    memset(s, 0, sizeof(*s));
    s->pid = -1;
    s->fd = -1;
    web_rx_init(&s->rx);
}

const char *browser_session_helper_path(void)
{
    const char *p = getenv("POCKETOS_BROWSER_HELPER");

    return p && *p ? p : HELPER_DEFAULT;
}

bool browser_session_active(const struct browser_session *s)
{
    return s && s->running;
}

/* ---- the picture directory -------------------------------------------------------- */

static void remove_dir(struct browser_session *s)
{
    DIR *d;
    struct dirent *e;

    if (!s->img_dir[0]) {
        return;
    }
    d = opendir(s->img_dir);
    if (d) {
        int dfd = dirfd(d);

        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] != '.') {
                unlinkat(dfd, e->d_name, 0);
            }
        }
        closedir(d);
    }
    rmdir(s->img_dir);
    s->img_dir[0] = '\0';
}

static void make_dir(struct browser_session *s, const char *runtime)
{
    char tmpl[BROWSER_PATH_MAX];
    int n;

    s->img_dir[0] = '\0';
    if (!runtime) {
        runtime = pocketos_runtime_dir();
    }
    if (pocketos_mkdir_p(runtime, 0755) != 0) {
        return;
    }
    n = snprintf(tmpl, sizeof(tmpl), "%s/browser.XXXXXX", runtime);
    if (n < 0 || (size_t)n >= sizeof(tmpl) || !mkdtemp(tmpl)) {
        return;
    }
    web_copy(s->img_dir, sizeof(s->img_dir), tmpl);
}

/* Read the picture the helper named, exactly w*h pixels, and remove it. */
static uint16_t *take_pixels(struct browser_session *s, const struct web_rx_msg *m)
{
    char path[BROWSER_PATH_MAX + WEB_FILE_NAME_MAX + 2];
    size_t want = (size_t)m->w * (size_t)m->h * sizeof(uint16_t);
    struct stat st;
    uint16_t *px = NULL;
    size_t got = 0;
    int fd;

    if (!s->img_dir[0] || !web_rx_file_name_ok(m->file)) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s", s->img_dir, m->file);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    unlink(path);
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != want ||
        !(px = malloc(want))) {
        close(fd);
        return NULL;
    }
    while (got < want) {
        ssize_t r = read(fd, (char *)px + got, want - got);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    if (got != want) {
        free(px);
        return NULL;
    }
    return px;
}

/* ---- sending ---------------------------------------------------------------------- */

static int send_line(struct browser_session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int send_line(struct browser_session *s, const char *fmt, ...)
{
    char line[WEB_LINE_MAX];
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
    /* A few bytes into a socket the helper reads between blocks of every
     * transfer: this never waits. MSG_NOSIGNAL, so a helper that has just
     * died cannot take the shell down with SIGPIPE. */
    return send(s->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) == n ? 0 : -1;
}

int browser_session_send(struct browser_session *s, const struct browser_cmd *cmd, int64_t now_ms)
{
    size_t i;

    switch (cmd->kind) {
    case BROWSER_CMD_OPEN:
        /* The address was checked by web_url_parse: no tab, no line break. */
        for (i = 0; cmd->url[i]; i++) {
            if ((unsigned char)cmd->url[i] < 0x20) {
                return -1;
            }
        }
        return send_line(s, "open\t%d\t%d\t%s\t%s", cmd->seq, cmd->max_width, cmd->images ? "i" : "-",
                         cmd->url);
    case BROWSER_CMD_STOP:
        if (send_line(s, "stop\t%d", cmd->seq) != 0) {
            return -1;
        }
        s->stop_seq = cmd->seq;
        s->stop_by = now_ms + BROWSER_STOP_GRACE_MS;
        return 0;
    case BROWSER_CMD_NONE:
        break;
    }
    return 0;
}

/* ---- the child -------------------------------------------------------------------- */

static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s: %s", what, strerror(errno));
    }
}

int browser_session_start(struct browser_session *s, const struct browser_session_config *cfg,
                          int64_t now_ms, char *err, size_t errlen)
{
    const char *helper = cfg && cfg->helper ? cfg->helper : browser_session_helper_path();
    char *argv[10];
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
    browser_session_init(s);
    if (strlen(helper) >= BROWSER_PATH_MAX ||
        (cfg && cfg->ca_file && strlen(cfg->ca_file) >= BROWSER_PATH_MAX)) {
        if (err && errlen) {
            snprintf(err, errlen, "helper arguments too long");
        }
        return -1;
    }
    make_dir(s, cfg ? cfg->runtime : NULL);
    argv[argc++] = (char *)helper;
    argv[argc++] = "session";
    if (cfg && cfg->fake) {
        argv[argc++] = "--fake";
    }
    if (s->img_dir[0]) {
        argv[argc++] = "--images";
        argv[argc++] = s->img_dir;
    }
    if (cfg && cfg->ca_file && *cfg->ca_file) {
        argv[argc++] = "--ca-file";
        argv[argc++] = (char *)cfg->ca_file;
    }
    argv[argc] = NULL;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        say(err, errlen, "socketpair");
        remove_dir(s);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        say(err, errlen, "fork");
        close(sv[0]);
        close(sv[1]);
        remove_dir(s);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int null;
        int fd;

        /* Leave with the shell. The check after closes the race where the
         * shell died before the prctl. */
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
    s->hello_by = now_ms + BROWSER_HELLO_MS;
    s->last_line_ms = now_ms;
    return 0;
}

/* ---- running ---------------------------------------------------------------------- */

static void kill_helper(struct browser_session *s, enum browser_exit why)
{
    if (s->running && s->pid > 0 && !s->killed) {
        kill(s->pid, SIGKILL);
        s->killed = true;
        s->exit_reason = why;
    }
}

static unsigned line_in(struct browser_session *s, struct browser_view *v, int64_t now)
{
    struct web_rx_msg *m = malloc(sizeof(*m));
    enum web_rx_kind k;
    uint16_t *px = NULL;
    unsigned changed = 0;

    if (!m) {
        return 0;
    }
    s->line[s->line_len] = '\0';
    s->last_line_ms = now;
    k = web_rx_line(&s->rx, s->line, m);
    if (k == WEB_RX_HELLO) {
        s->hello_by = 0;
        if (m->proto != WEB_PROTO_VERSION) {
            kill_helper(s, BROWSER_EXIT_PROTOCOL);
            free(m);
            return 0;
        }
    } else if (k == WEB_RX_BAD) {
        /* A damaged page is dropped by the reader; a helper that keeps
         * sending damage is not worth keeping. */
        if (s->rx.bad > BAD_LINES_MAX) {
            kill_helper(s, BROWSER_EXIT_PROTOCOL);
        }
        free(m);
        return 0;
    } else if (s->hello_by != 0) {
        free(m);
        return 0; /* nothing counts before hello */
    }
    if ((k == WEB_RX_IDLE || k == WEB_RX_FAIL) && m->seq == s->stop_seq) {
        s->stop_seq = 0; /* the stop was answered */
    }
    if (k == WEB_RX_PIXELS) {
        px = take_pixels(s, m);
        if (!px) {
            m->kind = WEB_RX_NOPIXELS;
            web_copy(m->text, sizeof(m->text), "could not be read");
        }
    }
    if (k != WEB_RX_NONE) {
        changed = browser_view_apply(v, m, &s->rx, px, now);
    }
    free(m);
    return changed;
}

static unsigned feed(struct browser_session *s, struct browser_view *v, const char *buf, size_t n, int64_t now)
{
    unsigned changed = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (s->overlong) {
                kill_helper(s, BROWSER_EXIT_PROTOCOL);
            } else {
                changed |= line_in(s, v, now);
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

static unsigned finish(struct browser_session *s, struct browser_view *v, int status, int64_t now)
{
    unsigned changed = 0;
    enum browser_exit reason;

    if (s->fd >= 0) {
        char buf[1024];
        ssize_t n;

        /* Anything the helper wrote just before it left. */
        while (!s->killed && (n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
            changed |= feed(s, v, buf, (size_t)n, now);
        }
        close(s->fd);
        s->fd = -1;
    }
    if (s->killed) {
        reason = s->exit_reason;
    } else if (WIFSIGNALED(status)) {
        reason = BROWSER_EXIT_CRASHED;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        reason = BROWSER_EXIT_START; /* exec failed: no helper at that path */
    } else {
        reason = BROWSER_EXIT_NORMAL;
    }
    if (s->stop_killed) {
        /* Stuck on a page nobody wants any more. A newer one is waiting:
         * the app starts a fresh helper and sends it again; nothing is
         * counted against the helper and no error is shown. */
        v->helper_up = false;
        if (v->loading) {
            changed |= BROWSER_CHANGED_RESTART;
        }
        v->images_loading = false;
    } else {
        changed |= browser_view_helper_stopped(v, reason, now);
    }
    s->pid = -1;
    s->running = false;
    s->hello_by = 0;
    s->stop_seq = 0;
    web_rx_free(&s->rx);
    remove_dir(s);
    return changed;
}

unsigned browser_session_poll(struct browser_session *s, struct browser_view *v, int64_t now_ms)
{
    unsigned changed = 0;
    char buf[4096];
    int status;
    int rounds = 0;
    pid_t r;

    if (!s->running) {
        return 0;
    }
    /* A bounded amount per call, so a flood of lines never holds a frame:
     * what is left is read on the next tick. */
    while (s->fd >= 0 && !s->eof && !s->killed && rounds++ < 64) {
        ssize_t n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);

        if (n > 0) {
            changed |= feed(s, v, buf, (size_t)n, now_ms);
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
        ((v->loading || v->images_loading) && now_ms - s->last_line_ms >= BROWSER_BUSY_SILENCE_MS)) {
        kill_helper(s, BROWSER_EXIT_HUNG);
    } else if (s->stop_seq && now_ms >= s->stop_by && !s->killed) {
        s->stop_killed = !v->loading || v->seq != s->stop_seq;
        kill_helper(s, BROWSER_EXIT_HUNG);
    }
    if (s->eof && !s->killed) {
        /* It closed its end: it is leaving, or it will be made to. */
        kill(s->pid, SIGTERM);
    }
    r = waitpid(s->pid, &status, WNOHANG);
    if (r == s->pid) {
        changed |= finish(s, v, status, now_ms);
    } else if (r < 0 && errno == ECHILD) {
        changed |= finish(s, v, 255 << 8, now_ms);
    }
    return changed;
}

/* ---- leaving ----------------------------------------------------------------------- */

static int64_t mono_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* True when the helper is gone within ms. */
static bool reap_within(struct browser_session *s, int ms)
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

void browser_session_abandon(struct browser_session *s, int grace_ms)
{
    if (s->running && s->pid > 0) {
        /* quit ends a helper that is waiting for work; SIGTERM ends one in
         * the middle of a transfer (it sees the flag between blocks). */
        send_line(s, "quit");
        if (!s->killed) {
            kill(s->pid, SIGTERM);
        }
        if (!reap_within(s, grace_ms < 0 ? 0 : grace_ms)) {
            kill(s->pid, SIGKILL);
            /* If even this is not enough the helper is stuck in the kernel;
             * it is left as a zombie rather than holding the LVGL thread. */
            reap_within(s, BROWSER_KILL_REAP_MS);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
    web_rx_free(&s->rx);
    remove_dir(s);
    browser_session_init(s);
}
