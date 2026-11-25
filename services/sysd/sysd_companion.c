/*
 * companion.*: the Companion gateway's switch and the device password
 * (sysd_companion.h says what and why; ADR-019 amendment 2).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_companion.h"

#include "../gatewayd/gw_gate.h"
#include "pocketlog/pocketlog.h"
#include "sysd_radio.h"

#include <crypt.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Exit codes of the job child. The word and the sentence travel in the pipe. */
enum {
    EXIT_DONE = 0,
    EXIT_FAILED = 1,
};

/* ---- small file helpers ------------------------------------------------- */

/* The whole file, or NULL (absent: *absent set). At most 256 KiB: these are
 * configuration files, and anything larger is not one. */
static char *slurp(const char *path, bool *absent)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f;

    if (absent) {
        *absent = false;
    }
    f = fopen(path, "re");
    if (!f) {
        if (absent && errno == ENOENT) {
            *absent = true;
        }
        return NULL;
    }
    buf = malloc(256 * 1024 + 1);
    if (buf) {
        len = fread(buf, 1, 256 * 1024 + 1, f);
        if (ferror(f) || len > 256 * 1024) {
            free(buf);
            buf = NULL;
        } else {
            buf[len] = '\0';
        }
    }
    fclose(f);
    return buf;
}

static void fsync_dir_of(const char *path)
{
    char *copy = strdup(path);
    int fd;

    if (!copy) {
        return;
    }
    fd = open(dirname(copy), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
    free(copy);
}

/* text into path by a temp file beside it, flushed, renamed, the directory
 * flushed: the file is the old one or the new one, never half of either. */
static int write_atomic(const char *path, const char *text, mode_t mode)
{
    char tmp[512];
    size_t len = strlen(text);
    size_t off = 0;
    int fd;

    if (snprintf(tmp, sizeof(tmp), "%s.doors-new", path) >= (int)sizeof(tmp)) {
        return -1;
    }
    (void)unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) {
        return -1;
    }
    if (fchmod(fd, mode) != 0) {
        goto fail;
    }
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);

        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            goto fail;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) {
        goto fail;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return -1;
    }
    fsync_dir_of(path);
    return 0;
fail:
    close(fd);
    (void)unlink(tmp);
    return -1;
}

static bool executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

/* The pid a pid file names is alive. */
static bool pidfile_alive(const char *path)
{
    char text[32] = "";
    FILE *f = path ? fopen(path, "re") : NULL;
    long pid;

    if (!f) {
        return false;
    }
    if (!fgets(text, sizeof(text), f)) {
        text[0] = '\0';
    }
    fclose(f);
    pid = strtol(text, NULL, 10);
    return pid > 1 && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
}

/* ---- SSH: the judgement and the rewrite (pure) ---------------------------- */

const char *sysd_companion_ssh_name(enum sysd_ssh_state s)
{
    switch (s) {
    case SYSD_SSH_ABSENT:
        return "absent";
    case SYSD_SSH_HARDENED:
        return "hardened";
    case SYSD_SSH_OPEN:
        return "open";
    default:
        return "unknown";
    }
}

/* One line of sshd_config: its keyword and the first word of its value. An
 * empty keyword: a blank line or a comment. sshd separates the two by spaces,
 * tabs or one '=', and takes a value in quotes without them. */
struct ssh_line {
    char key[32];
    char value[64];
};

static void ssh_parse(const char *line, size_t n, struct ssh_line *out)
{
    size_t i = 0;
    size_t k = 0;

    memset(out, 0, sizeof(*out));
    while (i < n && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= n || line[i] == '#' || line[i] == '\r') {
        return;
    }
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '=' && line[i] != '\r') {
        if (k + 1 < sizeof(out->key)) {
            out->key[k++] = (char)tolower((unsigned char)line[i]);
        }
        i++;
    }
    out->key[k] = '\0';
    while (i < n && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i < n && line[i] == '=') {
        i++;
        while (i < n && (line[i] == ' ' || line[i] == '\t')) {
            i++;
        }
    }
    if (i < n && line[i] == '"') {
        i++;
    }
    k = 0;
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '"' && line[i] != '\r' && line[i] != '#') {
        if (k + 1 < sizeof(out->value)) {
            out->value[k++] = (char)tolower((unsigned char)line[i]);
        }
        i++;
    }
    out->value[k] = '\0';
}

