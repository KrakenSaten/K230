/*
 * pocketipc frame writer test: bounded backpressure on non-blocking sockets
 * (Finding 5), oversized frames refused, slow-but-draining peers kept.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static long ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000L + (t1.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* Non-blocking sender with a small send buffer, receiver never reads. */
static void test_stalled_peer(void)
{
    int sv[2];
    int small = 4096;
    char frame[1024];
    int i;
    int rc = 0;
    int sent = 0;
    struct timespec t0;
    long elapsed;

    memset(frame, 'x', sizeof(frame));
    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    fcntl(sv[0], F_SETFL, O_NONBLOCK);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 10000; i++) {
        rc = pocketipc_write_frame(sv[0], frame, sizeof(frame));
        if (rc < 0) {
            break;
        }
        sent++;
    }
    elapsed = ms_since(&t0);
    check("stalled peer: write eventually fails", rc < 0);
    check("stalled peer: errno is ETIMEDOUT", errno == ETIMEDOUT);
    check("stalled peer: at least one frame was accepted", sent > 0);
    check("stalled peer: bounded wait (>= 150 ms)", elapsed >= 150);
    check("stalled peer: bounded wait (<= 2000 ms)", elapsed <= 2000);
    printf("     %d frames accepted, failed after %ld ms\n", sent, elapsed);

    /* once the peer drains, sending works again on the same socket */
    {
        char buf[65536];

        fcntl(sv[1], F_SETFL, O_NONBLOCK); /* drain what is queued, do not wait for more */
        while (read(sv[1], buf, sizeof(buf)) > 0) {
            /* drain */
        }
    }
    check("after drain: write succeeds again", pocketipc_write_frame(sv[0], frame, sizeof(frame)) == 0);
    close(sv[0]);
    close(sv[1]);
}

