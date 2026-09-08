/*
 * pocketpaths tests: the four roots, their overrides, and mkdir_p.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketpaths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;

static void check(const char *name, int ok)
{
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int is_dir(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void)
{
    char dir[] = "/tmp/pos_paths.XXXXXX";
    char deep[600];
    char cmd[700];

    unsetenv("POCKETOS_RUNTIME_DIR");
    unsetenv("POCKETOS_CONFIG_DIR");
    unsetenv("POCKETOS_STATE_DIR");
    unsetenv("POCKETOS_LOG_DIR");
    check("runtime default", strcmp(pocketos_runtime_dir(), "/run/pocketos") == 0);
    check("config default", strcmp(pocketos_config_dir(), "/etc/pocketos") == 0);
    check("state default", strcmp(pocketos_state_dir(), "/var/lib/pocketos") == 0);
    check("log default", strcmp(pocketos_log_dir(), "/var/lib/pocketos/log") == 0);

    setenv("POCKETOS_RUNTIME_DIR", "/x/run", 1);
    setenv("POCKETOS_CONFIG_DIR", "/x/etc", 1);
    setenv("POCKETOS_STATE_DIR", "/x/state", 1);
    setenv("POCKETOS_LOG_DIR", "/x/log", 1);
    check("runtime override", strcmp(pocketos_runtime_dir(), "/x/run") == 0);
    check("config override", strcmp(pocketos_config_dir(), "/x/etc") == 0);
    check("state override", strcmp(pocketos_state_dir(), "/x/state") == 0);
    check("log override", strcmp(pocketos_log_dir(), "/x/log") == 0);

    /* An empty override is not an override: the init scripts export these
     * unconditionally, and an empty value must not turn into "". */
    setenv("POCKETOS_RUNTIME_DIR", "", 1);
    check("empty override falls back", strcmp(pocketos_runtime_dir(), "/run/pocketos") == 0);
    unsetenv("POCKETOS_RUNTIME_DIR");

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(deep, sizeof(deep), "%s/a/b/c/d", dir);
    check("mkdir_p creates every level", pocketos_mkdir_p(deep, 0755) == 0 && is_dir(deep));
    check("mkdir_p is idempotent", pocketos_mkdir_p(deep, 0755) == 0);

    snprintf(deep, sizeof(deep), "%s/a/b/c/d/", dir);
    check("mkdir_p accepts a trailing slash", pocketos_mkdir_p(deep, 0755) == 0);

    /* A file in the way must be an error, not a silent success that turns
     * into a failed write somewhere else later. */
    snprintf(deep, sizeof(deep), "%s/file", dir);
    fclose(fopen(deep, "w"));
    check("mkdir_p refuses a file in the way", pocketos_mkdir_p(deep, 0755) < 0);
    snprintf(deep, sizeof(deep), "%s/file/below", dir);
    check("mkdir_p refuses to descend through a file", pocketos_mkdir_p(deep, 0755) < 0);

    check("mkdir_p rejects NULL", pocketos_mkdir_p(NULL, 0755) < 0);
    check("mkdir_p rejects an empty path", pocketos_mkdir_p("", 0755) < 0);

    {
        char toolong[POCKETOS_PATH_MAX + 64];

        memset(toolong, 'x', sizeof(toolong) - 1);
        toolong[0] = '/';
        toolong[sizeof(toolong) - 1] = '\0';
        check("mkdir_p rejects an overlong path", pocketos_mkdir_p(toolong, 0755) < 0);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("paths_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
