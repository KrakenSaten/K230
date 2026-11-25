/*
 * companion.status, companion.set_enabled and companion.set_password: Settings
 * turns DOORS Companion on and off and sets the device password, with no
 * shell, cable or command line (ADR-019 amendment 2; docs/api/system.md,
 * "Companion").
 *
 * gatewayd ships in every image and stays off unless a unit says otherwise in
 * /etc/default/gatewayd (GATEWAYD_ENABLE, ADR-019 rule 1). That file and the
 * init script are root's, and sysd already owns the system-wide changes the
 * owner asks for from the UI (radio setup writes /etc/default/meshcored and
 * restarts S65meshcored the same way), so the switch is sysd's. It is narrow
 * on purpose: one service whose script and binary are compiled in, one
 * boolean, argv executed directly. A request names no path, command or
 * other service.
 *
 *   on    the gateway must be installed and root must have a password (rule
 *         9b, read by gatewayd's own gate, gw_gate.c). SSH is hardened first
 *         (below). Then GATEWAYD_ENABLE=1, S70gatewayd start, and gatewayd
 *         must answer gateway.status in time; otherwise the file is put back
 *         and the service stopped.
 *   off   always accepted. GATEWAYD_ENABLE=0 is written BEFORE the stop, so
 *         a stop that fails leaves a unit that will not start the gateway at
 *         the next boot.
 *   password  8 to 64 printable ASCII characters. SSH is hardened first. The
 *         password is hashed with libxcrypt (SHA-512 crypt, a random salt)
 *         and written into root's field of /etc/shadow under lckpwdf(),
 *         temp-fsync-rename, every other line and field unchanged; then read
 *         back through the gate. It is never logged and never kept: the
 *         parent wipes its copy when the job is forked, the job wipes its
 *         copy once it is hashed.
 *
 * SSH hardening: every PermitRootLogin in sshd_config must be prohibit-password,
 * without-password, forced-commands-only or no, with one before the first
 * Match. An unsafe one is rewritten to prohibit-password (one is added at the
 * top when there is none), the result checked with `sshd -t -f`, the original
 * kept once as sshd_config.doors-orig, the new file renamed into place and
 * sshd reloaded. A file sysd cannot judge (an Include line, or unreadable) is
 * "unknown" and refused, never assumed safe.
 *
 * The work runs in a child, as radio_setup's does, so sysd keeps answering;
 * the init scripts' output goes to companion.log in the log directory.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSD_COMPANION_H
#define SYSD_COMPANION_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* How long a started gatewayd has to answer gateway.status. It opens its
 * local socket before it generates its key (the first start), so a second
 * is usual; the rest is the one hart being busy. */
#define SYSD_COMPANION_ANSWER_MS 10000
#define SYSD_COMPANION_PASSWORD_MIN 8
#define SYSD_COMPANION_PASSWORD_MAX 64
#define SYSD_COMPANION_MESSAGE_MAX 200
/* A change that has not finished in this long is stopped (SIGALRM in the job):
 * a hung init script must not keep reboot and power-off refused for ever.
 * Turning on waits up to SYSD_COMPANION_ANSWER_MS for gatewayd; sshd -t and
 * a reload are seconds. */
#define SYSD_COMPANION_JOB_LIMIT_S 90

struct sysd_companion_paths {
    const char *defaults;     /* /etc/default/gatewayd */
    const char *init;         /* /etc/init.d/S70gatewayd */
    const char *daemon;       /* /usr/sbin/gatewayd */
    const char *pidfile;      /* /run/pocketos/gatewayd.pid: pos-supervise's record of the daemon */
    const char *passwd;       /* /etc/passwd */
    const char *shadow;       /* /etc/shadow */
    const char *sshd_config;  /* /etc/ssh/sshd_config */
    const char *sshd;         /* /usr/sbin/sshd: `sshd -t -f`; not there means no SSH server */
    const char *sshd_init;    /* /etc/init.d/S50sshd: reload */
    const char *sshd_pidfile; /* /var/run/sshd.pid */
    const char *proc;         /* /proc: an sshd whose pid file is missing is still found by name */
    const char *log;          /* <log dir>/companion.log */
};

