/*
 * system_view implementation. See system_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "system_view.h"

#include <stdio.h>
#include <string.h>

/* Stale after three missed two-second polls: long enough not to flicker on a
 * single slow answer, short enough that nobody reads an old number as new. */
#define SYSTEM_VIEW_STALE_AFTER_MS 6000

static void set_text(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : SYSTEM_VIEW_UNKNOWN);
}

/* The one place null becomes a dash. A number - including 0 - is a value and
 * is formatted; only cJSON null, a missing key or a non-number is unknown. */
static int number_of(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (!cJSON_IsNumber(v)) {
        return 0;
    }
    *out = v->valuedouble;
    return 1;
}

static const char *string_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void metric(struct system_view_metric *m, const char *label, const char *value, int warn)
{
    snprintf(m->label, sizeof(m->label), "%s", label);
    set_text(m->value, sizeof(m->value), value);
    m->warn = warn;
}

static void metric_unknown(struct system_view_metric *m, const char *label)
{
    metric(m, label, SYSTEM_VIEW_UNKNOWN, 0);
}

void system_view_init(struct system_view *v)
{
    memset(v, 0, sizeof(*v));
    set_text(v->os_version, sizeof(v->os_version), NULL);
    set_text(v->card_version, sizeof(v->card_version), NULL);
    set_text(v->model, sizeof(v->model), NULL);
    set_text(v->kernel, sizeof(v->kernel), NULL);
    set_text(v->radio_state, sizeof(v->radio_state), "--");
    set_text(v->radio_detail, sizeof(v->radio_detail), NULL);
    metric_unknown(&v->vitals[0], "CPU");
    metric_unknown(&v->vitals[1], "TEMP");
    metric_unknown(&v->vitals[2], "MEMORY");
    metric_unknown(&v->vitals[3], "LOAD");
    metric_unknown(&v->vitals[4], "UPTIME");
    metric_unknown(&v->vitals[5], "CLOCK");
    snprintf(v->freshness, sizeof(v->freshness), "%s", SYSTEM_VIEW_UNKNOWN);
}

/* ---- identity ---------------------------------------------------------- */

/* "0.0.7 · fdc795f", or as much of it as is known. */
static void version_pair(char *dst, size_t n, const char *version, const char *build)
{
    if (version && build) {
        snprintf(dst, n, "%s \xC2\xB7 %s", version, build);
    } else if (version) {
        snprintf(dst, n, "%s", version);
    } else if (build) {
        snprintf(dst, n, "%s", build);
    } else {
        snprintf(dst, n, "%s", SYSTEM_VIEW_UNKNOWN);
    }
}

