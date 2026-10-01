/*
 * The Zabbix app's helper client against the real pos-zabbix on the fake
 * backend: it starts, the sets arrive whole, commands reach it, and every
 * way a helper can end - quit, crash, hang, a missing binary, a close in the
 * middle of a request - ends with a reaped child and a model that says so.
 *
 *   zabbix_session_test <pos-zabbix>
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "zabbix_session.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static const char *helper;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

/* Poll until cond or the time is up. */
#define POLL_UNTIL(s, m, cond, ms)                                                                 \
    do {                                                                                           \
        int64_t end_ = now_ms() + (ms);                                                            \
        while (!(cond) && now_ms() < end_) {                                                       \
            zabbix_session_poll((s), (m), now_ms());                                               \
            if (!(cond)) {                                                                         \
                nap(10);                                                                           \
            }                                                                                      \
        }                                                                                          \
    } while (0)

static bool alive(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static struct zabbix_session *session_new(void)
{
    struct zabbix_session *s = malloc(sizeof(*s));

    zabbix_session_init(s);
    return s;
}

static void test_demo(void)
{
    struct zabbix_session *s = session_new();
    struct zabbix_model *m = malloc(sizeof(*m));
    struct zabbix_session_config cfg = { .helper = helper, .fake = "demo" };
    char err[96];
    pid_t pid;
    int64_t t0;

    zabbix_model_init(m);
    check("demo: the helper starts", zabbix_session_start(s, &cfg, now_ms(), err, sizeof(err)) == 0);
    zabbix_model_helper_started(m, now_ms());
    pid = s->pid;
    t0 = now_ms();
    POLL_UNTIL(s, m, m->hosts_ms > 0, 5000);
    check("demo: hello, config and data within a few seconds",
          m->have_hello && m->fake && strcmp(m->scenario, "demo") == 0 && m->have_config &&
              m->problems_ms > 0 && m->hosts_ms > 0);
    printf("     first data after %lld ms\n", (long long)(now_ms() - t0));
    check("demo: online", m->state == ZBX_CONN_ONLINE && strcmp(m->version, "7.0.31") == 0);
    check("demo: the problems and hosts are the demo's", m->problems.count == 9 && m->hosts.count == 36 &&
                                                             m->problems.p[0].severity == 5);
    check("demo: the label", strcmp(m->label, "Demo server") == 0);

    check("demo: detail command", zabbix_session_detail(s, "10109") == 0);
    POLL_UNTIL(s, m, m->detail_ms > 0, 5000);
    check("demo: the host's detail arrives", strcmp(m->detail.host.name, "db-osl-02") == 0 &&
                                                 m->detail.item_count > 0);
    check("demo: a host id with a space is not sent", zabbix_session_detail(s, "10 09") == -1);
    check("demo: stop reading a host", zabbix_session_detail(s, NULL) == 0);

    check("demo: scenario command", zabbix_session_scenario(s, "healthy") == 0);
    POLL_UNTIL(s, m, strcmp(m->scenario, "healthy") == 0 && m->hosts.count == 24, 5000);
    check("demo: the helper switched scenario and read it from the start",
          strcmp(m->scenario, "healthy") == 0 && m->hosts.count == 24 && m->problems.count == 0);
    check("demo: refresh command", zabbix_session_refresh(s) == 0);

    zabbix_session_abandon(s, 300);
    check("demo: abandon reaps the helper", !zabbix_session_active(s) && !alive(pid) &&
                                                waitpid(pid, NULL, WNOHANG) < 0);
    check("demo: commands after abandon go nowhere", zabbix_session_refresh(s) == -1);
    free(s);
    free(m);
}

/* A large estate's sets are a burst of a few hundred lines, more than the
 * socket buffer holds while nobody reads. The helper must wait for the app
 * rather than give up: it once made its socket non-blocking for its reads,
 * which made its writes fail with EAGAIN and it left (found by the app test
 * on the large scenario). */
static void test_burst(void)
{
    struct zabbix_session *s = session_new();
    struct zabbix_model *m = malloc(sizeof(*m));
    struct zabbix_session_config cfg = { .helper = helper, .fake = "large" };
    char err[96];

    zabbix_model_init(m);
    zabbix_session_start(s, &cfg, now_ms(), err, sizeof(err));
    zabbix_model_helper_started(m, now_ms());
    /* Do not read for a while: let the helper fill the socket. */
    nap(1500);
    POLL_UNTIL(s, m, m->hosts_ms > 0, 8000);
    check("burst: the whole large host set arrives after a pause in reading",
          m->hosts.count == ZBX_HOST_MAX && m->problems.count == ZBX_PROBLEM_MAX);
    zabbix_session_poll(s, m, now_ms());
    check("burst: and the helper is still running", zabbix_session_active(s) && m->helper_running);
    zabbix_session_abandon(s, 300);
    free(s);
    free(m);
}

static void test_ends(void)
{
    struct zabbix_session *s = session_new();
    struct zabbix_model *m = malloc(sizeof(*m));
    struct zabbix_session_config cfg = { .helper = helper, .fake = "demo" };
    char err[96];
    unsigned changed = 0;
    int64_t end;

    /* A crash: SIGKILL from outside. */
    zabbix_model_init(m);
    zabbix_session_start(s, &cfg, now_ms(), err, sizeof(err));
    zabbix_model_helper_started(m, now_ms());
    POLL_UNTIL(s, m, m->problems_ms > 0, 5000);
    /* A signal nobody in the session sent. SIGKILL rather than SIGSEGV:
     * under the sanitizers the helper catches SIGSEGV itself and exits. */
    kill(s->pid, SIGKILL);
    end = now_ms() + 3000;
    while (zabbix_session_active(s) && now_ms() < end) {
        changed |= zabbix_session_poll(s, m, now_ms());
        nap(10);
    }
    check("crash: noticed and reaped", !zabbix_session_active(s) && (changed & ZABBIX_CHANGED_EXITED));
    check("crash: called a crash, and a restart is scheduled", m->exit_reason == ZABBIX_EXIT_CRASHED &&
                                                                   !m->helper_running &&
                                                                   m->restart_at_ms > 0);
    check("crash: the data it had delivered stays", m->problems.count == 9);
    check("crash: the problems now count as stale", zabbix_problems_stale(m, now_ms()));

    /* A missing binary. */
    {
        struct zabbix_session_config bad = { .helper = "/nonexistent/pos-zabbix", .fake = NULL };

        zabbix_model_init(m);
        zabbix_session_start(s, &bad, now_ms(), err, sizeof(err));
        zabbix_model_helper_started(m, now_ms());
        POLL_UNTIL(s, m, !zabbix_session_active(s), 3000);
        check("missing helper: it could not start", m->exit_reason == ZABBIX_EXIT_START);
    }

    /* A hang: a "helper" that never says hello. */
    {
        /* A script that ignores its arguments and sleeps. */
        struct zabbix_session_config sleeper = { .helper = "tests/zabbix_hang.sh", .fake = NULL };
        int64_t t0 = now_ms();

        zabbix_model_init(m);
        zabbix_session_start(s, &sleeper, t0, err, sizeof(err));
        zabbix_model_helper_started(m, t0);
        POLL_UNTIL(s, m, !zabbix_session_active(s), ZABBIX_HELLO_MS + 2000);
        check("hang: no hello within the deadline kills it", !zabbix_session_active(s) &&
                                                                 m->exit_reason == ZABBIX_EXIT_HUNG);
        check("hang: and not before it", now_ms() - t0 >= ZABBIX_HELLO_MS - 50);
    }

    /* A helper that leaves while a request is in flight (the slow scenario
     * sleeps 4 s inside every request): abandon must not wait for it. */
    {
        struct zabbix_session_config slow = { .helper = helper, .fake = "slow" };
        int64_t t0;
        pid_t pid;

        zabbix_model_init(m);
        zabbix_session_start(s, &slow, now_ms(), err, sizeof(err));
        zabbix_model_helper_started(m, now_ms());
        POLL_UNTIL(s, m, m->busy, 3000);
        pid = s->pid;
        t0 = now_ms();
        zabbix_session_abandon(s, 300);
        check("abandon mid-request: bounded wait", now_ms() - t0 < 300 + ZABBIX_KILL_REAP_MS + 100);
        check("abandon mid-request: the helper is gone", !alive(pid));
    }

    /* Restart delays double and are capped. */
    zabbix_model_init(m);
    {
        int d1;
        int d2;

        zabbix_model_helper_stopped(m, ZABBIX_EXIT_CRASHED, 139, 1000);
        d1 = (int)(m->restart_at_ms - 1000);
        zabbix_model_helper_stopped(m, ZABBIX_EXIT_CRASHED, 139, 1000);
        d2 = (int)(m->restart_at_ms - 1000);
        check("restart delay doubles", d1 == ZABBIX_RESTART_FIRST_MS && d2 == 2 * ZABBIX_RESTART_FIRST_MS);
        for (int i = 0; i < 10; i++) {
            zabbix_model_helper_stopped(m, ZABBIX_EXIT_CRASHED, 139, 1000);
        }
        check("restart delay is capped", m->restart_at_ms - 1000 == ZABBIX_RESTART_MAX_MS);
    }
    free(s);
    free(m);
}

static int fcntl_ok(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

static void test_repeat(void)
{
    struct zabbix_session *s = session_new();
    struct zabbix_model *m = malloc(sizeof(*m));
    struct zabbix_session_config cfg = { .helper = helper, .fake = "demo" };
    char err[96];
    int i;
    int ok = 0;

    /* Open and close twenty times, as a user flicking in and out of the
     * app: every helper reaped, no descriptor left behind. */
    int fds_before = 0;
    int fds_after = 0;
    int fd;

    for (fd = 0; fd < 256; fd++) {
        fds_before += fcntl_ok(fd);
    }
    for (i = 0; i < 20; i++) {
        pid_t pid;

        zabbix_model_init(m);
        if (zabbix_session_start(s, &cfg, now_ms(), err, sizeof(err)) != 0) {
            break;
        }
        pid = s->pid;
        POLL_UNTIL(s, m, m->have_hello, 3000);
        zabbix_session_abandon(s, 300);
        if (m->have_hello && !alive(pid)) {
            ok++;
        }
    }
    for (fd = 0; fd < 256; fd++) {
        fds_after += fcntl_ok(fd);
    }
    check("twenty opens and closes, every helper answered and was reaped", ok == 20);
    check("no descriptor leaked", fds_after == fds_before);
    free(s);
    free(m);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: zabbix_session_test <pos-zabbix>\n");
        return 2;
    }
    helper = argv[1];
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    if (!getenv("POCKETOS_LOG_DIR")) {
        setenv("POCKETOS_LOG_DIR", "/tmp", 1);
    }
    signal(SIGPIPE, SIG_IGN);
    test_demo();
    test_burst();
    test_ends();
    test_repeat();
    printf("zabbix_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
