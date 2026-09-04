/*
 * System app: kernel, memory, uptime and load from /proc. No services needed.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "app.h"
#include "pocketui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

struct system_app {
    lv_obj_t *model;
    lv_obj_t *kernel;
    lv_obj_t *memory;
    lv_obj_t *uptime;
    lv_obj_t *load;
    lv_obj_t *version;
};

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t len;

    if (!f) {
        return -1;
    }
    len = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[len] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long v = -1;
    size_t klen = strlen(key);

    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static void *system_create(lv_obj_t *root)
{
    struct system_app *a = calloc(1, sizeof(*a));
    lv_obj_t *card;
    struct utsname u;
    char buf[128];

    if (!a) {
        return NULL;
    }
    card = pocketui_card(root);
    a->model = pocketui_kv_row(card, "Model", "-");
    a->kernel = pocketui_kv_row(card, "Kernel", "-");
    a->version = pocketui_kv_row(card, "PocketOS", "-");
    card = pocketui_card(root);
    a->memory = pocketui_kv_row(card, "Memory", "-");
    a->uptime = pocketui_kv_row(card, "Uptime", "-");
    a->load = pocketui_kv_row(card, "Load", "-");

    if (read_line("/proc/device-tree/model", buf, sizeof(buf)) == 0) {
        lv_label_set_text(a->model, buf);
    } else {
        lv_label_set_text(a->model, "PC simulator");
    }
    if (uname(&u) == 0) {
        lv_label_set_text_fmt(a->kernel, "%s %s", u.release, u.machine);
    }
    if (read_line("/etc/pocketos-release", buf, sizeof(buf)) == 0) {
        lv_label_set_text(a->version, buf);
    } else {
        lv_label_set_text(a->version, "dev");
    }
    return a;
}

static void system_tick(void *priv)
{
    struct system_app *a = priv;
    struct sysinfo si;
    long total = meminfo_kb("MemTotal");
    long avail = meminfo_kb("MemAvailable");

    if (total > 0) {
        lv_label_set_text_fmt(a->memory, "%ld / %ld MB free", avail / 1024, total / 1024);
    }
    if (sysinfo(&si) == 0) {
        lv_label_set_text_fmt(a->uptime, "%ldh %02ldm", si.uptime / 3600, (si.uptime / 60) % 60);
        lv_label_set_text_fmt(a->load, "%.2f %.2f %.2f", si.loads[0] / 65536.0,
                              si.loads[1] / 65536.0, si.loads[2] / 65536.0);
    }
}

static void system_destroy(void *priv)
{
    free(priv);
}

const struct pocketos_app app_system = {
    .id = "system",
    .name = "System",
    .icon = LV_SYMBOL_SETTINGS,
    .create = system_create,
    .tick = system_tick,
    .destroy = system_destroy,
};
