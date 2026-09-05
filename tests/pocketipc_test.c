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

int main(void)
{
    test_stalled_peer();
    test_slow_peer();
    test_limits();
    printf("pocketipc_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
