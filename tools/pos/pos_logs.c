/*
 * pos logs: read pocketlog files and crash reports.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int cmd_logs(int argc, char **argv);

static const char *dir(void)
{
    const char *d = getenv("POCKETOS_LOG_DIR");

    return (d && *d) ? d : POCKETLOG_DEFAULT_DIR;
}

static int list_dir(const char *prefix, const char *suffix)
{
    DIR *d = opendir(dir());
    struct dirent *e;
    int count = 0;

    if (!d) {
        fprintf(stderr, "pos logs: cannot open %s: %s\n", dir(), strerror(errno));
        return 1;
    }
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        size_t sl = strlen(suffix);
        char path[600];
        struct stat st;

        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0 || n < sl ||
            strcmp(e->d_name + n - sl, suffix) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir(), e->d_name);
        if (stat(path, &st) == 0) {
            printf("%-40s %8ld bytes\n", e->d_name, (long)st.st_size);
            count++;
        }
    }
    closedir(d);
    if (count == 0) {
        printf("(none in %s)\n", dir());
    }
    return 0;
}

static int tail_file(const char *path, int lines)
{
    FILE *f = fopen(path, "r");
    char **ring;
    int i;
    int total = 0;
    char buf[2048];

    if (!f) {
        fprintf(stderr, "pos logs: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    ring = calloc((size_t)lines, sizeof(char *));
    if (!ring) {
        fclose(f);
        return 1;
    }
    while (fgets(buf, sizeof(buf), f)) {
        int slot = total % lines;

        free(ring[slot]);
        ring[slot] = strdup(buf);
        total++;
    }
    fclose(f);
    for (i = total > lines ? total - lines : 0; i < total; i++) {
        fputs(ring[i % lines], stdout);
    }
    for (i = 0; i < lines; i++) {
        free(ring[i]);
    }
    free(ring);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: pos logs                     list log files\n"
            "       pos logs <name> [-n LINES]   tail a service log (default 50 lines)\n"
            "       pos logs --crashes           list crash reports\n"
            "       pos logs --crash <file>      print a crash report\n"
            "Log directory: $POCKETOS_LOG_DIR or %s\n", POCKETLOG_DEFAULT_DIR);
    return 2;
}

int cmd_logs(int argc, char **argv)
{
    char path[600];
    int lines = 50;
    int i;

    if (argc == 0) {
        return list_dir("", ".log");
    }
    if (strcmp(argv[0], "--crashes") == 0) {
        return list_dir("crash-", ".txt");
    }
    if (strcmp(argv[0], "--crash") == 0 && argc >= 2) {
        snprintf(path, sizeof(path), "%s/%s", dir(), argv[1]);
        return tail_file(path, 10000);
    }
    if (argv[0][0] == '-') {
        return usage();
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            lines = atoi(argv[++i]);
            if (lines <= 0) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    snprintf(path, sizeof(path), "%s/%s.log", dir(), argv[0]);
    return tail_file(path, lines);
}
