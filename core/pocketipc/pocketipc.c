/*
 * pocketipc v0 implementation. See pocketipc.h and docs/api/pocketipc.md.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "pocketipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* MSG_NOSIGNAL is what turns a vanished peer into EPIPE instead of SIGPIPE.
 * Every PocketOS host is Linux, where it exists. On a platform without it
 * the code still compiles, but such a process must ignore SIGPIPE itself. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

const char *pocketipc_runtime_dir(void)
{
    return pocketos_runtime_dir();
}

int pocketipc_socket_path(const char *service, char *buf, size_t n)
{
    int w = snprintf(buf, n, "%s/%s.sock", pocketipc_runtime_dir(), service);

    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Write the whole buffer. On a non-blocking fd, EAGAIN waits for POLLOUT
 * within a bounded window (POCKETIPC_SEND_TIMEOUT_MS in total for this
 * frame); if the peer still does not drain, fail with errno ETIMEDOUT.
 * A peer that has gone away is an error (EPIPE), never a signal: send()
 * with MSG_NOSIGNAL, so no pocketipc user has to ignore SIGPIPE itself
 * (a radiod crash used to take the shell down with it). */
static int write_all(int fd, const void *data, size_t len, uint64_t *deadline)
{
    const uint8_t *p = data;

    while (len > 0) {
        ssize_t w = send(fd, p, len, MSG_NOSIGNAL);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                uint64_t now = now_ms();
                int remaining;

                if (*deadline == 0) {
                    *deadline = now + POCKETIPC_SEND_TIMEOUT_MS;
                }
                remaining = now >= *deadline ? 0 : (int)(*deadline - now);
                if (remaining <= 0) {
                    errno = ETIMEDOUT;
                    return -1;
                }
                if (poll(&pfd, 1, remaining) < 0 && errno != EINTR) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* deadline is a monotonic millisecond stamp, or 0 for "wait as long as it
 * takes", which is what every caller got before pocketipc_call_timeout(). */
static int read_all_until(int fd, void *data, size_t len, uint64_t deadline)
{
    uint8_t *p = data;

    while (len > 0) {
        ssize_t r;

        if (deadline != 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            uint64_t now = now_ms();
            int remaining = now >= deadline ? 0 : (int)(deadline - now);

            if (remaining <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            r = poll(&pfd, 1, remaining);
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            if (r == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        r = read(fd, p, len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* A non-blocking connection (pocketipc_connect_timeout) on a
                 * call without a deadline: wait for data as a blocking read
                 * would; with a deadline the poll above already bounded it. */
                struct pollfd pfd = { .fd = fd, .events = POLLIN };

                if (deadline == 0 && poll(&pfd, 1, -1) < 0 && errno != EINTR) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

/* deadline 0: one backpressure budget for the whole frame, started at the
 * first EAGAIN. Header and body used to start a fresh POCKETIPC_SEND_TIMEOUT_MS
 * each, so a stalled peer could hold the caller for twice the documented
 * policy per frame, and a broadcast to a full client table for 32 times that.
 * A non-zero deadline is a caller's own stamp (a request with a deadline
 * shares it with the wait for the reply). */
static int write_frame_until(int fd, const char *json, size_t len, uint64_t deadline)
{
    uint8_t hdr[4];

    if (len > POCKETIPC_MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }
    hdr[0] = (uint8_t)(len >> 24);
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    if (write_all(fd, hdr, 4, &deadline) < 0) {
        return -1;
    }
    return write_all(fd, json, len, &deadline);
}

int pocketipc_write_frame(int fd, const char *json, size_t len)
{
    return write_frame_until(fd, json, len, 0);
}

int pocketipc_send(int fd, const cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    int rc;

    if (!text) {
        errno = ENOMEM;
        return -1;
    }
    rc = pocketipc_write_frame(fd, text, strlen(text));
    free(text);
    return rc;
}

static char *read_frame_until(int fd, size_t *len, uint64_t deadline)
{
    uint8_t hdr[4];
    uint32_t n;
    char *buf;

    if (read_all_until(fd, hdr, 4, deadline) < 0) {
        return NULL;
    }
    n = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
        ((uint32_t)hdr[2] << 8) | hdr[3];
    if (n > POCKETIPC_MAX_FRAME) {
        errno = EMSGSIZE;
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        return NULL;
    }
    if (read_all_until(fd, buf, n, deadline) < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (len) {
        *len = n;
    }
    return buf;
}

char *pocketipc_read_frame(int fd, size_t *len)
{
    return read_frame_until(fd, len, 0);
}

char *pocketipc_read_frame_timeout(int fd, size_t *len, int timeout_ms)
{
    return read_frame_until(fd, len, timeout_ms > 0 ? now_ms() + (uint64_t)timeout_ms : 0);
}

void pocketipc_reader_init(struct pocketipc_reader *r)
{
    memset(r, 0, sizeof(*r));
}

void pocketipc_reader_free(struct pocketipc_reader *r)
{
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

int pocketipc_reader_feed(struct pocketipc_reader *r, const uint8_t *data, size_t n)
{
    if (r->len + n > r->cap) {
        size_t cap = r->cap ? r->cap : 4096;
        uint8_t *nb;

        while (cap < r->len + n) {
            cap *= 2;
        }
        if (cap > POCKETIPC_MAX_FRAME + 4) {
            cap = POCKETIPC_MAX_FRAME + 4;
            if (r->len + n > cap) {
                return -1;
            }
        }
        nb = realloc(r->buf, cap);
        if (!nb) {
            return -1;
        }
        r->buf = nb;
        r->cap = cap;
    }
    memcpy(r->buf + r->len, data, n);
    r->len += n;
    if (r->len >= 4) {
        uint32_t fl = ((uint32_t)r->buf[0] << 24) | ((uint32_t)r->buf[1] << 16) |
                      ((uint32_t)r->buf[2] << 8) | r->buf[3];
        if (fl > POCKETIPC_MAX_FRAME) {
            return -1;
        }
    }
    return 0;
}

cJSON *pocketipc_reader_next(struct pocketipc_reader *r, int *bad)
{
    uint32_t fl;
    cJSON *msg;

    *bad = 0;
    if (r->len < 4) {
        return NULL;
    }
    fl = ((uint32_t)r->buf[0] << 24) | ((uint32_t)r->buf[1] << 16) |
         ((uint32_t)r->buf[2] << 8) | r->buf[3];
    if (r->len < 4 + (size_t)fl) {
        return NULL;
    }
    msg = cJSON_ParseWithLength((const char *)r->buf + 4, fl);
    memmove(r->buf, r->buf + 4 + fl, r->len - 4 - fl);
    r->len -= 4 + fl;
    if (!msg || !cJSON_IsObject(msg)) {
        cJSON_Delete(msg);
        *bad = 1;
        return NULL;
    }
    return msg;
}

static int fill_addr(const char *service, struct sockaddr_un *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    if (pocketipc_socket_path(service, addr->sun_path, sizeof(addr->sun_path)) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int pocketipc_listen(const char *service)
{
    struct sockaddr_un addr;
    int fd;

    if (fill_addr(service, &addr) < 0) {
        return -1;
    }
    if (pocketos_mkdir_p(pocketipc_runtime_dir(), 0770) < 0) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        chmod(addr.sun_path, 0660) < 0 || listen(fd, 16) < 0) {
        int e = errno;

        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int pocketipc_connect(const char *service)
{
    return pocketipc_connect_timeout(service, 0);
}

/* A Unix stream connect() blocks with no timeout while the service's listen
 * backlog is full (the caller sleeps in the kernel's unix_wait_for_peer).
 * That is what froze the shell on unit A (2026-09-08): every timed-out
 * request had dropped its connection, but the kernel keeps each one queued
 * until the stopped radiod accepts it, and the sixteenth reconnect never
 * returned. Non-blocking, the same connect() fails at once with EAGAIN and
 * leaves nothing queued, so the deadline can bound the retries. */
int pocketipc_connect_timeout(const char *service, int timeout_ms)
{
    struct sockaddr_un addr;
    uint64_t deadline = timeout_ms > 0 ? now_ms() + (uint64_t)timeout_ms : 0;
    int fd;

    if (fill_addr(service, &addr) < 0) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (deadline ? SOCK_NONBLOCK : 0), 0);
    if (fd < 0) {
        return -1;
    }
    for (;;) {
        uint64_t now;
        int remaining;

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        if (deadline == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS)) {
            break;
        }
        now = now_ms();
        remaining = now >= deadline ? 0 : (int)(deadline - now);
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            break;
        }
        /* backlog full: nothing is queued, so wait a little and try again */
        poll(NULL, 0, remaining < 20 ? remaining : 20);
    }
    {
        int e = errno;

        close(fd);
        errno = e;
    }
    return -1;
}

cJSON *pocketipc_call(int fd, const char *method, cJSON *params, int *code,
                      char *errbuf, size_t errlen)
{
    return pocketipc_call_timeout(fd, method, params, 0, code, errbuf, errlen);
}

cJSON *pocketipc_call_timeout(int fd, const char *method, cJSON *params, int timeout_ms,
                              int *code, char *errbuf, size_t errlen)
{
    static int next_id = 1;
    int id = next_id++;
    cJSON *req = cJSON_CreateObject();
    /* One deadline for the whole exchange, not per read: events that arrive
     * while waiting are skipped below, and they must not extend the wait. */
    uint64_t deadline = timeout_ms > 0 ? now_ms() + (uint64_t)timeout_ms : 0;

    *code = 0;
    if (errlen) {
        errbuf[0] = '\0';
    }
    cJSON_AddNumberToObject(req, "id", id);
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    }
    {
        /* The request write shares the call's deadline (on a non-blocking
         * connection; a blocking one cannot stall on a fresh request). */
        char *text = cJSON_PrintUnformatted(req);
        int rc = -1;

        if (text) {
            rc = write_frame_until(fd, text, strlen(text), deadline);
            free(text);
        } else {
            errno = ENOMEM;
        }
        cJSON_Delete(req);
        if (rc < 0) {
            snprintf(errbuf, errlen, "send failed: %s", strerror(errno));
            return NULL;
        }
    }

    for (;;) {
        char *text = read_frame_until(fd, NULL, deadline);
        cJSON *msg;
        cJSON *rid;
        cJSON *result;
        cJSON *err;

        if (!text) {
            /* A timed-out call leaves a response that may still arrive on a
             * connection the caller can no longer interpret, so the error is
             * reported as a transport failure (code 0) and the caller is
             * expected to drop the connection, exactly as it does for EPIPE. */
            if (errno == ETIMEDOUT) {
                snprintf(errbuf, errlen, "%s timed out after %d ms", method, timeout_ms);
            } else {
                snprintf(errbuf, errlen, "connection closed");
            }
            return NULL;
        }
        msg = cJSON_Parse(text);
        free(text);
        if (!msg) {
            snprintf(errbuf, errlen, "invalid JSON from service");
            return NULL;
        }
        rid = cJSON_GetObjectItemCaseSensitive(msg, "id");
        if (!cJSON_IsNumber(rid) || (int)rid->valuedouble != id) {
            cJSON_Delete(msg); /* event or unrelated response */
            continue;
        }
        result = cJSON_DetachItemFromObjectCaseSensitive(msg, "result");
        err = cJSON_GetObjectItemCaseSensitive(msg, "error");
        if (result) {
            cJSON_Delete(msg);
            return result;
        }
        if (err) {
            cJSON *c = cJSON_GetObjectItemCaseSensitive(err, "code");
            cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");

            *code = cJSON_IsNumber(c) ? (int)c->valuedouble : -1;
            snprintf(errbuf, errlen, "%s", cJSON_IsString(m) ? m->valuestring : "error");
        } else {
            *code = -1;
            snprintf(errbuf, errlen, "malformed response");
        }
        cJSON_Delete(msg);
        return NULL;
    }
}

cJSON *pocketipc_response(const cJSON *id, cJSON *result)
{
    cJSON *msg = cJSON_CreateObject();

    cJSON_AddItemToObject(msg, "id", cJSON_Duplicate(id, 0));
    cJSON_AddItemToObject(msg, "result", result ? result : cJSON_CreateObject());
    return msg;
}

cJSON *pocketipc_error_response(const cJSON *id, int code, const char *message)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();

    cJSON_AddItemToObject(msg, "id", id ? cJSON_Duplicate(id, 0) : cJSON_CreateNull());
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(msg, "error", err);
    return msg;
}

cJSON *pocketipc_event(const char *name, cJSON *data)
{
    cJSON *msg = cJSON_CreateObject();

    cJSON_AddStringToObject(msg, "event", name);
    cJSON_AddItemToObject(msg, "data", data ? data : cJSON_CreateObject());
    return msg;
}
