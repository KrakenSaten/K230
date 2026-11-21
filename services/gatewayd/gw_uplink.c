/*
 * gatewayd: a pocketipc client that never blocks. See gw_uplink.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE

#include "gw_uplink.h"

#include "gw.h"

#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SETTLED_MS 5000 /* a connection that lived this long was a good one */

struct pending {
    bool used;
    double id;
    uint64_t tag;
    int64_t sent_ms;
    int64_t deadline_ms;
};

struct gw_uplink {
    char service[32];
    struct gw_uplink_cb cb;
    void *user;
    int fd;
    unsigned gen;               /* one more with every connection given up */
    bool broken;                /* a write failed: dropped by the next tick */
    int64_t next_try_ms;
    int64_t connected_ms;
    int backoff_ms;
    struct pocketipc_reader rd;
    uint8_t *out;
    size_t out_len;
    int64_t out_since_ms;
    int64_t last_rx_ms;         /* when the service last sent anything */
    double next_id;
    struct pending p[GW_UPLINK_PENDING];
};

struct gw_uplink *gw_uplink_new(const char *service, const struct gw_uplink_cb *cb, void *user)
{
    struct gw_uplink *u = calloc(1, sizeof(*u));

    if (u == NULL || strlen(service) >= sizeof(u->service)) {
        free(u);
        return NULL;
    }
    memcpy(u->service, service, strlen(service) + 1);
    u->cb = *cb;
    u->user = user;
    u->fd = -1;
    u->next_id = 1;
    u->out = malloc(GW_UPLINK_OUT_MAX);
    if (u->out == NULL) {
        free(u);
        return NULL;
    }
    pocketipc_reader_init(&u->rd);
    return u;
}

void gw_uplink_free(struct gw_uplink *u)
{
    if (u == NULL) {
        return;
    }
    if (u->fd >= 0) {
        close(u->fd);
    }
    pocketipc_reader_free(&u->rd);
    free(u->out);
    free(u);
}

int gw_uplink_fd(const struct gw_uplink *u)
{
    return u->fd;
}

bool gw_uplink_connected(const struct gw_uplink *u)
{
    return u->fd >= 0;
}

void gw_uplink_drop(struct gw_uplink *u, int64_t now_ms)
{
    int i;

    if (u->fd < 0) {
        return;
    }
    close(u->fd);
    u->fd = -1;
    u->gen++;
    u->broken = false;
    u->out_len = 0;
    pocketipc_reader_free(&u->rd);
    pocketipc_reader_init(&u->rd);
    /* A service that keeps closing on the gateway is asked less and less
     * often; one that held is asked again soon. */
    if (now_ms - u->connected_ms >= SETTLED_MS || u->backoff_ms == 0) {
        u->backoff_ms = GW_UPLINK_BACKOFF_MIN_MS;
    } else if (u->backoff_ms < GW_UPLINK_BACKOFF_MAX_MS) {
        u->backoff_ms *= 2;
    }
    u->next_try_ms = now_ms + u->backoff_ms;
    for (i = 0; i < GW_UPLINK_PENDING; i++) {
        if (u->p[i].used) {
            uint64_t tag = u->p[i].tag;

            u->p[i].used = false;
            u->cb.reply(u->user, tag, NULL, GW_ERR_UPSTREAM);
        }
    }
    u->cb.state(u->user, false);
}

static void try_connect(struct gw_uplink *u, int64_t now_ms)
{
    struct sockaddr_un sa;
    int fd;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    fd = pocketipc_socket_path(u->service, sa.sun_path, sizeof(sa.sun_path)) == 0
             ? socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
             : -1;
    /* A Unix-domain connect on a non-blocking socket succeeds or fails at
     * once: a full backlog is EAGAIN, not a wait. */
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        if (u->backoff_ms < GW_UPLINK_BACKOFF_MIN_MS) {
            u->backoff_ms = GW_UPLINK_BACKOFF_MIN_MS;
        } else if (u->backoff_ms < GW_UPLINK_BACKOFF_MAX_MS) {
            u->backoff_ms *= 2;
        }
        u->next_try_ms = now_ms + u->backoff_ms;
        return;
    }
    u->fd = fd;
    u->connected_ms = now_ms;
    u->cb.state(u->user, true);
}