static int same_or_both_absent(const char *a, const char *b)
{
    if (!a && !b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

void system_view_apply_info(struct system_view *v, const cJSON *info)
{
    const char *version;
    const char *build;
    const char *release;
    const char *release_build;
    const char *machine;
    const char *kernel;

    if (!info) {
        return;
    }
    version = string_of(info, "version");
    build = string_of(info, "build");
    release = string_of(info, "release_file");
    release_build = string_of(info, "release_build");
    kernel = string_of(info, "kernel");
    machine = string_of(info, "machine");

    version_pair(v->os_version, sizeof(v->os_version), version, build);
    version_pair(v->card_version, sizeof(v->card_version), release, release_build);
    /* The card row earns its place only by disagreeing: it exists to catch a
     * bench deployment, so its presence is the signal (v0.0.6 M7). */
    v->show_card = !(same_or_both_absent(version, release) &&
                     same_or_both_absent(build, release_build));
    set_text(v->model, sizeof(v->model), string_of(info, "model"));
    if (kernel && machine) {
        snprintf(v->kernel, sizeof(v->kernel), "%s %s", kernel, machine);
    } else {
        set_text(v->kernel, sizeof(v->kernel), kernel);
    }
    v->have_info = 1;
}

/* ---- vitals ------------------------------------------------------------ */

static void vitals_from(struct system_view *v, const cJSON *status)
{
    const cJSON *mem = cJSON_GetObjectItemCaseSensitive(status, "memory");
    const cJSON *load = cJSON_GetObjectItemCaseSensitive(status, "load");
    const cJSON *clock = cJSON_GetObjectItemCaseSensitive(status, "clock_set");
    char buf[SYSTEM_VIEW_TEXT];
    double d;

    if (number_of(status, "cpu_percent", &d)) {
        /* 0 is a measurement of an idle board, not an absence. */
        snprintf(buf, sizeof(buf), "%.0f %%", d);
        metric(&v->vitals[0], "CPU", buf, 0);
    } else {
        metric_unknown(&v->vitals[0], "CPU");
    }

    if (number_of(status, "temperature_c", &d)) {
        snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C", d);
        metric(&v->vitals[1], "TEMP", buf, 0);
    } else {
        metric_unknown(&v->vitals[1], "TEMP");
    }

    if (cJSON_IsObject(mem)) {
        double total;
        double avail;
        int have_total = number_of(mem, "total_kb", &total);
        int have_avail = number_of(mem, "available_kb", &avail);

        if (have_total && have_avail) {
            snprintf(buf, sizeof(buf), "%ld/%ld MB", (long)(avail / 1024), (long)(total / 1024));
        } else if (have_total) {
            snprintf(buf, sizeof(buf), "%s/%ld MB", SYSTEM_VIEW_UNKNOWN, (long)(total / 1024));
        } else {
            snprintf(buf, sizeof(buf), "%s", SYSTEM_VIEW_UNKNOWN);
        }
        metric(&v->vitals[2], "MEMORY", buf, 0);
    } else {
        metric_unknown(&v->vitals[2], "MEMORY");
    }

    if (cJSON_IsArray(load) && cJSON_GetArraySize(load) > 0 &&
        cJSON_IsNumber(cJSON_GetArrayItem(load, 0))) {
        snprintf(buf, sizeof(buf), "%.2f", cJSON_GetArrayItem(load, 0)->valuedouble);
        metric(&v->vitals[3], "LOAD", buf, 0);
    } else {
        metric_unknown(&v->vitals[3], "LOAD");
    }

    if (number_of(status, "uptime_s", &d)) {
        long s = (long)d;

        if (s >= 86400) {
            snprintf(buf, sizeof(buf), "%ldd %ldh", s / 86400, (s % 86400) / 3600);
        } else {
            snprintf(buf, sizeof(buf), "%ldh %02ldm", s / 3600, (s / 60) % 60);
        }
        metric(&v->vitals[4], "UPTIME", buf, 0);
    } else {
        metric_unknown(&v->vitals[4], "UPTIME");
    }

    /* No RTC on this board: an unset clock means every timestamp on the
     * screen is boot-relative nonsense, which is worth saying out loud. */
    if (cJSON_IsBool(clock)) {
        int set = cJSON_IsTrue(clock);

        metric(&v->vitals[5], "CLOCK", set ? "synced" : "not set", set ? 0 : 1);
    } else {
        metric_unknown(&v->vitals[5], "CLOCK");
    }
}

/* ---- storage ----------------------------------------------------------- */

static void mounts_from(struct system_view *v, const cJSON *status)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(status, "storage");
    const cJSON *e;

    v->mount_count = 0;
    if (!cJSON_IsArray(arr)) {
        return;
    }
    cJSON_ArrayForEach(e, arr) {
        struct system_view_mount *m;
        double total;
        double avail;
        int have_total;
        int have_avail;

        if (v->mount_count >= SYSTEM_VIEW_MAX_MOUNTS) {
            break;
        }
        m = &v->mounts[v->mount_count];
        memset(m, 0, sizeof(*m));
        set_text(m->mount, sizeof(m->mount), string_of(e, "mount"));
        have_total = number_of(e, "total_bytes", &total);
        have_avail = number_of(e, "avail_bytes", &avail);
        /* MB reads better for the card (574 MB root) but runs off the row on
         * anything bigger, so a volume of 10 GB or more switches units rather
         * than truncating the number a reader came for. */
        if (have_total && have_avail) {
            if (total >= 10.0 * 1024 * 1024 * 1024) {
                snprintf(m->detail, sizeof(m->detail), "%.1f GB free of %.1f GB",
                         avail / (1024.0 * 1024.0 * 1024.0), total / (1024.0 * 1024.0 * 1024.0));
            } else {
                snprintf(m->detail, sizeof(m->detail), "%ld MB free of %ld MB",
                         (long)(avail / (1024 * 1024)), (long)(total / (1024 * 1024)));
            }
        } else if (have_total) {
            snprintf(m->detail, sizeof(m->detail), "%s free of %ld MB", SYSTEM_VIEW_UNKNOWN,
                     (long)(total / (1024 * 1024)));
        } else {
            snprintf(m->detail, sizeof(m->detail), "%s", SYSTEM_VIEW_UNKNOWN);
        }
        /* A meter needs a real denominator. A zero or negative total is not a
         * full disk, it is no answer, and it must not divide. */
        if (have_total && have_avail && total > 0.0 && avail >= 0.0 && avail <= total) {
            double used = (total - avail) / total * 100.0;

            m->used_percent = (int)(used + 0.5);
            if (m->used_percent < 0) {
                m->used_percent = 0;
            } else if (m->used_percent > 100) {
                m->used_percent = 100;
            }
            m->have_percent = 1;
        }
        v->mount_count++;
    }
}

/* ---- network ----------------------------------------------------------- */

static int is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* A normal six-octet MAC, and nothing else. This is the whole of the screen's
 * network filter: the pseudo interfaces the kernel exposes alongside the real
 * ones (sit0 reports the four-octet 00:00:00:00 on unit A) have no such
 * address, and one rule beats a list of names to keep up with as the kernel
 * grows tunnel types. system.status is not filtered - `pos system status`
 * keeps showing everything, and this only decides what a person sees. */
static int is_six_octet_mac(const char *mac)
{
    int i;

    if (!mac || strlen(mac) != 17) {
        return 0;
    }
    for (i = 0; i < 17; i++) {
        if ((i % 3) == 2) {
            if (mac[i] != ':') {
                return 0;
            }
        } else if (!is_hex(mac[i])) {
            return 0;
        }
    }
    return 1;
}

static void ifaces_from(struct system_view *v, const cJSON *status)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(status, "network");
    const cJSON *e;

    v->iface_count = 0;
    v->ifaces_hidden = 0;
    if (!cJSON_IsArray(arr)) {
        return;
    }
    cJSON_ArrayForEach(e, arr) {
        struct system_view_iface *n;
        const char *operstate;

        if (!is_six_octet_mac(string_of(e, "mac"))) {
            v->ifaces_hidden++;
            continue;
        }
        if (v->iface_count >= SYSTEM_VIEW_MAX_IFACES) {
            v->ifaces_hidden++;
            continue;
        }
        n = &v->ifaces[v->iface_count];
        memset(n, 0, sizeof(*n));
        set_text(n->name, sizeof(n->name), string_of(e, "name"));
        operstate = string_of(e, "operstate");
        if (operstate) {
            size_t i;

            snprintf(n->state, sizeof(n->state), "%s", operstate);
            for (i = 0; n->state[i]; i++) {
                if (n->state[i] >= 'a' && n->state[i] <= 'z') {
                    n->state[i] = (char)(n->state[i] - 'a' + 'A');
                }
            }
            n->up = strcmp(operstate, "up") == 0;
        } else {
            snprintf(n->state, sizeof(n->state), "%s", "?");
        }
        set_text(n->addr, sizeof(n->addr), string_of(e, "ipv4"));
        v->iface_count++;
    }
}

