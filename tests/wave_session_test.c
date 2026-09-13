/*
 * Wave's helper client against a scripted fake helper (tests/fake_pos_wave.sh):
 * the event protocol, the text reaching stdin, a helper that crashes, one that
 * ignores SIGTERM, one that floods, one that prints garbage, one that is not
 * there, the stop escalation, abandoning a running helper within its bound,
 * and no descriptor leaking into the child.
 *
 * Real processes and real time, with bounds generous enough for a loaded host
 * and tight enough that a hang fails.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;

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
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static char helper[PATH_MAX];

/* Collect events until EXITED or the bound. Returns the number collected. */
static int run_until_exit(struct wave_session *s, struct wave_event *evs, int max, int bound_ms)
{
    int64_t end = now_ms() + bound_ms;
    int n = 0;

    while (now_ms() < end) {
        struct wave_event ev;

        while (wave_session_poll(s, &ev, now_ms())) {
            if (n < max) {
                evs[n++] = ev;
            }
            if (ev.kind == WAVE_EV_EXITED) {
                return n;
            }
        }
        nap(5);
    }
    return n;
}

static int find(const struct wave_event *evs, int n, enum wave_event_kind k)
{
    int i;

    for (i = 0; i < n; i++) {
        if (evs[i].kind == k) {
            return i;
        }
    }
    return -1;
}

static int count(const struct wave_event *evs, int n, enum wave_event_kind k)
{
    int i;
    int c = 0;

    for (i = 0; i < n; i++) {
        c += evs[i].kind == k;
    }
    return c;
}

static void test_parse(void)
{
    struct wave_event ev;

    check("parse: ready carries the board",
          wave_session_parse_line("ready k230-t-display", &ev) && ev.kind == WAVE_EV_READY &&
              strcmp(ev.text, "k230-t-display") == 0);
    check("parse: sending carries a duration",
          wave_session_parse_line("sending 2345", &ev) && ev.kind == WAVE_EV_SENDING &&
              ev.value == 2345);
    check("parse: listening", wave_session_parse_line("listening", &ev) &&
                                  ev.kind == WAVE_EV_LISTENING);
    check("parse: level 0..100", wave_session_parse_line("level 100", &ev) && ev.value == 100);
    check("parse: level above 100 is refused", !wave_session_parse_line("level 101", &ev));
    check("parse: received decodes hex into bytes",
          wave_session_parse_line("received 444f4F5253", &ev) && ev.kind == WAVE_EV_RECEIVED &&
              ev.len == 5 && memcmp(ev.text, "DOORS", 5) == 0);
    check("parse: received keeps embedded zero bytes by length",
          wave_session_parse_line("received 410042", &ev) && ev.len == 3 && ev.text[1] == '\0');
    check("parse: odd hex is refused", !wave_session_parse_line("received 414", &ev));
    check("parse: non-hex is refused", !wave_session_parse_line("received zz", &ev));
    check("parse: empty received is refused", !wave_session_parse_line("received", &ev));
    {
        char line[2 * WAVE_EVENT_TEXT_MAX + 16];
        int i;

        strcpy(line, "received ");
        for (i = 0; i < WAVE_EVENT_TEXT_MAX + 1; i++) {
            strcat(line, "41");
        }
        check("parse: received longer than the text limit is refused",
              !wave_session_parse_line(line, &ev));
    }
    check("parse: missed", wave_session_parse_line("missed", &ev) && ev.kind == WAVE_EV_MISSED);
    check("parse: sent / stopped",
          wave_session_parse_line("sent", &ev) && ev.kind == WAVE_EV_SENT &&
              wave_session_parse_line("stopped", &ev) && ev.kind == WAVE_EV_STOPPED);
    check("parse: error keeps code and message",
          wave_session_parse_line("error audio_busy another stream", &ev) &&
              ev.kind == WAVE_EV_ERROR && strcmp(ev.text, "audio_busy another stream") == 0);
    check("parse: a bare error is refused", !wave_session_parse_line("error", &ev));
    check("parse: a negative duration is refused", !wave_session_parse_line("sending -5", &ev));
    check("parse: a word that only starts like an event is refused",
          !wave_session_parse_line("sentence", &ev) && !wave_session_parse_line("listeningx", &ev));
    check("parse: trailing words on a bare event are refused", !wave_session_parse_line("sent now", &ev));
    check("parse: unknown lines are refused", !wave_session_parse_line("hello", &ev));
}