struct sysd_companion_ops {
    /* Run argv with stdout and stderr to log_fd; the exit status, or -1. */
    int (*run)(const char *const argv[], int log_fd, void *user);
    /* Ask service for method, giving up after timeout_ms. The result (caller
     * frees), or NULL. */
    cJSON *(*call)(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                   size_t errlen, void *user);
    /* The password files' lock (lckpwdf/ulckpwdf). 0 when held. */
    int (*lock)(void *user);
    void (*unlock)(void *user);
    void *user;
};

extern const struct sysd_companion_ops sysd_companion_real_ops;

/* The shipped paths; under SYSD_TEST_HOOKS (tests/sysd-testhooks only) each
 * is put under $SYSD_TEST_COMPANION_ROOT when that is set, and the password
 * lock is a no-op there. */
void sysd_companion_default_paths(struct sysd_companion_paths *p, const char *log_dir);
const struct sysd_companion_ops *sysd_companion_default_ops(void);

enum sysd_ssh_state {
    SYSD_SSH_ABSENT = 0, /* no SSH server installed: nothing listens */
    SYSD_SSH_HARDENED,   /* root by key only (or not at all) */
    SYSD_SSH_OPEN,       /* root may sign in with a password */
    SYSD_SSH_UNKNOWN,    /* the configuration cannot be judged: refused */
};

const char *sysd_companion_ssh_name(enum sysd_ssh_state s);

/* ---- pure, tests/sysd_companion_test.c ---------------------------------- */

/* sshd_config's text (NULL: unreadable) judged as above. */
enum sysd_ssh_state sysd_companion_ssh_judge(const char *text);
/* The same text with every unsafe PermitRootLogin rewritten, and one added at
 * the top when none comes before the first Match. malloc'd; NULL when it
 * cannot be judged (an Include line) or on no memory. */
char *sysd_companion_ssh_harden(const char *text);
/* 0 when the password may be set; -1 with a sentence for a person. */
int sysd_companion_password_check(const char *password, char *why, size_t n);
/* True when root's /etc/passwd field is "x": its password lives in shadow. */
bool sysd_companion_passwd_shadowed(const char *passwd_text);
/* The shadow text with root's password field replaced by hash, everything
 * else as it was. malloc'd; NULL when there is no root line or no memory. */
char *sysd_companion_shadow_set(const char *text, const char *hash);
/* A SHA-512 crypt hash of password with a random salt into out (at least
 * 128 bytes). 0, or -1. */
int sysd_companion_hash(const char *password, char *out, size_t n);

/* ---- the state ---------------------------------------------------------- */

enum sysd_companion_job {
    SYSD_COMPANION_NONE = 0,
    SYSD_COMPANION_ENABLE,
    SYSD_COMPANION_DISABLE,
    SYSD_COMPANION_PASSWORD,
};

struct sysd_companion {
    struct sysd_companion_paths paths;
    struct sysd_companion_ops ops;
    int answer_ms;                 /* SYSD_COMPANION_ANSWER_MS; shorter in tests */
    int job_limit_s;               /* SYSD_COMPANION_JOB_LIMIT_S; shorter in tests */
    pid_t pid;                     /* the running job, 0 when none */
    enum sysd_companion_job job;   /* the running or the last job */
    bool done;                     /* the last job finished as asked */
    bool failed;                   /* it did not: error and message say why */
    char error[32];                /* a word: docs/api/system.md lists them */
    char message[SYSD_COMPANION_MESSAGE_MAX];
};

void sysd_companion_init(struct sysd_companion *c, const struct sysd_companion_paths *paths,
                         const struct sysd_companion_ops *ops);

/* The requests. 0: the job started. Otherwise a pocketipc error code
 * (2 invalid, 3 refused, 4 could not start, 5 busy), with a word and a
 * sentence. set_password wipes `password` in every case. */
int sysd_companion_set_enabled(struct sysd_companion *c, bool on, char *word, size_t wn, char *msg, size_t mn);
int sysd_companion_set_password(struct sysd_companion *c, char *password, char *word, size_t wn, char *msg,
                                size_t mn);
/* Collect a finished job. */
void sysd_companion_reap(struct sysd_companion *c);
bool sysd_companion_busy(const struct sysd_companion *c);
cJSON *sysd_companion_status(struct sysd_companion *c);

#endif
