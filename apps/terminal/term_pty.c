/*
 * Terminal: the shell on its PTY. See term_pty.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "term_pty.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Descriptors above stderr the shell may hold without close-on-exec. The
 * child closes them all before exec, as the other helpers' children do. */
#define CHILD_FD_SCAN_MAX 1024
/* How often the close path looks again at a session it is clearing. */
#define CLOSE_POLL_MS 10
/* Leaders that could not be reaped within the bound (a process stuck in
 * the kernel ignores even SIGKILL until it returns). Retried on the next
 * start and close, so none is forgotten; bounded, like everything here. */
#define STUCK_MAX 8

static pid_t stuck[STUCK_MAX];

int64_t term_pty_now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    while (nanosleep(&d, &d) != 0 && errno == EINTR) {
    }
}

static void say(char *err, size_t len, const char *what)
{
    if (err && len) {
        snprintf(err, len, "%s: %s", what, strerror(errno));
    }
}

void term_pty_init(struct term_pty *p)
{
    memset(p, 0, sizeof(*p));
    p->state = TERM_PTY_IDLE;
    p->fd = -1;
    p->pid = -1;
    p->exit_code = -1;
}

/* ---- the session -------------------------------------------------------- */

/* Signal every live process of session sid (sig 0: just count them). */
static int session_signal(pid_t sid, int sig)
{
    DIR *d;
    struct dirent *e;
    int found = 0;

    if (sid <= 0) {
        return 0;
    }
    d = opendir("/proc");
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        char path[64];
        char buf[512];
        const char *rp;
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        char state;
        int ppid, pgrp, session;
        ssize_t n;
        int fd;

        if (*end != '\0' || pid <= 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        /* "pid (comm) state ppid pgrp session ...": comm may hold anything,
         * the last ')' ends it. */
        rp = strrchr(buf, ')');
        if (!rp || sscanf(rp + 1, " %c %d %d %d", &state, &ppid, &pgrp, &session) != 4) {
            continue;
        }
        if (session != sid || state == 'Z' || state == 'X') {
            continue;
        }
        found++;
        if (sig) {
            kill((pid_t)pid, sig);
        }
    }
    closedir(d);
    return found;
}

int term_pty_session_count(pid_t sid)
{
    return session_signal(sid, 0);
}

static void hang_up_session(pid_t sid)
{
    session_signal(sid, SIGHUP);
    /* A stopped job cannot act on SIGHUP until it runs again. */
    session_signal(sid, SIGCONT);
}

static void reap_stuck(void)
{
    int i;

    for (i = 0; i < STUCK_MAX; i++) {
        if (stuck[i] > 0) {
            pid_t r = waitpid(stuck[i], NULL, WNOHANG);

            if (r == stuck[i] || (r < 0 && errno == ECHILD)) {
                stuck[i] = 0;
            }
        }
    }
}

static void remember_stuck(pid_t pid)
{
    int i;

    for (i = 0; i < STUCK_MAX; i++) {
        if (stuck[i] <= 0) {
            stuck[i] = pid;
            return;
        }
    }
    /* Eight leaders stuck in the kernel at once: the ninth stays a zombie
     * until the shell exits. Nothing else can be done without blocking. */
}

/* ---- start -------------------------------------------------------------- */

static bool drop_from_env(const char *e, bool have_home)
{
    static const char *const drop[] = { "TERM=", "COLUMNS=", "LINES=", "SSH_CLIENT=",
                                        "SSH_CONNECTION=", "SSH_TTY=", "SSH_AUTH_SOCK=" };
    size_t i;

    for (i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
        if (strncmp(e, drop[i], strlen(drop[i])) == 0) {
            return true;
        }
    }
    return have_home && strncmp(e, "HOME=", 5) == 0;
}

/* The program's environment: the shell's own, which is what Doors runs
 * under, less what describes some other terminal, plus TERM and HOME. */
static char **build_env(const struct term_pty_spawn *sp, char *term, size_t term_len, char *home,
                        size_t home_len)
{
    size_t n = 0;
    size_t k = 0;
    char **env;
    char **e;

    for (e = environ; e && *e; e++) {
        n++;
    }
    env = calloc(n + 3, sizeof(*env));
    if (!env) {
        return NULL;
    }
    for (e = environ; e && *e; e++) {
        if (!drop_from_env(*e, sp->home != NULL)) {
            env[k++] = *e;
        }
    }
    snprintf(term, term_len, "TERM=%s", sp->term ? sp->term : "vt100");
    env[k++] = term;
    if (sp->home) {
        snprintf(home, home_len, "HOME=%s", sp->home);
        env[k++] = home;
    }
    env[k] = NULL;
    return env;
}

