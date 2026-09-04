/*
 * pocketipc server helper: listening socket, client table, incremental
 * frame parsing and dispatch. Non-blocking; integrate by calling
 * pocketipc_server_poll() from the owner's loop.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETIPC_SERVER_H
#define POCKETIPC_SERVER_H

#include "pocketipc.h"

#include <stdbool.h>

struct pocketipc_server;
struct pocketipc_client;

/* Called once per complete request. The handler must send exactly one
 * response with pocketipc_server_reply() (or an error response). req is
 * freed by the server after the handler returns. */
typedef void (*pocketipc_handler_t)(struct pocketipc_server *s,
                                    struct pocketipc_client *c, cJSON *req,
                                    void *user);

struct pocketipc_server *pocketipc_server_new(const char *service,
                                              pocketipc_handler_t handler, void *user);
void pocketipc_server_free(struct pocketipc_server *s);
/* Accept new clients, read and dispatch requests. timeout_ms 0 = do not
 * wait. Returns number of requests handled, or -1 on fatal error. */
int pocketipc_server_poll(struct pocketipc_server *s, int timeout_ms);
/* Same, but also waits on extra_fd (-1 to ignore); *extra_ready is set to 1
 * when extra_fd became readable. Lets a service wake on hardware events. */
int pocketipc_server_poll_fd(struct pocketipc_server *s, int timeout_ms, int extra_fd,
                             int *extra_ready);
/* Listening socket, for owners that run their own poll(). */
int pocketipc_server_fd(const struct pocketipc_server *s);
const char *pocketipc_server_path(const struct pocketipc_server *s);

/* msg is consumed. A failed write closes the client. */
void pocketipc_server_reply(struct pocketipc_server *s, struct pocketipc_client *c,
                            cJSON *msg);
/* msg is consumed; delivered to subscribed clients only. */
void pocketipc_server_broadcast(struct pocketipc_server *s, cJSON *msg);
void pocketipc_client_set_subscribed(struct pocketipc_client *c, bool on);
bool pocketipc_client_subscribed(const struct pocketipc_client *c);
int pocketipc_client_fd(const struct pocketipc_client *c);

#endif