/* Non-blocking sender, a peer that reads late but within the window. */
static void test_slow_peer(void)
{
    int sv[2];
    int small = 4096;
    char frame[1024];
    int i;
    int rc = 0;
    pid_t pid;
    int status;

    memset(frame, 'y', sizeof(frame));
    check("socketpair (slow)", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    pid = fork();
    if (pid == 0) {
        char buf[65536];
        ssize_t n;
        size_t total = 0;

        close(sv[0]);
        usleep(60 * 1000); /* reader wakes up after 60 ms, well inside 200 ms */
        while ((n = read(sv[1], buf, sizeof(buf))) > 0) {
            total += (size_t)n;
        }
        _exit(total == 64 * (sizeof(frame) + 4) ? 0 : 1);
    }
    close(sv[1]);
    for (i = 0; i < 64; i++) {
        rc = pocketipc_write_frame(sv[0], frame, sizeof(frame));
        if (rc < 0) {
            break;
        }
    }
    check("slow peer: all 64 frames sent", rc == 0 && i == 64);
    close(sv[0]);
    waitpid(pid, &status, 0);
    check("slow peer: received every byte", WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_limits(void)
{
    int sv[2];
    char *big = malloc(POCKETIPC_MAX_FRAME + 1);

    check("socketpair (limits)", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    check("oversized frame refused", pocketipc_write_frame(sv[0], big, POCKETIPC_MAX_FRAME + 1) < 0 &&
                                     errno == EMSGSIZE);
    free(big);
    close(sv[0]);
    close(sv[1]);
}

/* The peer has closed. SIGPIPE stays at its default disposition on purpose:
 * before send(MSG_NOSIGNAL) this test process died with signal 13 here,
 * which is exactly what happened to the shell when radiod crashed. */
static void test_dead_peer(void)
{
    int sv[2];
    char frame[] = "{\"id\":1,\"method\":\"radio.status\"}";
    int code = -1;
    char err[96] = "";
    cJSON *result;

    check("socketpair (dead peer)", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);
    errno = 0;
    check("write to a closed peer fails", pocketipc_write_frame(sv[0], frame, sizeof(frame) - 1) < 0);
    check("write to a closed peer reports EPIPE", errno == EPIPE);
    /* Reaching this line is the whole in-process proof: with the fix
     * reverted the process is gone by now and nothing below runs. The
     * named proof, which fails as a check instead, is in
     * test_service_restart(). */
    check("the writer reached the next statement rather than dying", 1);

    errno = 0;
    result = pocketipc_call(sv[0], "radio.status", NULL, &code, err, sizeof(err));
    check("call to a vanished service returns NULL", result == NULL);
    check("call reports a transport failure (code 0)", code == 0);
    check("call error names the send failure", strncmp(err, "send failed", 11) == 0);
    close(sv[0]);
}

/* A service that is alive, accepts the request and never answers. Without a
 * deadline this is the case that froze the shell: no crash, so the supervisor
 * saw nothing wrong, and the UI thread never came back. */
static void test_wedged_service(void)
{
    int sv[2];
    cJSON *result;
    int code = 99;
    char err[128] = "";
    struct timespec t0;
    long elapsed;

    check("socketpair (wedged)", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    result = pocketipc_call_timeout(sv[0], "radio.status", NULL, 200, &code, err, sizeof(err));
    elapsed = ms_since(&t0);
    check("wedged service: call returns NULL", result == NULL);
    check("wedged service: reported as a transport failure (code 0)", code == 0);
    check("wedged service: error names the timeout", strstr(err, "timed out") != NULL);
    check("wedged service: error names the method", strstr(err, "radio.status") != NULL);
    check("wedged service: waited at least the deadline", elapsed >= 150);
    check("wedged service: did not wait much longer", elapsed <= 2000);

    /* The same call without a deadline is the old behaviour and must stay:
     * verified by the peer answering late and the call still succeeding. */
    {
        char *text = pocketipc_read_frame(sv[1], NULL);   /* the request */
        cJSON *resp;

        free(text);
        resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "id", 2);
        cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
        check("wedged service: peer can still be answered", pocketipc_send(sv[1], resp) == 0);
        cJSON_Delete(resp);
    }
    close(sv[0]);
    close(sv[1]);
}

/* The service is alive but accepts nothing (radiod under SIGSTOP on unit A,
 * 2026-09-08, v0.0.4). Every request that timed out dropped its connection,
 * but the kernel keeps each one queued in the listen backlog until the
 * service accepts it; once the backlog is full a blocking connect() sleeps
 * in the kernel (unix_wait_for_peer) with no deadline, before the request
 * deadline can apply. The shell froze there. The bounded connect must fail
 * within its deadline and leave nothing queued; the old blocking connect is
 * shown still stuck; and once the service accepts again the bounded connect
 * succeeds and a call without a deadline still works on the non-blocking fd. */
static void test_full_backlog(void)
{
    char dir[] = "/tmp/pocketipc_test.XXXXXX";
    char path[256];
    int lfd;
    int fds[64];
    int n = 0;
    int fd;
    int afd;
    int i;
    struct timespec t0;
    long elapsed;
    pid_t pid;
    int status;

    check("backlog: runtime dir", mkdtemp(dir) != NULL);
    setenv("POCKETOS_RUNTIME_DIR", dir, 1);
    lfd = pocketipc_listen("wedged");
    check("backlog: service listening, never accepting", lfd >= 0);
    snprintf(path, sizeof(path), "%s/wedged.sock", dir);

    errno = 0;
    while (n < 64) {
        fd = pocketipc_connect_timeout("wedged", 50);
        if (fd < 0) {
            break;
        }
        fds[n++] = fd;
    }
    check("backlog: filled by the shell's own dropped connections", n > 0 && n < 64);
    check("backlog: the bounded connect then reports ETIMEDOUT", errno == ETIMEDOUT);
    printf("     %d connections queued before the backlog was full\n", n);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    fd = pocketipc_connect_timeout("wedged", 200);
    elapsed = ms_since(&t0);
    check("full backlog: bounded connect fails", fd < 0);
    check("full backlog: errno is ETIMEDOUT", errno == ETIMEDOUT);
    check("full backlog: waited at least the deadline", elapsed >= 150);
    check("full backlog: did not wait much longer", elapsed <= 2000);
    printf("     bounded connect gave up after %ld ms\n", elapsed);

    /* the old path, as the v0.0.4 shell had it: a blocking connect() */
    pid = fork();
    if (pid == 0) {
        int c = pocketipc_connect("wedged");

        _exit(c >= 0 ? 0 : 1);
    }
    usleep(500 * 1000);
    check("full backlog: the blocking connect is still stuck after 500 ms", waitpid(pid, &status, WNOHANG) == 0);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);

    /* the service resumes: it drains its backlog (radiod after SIGCONT
     * accepts every queued connection and finds each one already closed),
     * the bounded connect then succeeds, and a call without a deadline on
     * that fd waits for a late answer as it would on a blocking fd */
    {
        int drained = 0;

        fcntl(lfd, F_SETFL, O_NONBLOCK);
        while ((afd = accept(lfd, NULL, NULL)) >= 0) {
            close(afd);
            drained++;
        }
        fcntl(lfd, F_SETFL, 0);
        check("resume: the service drains exactly the queued connections", drained == n);
        printf("     %d queued connections drained\n", drained);
    }
    fd = pocketipc_connect_timeout("wedged", 200);
    check("resume: bounded connect succeeds once there is room", fd >= 0);
    check("resume: the connection is non-blocking", fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK));
    afd = accept(lfd, NULL, NULL);
    check("resume: the new connection is accepted", afd >= 0);
    pid = fork();
    if (pid == 0) {
        char *text = pocketipc_read_frame(afd, NULL);
        cJSON *req = text ? cJSON_Parse(text) : NULL;
        cJSON *resp = cJSON_CreateObject();
        int ok;

        usleep(100 * 1000);   /* answer late */
        cJSON_AddNumberToObject(resp, "id", req ? cJSON_GetObjectItemCaseSensitive(req, "id")->valuedouble : 0);
        cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
        ok = pocketipc_send(afd, resp) == 0;
        free(text);
        cJSON_Delete(req);
        cJSON_Delete(resp);
        _exit(ok ? 0 : 1);
    }
    {
        int code = 99;
        char err[128] = "";
        cJSON *result = pocketipc_call(fd, "ping", NULL, &code, err, sizeof(err));

        check("resume: a call without a deadline waits for the late answer on the non-blocking fd", result != NULL);
        cJSON_Delete(result);
    }
    waitpid(pid, &status, 0);
    check("resume: the peer answered", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(afd);
    if (fd >= 0) {
        close(fd);
    }
    for (i = 0; i < n; i++) {
        close(fds[i]);
    }
    close(lfd);
    unlink(path);
    rmdir(dir);
    unsetenv("POCKETOS_RUNTIME_DIR");
}

/* riftd will hold a pocketipc connection to radiod across a radiod restart,
 * so the whole sequence it depends on gets its own test: the service goes
 * away, a write on the stale fd reports EPIPE without a signal, the call
 * reports a plain transport failure, and the same process reconnects once
 * the service is back.
 *
 * The survival is proved in a child with SIGPIPE at its default disposition,
 * because test_dead_peer() above can only prove it by reaching the next
 * line: with send(MSG_NOSIGNAL) reverted to write() this binary dies of
 * signal 13 (rc 141) and every check after it never runs at all. Here the
 * parent names how the child ended, so a regression fails as a check. */
static void test_service_restart(void)
{
    char dir[] = "/tmp/pocketipc_restart.XXXXXX";
    char frame[] = "{\"id\":1,\"method\":\"radio.status\"}";
    char path[256];
    char err[96] = "";
    int lfd;
    int fd;
    int afd;
    int status = 0;
    int code = -1;
    int said;
    pid_t pid;
    cJSON *result;

    check("restart: runtime dir", mkdtemp(dir) != NULL);
    setenv("POCKETOS_RUNTIME_DIR", dir, 1);
    snprintf(path, sizeof(path), "%s/radiod.sock", dir);

    lfd = pocketipc_listen("radiod");
    check("restart: the service is listening", lfd >= 0);
    fd = pocketipc_connect_timeout("radiod", 500);
    check("restart: the client holds a connection", fd >= 0);
    afd = accept(lfd, NULL, NULL);
    check("restart: the service accepted it", afd >= 0);

    /* radiod dies under the client, exactly as it did on the bench */
    close(afd);
    close(lfd);
    unlink(path);

    /* Everything that touches the dead fd happens in the child, with SIGPIPE
     * at its default disposition: the parent must stay alive to report, and
     * a parent that wrote here would be killed by the same signal. The child
     * reports what it saw as a bitmask so each part gets its own check. */
    fflush(stdout);   /* the child must not inherit a full stdio buffer */
    pid = fork();
    check("restart: fork", pid >= 0);
    if (pid == 0) {
        int bad = 0;

        signal(SIGPIPE, SIG_DFL);   /* the default action is what used to kill */
        errno = 0;
        if (!(pocketipc_write_frame(fd, frame, sizeof(frame) - 1) < 0 && errno == EPIPE)) {
            bad |= 1;
        }
        code = -1;
        err[0] = '\0';
        result = pocketipc_call(fd, "radio.status", NULL, &code, err, sizeof(err));
        if (result != NULL) {
            bad |= 2;
        }
        if (code != 0) {
            bad |= 4;
        }
        if (strncmp(err, "send failed", 11) != 0) {
            bad |= 8;
        }
        cJSON_Delete(result);
        _exit(bad);
    }
    check("restart: the writer is reaped", waitpid(pid, &status, 0) == pid);
    check("restart: no signal killed the writer", !WIFSIGNALED(status));
    check("restart: SIGPIPE in particular did not kill it",
          !(WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE));
    check("restart: it exited normally", WIFEXITED(status));
    said = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    check("restart: the write reported EPIPE", said >= 0 && !(said & 1));
    check("restart: the call to the dead service returned NULL", said >= 0 && !(said & 2));
    check("restart: reported as a transport failure (code 0)", said >= 0 && !(said & 4));
    check("restart: the error names the send failure", said >= 0 && !(said & 8));
    close(fd);

    /* the service comes back and the same process reconnects on its own */
    lfd = pocketipc_listen("radiod");
    check("restart: the service is listening again", lfd >= 0);
    fd = pocketipc_connect_timeout("radiod", 500);
    check("restart: the client reconnects after the EPIPE", fd >= 0);
    afd = accept(lfd, NULL, NULL);
    check("restart: the new connection is accepted", afd >= 0);

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        char *text = pocketipc_read_frame(afd, NULL);
        cJSON *req = text ? cJSON_Parse(text) : NULL;
        cJSON *id = req ? cJSON_GetObjectItemCaseSensitive(req, "id") : NULL;
        cJSON *resp = cJSON_CreateObject();
        int ok;

        cJSON_AddNumberToObject(resp, "id", id ? id->valuedouble : 0);
        cJSON_AddItemToObject(resp, "result", cJSON_CreateObject());
        ok = pocketipc_send(afd, resp) == 0;
        free(text);
        cJSON_Delete(req);
        cJSON_Delete(resp);
        _exit(ok ? 0 : 1);
    }
    code = -1;
    err[0] = '\0';
    result = pocketipc_call(fd, "radio.status", NULL, &code, err, sizeof(err));
    check("restart: the reconnected client gets an answer", result != NULL);
    cJSON_Delete(result);
    check("restart: the restarted service answered",
          waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(afd);
    close(fd);
    close(lfd);
    unlink(path);
    rmdir(dir);
    unsetenv("POCKETOS_RUNTIME_DIR");
}

int main(void)
{
    test_stalled_peer();
    test_slow_peer();
    test_limits();
    /* test_service_restart() runs before test_dead_peer() on purpose: it is
     * the one that names a SIGPIPE death as a failed check, and test_dead_peer()
     * would take the whole binary down with it (rc 141) before it got there. */
    test_service_restart();
    test_dead_peer();
    test_wedged_service();
    test_full_backlog();
    printf("pocketipc_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