int term_pty_start(struct term_pty *p, const struct term_pty_spawn *sp, char *err, size_t errlen)
{
    char name[128];
    char arg0[256];
    char term[64];
    char home[512];
    char fail[320];
    char *argv[2];
    char **env;
    struct winsize ws;
    struct termios t;
    struct sigaction dfl;
    const char *base;
    const char *cwd;
    pid_t parent = getpid();
    pid_t pid;
    int mfd;
    int sfd;
    int fail_len;

    reap_stuck();
    if (p->state != TERM_PTY_IDLE) {
        term_pty_close(p);
    }
    term_pty_init(p);
    if (!sp || !sp->path || !*sp->path) {
        if (err && errlen) {
            snprintf(err, errlen, "no program to run");
        }
        return -1;
    }

    mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (mfd < 0) {
        say(err, errlen, "cannot open /dev/ptmx");
        return -1;
    }
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0 || ptsname_r(mfd, name, sizeof(name)) != 0) {
        say(err, errlen, "cannot set up the pseudo-terminal");
        close(mfd);
        return -1;
    }
    sfd = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (sfd < 0) {
        say(err, errlen, "cannot open the pseudo-terminal");
        close(mfd);
        return -1;
    }
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)(sp->cols > 0 ? sp->cols : 80);
    ws.ws_row = (unsigned short)(sp->rows > 0 ? sp->rows : 24);
    ioctl(mfd, TIOCSWINSZ, &ws);
    /* The line discipline as a Linux console has it, stated rather than
     * assumed: DEL erases (what Backspace sends), and erasing takes a whole
     * UTF-8 character. */
    if (tcgetattr(sfd, &t) == 0) {
        t.c_cc[VERASE] = 0x7F;
        t.c_iflag |= IUTF8;
        tcsetattr(sfd, TCSANOW, &t);
    }

    /* Everything the child needs is prepared here: after fork it may only
     * make async-signal-safe calls (the shell has threads). */
    base = strrchr(sp->path, '/');
    base = base ? base + 1 : sp->path;
    if (sp->login) {
        snprintf(arg0, sizeof(arg0), "-%s", base);
    } else {
        snprintf(arg0, sizeof(arg0), "%s", sp->path);
    }
    argv[0] = arg0;
    argv[1] = NULL;
    env = build_env(sp, term, sizeof(term), home, sizeof(home));
    if (!env) {
        say(err, errlen, "cannot build the environment");
        close(sfd);
        close(mfd);
        return -1;
    }
    cwd = sp->home ? sp->home : NULL;
    fail_len = snprintf(fail, sizeof(fail), "terminal: cannot run %s\r\n", sp->path);
    if (fail_len < 0 || fail_len >= (int)sizeof(fail)) {
        fail_len = (int)strlen(fail);
    }
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);

    pid = fork();
    if (pid < 0) {
        say(err, errlen, "cannot start the shell");
        free(env);
        close(sfd);
        close(mfd);
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int fd;
        int sig;

        if (setsid() < 0 || ioctl(sfd, TIOCSCTTY, 0) < 0) {
            _exit(126);
        }
        if (dup2(sfd, 0) < 0 || dup2(sfd, 1) < 0 || dup2(sfd, 2) < 0) {
            _exit(126);
        }
        for (fd = 3; fd < CHILD_FD_SCAN_MAX; fd++) {
            close(fd);
        }
        /* The Doors shell ignores or handles some signals; a program must
         * start with none of that, or Ctrl+C would not interrupt it. */
        for (sig = 1; sig < NSIG; sig++) {
            sigaction(sig, &dfl, NULL);
        }
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        /* Leave with the Doors shell; the check closes the race where it
         * died before the prctl. */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) {
            _exit(0);
        }
        if (!cwd || chdir(cwd) != 0) {
            if (chdir("/") != 0) {
                /* nowhere better to be */
            }
        }
        execve(sp->path, argv, env);
        if (write(1, fail, (size_t)fail_len) < 0) {
            /* nothing more can be said */
        }
        _exit(127);
    }

    free(env);
    close(sfd);
    p->fd = mfd;
    p->pid = pid;
    p->state = TERM_PTY_RUNNING;
    return 0;
}

/* ---- I/O ---------------------------------------------------------------- */

long term_pty_read(struct term_pty *p, uint8_t *buf, size_t cap)
{
    if (p->fd < 0 || p->eof) {
        return -1;
    }
    for (;;) {
        ssize_t n = read(p->fd, buf, cap);

        if (n > 0) {
            return (long)n;
        }
        if (n == 0) {
            p->eof = true;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        /* EIO: every slave descriptor is closed. */
        p->eof = true;
        return -1;
    }
}

/* Write what the PTY takes now. Returns how much went, -1 when the master
 * refuses for good. */
static long write_now(struct term_pty *p, const uint8_t *data, size_t n)
{
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(p->fd, data + done, n - done);

        if (w > 0) {
            done += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            return -1;
        }
    }
    return (long)done;
}

void term_pty_flush(struct term_pty *p)
{
    long w;

    if (p->fd < 0 || p->outq_len == 0) {
        return;
    }
    w = write_now(p, p->outq, p->outq_len);
    if (w < 0) {
        p->input_dropped += p->outq_len;
        p->outq_len = 0;
        return;
    }
    memmove(p->outq, p->outq + w, p->outq_len - (size_t)w);
    p->outq_len -= (size_t)w;
}