/* ---- services ---------------------------------------------------------- */

static void services_from(struct system_view *v, const cJSON *status)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(status, "services");
    const cJSON *e;

    v->service_count = 0;
    if (!cJSON_IsArray(arr)) {
        return;
    }
    cJSON_ArrayForEach(e, arr) {
        struct system_view_service *s;
        const cJSON *crashloop;
        const cJSON *running;
        double d;

        if (v->service_count >= SYSTEM_VIEW_MAX_SERVICES) {
            break;
        }
        s = &v->services[v->service_count];
        memset(s, 0, sizeof(*s));
        set_text(s->name, sizeof(s->name), string_of(e, "name"));
        crashloop = cJSON_GetObjectItemCaseSensitive(e, "crashloop");
        running = cJSON_GetObjectItemCaseSensitive(e, "running");

        if (cJSON_IsTrue(crashloop)) {
            char exitbit[32];
            char restartbit[32];

            s->state = SYSTEM_VIEW_SVC_CRASHLOOP;
            /* Only what is known. An unknown exit code is left out of the
             * sentence rather than printed as a dash next to a word. */
            if (number_of(e, "last_exit_code", &d)) {
                snprintf(exitbit, sizeof(exitbit), "exit %ld", (long)d);
            } else {
                exitbit[0] = '\0';
            }
            if (number_of(e, "restarts", &d)) {
                /* The window is part of the number: this is not a lifetime
                 * total and must never be read as one. Written tight because
                 * a row that truncates loses the window first, which is the
                 * half that stops it being misread. */
                snprintf(restartbit, sizeof(restartbit), "%ld restarts/60s", (long)d);
            } else {
                restartbit[0] = '\0';
            }
            if (exitbit[0] && restartbit[0]) {
                snprintf(s->detail, sizeof(s->detail), "%s \xC2\xB7 %s", exitbit, restartbit);
            } else if (exitbit[0]) {
                snprintf(s->detail, sizeof(s->detail), "%s", exitbit);
            } else if (restartbit[0]) {
                snprintf(s->detail, sizeof(s->detail), "%s", restartbit);
            } else {
                snprintf(s->detail, sizeof(s->detail), "%s", SYSTEM_VIEW_UNKNOWN);
            }
        } else if (cJSON_IsTrue(running)) {
            s->state = SYSTEM_VIEW_SVC_RUNNING;
            if (number_of(e, "pid", &d)) {
                snprintf(s->detail, sizeof(s->detail), "pid %ld", (long)d);
            } else {
                snprintf(s->detail, sizeof(s->detail), "%s", SYSTEM_VIEW_UNKNOWN);
            }
        } else {
            /* Stopped is stopped. The API cannot say why, so neither does
             * the screen. */
            s->state = SYSTEM_VIEW_SVC_STOPPED;
            snprintf(s->detail, sizeof(s->detail), "%s", SYSTEM_VIEW_UNKNOWN);
        }
        v->service_count++;
    }
}

