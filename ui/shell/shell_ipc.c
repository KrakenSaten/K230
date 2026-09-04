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
    result = pocketipc_call(c->fd, method, params, &code, err, errlen);
    if (!result && code == 0) {
        /* transport failure: drop the connection, reconnect next time */
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