int term_pty_send(struct term_pty *p, const uint8_t *data, size_t n)
{
    size_t room;
    long w = 0;

    if (p->fd < 0 || p->state != TERM_PTY_RUNNING) {
        p->input_dropped += n;
        return -1;
    }
    term_pty_flush(p);
    if (p->outq_len == 0) {
        w = write_now(p, data, n);
        if (w < 0) {
            p->input_dropped += n;
            return -1;
        }
    }
    data += w;
    n -= (size_t)w;
    room = TERM_PTY_OUTQ - p->outq_len;
    if (n > room) {
        p->input_dropped += n - room;
        memcpy(p->outq + p->outq_len, data, room);
        p->outq_len += room;
        return -1;
    }
    memcpy(p->outq + p->outq_len, data, n);
    p->outq_len += n;
    return 0;
}

void term_pty_resize(struct term_pty *p, int cols, int rows)
{
    struct winsize ws;

    if (p->fd < 0 || cols <= 0 || rows <= 0) {
        return;
    }
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

/* ---- the end ------------------------------------------------------------ */

/* Has the leader ended? Looks without reaping, so its pid stays reserved. */
static bool leader_ended(struct term_pty *p)
{
    siginfo_t si;

    memset(&si, 0, sizeof(si));
    if (waitid(P_PID, (id_t)p->pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0) {
        return errno == ECHILD; /* not ours any more: nothing to wait for */
    }
    if (si.si_pid != p->pid) {
        return false;
    }
    if (si.si_code == CLD_EXITED) {
        p->exit_code = si.si_status;
        p->exit_signal = 0;
    } else {
        p->exit_code = -1;
        p->exit_signal = si.si_status;
    }
    return true;
}

/* SIGKILL what is left of the session, then reap the leader. */
static bool finish_session(struct term_pty *p, int reap_ms)
{
    int64_t end = term_pty_now_ms() + reap_ms;

    p->leftovers_killed += (unsigned)session_signal(p->pid, SIGKILL);
    for (;;) {
        int status;
        pid_t r = waitpid(p->pid, &status, WNOHANG);

        if (r == p->pid || (r < 0 && errno == ECHILD)) {
            if (r == p->pid && p->exit_code < 0 && p->exit_signal == 0) {
                if (WIFEXITED(status)) {
                    p->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    p->exit_signal = WTERMSIG(status);
                }
            }
            p->pid = -1;
            return true;
        }
        if (r < 0 && errno != EINTR) {
            return false;
        }
        if (term_pty_now_ms() >= end) {
            return false;
        }
        sleep_ms(CLOSE_POLL_MS);
    }
}

void term_pty_poll(struct term_pty *p, int64_t now_ms)
{
    switch (p->state) {
    case TERM_PTY_RUNNING:
        term_pty_flush(p);
        if (leader_ended(p)) {
            hang_up_session(p->pid);
            p->state = TERM_PTY_ENDING;
            p->deadline_ms = now_ms + TERM_PTY_HUP_GRACE_MS;
        }
        break;
    case TERM_PTY_ENDING:
        if (term_pty_session_count(p->pid) == 0 || now_ms >= p->deadline_ms) {
            /* The leader is a zombie already, so this does not wait. */
            if (finish_session(p, 0)) {
                p->state = TERM_PTY_ENDED;
                p->outq_len = 0;
            }
        }
        break;
    case TERM_PTY_IDLE:
    case TERM_PTY_ENDED:
        break;
    }
}

void term_pty_release(struct term_pty *p)
{
    if (p->state == TERM_PTY_ENDED && p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}

void term_pty_close(struct term_pty *p)
{
    int64_t deadline;

    if (p->fd >= 0) {
        close(p->fd); /* the hangup: the kernel tells the session too */
        p->fd = -1;
    }
    if (p->pid > 0) {
        if (p->state == TERM_PTY_RUNNING) {
            hang_up_session(p->pid);
            deadline = term_pty_now_ms() + TERM_PTY_HUP_GRACE_MS;
        } else {
            deadline = p->deadline_ms;
        }
        /* Give the session its grace to leave on SIGHUP, as long as there
         * is anyone left in it - the leader included while it runs. */
        while (term_pty_session_count(p->pid) > 0 && term_pty_now_ms() < deadline) {
            sleep_ms(CLOSE_POLL_MS);
        }
        if (!finish_session(p, TERM_PTY_KILL_REAP_MS)) {
            remember_stuck(p->pid);
        }
    }
    reap_stuck();
    {
        unsigned long dropped = p->input_dropped;
        unsigned killed = p->leftovers_killed;
        int code = p->exit_code;
        int sig = p->exit_signal;

        term_pty_init(p);
        p->input_dropped = dropped;
        p->leftovers_killed = killed;
        p->exit_code = code;
        p->exit_signal = sig;
    }
}
