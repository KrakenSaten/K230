/*
 * pos system status: the live view from sysd (docs/api/system.md).
 * pos call: any method of any pocketipc service, for developers and tests.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cmd_system_status(void);
int cmd_call(int argc, char **argv);

/* tools/pos/pos_radio.c */
cJSON *pos_params_from_kv(const char *tool, int argc, char **argv);
int pos_print_json(cJSON *obj);

static int call_service(const char *tool, const char *service, const char *method, cJSON *params)
{
    int fd = pocketipc_connect(service);
    int code = 0;
    char err[160];
    cJSON *result;

    if (fd < 0) {
        cJSON_Delete(params);
        fprintf(stderr, "%s: cannot connect to %s: %s\n", tool, service, strerror(errno));
        return 1;
    }
    result = pocketipc_call(fd, method, params, &code, err, sizeof(err));
    close(fd);
    if (!result) {
        fprintf(stderr, "%s: %s failed (code %d): %s\n", tool, method, code, err);
        return 1;
    }
    pos_print_json(result);
    cJSON_Delete(result);
    return 0;
}

int cmd_system_status(void)
{
    return call_service("pos system", "sysd", "system.status", NULL);
}

int cmd_call(int argc, char **argv)
{
    cJSON *params;

    if (argc < 2) {
        fprintf(stderr, "usage: pos call <service> <method> [key=value ...]\n"
                        "  e.g. pos call sysd system.info\n");
        return 2;
    }
    params = pos_params_from_kv("pos call", argc - 2, argv + 2);
    if (!params) {
        return 2;
    }
    return call_service("pos call", argv[0], argv[1], params);
}
