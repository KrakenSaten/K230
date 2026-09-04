/*
 * pos app / pos shell: drive the shell over shell.* (docs/api/shell.md).
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

int cmd_app(int argc, char **argv);
int cmd_shell(int argc, char **argv);

static int shell_call(const char *method, cJSON *params, cJSON **result)
{
    int fd = pocketipc_connect("shell");
    int code = 0;
    char err[160];

    if (fd < 0) {
        char path[256];

        cJSON_Delete(params);
        pocketipc_socket_path("shell", path, sizeof(path));
        fprintf(stderr, "pos: cannot connect to %s: %s\n", path, strerror(errno));
        return 1;
    }
    *result = pocketipc_call(fd, method, params, &code, err, sizeof(err));
    close(fd);
    if (!*result) {
        fprintf(stderr, "pos: %s failed (code %d): %s\n", method, code, err);
        return 1;
    }
    return 0;
}

int cmd_app(int argc, char **argv)
{
    const char *sub = argc > 0 ? argv[0] : "list";
    cJSON *result;
    int rc;

    if (strcmp(sub, "list") == 0) {
        const cJSON *apps;
        const cJSON *a;
        const cJSON *cur;

        rc = shell_call("shell.info", NULL, &result);
        if (rc) {
            return rc;
        }
        cur = cJSON_GetObjectItemCaseSensitive(result, "current");
        apps = cJSON_GetObjectItemCaseSensitive(result, "apps");
        printf("%-12s %-20s %s\n", "id", "name", "state");
        cJSON_ArrayForEach(a, apps) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(a, "id");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            int open = cJSON_IsString(cur) && cJSON_IsString(id) &&
                       strcmp(cur->valuestring, id->valuestring) == 0;

            printf("%-12s %-20s %s\n", cJSON_IsString(id) ? id->valuestring : "?",
                   cJSON_IsString(name) ? name->valuestring : "?", open ? "open" : "");
        }
        cJSON_Delete(result);
        return 0;
    }
    if (strcmp(sub, "start") == 0 && argc >= 2) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddStringToObject(params, "id", argv[1]);
        rc = shell_call("shell.open", params, &result);
        if (!rc) {
            cJSON_Delete(result);
        }
        return rc;
    }
    if (strcmp(sub, "home") == 0) {
        rc = shell_call("shell.home", NULL, &result);
        if (!rc) {
            cJSON_Delete(result);
        }
        return rc;
    }
    fprintf(stderr, "usage: pos app list | start <id> | home\n");
    return 2;
}

int cmd_shell(int argc, char **argv)
{
    cJSON *result;
    int rc;

    if (argc >= 2 && strcmp(argv[0], "screenshot") == 0) {
        cJSON *params = cJSON_CreateObject();

        cJSON_AddStringToObject(params, "path", argv[1]);
        rc = shell_call("shell.screenshot", params, &result);
        if (!rc) {
            printf("saved %s\n", argv[1]);
            cJSON_Delete(result);
        }
        return rc;
    }
    if (argc >= 1 && strcmp(argv[0], "info") == 0) {
        char *text;

        rc = shell_call("shell.info", NULL, &result);
        if (rc) {
            return rc;
        }
        text = cJSON_Print(result);
        puts(text);
        free(text);
        cJSON_Delete(result);
        return 0;
    }
    fprintf(stderr, "usage: pos shell info | screenshot <path.png>\n");
    return 2;
}