static bool root_login_safe(const char *v)
{
    return strcmp(v, "prohibit-password") == 0 || strcmp(v, "without-password") == 0 ||
           strcmp(v, "forced-commands-only") == 0 || strcmp(v, "no") == 0;
}

/* Walk the file: whether it includes others, has an unsafe line, and has a
 * PermitRootLogin before the first Match. */
struct ssh_scan {
    bool include;
    bool unsafe;
    bool global;
};

static void ssh_scan(const char *text, struct ssh_scan *s)
{
    const char *p = text;
    bool in_match = false;

    memset(s, 0, sizeof(*s));
    while (*p) {
        size_t n = strcspn(p, "\n");
        struct ssh_line l;

        ssh_parse(p, n, &l);
        if (strcmp(l.key, "include") == 0) {
            s->include = true;
        } else if (strcmp(l.key, "match") == 0) {
            in_match = true;
        } else if (strcmp(l.key, "permitrootlogin") == 0) {
            if (!root_login_safe(l.value)) {
                s->unsafe = true;
            }
            if (!in_match) {
                s->global = true;
            }
        }
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
}

enum sysd_ssh_state sysd_companion_ssh_judge(const char *text)
{
    struct ssh_scan s;

    if (!text) {
        return SYSD_SSH_UNKNOWN;
    }
    ssh_scan(text, &s);
    if (s.include) {
        return SYSD_SSH_UNKNOWN;
    }
    /* No line at all leaves root to sshd's compiled-in default: not relied on. */
    return s.unsafe || !s.global ? SYSD_SSH_OPEN : SYSD_SSH_HARDENED;
}

#define SSH_HARDENED_LINE "PermitRootLogin prohibit-password"
#define SSH_HEADER                                                                                           \
    "# Doors: root signs in over SSH with a key only. Written by sysd before a device\n"                     \
    "# password or the Companion gateway (ADR-019 amendment 2); the original is\n"                          \
    "# sshd_config.doors-orig.\n"

char *sysd_companion_ssh_harden(const char *text)
{
    struct ssh_scan s;
    const char *p;
    size_t cap;
    size_t o = 0;
    char *out;

    if (!text) {
        return NULL;
    }
    ssh_scan(text, &s);
    if (s.include) {
        return NULL;
    }
    /* Each line at most as long as it was or the hardened line, plus the
     * header and one line at the top. */
    cap = strlen(text) * 2 + sizeof(SSH_HEADER) + sizeof(SSH_HARDENED_LINE) + 64;
    for (p = text; *p; p++) {
        if (*p == '\n') {
            cap += sizeof(SSH_HARDENED_LINE);
        }
    }
    out = malloc(cap);
    if (!out) {
        return NULL;
    }
    if (!s.global) {
        o += (size_t)snprintf(out + o, cap - o, "%s%s\n", SSH_HEADER, SSH_HARDENED_LINE);
    }
    p = text;
    while (*p) {
        size_t n = strcspn(p, "\n");
        struct ssh_line l;

        ssh_parse(p, n, &l);
        if (strcmp(l.key, "permitrootlogin") == 0 && !root_login_safe(l.value)) {
            o += (size_t)snprintf(out + o, cap - o, "%s", SSH_HARDENED_LINE);
        } else {
            memcpy(out + o, p, n);
            o += n;
        }
        p += n;
        if (*p == '\n') {
            out[o++] = '\n';
            p++;
        }
    }
    out[o] = '\0';
    return out;
}

/* ---- the password (pure) ------------------------------------------------- */

int sysd_companion_password_check(const char *pw, char *why, size_t n)
{
    size_t len = pw ? strlen(pw) : 0;
    size_t i;

    if (len < SYSD_COMPANION_PASSWORD_MIN) {
        snprintf(why, n, "The password needs at least %d characters.", SYSD_COMPANION_PASSWORD_MIN);
        return -1;
    }
    if (len > SYSD_COMPANION_PASSWORD_MAX) {
        snprintf(why, n, "The password can have at most %d characters.", SYSD_COMPANION_PASSWORD_MAX);
        return -1;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pw[i];

        if (c < 0x20 || c > 0x7e) {
            snprintf(why, n, "Use letters, digits, spaces and the symbols on the keyboard only.");
            return -1;
        }
    }
    return 0;
}

bool sysd_companion_passwd_shadowed(const char *text)
{
    const char *p = text;

    while (p && *p) {
        size_t n = strcspn(p, "\n");

        if (n >= 5 && strncmp(p, "root:", 5) == 0) {
            /* The first root entry is the one login uses. */
            return n >= 7 && p[5] == 'x' && p[6] == ':';
        }
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
    return false;
}

char *sysd_companion_shadow_set(const char *text, const char *hash)
{
    const char *p = text;
    const char *line = NULL;
    size_t cap;
    char *out;

    if (!text || !hash || strchr(hash, ':') || strchr(hash, '\n')) {
        return NULL;
    }
    while (*p) {
        size_t n = strcspn(p, "\n");

        if (n >= 5 && strncmp(p, "root:", 5) == 0) {
            line = p;
            break;
        }
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
    if (!line) {
        return NULL;
    }
    cap = strlen(text) + strlen(hash) + 2;
    out = malloc(cap);
    if (!out) {
        return NULL;
    }
    {
        const char *field = line + 5;
        const char *end = field + strcspn(field, ":\n");
        size_t head = (size_t)(field - text);

        memcpy(out, text, head);
        memcpy(out + head, hash, strlen(hash));
        snprintf(out + head + strlen(hash), cap - head - strlen(hash), "%s", end);
    }
    return out;
}

int sysd_companion_hash(const char *password, char *out, size_t n)
{
    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    char setting[3 + 16 + 1] = "$6$";
    struct crypt_data *cd;
    const char *h;
    size_t got = 0;
    int i;
    int rc = -1;

    while (got < sizeof(rnd)) {
        ssize_t r = getrandom(rnd + got, sizeof(rnd) - got, 0);

        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    for (i = 0; i < 16; i++) {
        setting[3 + i] = abc[rnd[i] % 64]; /* 256 is a multiple of 64: no bias */
    }
    setting[19] = '\0';
    explicit_bzero(rnd, sizeof(rnd));
    cd = calloc(1, sizeof(*cd));
    if (!cd) {
        return -1;
    }
    h = crypt_r(password, setting, cd);
    /* libxcrypt answers a failure with a token that starts with '*'. */
    if (h && strncmp(h, "$6$", 3) == 0 && strlen(h) < n && !strchr(h, ':')) {
        snprintf(out, n, "%s", h);
        rc = 0;
    }
    explicit_bzero(cd, sizeof(*cd));
    free(cd);
    return rc;
}

/* ---- the real operations ------------------------------------------------- */

static int real_lock(void *user)
{
    (void)user;
    return lckpwdf();
}

static void real_unlock(void *user)
{
    (void)user;
    (void)ulckpwdf();
}

static int run_fwd(const char *const argv[], int log_fd, void *user)
{
    return sysd_radio_real_ops.run(argv, log_fd, user);
}

static cJSON *call_fwd(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                       size_t errlen, void *user)
{
    return sysd_radio_real_ops.call(service, method, params, timeout_ms, err, errlen, user);
}

const struct sysd_companion_ops sysd_companion_real_ops = {
    .run = run_fwd,
    .call = call_fwd,
    .lock = real_lock,
    .unlock = real_unlock,
    .user = NULL,
};

#ifdef SYSD_TEST_HOOKS
static int hook_lock(void *user)
{
    (void)user;
    return 0;
}

static void hook_unlock(void *user)
{
    (void)user;
}

static const struct sysd_companion_ops hook_ops = {
    .run = run_fwd,
    .call = call_fwd,
    .lock = hook_lock,
    .unlock = hook_unlock,
    .user = NULL,
};
#endif

void sysd_companion_default_paths(struct sysd_companion_paths *p, const char *log_dir)
{
    static char log[512];
#ifdef SYSD_TEST_HOOKS
    /* tests/sysd-testhooks only: every path under a test's own root. */
    static char buf[11][512];
    const char *root = getenv("SYSD_TEST_COMPANION_ROOT");
#endif
    static const char *const real[11] = {
        "/etc/default/gatewayd", "/etc/init.d/S70gatewayd", "/usr/sbin/gatewayd", "/run/pocketos/gatewayd.pid",
        "/etc/passwd",           "/etc/shadow",             "/etc/ssh/sshd_config", "/usr/sbin/sshd",
        "/etc/init.d/S50sshd",   "/var/run/sshd.pid",       NULL,
    };
    const char *v[11];
    int i;

    snprintf(log, sizeof(log), "%s/companion.log", log_dir);
    for (i = 0; i < 10; i++) {
        v[i] = real[i];
#ifdef SYSD_TEST_HOOKS
        if (root && root[0]) {
            snprintf(buf[i], sizeof(buf[i]), "%s%s", root, real[i]);
            v[i] = buf[i];
        }
#endif
    }
    p->defaults = v[0];
    p->init = v[1];
    p->daemon = v[2];
    p->pidfile = v[3];
    p->passwd = v[4];
    p->shadow = v[5];
    p->sshd_config = v[6];
    p->sshd = v[7];
    p->sshd_init = v[8];
    p->sshd_pidfile = v[9];
    p->log = log;
}

const struct sysd_companion_ops *sysd_companion_default_ops(void)
{
#ifdef SYSD_TEST_HOOKS
    const char *root = getenv("SYSD_TEST_COMPANION_ROOT");

    if (root && root[0]) {
        return &hook_ops;
    }
#endif
    return &sysd_companion_real_ops;
}

/* ---- the job --------------------------------------------------------------- */

struct job {
    const struct sysd_companion *c;
    int log_fd;
    int report_fd;
};

/* The job's end: the word and the sentence go to the parent, then the exit. */
static void finish(struct job *j, const char *word, const char *fmt, ...) __attribute__((format(printf, 3, 4), noreturn));

static void finish(struct job *j, const char *word, const char *fmt, ...)
{
    char text[32 + SYSD_COMPANION_MESSAGE_MAX];
    int n = 0;
    va_list ap;

    if (word) {
        n = snprintf(text, sizeof(text), "%s\n", word);
        va_start(ap, fmt);
        vsnprintf(text + n, sizeof(text) - (size_t)n, fmt, ap);
        va_end(ap);
        if (write(j->report_fd, text, strlen(text)) < 0) {
            /* the parent then reports an unexplained failure */
        }
        if (j->log_fd >= 0) {
            dprintf(j->log_fd, "companion: FAILED %s", text);
            dprintf(j->log_fd, "\n");
        }
    } else if (j->log_fd >= 0) {
        dprintf(j->log_fd, "companion: done\n");
    }
    _exit(word ? EXIT_FAILED : EXIT_DONE);
}

static void say(struct job *j, const char *what)
{
    if (j->log_fd >= 0) {
        dprintf(j->log_fd, "companion: %s\n", what);
    }
}

/* SSH root by key only, or the job ends here. Nothing else has changed yet. */
static void harden_ssh(struct job *j)
{
    const struct sysd_companion_paths *p = &j->c->paths;
    bool absent = false;
    char *text;
    char *hard;
    char tmp[512];
    char orig[512];
    enum sysd_ssh_state st;

    if (!executable(p->sshd)) {
        say(j, "ssh: no SSH server installed");
        return;
    }
    text = slurp(p->sshd_config, &absent);
    st = sysd_companion_ssh_judge(text);
    if (st == SYSD_SSH_HARDENED) {
        free(text);
        say(j, "ssh: root already signs in with a key only");
        return;
    }
    if (st == SYSD_SSH_UNKNOWN) {
        free(text);
        finish(j, "ssh_unknown", "%s cannot be checked (it includes other files, or cannot be read); nothing was changed",
               p->sshd_config);
    }
    hard = sysd_companion_ssh_harden(text);
    if (!hard || sysd_companion_ssh_judge(hard) != SYSD_SSH_HARDENED) {
        free(text);
        free(hard);
        finish(j, "ssh_unknown", "%s could not be hardened; nothing was changed", p->sshd_config);
    }
    snprintf(tmp, sizeof(tmp), "%s.doors-check", p->sshd_config);
    (void)unlink(tmp);
    if (write_atomic(tmp, hard, 0644) != 0) {
        free(text);
        free(hard);
        finish(j, "ssh_failed", "the hardened SSH configuration could not be written; nothing was changed");
    }
    {
        const char *const argv[] = { p->sshd, "-t", "-f", tmp, NULL };

        if (j->c->ops.run(argv, j->log_fd, j->c->ops.user) != 0) {
            (void)unlink(tmp);
            free(text);
            free(hard);
            finish(j, "ssh_failed", "sshd refused the hardened configuration (companion.log); nothing was changed");
        }
    }
    (void)unlink(tmp);
    /* The original once, for whoever wants it back by hand. */
    snprintf(orig, sizeof(orig), "%s.doors-orig", p->sshd_config);
    if (access(orig, F_OK) != 0 && write_atomic(orig, text, 0644) != 0) {
        free(text);
        free(hard);
        finish(j, "ssh_failed", "a copy of the SSH configuration could not be kept; nothing was changed");
    }
    free(text);
    if (write_atomic(p->sshd_config, hard, 0644) != 0) {
        free(hard);
        finish(j, "ssh_failed", "the hardened SSH configuration could not be written; nothing was changed");
    }
    free(hard);
    say(j, "ssh: PermitRootLogin prohibit-password written");
    if (pidfile_alive(p->sshd_pidfile)) {
        const char *const argv[] = { p->sshd_init, "reload", NULL };

        if (!executable(p->sshd_init) || j->c->ops.run(argv, j->log_fd, j->c->ops.user) != 0) {
            finish(j, "ssh_failed",
                   "the SSH server did not reload its hardened configuration; restart the unit, then try again");
        }
        say(j, "ssh: reloaded");
    }
}

/* GATEWAYD_ENABLE=<value>, every other line kept; the previous text (or NULL
 * for "there was no file") into *before. */
static int write_enable(const struct sysd_companion *c, const char *value, char **before, bool *had)
{
    bool absent = false;
    char *text = slurp(c->paths.defaults, &absent);
    char *next;
    int rc;

    if (!text && !absent) {
        return -1;
    }
    if (before) {
        *before = text ? strdup(text) : NULL;
    }
    if (had) {
        *had = text != NULL;
    }
    if (!text) {
        text = strdup("# Doors: written by sysd for Settings > Companion (ADR-019 amendment 2).\n"
                      "# GATEWAYD_INTERFACE=wlan0\n");
        if (!text) {
            return -1;
        }
    }
    next = sysd_radio_set(text, "GATEWAYD_ENABLE", value);
    free(text);
    if (!next) {
        return -1;
    }
    rc = write_atomic(c->paths.defaults, next, 0644);
    free(next);
    return rc;
}

static bool gateway_answers(const struct sysd_companion *c, int within_ms)
{
    struct timespec t0;
    struct timespec t;
    char err[160];

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        cJSON *r = c->ops.call("gatewayd", "gateway.status", NULL, 500, err, sizeof(err), c->ops.user);
        long ms;

        if (r) {
            cJSON_Delete(r);
            return true;
        }
        clock_gettime(CLOCK_MONOTONIC, &t);
        ms = (long)(t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= within_ms) {
            return false;
        }
        usleep(200 * 1000);
    }
}

static void job_enable(struct job *j)
{
    const struct sysd_companion *c = j->c;
    char *before = NULL;
    bool had = false;

    if (!executable(c->paths.daemon) || !executable(c->paths.init)) {
        finish(j, "not_installed", "gatewayd is not installed on this unit");
    }
    if (!gw_gate_password_set(c->paths.passwd, c->paths.shadow)) {
        finish(j, "password_required", "this unit has no device password; set one first");
    }
    harden_ssh(j);
    if (write_enable(c, "1", &before, &had) != 0) {
        finish(j, "write_failed", "%s could not be written; nothing was changed", c->paths.defaults);
    }
    say(j, "GATEWAYD_ENABLE=1 written");
    {
        const char *const argv[] = { c->paths.init, "start", NULL };
        int rc = c->ops.run(argv, j->log_fd, c->ops.user);

        if (rc == 0 && gateway_answers(c, c->answer_ms)) {
            free(before);
            finish(j, NULL, "done");
        }
        /* Back as it was: the file, then the service. */
        if (had && before) {
            (void)write_atomic(c->paths.defaults, before, 0644);
        } else {
            (void)write_enable(c, "0", NULL, NULL);
        }
        free(before);
        {
            const char *const stop[] = { c->paths.init, "stop", NULL };

            (void)c->ops.run(stop, j->log_fd, c->ops.user);
        }
        if (rc != 0) {
            finish(j, "start_failed", "the gateway did not start (companion.log); it is off again");
        }
        finish(j, "no_answer", "the gateway started but did not answer within %d s; it is off again",
               (c->answer_ms + 999) / 1000);
    }
}

static void job_disable(struct job *j)
{
    const struct sysd_companion *c = j->c;
    bool written = write_enable(c, "0", NULL, NULL) == 0;
    int rc = 0;

    if (written) {
        say(j, "GATEWAYD_ENABLE=0 written");
    }
    if (executable(c->paths.init)) {
        const char *const argv[] = { c->paths.init, "stop", NULL };

        rc = c->ops.run(argv, j->log_fd, c->ops.user);
    }
    if (rc != 0) {
        finish(j, "stop_failed", "the gateway did not stop (companion.log)%s",
               written ? "; it will not start again at the next boot" : "");
    }
    if (!written) {
        finish(j, "write_failed", "the gateway stopped, but %s could not be written: it would start again at the next boot",
               c->paths.defaults);
    }
    finish(j, NULL, "done");
}

static void job_password(struct job *j, char *password)
{
    const struct sysd_companion *c = j->c;
    char hash[256];
    char *text;
    char *next;
    char *pw;

    harden_ssh(j);
    pw = slurp(c->paths.passwd, NULL);
    if (!sysd_companion_passwd_shadowed(pw)) {
        free(pw);
        explicit_bzero(password, strlen(password));
        finish(j, "password_layout", "root's password is not kept in %s on this unit; nothing was changed",
               c->paths.shadow);
    }
    free(pw);
    if (sysd_companion_hash(password, hash, sizeof(hash)) != 0) {
        explicit_bzero(password, strlen(password));
        finish(j, "password_failed", "the password could not be hashed; nothing was changed");
    }
    explicit_bzero(password, strlen(password));
    if (c->ops.lock(c->ops.user) != 0) {
        explicit_bzero(hash, sizeof(hash));
        finish(j, "busy", "the password files are locked by another program; try again");
    }
    text = slurp(c->paths.shadow, NULL);
    next = sysd_companion_shadow_set(text, hash);
    explicit_bzero(hash, sizeof(hash));
    if (text) {
        explicit_bzero(text, strlen(text));
        free(text);
    }
    if (!next || write_atomic(c->paths.shadow, next, 0600) != 0) {
        c->ops.unlock(c->ops.user);
        if (next) {
            explicit_bzero(next, strlen(next));
            free(next);
        }
        finish(j, "password_failed", "%s could not be written; the password is unchanged", c->paths.shadow);
    }
    explicit_bzero(next, strlen(next));
    free(next);
    c->ops.unlock(c->ops.user);
    if (!gw_gate_password_set(c->paths.passwd, c->paths.shadow)) {
        finish(j, "password_failed", "the password was written but does not read back as set");
    }
    say(j, "the device password was set");
    finish(j, NULL, "done");
}

/* ---- the state ------------------------------------------------------------- */

static int report_fd = -1;

void sysd_companion_init(struct sysd_companion *c, const struct sysd_companion_paths *paths,
                         const struct sysd_companion_ops *ops)
{
    memset(c, 0, sizeof(*c));
    c->paths = *paths;
    c->ops = *ops;
    c->answer_ms = SYSD_COMPANION_ANSWER_MS;
}

static const char *job_name(enum sysd_companion_job j)
{
    switch (j) {
    case SYSD_COMPANION_ENABLE:
        return "enable";
    case SYSD_COMPANION_DISABLE:
        return "disable";
    case SYSD_COMPANION_PASSWORD:
        return "password";
    default:
        return "none";
    }
}

static int start(struct sysd_companion *c, enum sysd_companion_job what, char *password, char *word, size_t wn,
                 char *msg, size_t mn)
{
    int fds[2];
    pid_t pid;

    if (pipe2(fds, O_CLOEXEC) < 0) {
        snprintf(word, wn, "busy");
        snprintf(msg, mn, "the change could not start: %s", strerror(errno));
        return 4;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(word, wn, "busy");
        snprintf(msg, mn, "the change could not start: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return 4;
    }
    if (pid == 0) {
        struct job j = { c, -1, fds[1] };

        close(fds[0]);
        j.log_fd = open(c->paths.log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (what == SYSD_COMPANION_ENABLE) {
            job_enable(&j);
        } else if (what == SYSD_COMPANION_DISABLE) {
            job_disable(&j);
        } else {
            job_password(&j, password);
        }
        _exit(EXIT_FAILED); /* not reached: every job finishes */
    }
    close(fds[1]);
    if (report_fd >= 0) {
        close(report_fd);
    }
    report_fd = fds[0];
    c->pid = pid;
    c->job = what;
    c->done = false;
    c->failed = false;
    c->error[0] = '\0';
    c->message[0] = '\0';
    LOG_INFO("companion: %s started", job_name(what));
    return 0;
}

static bool installed(const struct sysd_companion *c)
{
    return executable(c->paths.daemon) && executable(c->paths.init);
}

static enum sysd_ssh_state ssh_state(const struct sysd_companion *c)
{
    char *text;
    enum sysd_ssh_state s;

    if (!executable(c->paths.sshd)) {
        return SYSD_SSH_ABSENT;
    }
    text = slurp(c->paths.sshd_config, NULL);
    s = sysd_companion_ssh_judge(text);
    free(text);
    return s;
}

int sysd_companion_set_enabled(struct sysd_companion *c, bool on, char *word, size_t wn, char *msg, size_t mn)
{
    sysd_companion_reap(c);
    if (c->pid > 0) {
        snprintf(word, wn, "busy");
        snprintf(msg, mn, "a Companion change is already running");
        return 5;
    }
    if (on) {
        if (!installed(c)) {
            snprintf(word, wn, "not_installed");
            snprintf(msg, mn, "gatewayd is not installed on this unit");
            return 3;
        }
        if (!gw_gate_password_set(c->paths.passwd, c->paths.shadow)) {
            snprintf(word, wn, "password_required");
            snprintf(msg, mn, "this unit has no device password; set one first");
            return 3;
        }
        if (ssh_state(c) == SYSD_SSH_UNKNOWN) {
            snprintf(word, wn, "ssh_unknown");
            snprintf(msg, mn, "%s cannot be checked (it includes other files, or cannot be read)",
                     c->paths.sshd_config);
            return 3;
        }
    }
    return start(c, on ? SYSD_COMPANION_ENABLE : SYSD_COMPANION_DISABLE, NULL, word, wn, msg, mn);
}

int sysd_companion_set_password(struct sysd_companion *c, char *password, char *word, size_t wn, char *msg,
                                size_t mn)
{
    char why[128];
    char *pw;
    int rc;

    sysd_companion_reap(c);
    if (sysd_companion_password_check(password, why, sizeof(why)) != 0) {
        if (password) {
            explicit_bzero(password, strlen(password));
        }
        snprintf(word, wn, "invalid_password");
        snprintf(msg, mn, "%s", why);
        return 2;
    }
    if (c->pid > 0) {
        explicit_bzero(password, strlen(password));
        snprintf(word, wn, "busy");
        snprintf(msg, mn, "a Companion change is already running");
        return 5;
    }
    if (ssh_state(c) == SYSD_SSH_UNKNOWN) {
        explicit_bzero(password, strlen(password));
        snprintf(word, wn, "ssh_unknown");
        snprintf(msg, mn, "%s cannot be checked (it includes other files, or cannot be read)",
                 c->paths.sshd_config);
        return 3;
    }
    pw = slurp(c->paths.passwd, NULL);
    if (!sysd_companion_passwd_shadowed(pw)) {
        free(pw);
        explicit_bzero(password, strlen(password));
        snprintf(word, wn, "password_layout");
        snprintf(msg, mn, "root's password is not kept in %s on this unit", c->paths.shadow);
        return 3;
    }
    free(pw);
    /* The child has its own copy from here; this one goes now. */
    rc = start(c, SYSD_COMPANION_PASSWORD, password, word, wn, msg, mn);
    explicit_bzero(password, strlen(password));
    return rc;
}

void sysd_companion_reap(struct sysd_companion *c)
{
    char text[32 + SYSD_COMPANION_MESSAGE_MAX] = "";
    int status = 0;
    pid_t w;
    int code;

    if (c->pid <= 0) {
        return;
    }
    w = waitpid(c->pid, &status, WNOHANG);
    if (w == 0 || (w < 0 && errno == EINTR)) {
        return;
    }
    c->pid = 0;
    code = w > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (report_fd >= 0) {
        ssize_t n = read(report_fd, text, sizeof(text) - 1);

        text[n > 0 ? n : 0] = '\0';
        close(report_fd);
        report_fd = -1;
    }
    if (code == EXIT_DONE) {
        c->done = true;
        LOG_INFO("companion: %s done", job_name(c->job));
        return;
    }
    c->failed = true;
    {
        char *nl = strchr(text, '\n');

        if (nl) {
            *nl = '\0';
            snprintf(c->error, sizeof(c->error), "%.31s", text);
            snprintf(c->message, sizeof(c->message), "%s", nl + 1);
        } else {
            snprintf(c->error, sizeof(c->error), "failed");
            snprintf(c->message, sizeof(c->message), "the change did not finish (exit %d)", code);
        }
    }
    LOG_ERROR("companion: %s failed: %s: %s. See %s.", job_name(c->job), c->error, c->message, c->paths.log);
}

bool sysd_companion_busy(const struct sysd_companion *c)
{
    return c->pid > 0;
}

cJSON *sysd_companion_status(struct sysd_companion *c)
{
    cJSON *o = cJSON_CreateObject();
    char v[16] = "";
    char *text;

    sysd_companion_reap(c);
    text = slurp(c->paths.defaults, NULL);
    cJSON_AddBoolToObject(o, "installed", installed(c));
    cJSON_AddBoolToObject(o, "enabled", text && sysd_radio_value(text, "GATEWAYD_ENABLE", v, sizeof(v)) &&
                                            strcmp(v, "1") == 0);
    free(text);
    cJSON_AddBoolToObject(o, "running", pidfile_alive(c->paths.pidfile));
    cJSON_AddBoolToObject(o, "password_set", gw_gate_password_set(c->paths.passwd, c->paths.shadow));
    cJSON_AddStringToObject(o, "ssh", sysd_companion_ssh_name(ssh_state(c)));
    cJSON_AddStringToObject(o, "state", c->pid > 0 ? "running" : c->failed ? "failed" : c->done ? "done" : "idle");
    if (c->job != SYSD_COMPANION_NONE) {
        cJSON_AddStringToObject(o, "action", job_name(c->job));
    }
    if (c->failed) {
        cJSON_AddStringToObject(o, "error", c->error);
        cJSON_AddStringToObject(o, "message", c->message);
    }
    return o;
}
