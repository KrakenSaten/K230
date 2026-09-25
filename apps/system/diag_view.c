/*
 * System > Diagnostics, the decisions. See diag_view.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "diag_view.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define UNKNOWN SYSTEM_VIEW_UNKNOWN
/* The same line as system.status.clock_set: before it, a time is not one. */
#define DIAG_CLOCK_PLAUSIBLE_AFTER 1735689600L

static const cJSON *get(const cJSON *o, const char *k)
{
    return cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = get(o, k);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Copy at most n - 1 bytes, never splitting a UTF-8 character. */
static void copy_text(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);

    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void row_set(struct diag_view *v, enum diag_row r, const char *value, int warn)
{
    copy_text(v->rows[r].value, sizeof(v->rows[r].value), value && *value ? value : UNKNOWN);
    v->rows[r].warn = warn;
}

void diag_view_init(struct diag_view *v)
{
    static const char *const labels[DIAG_ROW_COUNT] = {
        "Doors", "Uptime", "Memory", "Storage", "Battery",
        "Services", "LoRa radio", "Mesh", "Bluetooth", "Crashes"
    };
    int i;

    memset(v, 0, sizeof(*v));
    for (i = 0; i < DIAG_ROW_COUNT; i++) {
        snprintf(v->rows[i].label, sizeof(v->rows[i].label), "%s", labels[i]);
        row_set(v, (enum diag_row)i, NULL, 0);
    }
    snprintf(v->log_status, sizeof(v->log_status), "Not read yet");
}

/* ---- refreshing in steps ------------------------------------------------- */

void diag_view_refresh(struct diag_view *v)
{
    if (v->step == DIAG_STEP_IDLE) {
        v->step = DIAG_STEP_STATUS;
    }
}

void diag_view_set_filter(struct diag_view *v, enum diag_filter f)
{
    if (f < 0 || f >= DIAG_FILTER_COUNT) {
        return;
    }
    v->filter = f;
    /* A full refresh in progress reaches the log last and will ask with the
     * new filter; otherwise only the log is asked for again. */
    if (v->step == DIAG_STEP_IDLE) {
        v->step = DIAG_STEP_LOGS;
    }
}

enum diag_step diag_view_step(const struct diag_view *v)
{
    return v->step;
}

void diag_view_step_done(struct diag_view *v)
{
    switch (v->step) {
    case DIAG_STEP_STATUS:
        v->step = DIAG_STEP_RADIO;
        break;
    case DIAG_STEP_RADIO:
        v->step = DIAG_STEP_MESH;
        break;
    case DIAG_STEP_MESH:
        v->step = DIAG_STEP_CRASHES;
        break;
    case DIAG_STEP_CRASHES:
        v->step = DIAG_STEP_LOGS;
        break;
    case DIAG_STEP_LOGS:
    case DIAG_STEP_IDLE:
    default:
        v->step = DIAG_STEP_IDLE;
        break;
    }
}

int diag_view_busy(const struct diag_view *v)
{
    return v->step != DIAG_STEP_IDLE;
}

const char *diag_view_filter_label(enum diag_filter f)
{
    switch (f) {
    case DIAG_FILTER_WARN:
        return "Warnings";
    case DIAG_FILTER_ERROR:
        return "Errors";
    case DIAG_FILTER_ALL:
    default:
        return "All";
    }
}

cJSON *diag_view_logs_params(const struct diag_view *v)
{
    static const char *const levels[DIAG_FILTER_COUNT] = { "all", "warn", "error" };
    cJSON *p = cJSON_CreateObject();

    cJSON_AddStringToObject(p, "level", levels[v->filter < DIAG_FILTER_COUNT ? v->filter : 0]);
    cJSON_AddNumberToObject(p, "limit", DIAG_LOG_MAX);
    return p;
}

/* ---- sysd: the machine ---------------------------------------------------- */

static const char *battery_word(const char *st)
{
    if (st && strcmp(st, "charging") == 0) {
        return "Charging";
    }
    if (st && strcmp(st, "discharging") == 0) {
        return "On battery";
    }
    if (st && strcmp(st, "full") == 0) {
        return "Full";
    }
    if (st && strcmp(st, "not_charging") == 0) {
        return "Not charging";
    }
    return NULL;
}

static void battery_row(struct diag_view *v, const cJSON *status)
{
    const cJSON *power = get(status, "power");
    const cJSON *bat = get(power, "battery");
    const cJSON *cap = get(bat, "capacity_percent");
    const char *w = battery_word(str(bat, "status"));
    char line[DIAG_TEXT];

    if (!cJSON_IsObject(power)) {
        row_set(v, DIAG_ROW_BATTERY, NULL, 0);
    } else if (!cJSON_IsObject(bat)) {
        const char *src = str(power, "source");

        row_set(v, DIAG_ROW_BATTERY,
                src && strcmp(src, "external") == 0 ? "No battery, external power" : NULL, 0);
    } else if (cJSON_IsFalse(get(bat, "present"))) {
        row_set(v, DIAG_ROW_BATTERY, "No battery", 0);
    } else if (cJSON_IsNumber(cap) && cap->valuedouble >= 0 && cap->valuedouble <= 100) {
        const cJSON *volt = get(bat, "voltage_v");

        snprintf(line, sizeof(line), "%d %%%s%s", (int)cap->valuedouble, w ? " \xC2\xB7 " : "",
                 w ? w : "");
        if (cJSON_IsNumber(volt)) {
            size_t n = strlen(line);

            snprintf(line + n, sizeof(line) - n, " \xC2\xB7 %.2f V", volt->valuedouble);
        }
        /* Low and not charging is the one battery state worth a warning. */
        row_set(v, DIAG_ROW_BATTERY, line,
                cap->valuedouble <= 15 && !(w && strcmp(w, "Charging") == 0));
    } else {
        row_set(v, DIAG_ROW_BATTERY, w ? w : "Battery, level unknown", 0);
    }
}

/* Services: the supervisor's view, reduced to what is wrong, if anything. */
static void services_row(struct diag_view *v, const cJSON *status)
{
    const cJSON *arr = get(status, "services");
    const cJSON *s;
    char bad[DIAG_TEXT] = "";
    char restarted[DIAG_TEXT] = "";
    int running = 0;
    int total = 0;
    int warn = 0;
    char line[2 * DIAG_TEXT];

    if (!cJSON_IsArray(arr)) {
        row_set(v, DIAG_ROW_SERVICES, NULL, 0);
        return;
    }
    cJSON_ArrayForEach(s, arr) {
        const char *name = str(s, "name");
        const cJSON *restarts = get(s, "restarts");
        size_t n;

        if (!name) {
            continue;
        }
        total++;
        if (cJSON_IsTrue(get(s, "crashloop"))) {
            n = strlen(bad);
            snprintf(bad + n, sizeof(bad) - n, "%s%s crash loop", bad[0] ? ", " : "", name);
            warn = 1;
        } else if (cJSON_IsTrue(get(s, "running"))) {
            running++;
            if (cJSON_IsNumber(restarts) && restarts->valuedouble > 0) {
                n = strlen(restarted);
                snprintf(restarted + n, sizeof(restarted) - n, "%s%s %dx", restarted[0] ? ", " : "",
                         name, (int)restarts->valuedouble);
                warn = 1;
            }
        } else {
            n = strlen(bad);
            snprintf(bad + n, sizeof(bad) - n, "%s%s stopped", bad[0] ? ", " : "", name);
            warn = 1;
        }
    }
    if (total == 0) {
        row_set(v, DIAG_ROW_SERVICES, "None supervised", 0);
        return;
    }
    if (bad[0]) {
        snprintf(line, sizeof(line), "%s", bad);
    } else if (restarted[0]) {
        snprintf(line, sizeof(line), "%d running \xC2\xB7 restarted: %s", running, restarted);
    } else {
        snprintf(line, sizeof(line), "%d running", running);
    }
    row_set(v, DIAG_ROW_SERVICES, line, warn);
}

void diag_view_apply_system(struct diag_view *v, const struct system_view *sv, const cJSON *status)
{
    const cJSON *ctl;
    int i;

    if (sv) {
        row_set(v, DIAG_ROW_VERSION, sv->os_version, 0);
    }
    if (!status) {
        /* The rest is sysd's; saying "not answering" once is enough. */
        row_set(v, DIAG_ROW_SERVICES, "sysd not answering", 1);
        return;
    }
    if (sv) {
        row_set(v, DIAG_ROW_UPTIME, sv->vitals[4].value, 0);
        row_set(v, DIAG_ROW_MEMORY, sv->vitals[2].value, sv->vitals[2].warn);
        row_set(v, DIAG_ROW_STORAGE, NULL, 0);
        for (i = 0; i < sv->mount_count && i < SYSTEM_VIEW_MAX_MOUNTS; i++) {
            if (strcmp(sv->mounts[i].mount, "/") == 0) {
                row_set(v, DIAG_ROW_STORAGE, sv->mounts[i].detail,
                        sv->mounts[i].have_percent && sv->mounts[i].used_percent >= 90);
            }
        }
    }
    battery_row(v, status);
    services_row(v, status);
    ctl = get(get(status, "bluetooth"), "controllers");
    if (!cJSON_IsArray(ctl) || cJSON_GetArraySize(ctl) == 0) {
        row_set(v, DIAG_ROW_BLUETOOTH, "No controller", 0);
    } else {
        char line[DIAG_TEXT] = "";
        const cJSON *c;

        cJSON_ArrayForEach(c, ctl) {
            size_t n = strlen(line);

            if (cJSON_IsString(c)) {
                snprintf(line + n, sizeof(line) - n, "%s%s", line[0] ? ", " : "", c->valuestring);
            }
        }
        row_set(v, DIAG_ROW_BLUETOOTH, line, 0);
    }
}

/* ---- radiod and meshcored ---------------------------------------------------- */

void diag_view_apply_radio(struct diag_view *v, const cJSON *radio_status)
{
    const char *state = str(radio_status, "state");
    const cJSON *enabled = get(radio_status, "enabled");
    const cJSON *prof = get(radio_status, "profile");
    const cJSON *f = get(prof, "frequency_mhz");
    const cJSON *sf = get(prof, "spreading_factor");
    char line[DIAG_TEXT];
    const char *word;
    int warn = 0;

    if (!radio_status || !state) {
        row_set(v, DIAG_ROW_RADIO, "radiod not answering", 1);
        return;
    }
    if (strcmp(state, "off") == 0 || cJSON_IsFalse(enabled)) {
        row_set(v, DIAG_ROW_RADIO, "Off (switched off)", 0);
        return;
    }
    if (strcmp(state, "rx") == 0) {
        word = "Receiving";
    } else if (strcmp(state, "tx") == 0) {
        word = "Transmitting";
    } else if (strcmp(state, "error") == 0) {
        word = "On, not receiving";
        warn = 1;
    } else {
        word = state;
    }
    if (cJSON_IsNumber(f) && cJSON_IsNumber(sf)) {
        snprintf(line, sizeof(line), "%s \xC2\xB7 %.3f MHz SF%d", word, f->valuedouble,
                 (int)sf->valuedouble);
    } else {
        snprintf(line, sizeof(line), "%s", word);
    }
    if (cJSON_IsTrue(get(radio_status, "profile_uncertain"))) {
        size_t n = strlen(line);

        snprintf(line + n, sizeof(line) - n, " \xC2\xB7 profile uncertain");
        warn = 1;
    }
    row_set(v, DIAG_ROW_RADIO, line, warn);
}

void diag_view_apply_mesh(struct diag_view *v, const cJSON *mesh_status)
{
    const char *state = str(mesh_status, "state");
    const char *reason = str(mesh_status, "reason");
    const char *radio_state = str(get(mesh_status, "radio"), "radio_state");
    char line[DIAG_TEXT];

    if (!mesh_status || !state) {
        /* meshcored is off by default (MESHCORED_ENABLE=0): not answering is
         * the ordinary answer on most cards, not a fault. */
        row_set(v, DIAG_ROW_MESH, "meshcored not running", 0);
        return;
    }
    if (strcmp(state, "online") == 0) {
        row_set(v, DIAG_ROW_MESH, "Online", 0);
        return;
    }
    if (strcmp(state, "degraded") == 0 && radio_state && strcmp(radio_state, "off") == 0) {
        row_set(v, DIAG_ROW_MESH, "Waiting: the radio is switched off", 0);
        return;
    }
    if (reason && *reason) {
        snprintf(line, sizeof(line), "%s: %s", state, reason);
    } else {
        snprintf(line, sizeof(line), "%s", state);
    }
    row_set(v, DIAG_ROW_MESH, line,
            strcmp(state, "error") == 0 || strcmp(state, "degraded") == 0);
}

/* ---- crash reports ------------------------------------------------------------ */

/* "09-24 13:20" from a UNIX time, or a note that the clock was not set when
 * the report was written (this board has no RTC). */
static void short_time(long t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;

    if (t < DIAG_CLOCK_PLAUSIBLE_AFTER || !gmtime_r(&tt, &tm)) {
        snprintf(out, n, "clock not set");
        return;
    }
    snprintf(out, n, "%02d-%02d %02d:%02d UTC", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void diag_view_apply_crashes(struct diag_view *v, const cJSON *crashes)
{
    const cJSON *reps = get(crashes, "reports");
    const cJSON *total = get(crashes, "total");
    const cJSON *r;
    char line[DIAG_TEXT];
    int n = 0;

    if (!crashes) {
        row_set(v, DIAG_ROW_CRASHES, "sysd not answering", 1);
        return;
    }
    v->crash_count = 0;
    cJSON_ArrayForEach(r, reps) {
        const char *proc = str(r, "process");
        const char *sig = str(r, "signal_name");
        const cJSON *signum = get(r, "signal");
        const cJSON *t = get(r, "time");
        const cJSON *frames = get(r, "frames");
        const cJSON *f0 = cJSON_IsArray(frames) ? cJSON_GetArrayItem(frames, 0) : NULL;
        char when[32];
        char signame[24];
        struct diag_crash *c;

        if (n >= DIAG_CRASH_MAX) {
            break;
        }
        c = &v->crashes[n++];
        short_time(cJSON_IsNumber(t) ? (long)t->valuedouble : 0, when, sizeof(when));
        if (sig) {
            snprintf(signame, sizeof(signame), "%s", sig);
        } else if (cJSON_IsNumber(signum)) {
            snprintf(signame, sizeof(signame), "signal %d", (int)signum->valuedouble);
        } else {
            snprintf(signame, sizeof(signame), "signal unknown");
        }
        snprintf(c->line, sizeof(c->line), "%s \xC2\xB7 %s \xC2\xB7 %s", proc ? proc : UNKNOWN, signame,
                 when);
        snprintf(c->frame, sizeof(c->frame), "%s", cJSON_IsString(f0) ? f0->valuestring : "");
    }
    v->crash_count = n;
    v->crash_generation++;
    if (!cJSON_IsNumber(total) || total->valuedouble <= 0) {
        row_set(v, DIAG_ROW_CRASHES, "None", 0);
    } else {
        snprintf(line, sizeof(line), "%d report%s", (int)total->valuedouble,
                 total->valuedouble == 1 ? "" : "s");
        row_set(v, DIAG_ROW_CRASHES, line, 1);
    }
}

/* ---- the log --------------------------------------------------------------- */

/* "09-24 13:20:01" from "2026-09-24T13:20:01.123Z". */
static void log_time(const char *ts, char *out, size_t n)
{
    if (ts && strlen(ts) >= 19 && ts[4] == '-' && ts[10] == 'T') {
        snprintf(out, n, "%.5s %.8s", ts + 5, ts + 11);
    } else {
        snprintf(out, n, "%s", UNKNOWN);
    }
}

void diag_view_apply_logs(struct diag_view *v, const cJSON *logs)
{
    const cJSON *entries = get(logs, "entries");
    const cJSON *skipped = get(logs, "skipped");
    const cJSON *e;
    int n = 0;
    size_t len;

    if (!logs) {
        /* Keep what is on screen; say why it did not change. */
        snprintf(v->log_status, sizeof(v->log_status), "sysd not answering; the list is from before");
        return;
    }
    if (cJSON_IsFalse(get(logs, "available"))) {
        v->log_count = 0;
        v->log_generation++;
        snprintf(v->log_status, sizeof(v->log_status), "No log directory on this card");
        return;
    }
    cJSON_ArrayForEach(e, entries) {
        struct diag_log_entry *d;
        const char *level = str(e, "level");
        const char *source = str(e, "source");
        const char *msg = str(e, "message");
        char when[24];

        if (n >= DIAG_LOG_MAX) {
            break;
        }
        d = &v->log[n++];
        log_time(str(e, "ts"), when, sizeof(when));
        if (level && strcmp(level, "error") == 0) {
            d->severity = DIAG_SEV_ERROR;
        } else if (level && strcmp(level, "warn") == 0) {
            d->severity = DIAG_SEV_WARN;
        } else {
            d->severity = DIAG_SEV_INFO;
        }
        snprintf(d->head, sizeof(d->head), "%s  %s  %s", when, source ? source : UNKNOWN,
                 d->severity == DIAG_SEV_ERROR ? "ERROR" : d->severity == DIAG_SEV_WARN ? "WARN"
                 : level && strcmp(level, "debug") == 0 ? "DEBUG" : "INFO");
        snprintf(d->message, sizeof(d->message), "%s", msg ? msg : "");
    }
    v->log_count = n;
    v->log_generation++;
    if (n == 0) {
        snprintf(v->log_status, sizeof(v->log_status), "%s",
                 v->filter == DIAG_FILTER_ERROR ? "No errors logged"
                 : v->filter == DIAG_FILTER_WARN ? "No warnings or errors logged"
                                                 : "Nothing logged");
    } else {
        snprintf(v->log_status, sizeof(v->log_status), "%d newest%s", n,
                 v->filter == DIAG_FILTER_ERROR  ? " errors"
                 : v->filter == DIAG_FILTER_WARN ? " warnings and errors"
                                                 : " entries");
    }
    len = strlen(v->log_status);
    if (cJSON_IsNumber(skipped) && skipped->valuedouble > 0) {
        snprintf(v->log_status + len, sizeof(v->log_status) - len, " \xC2\xB7 %d unreadable",
                 (int)skipped->valuedouble);
    }
}
