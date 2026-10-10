/*
 * System > Diagnostics: everything the page decides, with no LVGL in it.
 *
 * Enough to diagnose the device without SSH, from the services that already
 * know each fact - never read by the app from /proc, /sys, /run or the log
 * directory itself:
 *
 *   sysd     system.status (uptime, memory, storage, battery, Bluetooth,
 *            services and their restarts), system.crashes, system.logs
 *   radiod   radio.status (on/off, state, profile)
 *   meshcored mesh.status (state and why)
 *
 * plus the version and build the System screen already has (system_view).
 *
 * The page refreshes in steps, one bounded call per app tick
 * (diag_view_step / diag_view_step_done), so a refresh never holds the LVGL
 * thread for more than one SHELL_IPC_UI_TIMEOUT_MS however many services
 * are down. Changing the log filter re-asks for the log only. The log is at
 * most DIAG_LOG_MAX entries, newest first, each already bounded by sysd
 * (docs/api/system.md, system.logs); nothing here grows with the log files.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_DIAG_VIEW_H
#define POCKETOS_DIAG_VIEW_H

#include "system_view.h"

#include <cjson/cJSON.h>

#define DIAG_TEXT 96
#define DIAG_MESSAGE 248
#define DIAG_LOG_MAX 40
#define DIAG_CRASH_MAX 5

enum diag_row {
    DIAG_ROW_VERSION = 0,
    DIAG_ROW_UPTIME,
    DIAG_ROW_MEMORY,
    DIAG_ROW_STORAGE,
    DIAG_ROW_BATTERY,
    DIAG_ROW_GAUGE,  /* the base gauge's own figures, unvalidated */
    DIAG_ROW_SERVICES,
    DIAG_ROW_RADIO,
    DIAG_ROW_MESH,
    DIAG_ROW_BLUETOOTH,
    DIAG_ROW_CRASHES,
    DIAG_ROW_COUNT
};

enum diag_filter {
    DIAG_FILTER_ALL = 0,
    DIAG_FILTER_WARN,
    DIAG_FILTER_ERROR,
    DIAG_FILTER_COUNT
};

enum diag_severity {
    DIAG_SEV_INFO = 0, /* debug and info */
    DIAG_SEV_WARN,
    DIAG_SEV_ERROR
};

/* What the next tick should ask for. */
enum diag_step {
    DIAG_STEP_IDLE = 0,
    DIAG_STEP_STATUS,  /* sysd system.status */
    DIAG_STEP_RADIO,   /* radiod radio.status */
    DIAG_STEP_MESH,    /* meshcored mesh.status */
    DIAG_STEP_CRASHES, /* sysd system.crashes */
    DIAG_STEP_LOGS     /* sysd system.logs {level, limit} */
};

struct diag_line {
    char label[16];
    char value[DIAG_TEXT];
    int warn;
};

struct diag_log_entry {
    char head[DIAG_TEXT];        /* "09-24 13:20:01  radiod  WARN" */
    char message[DIAG_MESSAGE];
    enum diag_severity severity;
};

struct diag_crash {
    char line[DIAG_TEXT];        /* "radiod · SIGSEGV · 09-24 13:20" */
    char frame[DIAG_TEXT];       /* the first backtrace line, or empty */
};

struct diag_view {
    struct diag_line rows[DIAG_ROW_COUNT];

    enum diag_filter filter;
    struct diag_log_entry log[DIAG_LOG_MAX];
    int log_count;
    char log_status[DIAG_TEXT];
    unsigned log_generation;     /* bumped whenever the list is replaced */

    struct diag_crash crashes[DIAG_CRASH_MAX];
    int crash_count;
    unsigned crash_generation;

    enum diag_step step;
};

void diag_view_init(struct diag_view *v);

/* Start a full refresh from the first step (a no-op while one is running). */
void diag_view_refresh(struct diag_view *v);
/* Choose the log filter; re-asks for the log only. */
void diag_view_set_filter(struct diag_view *v, enum diag_filter f);
/* The step the next tick should run, and the move to the one after it. */
enum diag_step diag_view_step(const struct diag_view *v);
void diag_view_step_done(struct diag_view *v);
int diag_view_busy(const struct diag_view *v);

/* The params system.logs is asked with for the current filter (caller frees). */
cJSON *diag_view_logs_params(const struct diag_view *v);
const char *diag_view_filter_label(enum diag_filter f);

/* One answer each; NULL means the service did not answer, which is shown as
 * that and never as a zero or an empty list. sv is the System screen's own
 * view, already updated with the same status, for the values it formats. */
void diag_view_apply_system(struct diag_view *v, const struct system_view *sv, const cJSON *status);
void diag_view_apply_radio(struct diag_view *v, const cJSON *radio_status);
void diag_view_apply_mesh(struct diag_view *v, const cJSON *mesh_status);
void diag_view_apply_crashes(struct diag_view *v, const cJSON *crashes);
void diag_view_apply_logs(struct diag_view *v, const cJSON *logs);

#endif