static void test_send(void)
{
    struct wave_session s;
    struct wave_event evs[32];
    char err[160];
    int n;
    int i;

    wave_session_init(&s);
    setenv("WAVE_FAKE", "send_ok", 1);
    check("send: starts", wave_session_start_send(&s, helper, "audible_fast", 25, "DOORS", 5, err,
                                                  sizeof(err)) == 0);
    check("send: the session is active", wave_session_active(&s));
    check("send: a second start while running is refused",
          wave_session_start_listen(&s, helper, 5, err, sizeof(err)) == -1 &&
              strstr(err, "already") != NULL);
    n = run_until_exit(&s, evs, 32, 3000);
    check("send: events arrive in order",
          find(evs, n, WAVE_EV_READY) == 0 && find(evs, n, WAVE_EV_SENDING) == 1 &&
              find(evs, n, WAVE_EV_SENT) == 3 && find(evs, n, WAVE_EV_EXITED) == 4);
    i = find(evs, n, WAVE_EV_RECEIVED);
    check("send: the text reached the helper's stdin, byte for byte",
          i >= 0 && evs[i].len == 5 && memcmp(evs[i].text, "DOORS", 5) == 0);
    i = find(evs, n, WAVE_EV_EXITED);
    check("send: exit code 0", i >= 0 && evs[i].value == 0);
    check("send: idle afterwards", !wave_session_active(&s) && s.fd < 0 && s.pid < 0);

    setenv("WAVE_FAKE", "args", 1);
    wave_session_start_send(&s, helper, "audible_normal", 7, "x", 1, err, sizeof(err));
    n = run_until_exit(&s, evs, 32, 3000);
    check("send: argv is send --events --protocol P --volume V, and no text",
          n > 0 && evs[0].kind == WAVE_EV_READY &&
              strcmp(evs[0].text, "send,--events,--protocol,audible_normal,--volume,7") == 0);

    wave_session_start_listen(&s, helper, 30, err, sizeof(err));
    n = run_until_exit(&s, evs, 32, 3000);
    check("listen: argv is listen --events --seconds N",
          n > 0 && strcmp(evs[0].text, "listen,--events,--seconds,30") == 0);

    check("send: an invalid protocol word is refused before any process",
          wave_session_start_send(&s, helper, "Fast!", 25, "x", 1, err, sizeof(err)) == -1 &&
              !wave_session_active(&s));
    check("send: volume 0 is refused",
          wave_session_start_send(&s, helper, "audible_fast", 0, "x", 1, err, sizeof(err)) == -1);
    check("send: volume 101 is refused",
          wave_session_start_send(&s, helper, "audible_fast", 101, "x", 1, err, sizeof(err)) == -1);
    check("send: empty text is refused",
          wave_session_start_send(&s, helper, "audible_fast", 10, "", 0, err, sizeof(err)) == -1);
    {
        char big[WAVE_EVENT_TEXT_MAX + 1];

        memset(big, 'a', sizeof(big));
        check("send: text over the limit is refused",
              wave_session_start_send(&s, helper, "audible_fast", 10, big, sizeof(big), err,
                                      sizeof(err)) == -1);
    }
    check("listen: 0 seconds is refused", wave_session_start_listen(&s, helper, 0, err, sizeof(err)) == -1);
}

