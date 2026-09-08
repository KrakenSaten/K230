/*
 * pocketipc frame writer test: bounded backpressure on non-blocking sockets
 * (Finding 5), oversized frames refused, slow-but-draining peers kept.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <fcntl.h>
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
    check("the writer is still alive", 1);

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

int main(void)
{
    test_stalled_peer();
    test_slow_peer();
    test_limits();
    test_dead_peer();
    test_wedged_service();
    printf("pocketipc_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
