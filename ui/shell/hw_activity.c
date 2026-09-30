/*
 * Microphone and camera activity. See hw_activity.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "hw_activity.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool all_digits(const char *s)
{
    if (!*s) {
        return false;
    }
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int hw_activity_init(struct hw_activity *a, const char *proc_root, const char *sys_root)
{
    char dir[256];
    char path[512];
    char name[64];
    struct dirent *e;
    DIR *d;
    FILE *f;

    if (!a) {
        return 0;
    }
    memset(a, 0, sizeof(*a));
    snprintf(a->proc_root, sizeof(a->proc_root), "%s", proc_root ? proc_root : "/proc");
    snprintf(dir, sizeof(dir), "%s/class/video4linux", sys_root ? sys_root : "/sys");
    d = opendir(dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL && a->camera_nodes < HW_ACTIVITY_MAX_CAMERA_NODES) {
        if (!starts(e->d_name, "video") || strlen(e->d_name) > 16) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s/name", dir, e->d_name) >= (int)sizeof(path)) {
            continue;
        }
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        if (fgets(name, sizeof(name), f) && starts(name, "vvcam") &&
            snprintf(a->camera_node[a->camera_nodes], sizeof(a->camera_node[0]), "/dev/%s", e->d_name) <
                (int)sizeof(a->camera_node[0])) {
            a->camera_nodes++;
        }
        fclose(f);
    }
    closedir(d);
    return a->camera_nodes;
}

/* ---- microphone ---------------------------------------------------------- */

/* One substream's status file: "closed" while nobody has it open, else
 * "state: RUNNING" (or SETUP, PREPARED, XRUN...) and more. */
static int substream_open(const char *path)
{
    char line[64];
    FILE *f = fopen(path, "r");
    int open = 0;

    if (!f) {
        return 0;
    }
    if (fgets(line, sizeof(line), f)) {
        open = !starts(line, "closed");
    }
    fclose(f);
    return open;
}

int hw_activity_mic(const struct hw_activity *a)
{
    char asound[160];
    char path[512];
    char card[256];
    char pcm[384];
    struct dirent *ce;
    struct dirent *pe;
    struct dirent *se;
    DIR *cd;
    DIR *pd;
    DIR *sd;
    int active = 0;

    if (!a) {
        return 0;
    }
    snprintf(asound, sizeof(asound), "%s/asound", a->proc_root);
    cd = opendir(asound);
    if (!cd) {
        return 0;
    }
    while (!active && (ce = readdir(cd)) != NULL) {
        if (!starts(ce->d_name, "card") || !all_digits(ce->d_name + 4)) {
            continue;
        }
        if (snprintf(card, sizeof(card), "%s/%s", asound, ce->d_name) >= (int)sizeof(card)) {
            continue;
        }
        pd = opendir(card);
        if (!pd) {
            continue;
        }
        while (!active && (pe = readdir(pd)) != NULL) {
            size_t n = strlen(pe->d_name);

            /* pcm<device>c: a capture stream. */
            if (!starts(pe->d_name, "pcm") || n < 5 || pe->d_name[n - 1] != 'c') {
                continue;
            }
            if (snprintf(pcm, sizeof(pcm), "%s/%s", card, pe->d_name) >= (int)sizeof(pcm)) {
                continue;
            }
            sd = opendir(pcm);
            if (!sd) {
                continue;
            }
            while (!active && (se = readdir(sd)) != NULL) {
                if (!starts(se->d_name, "sub") || !all_digits(se->d_name + 3)) {
                    continue;
                }
                if (snprintf(path, sizeof(path), "%s/%s/status", pcm, se->d_name) < (int)sizeof(path)) {
                    active = substream_open(path);
                }
            }
            closedir(sd);
        }
        closedir(pd);
    }
    closedir(cd);
    return active;
}

/* ---- camera -------------------------------------------------------------- */

static bool is_camera_node(const struct hw_activity *a, const char *target)
{
    int k;

    for (k = 0; k < a->camera_nodes; k++) {
        if (strcmp(target, a->camera_node[k]) == 0) {
            return true;
        }
    }
    return false;
}

/* The vendor's ISP daemon, isp_media_server, opens every capture node at
 * boot and holds them for as long as the board runs (VERIFIED on unit B,
 * 2026-09-30: pid 148 holding /dev/video1..3 with no app open). It is the
 * ISP's broker, not a consumer: counting it would light the camera LED for
 * good. Its comm is cut to the kernel's 15 characters. */
static bool resident(const struct hw_activity *a, const char *pid)
{
    char path[256];
    char comm[32] = "";
    FILE *f;

    if (snprintf(path, sizeof(path), "%s/%s/comm", a->proc_root, pid) >= (int)sizeof(path)) {
        return false;
    }
    f = fopen(path, "r");
    if (!f) {
        return false;
    }
    if (!fgets(comm, sizeof(comm), f)) {
        comm[0] = '\0';
    }
    fclose(f);
    comm[strcspn(comm, "\n")] = '\0';
    return strcmp(comm, HW_ACTIVITY_ISP_DAEMON) == 0;
}

/* Whether this pid has a camera node open, the ISP's own daemon aside. Its
 * fd directory is unreadable for a process that just exited, which is
 * simply "no". */
static bool holds_camera(const struct hw_activity *a, const char *pid)
{
    char dir[256];
    char link[320];
    char target[64];
    struct dirent *e;
    DIR *d;
    ssize_t n;
    bool found = false;

    if (snprintf(dir, sizeof(dir), "%s/%s/fd", a->proc_root, pid) >= (int)sizeof(dir)) {
        return false;
    }
    d = opendir(dir);
    if (!d) {
        return false;
    }
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (snprintf(link, sizeof(link), "%s/%s", dir, e->d_name) >= (int)sizeof(link)) {
            continue;
        }
        n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = '\0';
        found = is_camera_node(a, target);
    }
    closedir(d);
    return found && !resident(a, pid);
}

int hw_activity_camera(struct hw_activity *a)
{
    char pid[24];
    struct dirent *e;
    DIR *d;

    if (!a || a->camera_nodes == 0) {
        return 0;
    }
    if (a->camera_pid > 0) {
        snprintf(pid, sizeof(pid), "%d", a->camera_pid);
        if (holds_camera(a, pid)) {
            return 1;
        }
        a->camera_pid = 0;
    }
    d = opendir(a->proc_root);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (!all_digits(e->d_name)) {
            continue;
        }
        if (holds_camera(a, e->d_name)) {
            sscanf(e->d_name, "%d", &a->camera_pid);
            break;
        }
    }
    closedir(d);
    return a->camera_pid > 0;
}
