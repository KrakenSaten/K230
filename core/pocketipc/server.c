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
    uint64_t id;
    struct pocketipc_reader rd;
    bool subscribed;
    /* Its disconnect callback is owed but has not been delivered yet, and
     * the id to deliver it with - the slot's own id is cleared on close so
     * a reused slot cannot be mistaken for the connection that left. */
    bool disconnect_pending;
    uint64_t closed_id;
};

struct pocketipc_server {
    int listen_fd;
    char path[256];
    pocketipc_handler_t handler;
    void *user;
    pocketipc_disconnect_t on_disconnect;
    void *disconnect_user;
    /* Set while a disconnect callback is running, so a client dropped by
     * something the callback did (a broadcast whose write fails, say) is
     * queued instead of re-entering the callback from inside itself. */
    bool dispatching;
    bool quiet;              /* teardown: no callbacks are owed any more */
    uint64_t next_client_id;
    struct pocketipc_client clients[SERVER_MAX_CLIENTS];
};

static void dispatch_disconnects(struct pocketipc_server *s);

static void client_close(struct pocketipc_server *s, struct pocketipc_client *c)
{
    uint64_t id = c->id;
    bool was_open = c->fd >= 0;

    if (was_open) {
        close(c->fd);
        c->fd = -1;
    }
    pocketipc_reader_free(&c->rd);
    c->subscribed = false;
    c->id = 0;
    if (!was_open || !s || !s->on_disconnect || s->quiet || id == 0) {
        return;
    }
    /* The slot is already free and the id already detached, so whatever the
     * callback does - including closing other clients - cannot reach this
     * one again. */
    c->disconnect_pending = true;
    c->closed_id = id;
    if (!s->dispatching) {
        dispatch_disconnects(s);
    }
}

/* Deliver every owed callback, including ones raised while delivering. */
static void dispatch_disconnects(struct pocketipc_server *s)
{
    int i;
    int again = 1;

    s->dispatching = true;
    while (again) {
        again = 0;
        for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
            if (s->clients[i].disconnect_pending) {
                uint64_t id = s->clients[i].closed_id;

                s->clients[i].disconnect_pending = false;
                s->clients[i].closed_id = 0;
                s->on_disconnect(s, id, s->disconnect_user);
                again = 1;
            }
        }
    }
    s->dispatching = false;
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
    s->next_client_id = 1;
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

void pocketipc_server_set_on_disconnect(struct pocketipc_server *s,
                                        pocketipc_disconnect_t cb, void *user)
{
    if (!s) {
        return;
    }
    s->on_disconnect = cb;
    s->disconnect_user = user;
}

void pocketipc_server_free(struct pocketipc_server *s)
{
    int i;

    if (!s) {
        return;
    }
    /* Teardown, not a disconnect: every connection is ending at once and the
     * owner is on its way out, so per-connection cleanup has nothing left to
     * protect and a callback here would run against a half-freed service. */
    s->quiet = true;
    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        client_close(s, &s->clients[i]);
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
        /* A slot still owing its previous occupant's disconnect callback is
         * not free: reusing it would hand the new connection the old one's
         * unfinished business. */
        if (s->clients[i].fd < 0 && !s->clients[i].disconnect_pending) {
            s->clients[i].fd = fd;
            s->clients[i].id = s->next_client_id++;
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
        client_close(s, c);
        return 0;
    }
    if (r < 0) {
        return 0;
    }
    if (pocketipc_reader_feed(&c->rd, buf, (size_t)r) < 0) {
        client_close(s, c);
        return 0;
    }
    while (c->fd >= 0 && (req = pocketipc_reader_next(&c->rd, &bad)) != NULL) {
        s->handler(s, c, req, s->user);
        cJSON_Delete(req);
        handled++;
    }
    if (bad && c->fd >= 0) {
        client_close(s, c);
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
    if (c->fd >= 0 && pocketipc_send(c->fd, msg) < 0) {
        client_close(s, c);
    }
    cJSON_Delete(msg);
}

void pocketipc_server_broadcast(struct pocketipc_server *s, cJSON *msg)
{
    int i;

    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        struct pocketipc_client *c = &s->clients[i];

        if (c->fd >= 0 && c->subscribed && pocketipc_send(c->fd, msg) < 0) {
            client_close(s, c);
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

uint64_t pocketipc_client_id(const struct pocketipc_client *c)
{
    return c->id;
}

bool pocketipc_server_client_alive(const struct pocketipc_server *s, uint64_t client_id)
{
    int i;

    if (!s || client_id == 0) {
        return false;
    }
    for (i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (s->clients[i].fd >= 0 && s->clients[i].id == client_id) {
            return true;
        }
    }
    return false;
}