/* ---- freshness --------------------------------------------------------- */

void system_view_refresh_freshness(struct system_view *v, unsigned long now_ms)
{
    unsigned long age;

    if (!v->have_status) {
        snprintf(v->freshness, sizeof(v->freshness), "%s", SYSTEM_VIEW_UNKNOWN);
        return;
    }
    age = now_ms - v->last_ok_ms;
    if (age >= SYSTEM_VIEW_STALE_AFTER_MS) {
        v->stale = 1;
        snprintf(v->freshness, sizeof(v->freshness), "STALE %lus", age / 1000u);
    } else {
        v->stale = 0;
        snprintf(v->freshness, sizeof(v->freshness), "LIVE");
    }
}

void system_view_apply_status(struct system_view *v, const cJSON *status, unsigned long now_ms)
{
    if (!status) {
        /* The poll failed. Everything on screen stays exactly as it was: a
         * lost connection is not news about the machine, and blanking the
         * numbers would throw away the last thing anybody knew. */
        system_view_refresh_freshness(v, now_ms);
        return;
    }
    vitals_from(v, status);
    mounts_from(v, status);
    ifaces_from(v, status);
    services_from(v, status);
    v->have_status = 1;
    v->last_ok_ms = now_ms;
    system_view_refresh_freshness(v, now_ms);
}

void system_view_set_radio_state(struct system_view *v, const char *state)
{
    v->radio_state_known = state != NULL;
    if (state) {
        size_t i;

        snprintf(v->radio_state, sizeof(v->radio_state), "%s", state);
        for (i = 0; v->radio_state[i]; i++) {
            if (v->radio_state[i] >= 'a' && v->radio_state[i] <= 'z') {
                v->radio_state[i] = (char)(v->radio_state[i] - 'a' + 'A');
            }
        }
    } else {
        /* radiod is not answering. "--" and not an em dash: the status bar
         * chip uses the symbol font, which has none. */
        snprintf(v->radio_state, sizeof(v->radio_state), "--");
    }
}

enum system_view_radio_chip system_view_radio_chip_state(const struct system_view *v)
{
    if (!v->radio_state_known) {
        return SYSTEM_VIEW_RADIO_UNKNOWN;
    }
    if (strcmp(v->radio_state, "RX") == 0) {
        return SYSTEM_VIEW_RADIO_RX;
    }
    if (strcmp(v->radio_state, "TX") == 0) {
        return SYSTEM_VIEW_RADIO_TX;
    }
    return SYSTEM_VIEW_RADIO_IDLE;
}

