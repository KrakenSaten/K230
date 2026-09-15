/*
 * pos app / pos shell: drive the shell over shell.* (docs/api/shell.md).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketipc/pocketipc.h"
#include "pos_cli.h"

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
        fprintf(stderr, "%s: cannot connect to %s: %s\n", pos_cli_name, path, strerror(errno));
        return 1;
    }
    *result = pocketipc_call(fd, method, params, &code, err, sizeof(err));
    close(fd);
    if (!*result) {
        fprintf(stderr, "%s: %s failed (code %d): %s\n", pos_cli_name, method, code, err);
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
    fprintf(stderr, "usage: %s app list | start <id> | home\n", pos_cli_name);
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
    if (argc >= 2 && strcmp(argv[0], "theme") == 0) {
        cJSON *params = cJSON_CreateObject();
        const cJSON *v;

        cJSON_AddStringToObject(params, "theme", argv[1]);
        if (argc >= 3) {
            cJSON_AddStringToObject(params, "mode", argv[2]);
        }
        rc = shell_call("shell.theme", params, &result);
        if (rc) {
            return rc;
        }
        v = cJSON_GetObjectItemCaseSensitive(result, "fallback");
        printf("theme %s mode %s%s\n",
               cJSON_GetObjectItemCaseSensitive(result, "theme")->valuestring,
               cJSON_GetObjectItemCaseSensitive(result, "mode")->valuestring,
               cJSON_IsTrue(v) ? " (fallback)" : "");
        if (cJSON_IsTrue(v)) {
            fprintf(stderr, "%s: %s\n", pos_cli_name,
                    cJSON_GetObjectItemCaseSensitive(result, "reason")->valuestring);
        }
        cJSON_Delete(result);
        return cJSON_IsTrue(v) ? 3 : 0;
    }
    if (argc >= 1 && strcmp(argv[0], "brightness") == 0) {
        cJSON *params = NULL;
        const cJSON *pct;

        if (argc >= 2) {
            char *end;
            long v;

            errno = 0;
            v = strtol(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' || v < 0 || v > 1000) {
                fprintf(stderr, "%s: brightness takes a whole percentage, e.g. 60\n", pos_cli_name);
                return 2;
            }
            params = cJSON_CreateObject();
            cJSON_AddNumberToObject(params, "percent", (double)v);
        }
        rc = shell_call("shell.brightness", params, &result);
        if (rc) {
            return rc;
        }
        pct = cJSON_GetObjectItemCaseSensitive(result, "percent");
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "supported"))) {
            printf("brightness unsupported on this display\n");
        } else if (cJSON_IsNumber(pct)) {
            printf("brightness %d%% (%s)\n", pct->valueint,
                   cJSON_GetObjectItemCaseSensitive(result, "device")->valuestring);
        } else {
            printf("brightness unknown (level unreadable)\n");
        }
        cJSON_Delete(result);
        return 0;
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
    /* The second line lines up under the first, whichever name is printed. */
    fprintf(stderr, "usage: %s shell info | screenshot <path.png> | theme <id> [normal|outdoor|night]\n"
                    "%*s| brightness [10..100]\n",
            pos_cli_name, (int)strlen("usage:  shell ") + (int)strlen(pos_cli_name), "");
    return 2;
}
