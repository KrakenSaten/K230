/*
 * pocketlog tests: line format, level filter, rotation, crash report.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    int found = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static char *find_crash_report(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *result = NULL;

    if (!d) {
        return NULL;
    }
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "crash-t-", 8) == 0) {
            result = malloc(strlen(dir) + strlen(e->d_name) + 2);
            sprintf(result, "%s/%s", dir, e->d_name);
            break;
        }
    }
    closedir(d);
    return result;
}

int main(void)
{
    char dir[] = "/tmp/pocketlog_test.XXXXXX";
    char path[300];
    char cmd[400];
    int i;
    pid_t pid;
    int status;
    char *report;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("POCKETOS_LOG_DIR", dir, 1);
    setenv("POCKETOS_LOG_LEVEL", "info", 1);
    snprintf(path, sizeof(path), "%s/t.log", dir);

    pocketlog_init("t");
    LOG_DEBUG("hidden %d", 1);
    LOG_INFO("hello %s", "world");
    LOG_WARN("careful");
    check("info line written", file_contains(path, " t INFO  hello world"));
    check("warn line written", file_contains(path, " t WARN  careful"));
    check("debug filtered", !file_contains(path, "hidden"));
    check("timestamp format", file_contains(path, "T") && file_contains(path, "Z t "));

    pocketlog_set_level(POCKETLOG_DEBUG);
    LOG_DEBUG("now visible");
    check("debug after set_level", file_contains(path, "now visible"));

    for (i = 0; i < 6000; i++) {
        LOG_INFO("padding line %05d %s", i,
                 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    }
    snprintf(cmd, sizeof(cmd), "%s.1", path);
    check("rotated to .1", file_size(cmd) >= (long)POCKETLOG_MAX_BYTES);
    check("fresh log after rotation", file_size(path) >= 0 &&
                                      file_size(path) < (long)POCKETLOG_MAX_BYTES);
    pocketlog_close();

    /* crash report from a child process */
    pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", 1);

        dup2(devnull, 2);
        pocketlog_init("t");
        pocketlog_install_crash_handler();
        raise(SIGSEGV);
        _exit(0);
    }
    waitpid(pid, &status, 0);
    check("child died by SIGSEGV", WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    report = find_crash_report(dir);
    check("crash report written", report != NULL);
    if (report) {
        check("crash report has signal", file_contains(report, "signal: 11"));
        check("crash report has backtrace", file_contains(report, "backtrace:"));
        free(report);
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cleanup failed\n");
    }
    printf("pocketlog_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
