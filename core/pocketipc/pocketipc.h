/*
 * pocketipc v0: length-prefixed JSON over Unix-domain sockets.
 * See docs/api/pocketipc.md. Depends on cJSON (MIT).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETIPC_H
#define POCKETIPC_H

#include "pocketpaths.h"

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#define POCKETIPC_MAX_FRAME (1u << 20)
/* Backpressure policy (docs/api/pocketipc.md): on a non-blocking socket a
 * write that would block waits up to this long for the peer to drain, then
 * fails with ETIMEDOUT and the peer is disconnected by the caller. */
#define POCKETIPC_SEND_TIMEOUT_MS 200
/* Kept as the historical name; the value belongs to pocketpaths.h. */
#define POCKETIPC_DEFAULT_DIR POCKETOS_RUNTIME_DIR_DEFAULT

enum pocketipc_error {
    POCKETIPC_ERR_UNKNOWN_METHOD = 1,
    POCKETIPC_ERR_INVALID_PARAMS = 2,
    POCKETIPC_ERR_POLICY = 3,
    POCKETIPC_ERR_BACKEND = 4,
    POCKETIPC_ERR_BUSY = 5,
    POCKETIPC_ERR_UNSUPPORTED = 6,
};

/* $POCKETOS_RUNTIME_DIR or POCKETIPC_DEFAULT_DIR. */
const char *pocketipc_runtime_dir(void);
/* Fill buf with "<runtime dir>/<service>.sock". Returns 0 or -1 if too long. */
int pocketipc_socket_path(const char *service, char *buf, size_t n);

/* Blocking frame I/O on a connected socket. Return 0 on success, -1 on
 * error (errno set; EPIPE when the peer has gone, without SIGPIPE). */
int pocketipc_write_frame(int fd, const char *json, size_t len);
int pocketipc_send(int fd, const cJSON *msg);
/* Read one frame; returns malloc'd NUL-terminated JSON text (caller frees)
 * or NULL on EOF, error or oversized frame. */
char *pocketipc_read_frame(int fd, size_t *len);
/* Same, giving up after timeout_ms with errno ETIMEDOUT. 0 waits forever. */
char *pocketipc_read_frame_timeout(int fd, size_t *len, int timeout_ms);

/* Incremental reader for non-blocking servers. */
struct pocketipc_reader {
    uint8_t *buf;
    size_t len;
    size_t cap;
};

void pocketipc_reader_init(struct pocketipc_reader *r);
void pocketipc_reader_free(struct pocketipc_reader *r);
/* Append received bytes. Returns 0, or -1 on a protocol violation
 * (frame larger than POCKETIPC_MAX_FRAME); the peer should be dropped. */
int pocketipc_reader_feed(struct pocketipc_reader *r, const uint8_t *data, size_t n);
/* Pop the next complete frame as parsed JSON, or NULL if none is complete.
 * *bad is set to 1 if a complete frame was not valid JSON (peer should be
 * dropped), else 0. */
cJSON *pocketipc_reader_next(struct pocketipc_reader *r, int *bad);

/* Server: create the runtime dir if needed, unlink a stale socket, listen.
 * Returns listening fd or -1 (errno set). Socket mode is 0660. */
int pocketipc_listen(const char *service);
/* Client: connect to a service. Returns fd or -1 (errno set). Blocking, as
 * every caller had: while the service's listen backlog is full this waits
 * with no limit. */
int pocketipc_connect(const char *service);
/* Same, giving up after timeout_ms with errno ETIMEDOUT (0 behaves exactly
 * as pocketipc_connect). A full backlog (the service is alive but not
 * accepting, radiod under SIGSTOP on unit A) no longer blocks the caller;
 * the connect is retried until the deadline and nothing is left queued on a
 * failure. The returned fd is non-blocking: a request with a deadline is
 * then bounded end to end (connect, write, reply), and one without waits
 * as long as it takes exactly as on a blocking fd. */
int pocketipc_connect_timeout(const char *service, int timeout_ms);

/* Client: synchronous request. params is consumed (may be NULL). Returns the
 * result object (caller frees) or NULL; on NULL *code and errbuf describe
 * the error (code 0 means transport failure). Events that arrive while
 * waiting are discarded. */
cJSON *pocketipc_call(int fd, const char *method, cJSON *params, int *code,
                      char *errbuf, size_t errlen);
/* As pocketipc_call, but gives up after timeout_ms and reports a transport
 * failure (*code 0, "... timed out after N ms"). timeout_ms 0 waits forever,
 * which is what pocketipc_call does and what every caller had before.
 *
 * The deadline covers the whole exchange, including events skipped while
 * waiting. A caller that times out MUST close the connection: the response
 * may still arrive, and reading it as the answer to the next request would
 * desynchronise the two. That is why the failure is reported as a transport
 * failure rather than an error response.
 *
 * A deadline is right for a periodic poll, which can simply ask again, and
 * wrong for a request whose completion is the point: a radio.send that timed
 * out would report failure for a packet that went out. See docs/api/pocketipc.md. */
cJSON *pocketipc_call_timeout(int fd, const char *method, cJSON *params, int timeout_ms,
                              int *code, char *errbuf, size_t errlen);

/* Message builders for services. result/data are consumed. */
cJSON *pocketipc_response(const cJSON *id, cJSON *result);
cJSON *pocketipc_error_response(const cJSON *id, int code, const char *message);
cJSON *pocketipc_event(const char *name, cJSON *data);

#endif
