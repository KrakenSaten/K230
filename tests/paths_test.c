/*
 * pocketpaths tests: the four roots, their overrides, mkdir_p, and the
 * release file with its compatibility name.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketpaths.h"

#include <errno.h>
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

    /* ---- the release file (ADR-005 Phase 2) ---- */
    {
        struct pocketos_release rel;
        char etc[600];
        char p_new[700];
        char p_old[700];
        char want[700];
        FILE *f;
        int rc;

        check("release file names", strcmp(POCKETOS_RELEASE_FILE, "/etc/doors-release") == 0 &&
                                        strcmp(POCKETOS_RELEASE_FILE_COMPAT, "/etc/pocketos-release") == 0);
        snprintf(etc, sizeof(etc), "%s/etc", dir);
        check("release test root", pocketos_mkdir_p(etc, 0755) == 0);
        snprintf(p_new, sizeof(p_new), "%s/doors-release", etc);
        snprintf(p_old, sizeof(p_old), "%s/pocketos-release", etc);

        errno = 0;
        rc = pocketos_release_read(dir, &rel);
        check("neither file: -1 with ENOENT and nothing filled",
              rc < 0 && errno == ENOENT && rel.path[0] == '\0' && rel.version[0] == '\0');

        f = fopen(p_old, "w");
        fputs("0.0.9\nBUILD_ID=c6cf41b\n", f);
        fclose(f);
        snprintf(want, sizeof(want), "%s/etc/pocketos-release", dir);
        check("only the old file: it is read",
              pocketos_release_read(dir, &rel) == 0 && strcmp(rel.path, want) == 0 &&
                  strcmp(rel.version, "0.0.9") == 0 && strcmp(rel.build, "c6cf41b") == 0);

        f = fopen(p_new, "w");
        fputs("0.0.10\nBUILD_ID=d00r5aa\n", f);
        fclose(f);
        unlink(p_old);
        check("the compatibility symlink can be made", symlink("doors-release", p_old) == 0);
        snprintf(want, sizeof(want), "%s/etc/doors-release", dir);
        check("new file plus symlink: the new name is read",
              pocketos_release_read(dir, &rel) == 0 && strcmp(rel.path, want) == 0 &&
                  strcmp(rel.version, "0.0.10") == 0 && strcmp(rel.build, "d00r5aa") == 0);
        unlink(p_old);
        check("only the new file: it is read",
              pocketos_release_read(dir, &rel) == 0 && strcmp(rel.path, want) == 0 &&
                  strcmp(rel.version, "0.0.10") == 0);

        /* The format's edges: a version line without a newline, no BUILD_ID,
         * an empty BUILD_ID, and a BUILD_ID only the first of which counts. */
        f = fopen(p_new, "w");
        fputs("0.0.10", f);
        fclose(f);
        check("a single line without a newline is the version, with no build",
              pocketos_release_read(dir, &rel) == 0 && strcmp(rel.version, "0.0.10") == 0 &&
                  rel.build[0] == '\0');
        f = fopen(p_new, "w");
        fputs("0.0.10\nBUILD_ID=\nBUILD_ID=later\n", f);
        fclose(f);
        check("the first BUILD_ID line decides, even when empty",
              pocketos_release_read(dir, &rel) == 0 && rel.build[0] == '\0');
        f = fopen(p_new, "w");
        fputs("BUILD_ID=notaversion\n0.0.10\n", f);
        fclose(f);
        check("line 1 is the version whatever it says",
              pocketos_release_read(dir, &rel) == 0 && strcmp(rel.version, "BUILD_ID=notaversion") == 0 &&
                  rel.build[0] == '\0');
        f = fopen(p_new, "w");
        for (rc = 0; rc < 40; rc++) {
            fputs("BUILD_ID=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", f);
        }
        fputs("\nBUILD_ID=real123\n", f);
        fclose(f);
        check("an overlong line 1 is cut, and its pieces are not taken for lines",
              pocketos_release_read(dir, &rel) == 0 && strlen(rel.version) == sizeof(rel.version) - 1 &&
                  strcmp(rel.build, "real123") == 0);

        /* The new name dangling, or present but unreadable. */
        unlink(p_new);
        f = fopen(p_old, "w");
        fputs("0.0.9\n", f);
        fclose(f);
        check("a dangling new name can be made", symlink("nowhere", p_new) == 0);
        check("a dangling new name falls back to the old file",
              pocketos_release_read(dir, &rel) == 0 && strcmp(rel.version, "0.0.9") == 0);
        unlink(p_new);
        if (geteuid() != 0) {
            f = fopen(p_new, "w");
            fputs("0.0.10\n", f);
            fclose(f);
            chmod(p_new, 0);
            errno = 0;
            rc = pocketos_release_read(dir, &rel);
            check("an unreadable new file is an error, not a fallback to the old one",
                  rc < 0 && errno == EACCES);
            chmod(p_new, 0644);
        }
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("paths_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
