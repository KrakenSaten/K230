/*
 * Reader for pos-supervise's state file. See sysd_services.h for why this is
 * here, and the header of tools/supervise/pos-supervise for the format it
 * parses; that script is the only writer and owns the contract.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "sysd_services.h"

#include "pocketpaths.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The format this reader understands. A file claiming anything else is
 * reported as a service whose state is unknown rather than guessed at. */
#define SYSD_STATE_VERSION 1
#define SYSD_STATE_SUFFIX ".state"
#define SYSD_MAX_SERVICES 64
/* starttime is field 22 of /proc/<pid>/stat, and comm is field 2, so it is the
 * twentieth token after comm ends. */
#define SYSD_STAT_STARTTIME_AFTER_COMM 20
/* How far apart the supervisor's recorded start and the kernel's may be and
 * still be the same event. The supervisor reads the clock just before it
 * forks and stores whole seconds, so the gap is a truncated second plus a
 * fork; this is loose enough never to disown a healthy service and far
 * tighter than the minutes a pid takes to come round again. */
#define SYSD_START_TOLERANCE_S 5

struct svc_state {
    long version;
    long child_pid;
    long last_exit_code;
    long restarts;
    long started_uptime_s;
    int running;
    int crashloop;
    int have_child_pid;
    int have_last_exit_code;
    int have_restarts;
    int have_started_uptime_s;
};

/* A whole decimal number, or -1 when the value is empty or not one. The
 * supervisor writes an empty value for "I do not know", which is the case
 * this has to keep apart from a real 0. */
static int parse_long(const char *value, long *out)
{
    char *end;
    long v;

    if (!value || *value == '\0') {
        return -1;
    }
    v = strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static int parse_flag(const char *value)
{
    return value && strcmp(value, "1") == 0;
}

/* Read one state file. Returns 0 when the file was read (whatever it held),
 * -1 when it could not be opened at all, which is the file having been
 * removed between the readdir and here: that service is simply gone. */
static int read_state(const char *path, struct svc_state *st)
{
    FILE *f = fopen(path, "r");
    char line[256];

    memset(st, 0, sizeof(*st));
    st->version = -1;
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *eq;
        const char *value;

        line[strcspn(line, "\n")] = '\0';
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        value = eq + 1;
        if (strcmp(line, "state_version") == 0) {
            parse_long(value, &st->version);
        } else if (strcmp(line, "child_pid") == 0) {
            st->have_child_pid = parse_long(value, &st->child_pid) == 0;
        } else if (strcmp(line, "running") == 0) {
            st->running = parse_flag(value);
        } else if (strcmp(line, "crashloop") == 0) {
            st->crashloop = parse_flag(value);
        } else if (strcmp(line, "last_exit_code") == 0) {
            st->have_last_exit_code = parse_long(value, &st->last_exit_code) == 0;
        } else if (strcmp(line, "restarts") == 0) {
            st->have_restarts = parse_long(value, &st->restarts) == 0;
        } else if (strcmp(line, "started_uptime_s") == 0) {
            st->have_started_uptime_s = parse_long(value, &st->started_uptime_s) == 0;
        }
        /* Unknown keys are ignored: the supervisor may add facts this
         * reader has no use for without becoming unreadable. */
    }
    fclose(f);
    return 0;
}

/* The boot-relative second a live process started, from field 22 of
 * /proc/<pid>/stat. Returns 0 on success, -1 when the file is gone or does not
 * look like a stat line.
 *
 * The fields before starttime cannot be counted from the left: comm is field 2
 * and holds the executable's name, which may contain spaces and parentheses
 * both. What can be relied on is that comm is wrapped in parentheses and is
 * the only field that may contain them, so the *last* ')' in the line ends it
 * and the token after that is field 3. */
static int proc_start_uptime_s(long pid, long *out)
{
    char path[64];
    char line[1024];
    FILE *f;
    char *p;
    char *end;
    unsigned long long ticks;
    long hz;
    size_t len;
    int token;

    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    len = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    line[len] = '\0';
    p = strrchr(line, ')');
    if (!p) {
        return -1;
    }
    p++;
    /* Step over the nineteen tokens between comm and starttime. */
    for (token = 1; token < SYSD_STAT_STARTTIME_AFTER_COMM; token++) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            return -1;
        }
        while (*p != ' ' && *p != '\0') {
            p++;
        }
    }
    while (*p == ' ') {
        p++;
    }
    ticks = strtoull(p, &end, 10);
    if (end == p) {
        return -1;
    }
    hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) {
        return -1;
    }
    *out = (long)(ticks / (unsigned long long)hz);
    return 0;
}