static void test_listen_stop(void)
{
    struct wave_session s;
    struct wave_event evs[32];
    struct wave_event ev;
    char err[160];
    int64_t end;
    int got_rx = 0;
    int n;
    int64_t t0;

    wave_session_init(&s);
    setenv("WAVE_FAKE", "listen_ok", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    end = now_ms() + 3000;
    while (now_ms() < end && !got_rx) {
        while (wave_session_poll(&s, &ev, now_ms())) {
            if (ev.kind == WAVE_EV_RECEIVED) {
                got_rx = 1;
            }
        }
        nap(5);
    }
    check("listen: a message arrives while listening", got_rx);
    t0 = now_ms();
    wave_session_stop(&s, t0);
    check("listen: stopping", s.state == WAVE_SESSION_STOPPING);
    n = run_until_exit(&s, evs, 32, 3000);
    check("listen: the helper acknowledges the stop", find(evs, n, WAVE_EV_STOPPED) >= 0);
    check("listen: then exits 0", find(evs, n, WAVE_EV_EXITED) >= 0 &&
                                      evs[find(evs, n, WAVE_EV_EXITED)].value == 0);
    check("listen: a cooperative stop needs no SIGKILL", !s.killed && now_ms() - t0 < WAVE_STOP_GRACE_MS);
    wave_session_stop(&s, now_ms());
    check("stop on an idle session does nothing", s.state == WAVE_SESSION_IDLE);
}

static void test_ignore_term(void)
{
    struct wave_session s;
    struct wave_event evs[32];
    char err[160];
    int n;
    int i;
    pid_t pid;

    wave_session_init(&s);
    setenv("WAVE_FAKE", "ignore_term", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    nap(200);
    wave_session_stop(&s, now_ms());
    /* Pretend the grace has run out. */
    {
        struct wave_event ev;

        wave_session_poll(&s, &ev, s.stop_deadline_ms + 1);
    }
    check("stop: a helper that ignores SIGTERM is killed after the grace", s.killed);
    n = run_until_exit(&s, evs, 32, 3000);
    i = find(evs, n, WAVE_EV_EXITED);
    check("stop: and reported as killed (128 + SIGKILL)", i >= 0 && evs[i].value == 128 + SIGKILL);

    setenv("WAVE_FAKE", "ignore_term", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    pid = s.pid;
    nap(200);
    {
        int64_t t0 = now_ms();

        wave_session_abandon(&s, 150);
        check("abandon: bounded by grace + reap even for a stubborn helper",
              now_ms() - t0 <= 150 + WAVE_KILL_REAP_MS + 150);
    }
    check("abandon: the helper is gone", kill(pid, 0) != 0 && errno == ESRCH);
    check("abandon: the session is idle", !wave_session_active(&s) && s.fd < 0);

    setenv("WAVE_FAKE", "listen_ok", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    pid = s.pid;
    nap(200);
    {
        int64_t t0 = now_ms();

        wave_session_abandon(&s, 1000);
        check("abandon: a cooperative helper leaves well inside the grace", now_ms() - t0 < 800);
    }
    check("abandon: and is gone", kill(pid, 0) != 0 && errno == ESRCH);
    wave_session_abandon(&s, 100);
    check("abandon on an idle session is harmless", !wave_session_active(&s));
}

static void test_misbehaving(void)
{
    struct wave_session s;
    struct wave_event evs[64];
    char err[160];
    int n;
    int i;

    wave_session_init(&s);
    setenv("WAVE_FAKE", "crash", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    n = run_until_exit(&s, evs, 64, 3000);
    i = find(evs, n, WAVE_EV_EXITED);
    check("crash: a helper killed mid-run is reported, not waited for",
          i >= 0 && evs[i].value == 128 + SIGKILL && !wave_session_active(&s));

    setenv("WAVE_FAKE", "garbage", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    n = run_until_exit(&s, evs, 64, 3000);
    check("garbage: an overlong line becomes one protocol error",
          n > 0 && evs[0].kind == WAVE_EV_ERROR && strstr(evs[0].text, "too long") != NULL);
    check("garbage: malformed lines produce nothing",
          count(evs, n, WAVE_EV_RECEIVED) == 0 && count(evs, n, WAVE_EV_LEVEL) == 0 &&
              count(evs, n, WAVE_EV_SENDING) == 0);
    i = find(evs, n, WAVE_EV_ERROR);
    check("garbage: a real error line still gets through",
          count(evs, n, WAVE_EV_ERROR) == 2 && strstr(evs[n - 3].text, "audio_disabled") != NULL &&
              i == 0);
    check("garbage: and the exit code", evs[n - 1].kind == WAVE_EV_EXITED && evs[n - 1].value == 5);

    setenv("WAVE_FAKE", "flood", 1);
    wave_session_start_listen(&s, helper, 60, err, sizeof(err));
    nap(500); /* let it all pile up before the first poll */
    n = run_until_exit(&s, evs, 64, 3000);
    check("flood: the queue stays bounded", n <= WAVE_EVENT_QUEUE);
    check("flood: meter readings are dropped, the message is not",
          find(evs, n, WAVE_EV_RECEIVED) >= 0 && evs[find(evs, n, WAVE_EV_RECEIVED)].text[0] == 'A');
    check("flood: and the exit is not", find(evs, n, WAVE_EV_EXITED) == n - 1);

    setenv("WAVE_FAKE", "exit_fast", 1);
    wave_session_start_send(&s, helper, "audible_fast", 10, "hello", 5, err, sizeof(err));
    n = run_until_exit(&s, evs, 64, 3000);
    check("exit_fast: a helper that never reads its text costs no SIGPIPE here",
          find(evs, n, WAVE_EV_EXITED) >= 0 && evs[find(evs, n, WAVE_EV_EXITED)].value == 3);

    check("missing helper: start itself succeeds (the failure is the child's)",
          wave_session_start_listen(&s, "/nonexistent/pos-wave", 5, err, sizeof(err)) == 0);
    n = run_until_exit(&s, evs, 64, 3000);
    i = find(evs, n, WAVE_EV_ERROR);
    check("missing helper: reported as an exec error", i >= 0 && strncmp(evs[i].text, "exec ", 5) == 0);
    check("missing helper: exit 127", find(evs, n, WAVE_EV_EXITED) >= 0 &&
                                          evs[find(evs, n, WAVE_EV_EXITED)].value == 127);
}

static void test_fds(void)
{
    struct wave_session s;
    struct wave_event evs[8];
    char err[160];
    char num[16];
    /* Deliberately without O_CLOEXEC, and at a number no shell uses for
     * itself, so what the fake finds open can only be this one. */
    int fd = dup2(open("/dev/null", O_RDONLY), 77);
    int n;

    wave_session_init(&s);
    snprintf(num, sizeof(num), "%d", fd);
    setenv("WAVE_FAKE_FD", num, 1);
    setenv("WAVE_FAKE", "fds", 1);
    wave_session_start_listen(&s, helper, 5, err, sizeof(err));
    n = run_until_exit(&s, evs, 8, 3000);
    check("fds: a descriptor the parent holds does not leak into the helper",
          n > 0 && evs[0].kind == WAVE_EV_READY && strcmp(evs[0].text, "clean") == 0);
    close(fd);
    unsetenv("WAVE_FAKE_FD");
}

/* The shell dies with a listen running and never stops it. PR_SET_PDEATHSIG
 * must end the helper anyway: a microphone must not outlive the process that
 * shows it is on. The fake helper loops forever unless signalled. */
static void test_parent_death(void)
{
    int p[2];
    pid_t mid;
    pid_t hp = -1;
    int64_t end;
    int status;

    if (pipe(p) != 0) {
        check("parent death: pipe", 0);
        return;
    }
    mid = fork();
    if (mid == 0) {
        struct wave_session s;
        char err[160];

        close(p[0]);
        wave_session_init(&s);
        setenv("WAVE_FAKE", "listen_ok", 1);
        if (wave_session_start_listen(&s, helper, 60, err, sizeof(err)) != 0) {
            _exit(1);
        }
        if (write(p[1], &s.pid, sizeof(s.pid)) != sizeof(s.pid)) {
            _exit(1);
        }
        nap(300);
        _exit(0); /* gone, without stop() or abandon() */
    }
    close(p[1]);
    if (read(p[0], &hp, sizeof(hp)) != sizeof(hp)) {
        hp = -1;
    }
    close(p[0]);
    waitpid(mid, &status, 0);
    check("parent death: the helper was running", hp > 0);
    end = now_ms() + 3000;
    while (hp > 0 && kill(hp, 0) == 0 && now_ms() < end) {
        nap(10);
    }
    check("parent death: the helper is ended by the kernel's death signal", hp > 0 && kill(hp, 0) != 0);
}

int main(int argc, char **argv)
{
    char dir[64] = "/tmp/wave-session-XXXXXX";
    char script[PATH_MAX];
    FILE *f;

    if (argc < 2 || !realpath(argv[1], script)) {
        fprintf(stderr, "usage: wave_session_test tests/fake_pos_wave.sh\n");
        return 2;
    }
    /* A wrapper with the exec bit, so the fake needs none in the checkout. */
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(helper, sizeof(helper), "%s/pos-wave", dir);
    f = fopen(helper, "w");
    if (!f) {
        perror(helper);
        return 2;
    }
    fprintf(f, "#!/bin/sh\nexec bash '%s' \"$@\"\n", script);
    fclose(f);
    chmod(helper, 0755);
    unsetenv("POCKETOS_WAVE_HELPER");
    check("the helper defaults to /usr/bin/pos-wave",
          strcmp(wave_session_helper_path(), "/usr/bin/pos-wave") == 0);
    setenv("POCKETOS_WAVE_HELPER", "/opt/x/pos-wave", 1);
    check("POCKETOS_WAVE_HELPER overrides it", strcmp(wave_session_helper_path(), "/opt/x/pos-wave") == 0);
    unsetenv("POCKETOS_WAVE_HELPER");

    test_parse();
    test_send();
    test_listen_stop();
    test_ignore_term();
    test_misbehaving();
    test_fds();
    test_parent_death();

    unlink(helper);
    rmdir(dir);
    printf("wave_session_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
