/*
 * The Zabbix app's helper client against the real pos-zabbix on the fake
 * backend: it starts, the sets arrive whole, commands reach it, and every
 * way a helper can end - quit, crash, hang, a missing binary, a close in the
 * middle of a request - ends with a reaped child and a model that says so.
 *
 *   zabbix_session_test <pos-zabbix>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "zabbix_session.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static const char *helper;

/* The fake's one user (core/zabbix/zbx_fake.h ZBX_FAKE_USER; the app side
 * does not include the fake). */
#define ZBX_FAKE_USER_FOR_TEST "demo"

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

/* ---- the CONNECTION screen, through the real helper ----------------------------- */

static bool file_has(const char *path, const char *needle)
{
    char buf[16384];
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* A file of the process, read whole (NULs kept as NULs): cmdline, environ. */
static bool proc_has(pid_t pid, const char *what, const char *needle)
{
    char path[64];
    static char buf[65536];
    size_t n;
    size_t i;
    size_t k = strlen(needle);
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, what);
    f = fopen(path, "r");
    if (!f) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    for (i = 0; i + k <= n; i++) {
        if (memcmp(buf + i, needle, k) == 0) {
            return true;
        }
    }
    return false;
}

static struct zabbix_session *start_on(struct zabbix_model *m)
{
    struct zabbix_session *s = session_new();
    struct zabbix_session_config cfg = { .helper = helper, .fake = "demo" };
    char err[96];

    zabbix_model_init(m);
    if (zabbix_session_start(s, &cfg, now_ms(), err, sizeof(err)) == 0) {
        zabbix_model_helper_started(m, now_ms());
    }
    return s;
}

static void test_connection(void)
{
    static const char old_token[] = "old-token-7f3e9a1c55d04b28";
    static const char new_token[] = "new-token-0c1d2e3f4a5b6c7d";
    static const char password[] = "s3cret pass\tword";
    char dir[] = "/tmp/zabbix-conn-XXXXXX";
    char conf[128];
    char sdir[128];
    char secret[160];
    char logf[160];
    struct zabbix_model *m = malloc(sizeof(*m));
    struct zabbix_session *s;
    FILE *f;
    pid_t pid;
    bool leaked = false;

    if (!mkdtemp(dir)) {
        check("connection: a temporary directory", 0);
        free(m);
        return;
    }
    snprintf(conf, sizeof(conf), "%s/zabbix.conf", dir);
    snprintf(sdir, sizeof(sdir), "%s/zabbix", dir);
    snprintf(secret, sizeof(secret), "%s/secret", sdir);
    snprintf(logf, sizeof(logf), "%s/pos-zabbix.log", dir);
    setenv("POCKETOS_CONFIG_DIR", dir, 1);
    setenv("POCKETOS_STATE_DIR", dir, 1);
    setenv("POCKETOS_LOG_DIR", dir, 1);
    setenv("POCKETOS_LOG_LEVEL", "debug", 1);
    /* The simulator's backend: the trial runs against the fake as well. */
    setenv("POCKETOS_ZABBIX_BACKEND", "fake", 1);
    setenv("POCKETOS_ZABBIX_FAKE", "demo", 1);
    f = fopen(conf, "w");
    fprintf(f, "url=https://old.example.com/\nlabel=Desk\n");
    fclose(f);
    mkdir(sdir, 0700);
    f = fopen(secret, "w");
    fprintf(f, "token=%s\n", old_token);
    fclose(f);
    chmod(secret, 0600);

    s = start_on(m);
    pid = s->pid;
    POLL_UNTIL(s, m, m->have_settings, 5000);
    check("connection: the helper says what the files hold",
          m->have_settings && strcmp(m->set_url, "https://old.example.com/") == 0 && !m->set_password &&
              m->set_stored && !m->set_note[0]);
    check("connection: and not the stored token", !strstr(m->set_url, old_token) && !strstr(m->set_user, old_token) &&
                                                      !strstr(m->set_note, old_token));

    check("connection: a test is sent", zabbix_session_settings(s, false, "https://new.example.com/", "password",
                                                                "nobody", "wrong") == 0);
    POLL_UNTIL(s, m, m->cresult_seq == 1, 5000);
    check("connection: a refused password is AUTH FAILED, nothing saved",
          m->cresult_seq == 1 && !m->cresult_save && m->cresult == ZBX_CRESULT_AUTH_FAILED && !m->cresult_saved);

    zabbix_session_settings(s, false, "https://new.example.com/", "token", "", new_token);
    POLL_UNTIL(s, m, m->cresult_seq == 2, 5000);
    check("connection: a token is CONNECTED", m->cresult == ZBX_CRESULT_CONNECTED && !m->cresult_saved);
    check("connection: and the files are as they were", file_has(conf, "url=https://old.example.com/\n") &&
                                                            file_has(secret, old_token));

    zabbix_session_settings(s, true, "ftp://new.example.com/", "token", "", new_token);
    POLL_UNTIL(s, m, m->cresult_seq == 3, 5000);
    check("connection: an invalid address is INVALID CONFIG, not saved",
          m->cresult == ZBX_CRESULT_INVALID && m->cresult_save && !m->cresult_saved);

    check("connection: the helper is still the same one, still serving data",
          s->pid == pid && zabbix_session_active(s) && m->problems_ms > 0);
    if (s->pid > 0) {
        leaked = proc_has(s->pid, "cmdline", "s3cret") || proc_has(s->pid, "environ", "s3cret");
    }

    zabbix_session_settings(s, true, "https://new.example.com/zabbix/", "password", ZBX_FAKE_USER_FOR_TEST,
                            password);
    POLL_UNTIL(s, m, m->cresult_seq == 4 && m->set_password, 5000);
    check("connection: a password that connects is saved",
          m->cresult == ZBX_CRESULT_CONNECTED && m->cresult_save && m->cresult_saved);
    check("connection: and the helper says so: the new settings, a password stored",
          strcmp(m->set_url, "https://new.example.com/zabbix/") == 0 && m->set_password &&
              strcmp(m->set_user, ZBX_FAKE_USER_FOR_TEST) == 0 && m->set_stored);
    check("connection: the files hold them, the label kept",
          file_has(conf, "url=https://new.example.com/zabbix/\n") && file_has(conf, "auth=password\n") &&
              file_has(conf, "label=Desk\n") && file_has(secret, "password=s3cret pass\tword\n"));
    zabbix_session_abandon(s, 300);
    free(s);

    /* The app opened again: the stored settings are what it is shown. */
    s = start_on(m);
    POLL_UNTIL(s, m, m->have_settings, 5000);
    check("connection: persisted - a new helper reads the saved settings",
          strcmp(m->set_url, "https://new.example.com/zabbix/") == 0 && m->set_password && m->set_stored);
    zabbix_session_abandon(s, 300);
    free(s);

    check("connection: the secret was in neither the helper's command line nor its environment", !leaked);
    check("connection: the helper's log has the trials, and no token or password",
          file_has(logf, "zabbix: settings save: connected") && !file_has(logf, "s3cret") &&
              !file_has(logf, new_token) && !file_has(logf, old_token) && !file_has(logf, "wrong"));
    {
        char cmd[200];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) {
            printf("note: could not remove %s\n", dir);
        }
    }
    unsetenv("POCKETOS_ZABBIX_BACKEND");
    unsetenv("POCKETOS_ZABBIX_FAKE");
    unsetenv("POCKETOS_CONFIG_DIR");
    unsetenv("POCKETOS_STATE_DIR");
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
    test_connection();
    printf("zabbix_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