/* Is the process now holding child_pid the one the supervisor started?
 *
 * kill(pid, 0) only says that a process by that number exists. A supervisor
 * killed outright leaves a file claiming a child that has since gone, and the
 * kernel hands pids out again, so the number on its own can name something
 * else entirely. The supervisor records when it started the child, boot
 * relative; the kernel records when the process holding that pid started, the
 * same way. The two agreeing is what makes it the same process.
 *
 * Everything undecidable answers no. Reporting a service as healthy is the
 * claim that needs evidence. */
static int child_is_the_same_process(const struct svc_state *st)
{
    long proc_start;
    long delta;

    if (kill((pid_t)st->child_pid, 0) != 0 && errno != EPERM) {
        return 0;
    }
    if (!st->have_started_uptime_s) {
        return 0;
    }
    if (proc_start_uptime_s(st->child_pid, &proc_start) != 0) {
        return 0;
    }
    /* The clock was read just before the fork, so the process cannot have
     * started earlier than the supervisor recorded. */
    delta = proc_start - st->started_uptime_s;
    return delta >= 0 && delta <= SYSD_START_TOLERANCE_S;
}

static void add_long_or_null(cJSON *o, const char *key, int have, long value)
{
    if (have) {
        cJSON_AddNumberToObject(o, key, (double)value);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

/* One entry, with the six keys system.status.services has always had. */
static void add_service(cJSON *arr, const char *name, const struct svc_state *st)
{
    cJSON *s = cJSON_CreateObject();
    int running = 0;

    cJSON_AddStringToObject(s, "name", name);
    if (st->version != SYSD_STATE_VERSION) {
        /* A state file from a supervisor this sysd does not understand. The
         * service exists; nothing else here is safe to claim. */
        cJSON_AddNullToObject(s, "pid");
        cJSON_AddBoolToObject(s, "running", 0);
        cJSON_AddBoolToObject(s, "crashloop", 0);
        cJSON_AddNullToObject(s, "last_exit_code");
        cJSON_AddNullToObject(s, "restarts");
        cJSON_AddItemToArray(arr, s);
        return;
    }
    /* The supervisor says whether it has a child; the kernel says whether the
     * process holding that pid is still the same one. Both have to agree. */
    if (st->have_child_pid && st->child_pid > 0) {
        cJSON_AddNumberToObject(s, "pid", (double)st->child_pid);
        running = st->running && child_is_the_same_process(st);
    } else {
        cJSON_AddNullToObject(s, "pid");
    }
    cJSON_AddBoolToObject(s, "running", running);
    cJSON_AddBoolToObject(s, "crashloop", st->crashloop);
    add_long_or_null(s, "last_exit_code", st->have_last_exit_code, st->last_exit_code);
    add_long_or_null(s, "restarts", st->have_restarts, st->restarts);
    cJSON_AddItemToArray(arr, s);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void sysd_services_add(cJSON *status)
{
    char *names[SYSD_MAX_SERVICES];
    cJSON *arr = cJSON_CreateArray();
    const char *run = pocketos_runtime_dir();
    size_t suffix_len = strlen(SYSD_STATE_SUFFIX);
    DIR *d = opendir(run);
    struct dirent *e;
    int n = 0;
    int i;

    if (d) {
        while ((e = readdir(d)) != NULL && n < SYSD_MAX_SERVICES) {
            size_t len = strlen(e->d_name);

            if (len <= suffix_len ||
                strcmp(e->d_name + len - suffix_len, SYSD_STATE_SUFFIX) != 0) {
                continue;
            }
            names[n] = strndup(e->d_name, len - suffix_len);
            if (names[n]) {
                n++;
            }
        }
        closedir(d);
    }
    /* readdir order is not stable; the array is. */
    qsort(names, (size_t)n, sizeof(names[0]), name_cmp);
    for (i = 0; i < n; i++) {
        char path[POCKETOS_PATH_MAX];
        struct svc_state st;

        snprintf(path, sizeof(path), "%s/%s%s", run, names[i], SYSD_STATE_SUFFIX);
        if (read_state(path, &st) == 0) {
            add_service(arr, names[i], &st);
        }
        free(names[i]);
    }
    cJSON_AddItemToObject(status, "services", arr);
}
