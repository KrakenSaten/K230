/*
 * radio_setup.start and radio_setup.status: setting a unit up for RIFT from
 * the UI, without a shell (docs/api/system.md, "Radio setup").
 *
 * A freshly flashed card runs radiod on the mock backend and meshcored
 * disabled: both are per-unit choices in /etc/default (S60radiod,
 * S65meshcored), and until 0.3.5 the only way to make them was a shell.
 * Controls' LoRa radio tile now offers it, after the antenna question, and
 * sysd does it - sysd because it already owns the system-wide changes the
 * owner asks for (reboot, storage), and the files and init scripts are
 * root's:
 *
 *   1. /etc/default/radiod     RADIOD_BACKEND=sx1262 (other lines kept)
 *   2. S60radiod restart, then radiod must answer radio.info on sx1262
 *   3. radio.set_enabled on    - the owner answered the antenna question
 *                                to ask for this (antenna_confirmed)
 *   4. /etc/default/meshcored  MESHCORED_ENABLE=1 (other lines kept)
 *   5. S65meshcored restart, then meshcored must answer mesh.status
 *
 * The init scripts start their daemon in the background and report only
 * whether the old one stopped, so "started" is proven by the service
 * answering, within a bound. Any step that fails puts both files back as
 * they were and restarts both services on them: the unit is either set up
 * or as it was, and the status says which step failed and why. The work
 * runs in a child, as storage.expand's does, so sysd keeps answering; the
 * scripts' output goes to radio-setup.log in the log directory.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSD_RADIO_H
#define SYSD_RADIO_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* How long a restarted service has to answer. radiod on the SX1262 starts
 * with the radio off (its stored choice belongs to the mock, rf_state.h), so
 * it answers within a second; meshcored opens its socket at once. */
#define SYSD_RADIO_ANSWER_MS 15000
/* The radio switch is the transceiver's whole start-up (radiod's own budget
 * in Controls is 2 s); more here, as nothing waits on the UI thread. */
#define SYSD_RADIO_SWITCH_MS 5000

#define SYSD_RADIO_BACKEND "sx1262"

struct sysd_radio_paths {
    const char *radiod_default;    /* /etc/default/radiod */
    const char *meshcored_default; /* /etc/default/meshcored */
    const char *radiod_init;       /* /etc/init.d/S60radiod */
    const char *meshcored_init;    /* /etc/init.d/S65meshcored */
    const char *log;               /* <log dir>/radio-setup.log */
};

struct sysd_radio_ops {
    /* Run argv with stdout and stderr to log_fd; the exit status, or -1. */
    int (*run)(const char *const argv[], int log_fd, void *user);
    /* Ask service for method with params (consumed, may be NULL), giving up
     * after timeout_ms. The result (caller frees), or NULL with err set. */
    cJSON *(*call)(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                   size_t errlen, void *user);
    void *user;
};

extern const struct sysd_radio_ops sysd_radio_real_ops;

/* Exit codes of the job child, also used by tests. */
enum {
    SYSD_RADIO_EXIT_DONE = 0,
    SYSD_RADIO_EXIT_WRITE = 10,      /* /etc/default/radiod could not be written; nothing changed */
    SYSD_RADIO_EXIT_WRITE_MESH = 11, /* /etc/default/meshcored could not be written; restored */
    SYSD_RADIO_EXIT_RADIOD = 20,     /* radiod did not come back on the SX1262 */
    SYSD_RADIO_EXIT_SWITCH = 30,     /* the SX1262 did not switch on */
    SYSD_RADIO_EXIT_MESHCORED = 40   /* meshcored did not come back */
};

struct sysd_radio {
    struct sysd_radio_paths paths;
    struct sysd_radio_ops ops;
    int answer_ms;      /* SYSD_RADIO_ANSWER_MS; shorter in tests */
    pid_t pid;          /* the running job, 0 when none */
    bool done;          /* the last job set the unit up */
    bool failed;        /* the last job failed; error says why */
    char error[200];
};

/* What the files say now. backend: the last RADIOD_BACKEND (quotes
 * stripped), "mock" when the file or the line is absent, as S60radiod's
 * default is. */
struct sysd_radio_config {
    char backend[32];
    bool meshcored_enabled;
};

void sysd_radio_init(struct sysd_radio *r, const struct sysd_radio_paths *paths,
                     const struct sysd_radio_ops *ops);
void sysd_radio_read(const struct sysd_radio *r, struct sysd_radio_config *c);
/* Whether the unit still needs setting up: not on the SX1262, or no
 * meshcored. */
bool sysd_radio_needed(const struct sysd_radio_config *c);

/* The value of KEY= in a key=value file's text (last one wins, surrounding
 * quotes removed), into out. Returns true when there is one. */
bool sysd_radio_value(const char *text, const char *key, char *out, size_t len);
/* text with every KEY= line replaced by one KEY=value at the end, other
 * lines (comments included) kept in order. Returns a malloc'd string. */
char *sysd_radio_set(const char *text, const char *key, const char *value);

/* Start the job. antenna_confirmed: the owner answered the antenna
 * question. Returns 0, -1 (refused: no confirmation, or nothing to do),
 * -2 (already running) or -3 (could not start), with a message. */
int sysd_radio_start(struct sysd_radio *r, bool antenna_confirmed, char *msg, size_t msg_len);
/* Collect a finished job. */
void sysd_radio_reap(struct sysd_radio *r);
bool sysd_radio_busy(const struct sysd_radio *r);
/* radio_setup.status's result. */
cJSON *sysd_radio_status(struct sysd_radio *r);

#endif
