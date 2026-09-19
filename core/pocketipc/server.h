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
#include <stdint.h>

struct pocketipc_server;
struct pocketipc_client;

/* Called once per complete request. The handler must send exactly one
 * response with pocketipc_server_reply() (or an error response). req is
 * freed by the server after the handler returns. */
typedef void (*pocketipc_handler_t)(struct pocketipc_server *s,
                                    struct pocketipc_client *c, cJSON *req,
                                    void *user);

/* Called once when a client goes away, whatever ended the connection: the
 * peer closed it, it sent a frame the reader refused, or a write to it
 * failed. The client struct is already gone by then, so the connection is
 * named by the id the handler saw through pocketipc_client_id(). A service
 * that keeps per-connection state - radiod's radio lease, for one - drops it
 * here, rather than discovering on some later request that the owner has not
 * existed for hours.
 *
 * It is not called from pocketipc_server_free(): the service is shutting
 * down, every connection is ending at once, and per-connection cleanup has
 * nothing left to protect. */
typedef void (*pocketipc_disconnect_t)(struct pocketipc_server *s,
                                       uint64_t client_id, void *user);

struct pocketipc_server *pocketipc_server_new(const char *service,
                                              pocketipc_handler_t handler, void *user);
void pocketipc_server_free(struct pocketipc_server *s);
/* Optional; NULL clears it. user is passed through unchanged. Callbacks are
 * never nested: one raised while another is running is queued and delivered
 * after it returns, so a handler is free to broadcast (which can itself drop
 * a client) without re-entering itself. */
void pocketipc_server_set_on_disconnect(struct pocketipc_server *s,
                                        pocketipc_disconnect_t cb, void *user);
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
/* A connection's identity, 1 upwards, never reused while the server lives.
 * The file descriptor is not an identity: the kernel hands the same number
 * to the next client the moment this one closes, so state keyed by fd can be
 * inherited by a stranger. Ids restart at 1 with the server, which is safe
 * because a restart closes every connection there was. */
uint64_t pocketipc_client_id(const struct pocketipc_client *c);
/* Is that connection still open? For a service holding an id across calls. */
bool pocketipc_server_client_alive(const struct pocketipc_server *s, uint64_t client_id);

#endif