/* Writes what is queued, as far as the socket takes it. */
static void flush(struct gw_uplink *u)
{
    size_t off = 0;

    while (u->fd >= 0 && !u->broken && off < u->out_len) {
        ssize_t w = send(u->fd, u->out + off, u->out_len - off, MSG_NOSIGNAL);

        if (w > 0) {
            off += (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            u->broken = true; /* given up by the next tick, not from here */
        }
    }
    if (off > 0) {
        memmove(u->out, u->out + off, u->out_len - off);
        u->out_len -= off;
    }
}

int gw_uplink_call(struct gw_uplink *u, const char *method, cJSON *params, uint64_t tag, int64_t now_ms,
                   int deadline_ms)
{
    struct pending *slot = NULL;
    cJSON *req;
    char *text;
    size_t len;
    int i;

    for (i = 0; i < GW_UPLINK_PENDING && slot == NULL; i++) {
        if (!u->p[i].used) {
            slot = &u->p[i];
        }
    }
    req = cJSON_CreateObject();
    if (u->fd < 0 || u->broken || slot == NULL || req == NULL) {
        cJSON_Delete(req);
        cJSON_Delete(params);
        return -1;
    }
    cJSON_AddNumberToObject(req, "id", u->next_id);
    cJSON_AddStringToObject(req, "method", method);
    if (params != NULL) {
        cJSON_AddItemToObject(req, "params", params);
    }
    text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (text == NULL) {
        return -1;
    }
    len = strlen(text);
    if (len + 4 > GW_UPLINK_OUT_MAX - u->out_len) {
        free(text);
        return -1;
    }
    if (u->out_len == 0) {
        u->out_since_ms = now_ms;
    }
    u->out[u->out_len] = (uint8_t)(len >> 24);
    u->out[u->out_len + 1] = (uint8_t)(len >> 16);
    u->out[u->out_len + 2] = (uint8_t)(len >> 8);
    u->out[u->out_len + 3] = (uint8_t)len;
    memcpy(u->out + u->out_len + 4, text, len);
    u->out_len += 4 + len;
    free(text);
    slot->used = true;
    slot->id = u->next_id;
    slot->tag = tag;
    slot->sent_ms = now_ms;
    slot->deadline_ms = now_ms + deadline_ms;
    u->next_id += 1;
    flush(u);
    return 0;
}

static void dispatch(struct gw_uplink *u, const cJSON *msg)
{
    const cJSON *ev = cJSON_GetObjectItemCaseSensitive(msg, "event");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    int i;

    if (cJSON_IsString(ev)) {
        u->cb.event(u->user, ev->valuestring, cJSON_GetObjectItemCaseSensitive(msg, "data"));
        return;
    }
    if (!cJSON_IsNumber(id)) {
        return; /* neither an event nor an answer: ignored */
    }
    for (i = 0; i < GW_UPLINK_PENDING; i++) {
        if (u->p[i].used && u->p[i].id == id->valuedouble) {
            const cJSON *err = cJSON_GetObjectItemCaseSensitive(msg, "error");
            const cJSON *res = cJSON_GetObjectItemCaseSensitive(msg, "result");
            uint64_t tag = u->p[i].tag;

            u->p[i].used = false;
            if (err == NULL && cJSON_IsObject(res)) {
                u->cb.reply(u->user, tag, res, 0);
            } else {
                const cJSON *code = cJSON_GetObjectItemCaseSensitive(err, "code");
                int c = GW_ERR_UPSTREAM;

                if (cJSON_IsNumber(code) && code->valuedouble >= 1 && code->valuedouble <= 6) {
                    c = (int)code->valuedouble;
                }
                u->cb.reply(u->user, tag, NULL, c);
            }
            return;
        }
    }
    /* An answer nobody waits for any more: its deadline passed. */
}

void gw_uplink_io(struct gw_uplink *u, int64_t now_ms)
{
    uint8_t buf[16384];
    unsigned gen = u->gen;
    int rounds;

    /* A bounded share of the loop: what is left is read on the next turn. */
    for (rounds = 0; rounds < 32 && u->fd >= 0 && u->gen == gen; rounds++) {
        ssize_t n = read(u->fd, buf, sizeof(buf));
        cJSON *msg;
        int bad = 0;

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (n > 0) {
            u->last_rx_ms = now_ms;
        }
        if (n <= 0 || pocketipc_reader_feed(&u->rd, buf, (size_t)n) != 0) {
            gw_uplink_drop(u, now_ms);
            return;
        }
        while (u->gen == gen && (msg = pocketipc_reader_next(&u->rd, &bad)) != NULL) {
            if (cJSON_IsObject(msg)) {
                dispatch(u, msg);
            }
            cJSON_Delete(msg);
        }
        if (u->gen == gen && bad) {
            gw_uplink_drop(u, now_ms);
            return;
        }
    }
}

void gw_uplink_tick(struct gw_uplink *u, int64_t now_ms)
{
    bool silent = false;
    int i;

    if (u->fd < 0) {
        if (now_ms >= u->next_try_ms) {
            try_connect(u, now_ms);
        }
        return;
    }
    flush(u);
    if (u->broken || (u->out_len > 0 && now_ms - u->out_since_ms >= GW_UPLINK_FLUSH_MS)) {
        gw_uplink_drop(u, now_ms);
        return;
    }
    if (u->out_len == 0) {
        u->out_since_ms = now_ms;
    }
    for (i = 0; i < GW_UPLINK_PENDING; i++) {
        if (u->p[i].used && now_ms >= u->p[i].deadline_ms) {
            uint64_t tag = u->p[i].tag;

            /* Not one byte from the service since this was asked: it is
             * not slow with one answer, it is not there. */
            silent = silent || u->last_rx_ms < u->p[i].sent_ms;
            u->p[i].used = false;
            u->cb.reply(u->user, tag, NULL, GW_ERR_UPSTREAM);
        }
    }
    if (silent) {
        /* Given up and made again, so that whoever watches is told the
         * service went away instead of being served what was last known. */
        gw_uplink_drop(u, now_ms);
    }
}
