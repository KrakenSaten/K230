/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "ninja_session.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
void ninja_session_init(struct ninja_session *s)
{
    memset(s, 0, sizeof(*s)); s->fd = -1; s->pid = -1;
}
void ninja_session_close(struct ninja_session *s)
{
    if (s->fd >= 0) close(s->fd);
    if (s->pid > 0) {
        kill(s->pid, SIGKILL);
        /* Only process teardown, never HTTP: bounded to 50 ms as in Zabbix.
         * A task stuck uninterruptibly in the kernel is an OS-level limitation. */
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(s->pid, NULL, WNOHANG);
            if (r == s->pid || (r < 0 && errno == ECHILD)) break;
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
    }
    free(s->incoming); ninja_session_init(s);
}
int ninja_session_start(struct ninja_session *s, int64_t now, const char *fake)
{
    int p[2];
    const char *helper = getenv("POCKETOS_NINJAONE_HELPER");
    pid_t parent = getpid();
    ninja_session_close(s);
    s->incoming = calloc(1, sizeof(*s->incoming));
    if (!s->incoming) return -1;
    if (pipe2(p, O_CLOEXEC)) { ninja_session_close(s); return -1; }
    s->pid = fork();
    if (s->pid < 0) { close(p[0]); close(p[1]); ninja_session_close(s); return -1; }
    if (!s->pid) {
        int null;
        struct rlimit limit;
        sigset_t none;
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(0);
        sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL); signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
        if (dup2(p[1], STDOUT_FILENO) < 0) _exit(127);
        null = open("/dev/null", O_RDWR);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0) _exit(127);
        /* No shell descriptor can survive into the helper, including high FDs. */
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3u, ~0u, 0u) < 0)
#endif
        {
            if (getrlimit(RLIMIT_NOFILE, &limit) || limit.rlim_cur > 1048576) _exit(127);
            for (rlim_t fd = 3; fd < limit.rlim_cur; fd++) close((int)fd);
        }
        if (!helper || !*helper) helper = "/usr/bin/pos-ninjaone";
        if (fake) execl(helper, helper, "snapshot", "--fake", fake, (char *)NULL);
        else execl(helper, helper, "snapshot", (char *)NULL);
        _exit(127);
    }
    close(p[1]); s->fd = p[0];
    if (fcntl(s->fd, F_SETFL, O_NONBLOCK) < 0) { ninja_session_close(s); return -1; }
    s->deadline = now + 120000;
    return 0;
}
int ninja_session_poll(struct ninja_session *s, int64_t now, struct ninja_snapshot *out)
{
    size_t budget = 65536;
    if (s->fd < 0) return -1;
    if (now >= s->deadline) { ninja_session_close(s); return -1; }
    while (budget && s->used < sizeof(*out)) {
        size_t n = sizeof(*out) - s->used;
        if (n > budget) n = budget;
        ssize_t got = read(s->fd, (char *)s->incoming + s->used, n);
        if (got > 0) { s->used += (size_t)got; budget -= (size_t)got; }
        else if (got < 0 && errno == EINTR) continue;
        else if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        else { ninja_session_close(s); return -1; }
    }
    if (s->used != sizeof(*out)) return 0;
    if (!ninja_snapshot_valid(s->incoming)) { ninja_session_close(s); return -1; }
    memcpy(out, s->incoming, sizeof(*out));
    ninja_session_close(s); return 1;
}
