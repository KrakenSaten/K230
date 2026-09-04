/*
 * pocketipc v0: length-prefixed JSON over Unix-domain sockets.
 * See docs/api/pocketipc.md. Depends on cJSON (MIT).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETIPC_H
#define POCKETIPC_H

#include <cjson/cJSON.h>
#include <stddef.h>
#include <stdint.h>

#define POCKETIPC_MAX_FRAME (1u << 20)
#define POCKETIPC_DEFAULT_DIR "/run/pocketos"

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

/* Blocking frame I/O on a connected socket. Return 0 on success, -1 on error. */
int pocketipc_write_frame(int fd, const char *json, size_t len);
int pocketipc_send(int fd, const cJSON *msg);
/* Read one frame; returns malloc'd NUL-terminated JSON text (caller frees)
 * or NULL on EOF, error or oversized frame. */
char *pocketipc_read_frame(int fd, size_t *len);

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
/* Client: connect to a service. Returns fd or -1 (errno set). */
int pocketipc_connect(const char *service);

/* Client: synchronous request. params is consumed (may be NULL). Returns the
 * result object (caller frees) or NULL; on NULL *code and errbuf describe
 * the error (code 0 means transport failure). Events that arrive while
 * waiting are discarded. */
cJSON *pocketipc_call(int fd, const char *method, cJSON *params, int *code,
                      char *errbuf, size_t errlen);

/* Message builders for services. result/data are consumed. */
cJSON *pocketipc_response(const cJSON *id, cJSON *result);
cJSON *pocketipc_error_response(const cJSON *id, int code, const char *message);
cJSON *pocketipc_event(const char *name, cJSON *data);

#endif
