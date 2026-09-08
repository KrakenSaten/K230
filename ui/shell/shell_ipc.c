/*
 * shell_ipc implementation. See shell_ipc.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "shell_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_SERVICES 8

struct conn {
    char service[32];
    int fd;
};

static struct conn conns[MAX_SERVICES];

static struct conn *find_conn(const char *service)
{
    int i;

    for (i = 0; i < MAX_SERVICES; i++) {
        if (conns[i].service[0] && strcmp(conns[i].service, service) == 0) {
            return &conns[i];
        }
    }
    for (i = 0; i < MAX_SERVICES; i++) {
        if (!conns[i].service[0]) {
            snprintf(conns[i].service, sizeof(conns[i].service), "%s", service);
            conns[i].fd = -1;
            return &conns[i];
        }
    }
    return NULL;
}

cJSON *shell_ipc_call(const char *service, const char *method, cJSON *params,
                      char *err, size_t errlen)
{
    return shell_ipc_call_timeout(service, method, params, 0, err, errlen);
}

cJSON *shell_ipc_call_timeout(const char *service, const char *method, cJSON *params,
                              int timeout_ms, char *err, size_t errlen)
{
    struct conn *c = find_conn(service);
    cJSON *result;
    int code = 0;

    if (errlen) {
        err[0] = '\0';
    }
    if (!c) {
        cJSON_Delete(params);
        snprintf(err, errlen, "too many services");
        return NULL;
    }
    if (c->fd < 0) {
        c->fd = pocketipc_connect(service);
        if (c->fd < 0) {
            cJSON_Delete(params);
            snprintf(err, errlen, "%s unavailable", service);
            return NULL;
        }
    }
    result = pocketipc_call_timeout(c->fd, method, params, timeout_ms, &code, err, errlen);
    if (!result && code == 0) {
        /* Transport failure: drop the connection, reconnect next time. A
         * timeout reports code 0 for exactly this reason -- a late response
         * must not be read as the answer to the next request. */
        close(c->fd);
        c->fd = -1;
    }
    return result;
}

void shell_ipc_shutdown(void)
{
    int i;

    for (i = 0; i < MAX_SERVICES; i++) {
        if (conns[i].service[0] && conns[i].fd >= 0) {
            close(conns[i].fd);
            conns[i].fd = -1;
        }
    }
}