void system_view_set_radio_detail(struct system_view *v, const char *region, const char *backend)
{
    if (region && backend) {
        snprintf(v->radio_detail, sizeof(v->radio_detail), "%s \xC2\xB7 %s", region, backend);
    } else if (region) {
        snprintf(v->radio_detail, sizeof(v->radio_detail), "%s", region);
    } else if (backend) {
        snprintf(v->radio_detail, sizeof(v->radio_detail), "%s", backend);
    } else {
        snprintf(v->radio_detail, sizeof(v->radio_detail), "%s", SYSTEM_VIEW_UNKNOWN);
    }
}

/* ---- the two destructive actions --------------------------------------- */

int system_view_is_polling(const struct system_view *v)
{
    return v->phase != SYSTEM_VIEW_TERMINAL_REBOOT && v->phase != SYSTEM_VIEW_TERMINAL_POWEROFF;
}

void system_view_request(struct system_view *v, enum system_view_action action)
{
    if (!system_view_is_polling(v)) {
        return;
    }
    v->error[0] = '\0';
    v->phase = action == SYSTEM_VIEW_ACTION_REBOOT ? SYSTEM_VIEW_CONFIRM_REBOOT
                                                   : SYSTEM_VIEW_CONFIRM_POWEROFF;
}

void system_view_cancel(struct system_view *v)
{
    if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT || v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        v->phase = SYSTEM_VIEW_LIVE;
    }
}

const char *system_view_confirm(struct system_view *v)
{
    /* The only route from this screen to a method that stops the machine, and
     * it opens only from a confirmation. Everything else returns NULL, so the
     * app has nothing to call. */
    if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT) {
        return "system.reboot";
    }
    if (v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        return "system.poweroff";
    }
    return NULL;
}

void system_view_action_ok(struct system_view *v)
{
    if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT) {
        v->phase = SYSTEM_VIEW_TERMINAL_REBOOT;
    } else if (v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        v->phase = SYSTEM_VIEW_TERMINAL_POWEROFF;
    }
    v->error[0] = '\0';
}

void system_view_action_failed(struct system_view *v, const char *message)
{
    if (v->phase != SYSTEM_VIEW_CONFIRM_REBOOT && v->phase != SYSTEM_VIEW_CONFIRM_POWEROFF) {
        return;
    }
    v->phase = SYSTEM_VIEW_LIVE;
    snprintf(v->error, sizeof(v->error), "%s",
             message && message[0] ? message : "sysd is not answering");
}

enum system_view_emphasis system_view_dialog_emphasis(const struct system_view *v)
{
    /* A restart costs about thirty-five seconds and undoes itself. A
     * power-off costs a walk to the bench and thirty seconds with the cable
     * out, so the glass must not put the brighter treatment on it. */
    return v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF ? SYSTEM_VIEW_EMPHASIS_CANCEL
                                                    : SYSTEM_VIEW_EMPHASIS_CONFIRM;
}

const char *system_view_dialog_title(const struct system_view *v)
{
    if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT) {
        return "Restart Doors?";
    }
    if (v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        return "Power off Doors?";
    }
    return NULL;
}

const char *system_view_dialog_body(const struct system_view *v)
{
    if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT) {
        return "Services stop and the board reboots. This takes about 35 seconds.";
    }
    if (v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        /* The recovery sentence is not decoration. VERIFIED on unit A
         * 2026-09-10: re-plugging USB does not restart the board
         * (docs/hardware/V0.0.7_BLOCK2C_SMOKE.md). */
        return "The board will shut down and cannot be restarted remotely. "
               "To switch it back on, disconnect USB power for about 30 seconds, "
               "then reconnect.";
    }
    return NULL;
}

const char *system_view_dialog_confirm_label(const struct system_view *v)
{
    if (v->phase == SYSTEM_VIEW_CONFIRM_REBOOT) {
        return "Restart";
    }
    if (v->phase == SYSTEM_VIEW_CONFIRM_POWEROFF) {
        return "Power off";
    }
    return NULL;
}

const char *system_view_terminal_text(const struct system_view *v)
{
    if (v->phase == SYSTEM_VIEW_TERMINAL_REBOOT) {
        return "Restarting...";
    }
    if (v->phase == SYSTEM_VIEW_TERMINAL_POWEROFF) {
        return "Powering off...\n\nDisconnect USB power for about 30 seconds, "
               "then reconnect to switch the board back on.";
    }
    return NULL;
}
