/*
 * pocketipc server helper implementation. See server.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_MAX_CLIENTS 32

struct pocketipc_client {
    int fd;
    struct pocketipc_reader rd;
    bool subscribed;
};

struct pocketipc_server {
    int listen_fd;
    char path[256];
    pocketipc_handler_t handler;
    void *user;
    struct pocketipc_client clients[SERVER_MAX_CLIENTS];
};

static void client_close(struct pocketipc_client *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    pocketipc_reader_free(&c->rd);
    c->subscribed = false;
}

struct pocketipc_server *pocketipc_server_new(const char *service,
                                              pocketipc_handler_t handler, void *user)
{
    struct pocketipc_server *s = calloc(1, sizeof(*s));
    int i;

    if (!s) {
        return NULL;
    }
    s->handler = handler;
    s->user = user;
    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        s->clients[i].fd = -1;
    }
    s->listen_fd = pocketipc_listen(service);
    if (s->listen_fd < 0) {
        free(s);
        return NULL;
    }
    pocketipc_socket_path(service, s->path, sizeof(s->path));
    return s;
}

void pocketipc_server_free(struct pocketipc_server *s)
{
    int i;

    if (!s) {
        return;
    }
    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        client_close(&s->clients[i]);
    }
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        unlink(s->path);
    }
    free(s);
}

int pocketipc_server_fd(const struct pocketipc_server *s)
{
    return s->listen_fd;
}

const char *pocketipc_server_path(const struct pocketipc_server *s)
{
    return s->path;
}

static void accept_client(struct pocketipc_server *s)
{
    int fd = accept4(s->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    int i;

    if (fd < 0) {
        return;
    }
    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (s->clients[i].fd < 0) {
            s->clients[i].fd = fd;
            pocketipc_reader_init(&s->clients[i].rd);
            s->clients[i].subscribed = false;
            return;
        }
    }
    close(fd);
}

static int service_client(struct pocketipc_server *s, struct pocketipc_client *c)
{
    uint8_t buf[4096];
    ssize_t r = read(c->fd, buf, sizeof(buf));
    int bad = 0;
    int handled = 0;
    cJSON *req;

    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
        client_close(c);
        return 0;
    }
    if (r < 0) {
        return 0;
    }
    if (pocketipc_reader_feed(&c->rd, buf, (size_t)r) < 0) {
        client_close(c);
        return 0;
    }
    while (c->fd >= 0 && (req = pocketipc_reader_next(&c->rd, &bad)) != NULL) {
        s->handler(s, c, req, s->user);
        cJSON_Delete(req);
        handled++;
    }
    if (bad && c->fd >= 0) {
        client_close(c);
    }
    return handled;
}

int pocketipc_server_poll_fd(struct pocketipc_server *s, int timeout_ms, int extra_fd,
                             int *extra_ready)
{
    struct pollfd fds[SERVER_MAX_CLIENTS + 2];
    int idx[SERVER_MAX_CLIENTS];
    int n = 1;
    int nclients;
    int i;
    int handled = 0;

    if (extra_ready) {
        *extra_ready = 0;
    }
    fds[0].fd = s->listen_fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (s->clients[i].fd >= 0) {
            fds[n].fd = s->clients[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            idx[n - 1] = i;
            n++;
        }
    }
    nclients = n;
    if (extra_fd >= 0) {
        fds[n].fd = extra_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    if (poll(fds, (nfds_t)n, timeout_ms) < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (fds[0].revents & POLLIN) {
        accept_client(s);
    }
    for (i = 1; i < nclients; i++) {
        if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            handled += service_client(s, &s->clients[idx[i - 1]]);
        }
    }
    if (extra_fd >= 0 && extra_ready && (fds[nclients].revents & (POLLIN | POLLHUP | POLLERR))) {
        *extra_ready = 1;
    }
    return handled;
}

int pocketipc_server_poll(struct pocketipc_server *s, int timeout_ms)
{
    return pocketipc_server_poll_fd(s, timeout_ms, -1, NULL);
}

void pocketipc_server_reply(struct pocketipc_server *s, struct pocketipc_client *c,
                            cJSON *msg)
{
    (void)s;
    if (c->fd >= 0 && pocketipc_send(c->fd, msg) < 0) {
        client_close(c);
    }
    cJSON_Delete(msg);
}

void pocketipc_server_broadcast(struct pocketipc_server *s, cJSON *msg)
{
    int i;

    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        struct pocketipc_client *c = &s->clients[i];

        if (c->fd >= 0 && c->subscribed && pocketipc_send(c->fd, msg) < 0) {
            client_close(c);
        }
    }
    cJSON_Delete(msg);
}

void pocketipc_client_set_subscribed(struct pocketipc_client *c, bool on)
{
    c->subscribed = on;
}

bool pocketipc_client_subscribed(const struct pocketipc_client *c)
{
    return c->subscribed;
}

int pocketipc_client_fd(const struct pocketipc_client *c)
{
    return c->fd;
}
