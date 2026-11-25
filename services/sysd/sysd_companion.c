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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
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
        /* A NUL byte: the text a reader of C strings sees is not the file
         * the program that reads it sees, so it is no file to judge. */
        if (ferror(f) || len > 256 * 1024 || memchr(buf, 0, len)) {
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
    bool odd; /* a line sshd may read otherwise than this does: judged unknown */
};

static void ssh_parse(const char *line, size_t n, struct ssh_line *out)
{
    size_t i = 0;
    size_t k = 0;

    memset(out, 0, sizeof(*out));
    /* sshd's delimiters are space, tab and CR, and one '='. */
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        i++;
    }
    if (i >= n || line[i] == '#') {
        return;
    }
    if (line[i] == '=') {
        out->odd = true; /* no keyword before the '=' */
        return;
    }
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '=' && line[i] != '\r') {
        if (line[i] == '"' || line[i] == '\'') {
            out->odd = true; /* sshd takes a quoted keyword; this does not try */
        }
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
    bool odd;
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
        if (l.odd) {
            s->odd = true;
        }
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
    if (s.include || s.odd) {
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
    if (s.include || s.odd) {
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

void sysd_companion_default_paths(struct sysd_companion_paths *p, const char *log_dir, const char *state_dir)
{
    static char log[512];
    static char marker[512];
#ifdef SYSD_TEST_HOOKS
    /* tests/sysd-testhooks only: every fixed path under a test's own root
     * (the marker is under the state directory, which a test sets itself). */
    static char buf[12][512];
    const char *root = getenv("SYSD_TEST_COMPANION_ROOT");
#endif
    static const char *const real[12] = {
        "/etc/default/gatewayd", "/etc/init.d/S70gatewayd", "/usr/sbin/gatewayd", "/run/pocketos/gatewayd.pid",
        "/etc/passwd",           "/etc/shadow",             "/etc/ssh/sshd_config", "/usr/sbin/sshd",
        "/etc/init.d/S50sshd",   "/var/run/sshd.pid",       "/proc",
        "/proc/sys/kernel/random/boot_id",
    };
    const char *v[12];
    int i;

    snprintf(log, sizeof(log), "%s/companion.log", log_dir);
    snprintf(marker, sizeof(marker), "%s/companion/activating", state_dir);
    for (i = 0; i < 12; i++) {
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
    p->proc = v[10];
    p->boot_id = v[11];
    p->marker = marker;
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

/* Whether an sshd may be running: the pid file names a live process, or a
 * process called sshd is in /proc (a pid file can be missing or stale, and a
 * reload skipped for that reason would leave the old configuration live). */
static bool sshd_running(const struct sysd_companion_paths *p)
{
    struct dirent *e;
    bool found = false;
    DIR *d;

    if (pidfile_alive(p->sshd_pidfile)) {
        return true;
    }
    d = p->proc ? opendir(p->proc) : NULL;
    if (!d) {
        return false;
    }
    while (!found && (e = readdir(d)) != NULL) {
        char path[600];
        char comm[32] = "";
        FILE *f;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/comm", p->proc, e->d_name);
        f = fopen(path, "re");
        if (!f) {
            continue;
        }
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = '\0';
            found = strcmp(comm, "sshd") == 0;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* SSH root by key only, or the job ends here. Nothing else has changed yet.
 *
 * The file is made safe if it is not, and then - whether or not it had to
 * be - a running sshd is told to read it again. That second step is what
 * makes a retry safe: a reload that failed last time leaves a hardened file
 * and a server still running the old one, and judging the file alone would
 * set a password that the running server accepts. */
static void harden_ssh(struct job *j)
{
    const struct sysd_companion_paths *p = &j->c->paths;
    bool have_sshd = executable(p->sshd);
    bool absent = false;
    char *text;
    char *hard;
    char tmp[512];
    char orig[512];
    enum sysd_ssh_state st;

    text = slurp(p->sshd_config, &absent);
    if (absent && !have_sshd) {
        say(j, "ssh: no SSH server installed");
        return;
    }
    st = sysd_companion_ssh_judge(text);
    if (st == SYSD_SSH_UNKNOWN) {
        free(text);
        finish(j, "ssh_unknown",
               "%s cannot be checked (it includes other files, has lines sshd may read differently, or cannot be "
               "read); nothing was changed",
               p->sshd_config);
    }
    if (st == SYSD_SSH_HARDENED) {
        free(text);
        say(j, "ssh: root already signs in with a key only");
    } else {
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
        /* Checked by sshd itself when it is there. A configuration whose sshd
         * is not installed (or not executable) is made safe all the same, so
         * an sshd made runnable later does not find root open. */
        if (have_sshd) {
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
    }
    if (sshd_running(p)) {
        const char *const argv[] = { p->sshd_init, "reload", NULL };

        if (!executable(p->sshd_init) || j->c->ops.run(argv, j->log_fd, j->c->ops.user) != 0) {
            finish(j, "ssh_failed",
                   "the SSH server did not reload its configuration, so it may still let root in with a password; "
                   "restart the unit, then try again");
        }
        say(j, "ssh: reloaded");
    }
}

/* GATEWAYD_ENABLE=<value>, every other line kept; the previous text (or NULL
 * for "there was no file") into *before. */
/* Whether text is settings lines only - blank lines, comments and NAME=value -
 * the only kind of file this edits line by line. S70gatewayd sources it, so
 * a hand-written file with shell in it (an if ... fi around the switch) is
 * left to the hand that wrote it: dropping one of its lines could leave a
 * script that no longer parses, and an init script that can stop nothing. */
static bool defaults_simple(const char *text)
{
    const char *p = text;

    while (p && *p) {
        size_t n = strcspn(p, "\n");
        size_t i = 0;

        while (i < n && (p[i] == ' ' || p[i] == '\t')) {
            i++;
        }
        if (i < n && p[i] != '#') {
            if (i != 0 || !(isalpha((unsigned char)p[0]) || p[0] == '_')) {
                return false;
            }
            for (i = 1; i < n && (isalnum((unsigned char)p[i]) || p[i] == '_'); i++) {
            }
            if (i >= n || p[i] != '=') {
                return false;
            }
        }
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
    return true;
}

static int write_enable(const struct sysd_companion *c, const char *value, char **before, bool *had)
{
    bool absent = false;
    char *text = slurp(c->paths.defaults, &absent);
    char *next;
    int rc;

    if (!text && !absent) {
        return -1;
    }
    if (text && !defaults_simple(text)) {
        free(text);
        return -2;
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

/* ---- fail-closed activation ------------------------------------------------ *
 *
 * Turning the gateway on writes GATEWAYD_ENABLE=1 and then has to see it
 * answer, and anything can stop it in between: its own time limit, sysd
 * being killed or restarted, the unit losing power. A switch left at 1 by
 * an activation that never finished would start the gateway at the next
 * boot with nobody having seen it work. So the activation is recorded
 * first, in a marker under sysd's state directory ("<boot_id> <pgid>"), and
 * the marker goes only once gatewayd has answered or the switch is back at
 * 0. A marker found with no activation running means one was interrupted:
 * sysd writes the switch back to 0 and stops the service (the recover job),
 * and S70gatewayd itself refuses to start while a marker names a boot that
 * is over or a job that is gone, so a boot after a power cut never starts
 * it either, whichever of sysd and S70gatewayd runs first. */

static void read_boot_id(const struct sysd_companion_paths *p, char *out, size_t n)
{
    FILE *f = p->boot_id ? fopen(p->boot_id, "re") : NULL;

    out[0] = '\0';
    if (f) {
        if (!fgets(out, (int)n, f)) {
            out[0] = '\0';
        }
        out[strcspn(out, " \n")] = '\0';
        fclose(f);
    }
}

static int marker_write(const struct sysd_companion *c)
{
    char dir[512];
    char text[128];
    char boot[64];
    char *slash;

    snprintf(dir, sizeof(dir), "%s", c->paths.marker);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
    }
    read_boot_id(&c->paths, boot, sizeof(boot));
    snprintf(text, sizeof(text), "%s %ld\n", boot[0] ? boot : "-", (long)getpgrp());
    return write_atomic(c->paths.marker, text, 0600);
}

static void marker_clear(const struct sysd_companion *c)
{
    if (unlink(c->paths.marker) == 0) {
        fsync_dir_of(c->paths.marker);
    }
}

/* The marker's boot id and process group; false when there is none. */
static bool marker_read(const struct sysd_companion *c, char *boot, size_t bn, long *pgid)
{
    char text[128] = "";
    FILE *f = fopen(c->paths.marker, "re");
    char *sp;

    *pgid = 0;
    boot[0] = '\0';
    if (!f) {
        return false;
    }
    if (!fgets(text, sizeof(text), f)) {
        text[0] = '\0';
    }
    fclose(f);
    text[strcspn(text, "\n")] = '\0';
    sp = strchr(text, ' ');
    if (sp) {
        *sp = '\0';
        *pgid = strtol(sp + 1, NULL, 10);
    }
    snprintf(boot, bn, "%s", text);
    return true;
}

/* The recover job: the switch back to 0 (sysd wrote it already when it
 * could; again here in case it could not), the marker gone once it is, and
 * the service stopped. It ends "failed": the activation did not happen. */
static void job_recover(struct job *j)
{
    const struct sysd_companion *c = j->c;
    int w = write_enable(c, "0", NULL, NULL);
    int rc = 0;

    if (w == 0) {
        marker_clear(c);
        say(j, "recover: GATEWAYD_ENABLE=0 written");
    }
    if (executable(c->paths.init)) {
        const char *const argv[] = { c->paths.init, "stop", NULL };

        rc = c->ops.run(argv, j->log_fd, c->ops.user);
    }
    finish(j, "activation_interrupted", "turning the gateway on did not finish (%s); %s%s", c->recover_why,
           w == 0 ? "it is switched off again" : "the switch could not be written back, but it will not start at a boot",
           rc == 0 ? "" : ", and it did not stop (companion.log): restart the unit");
}

/* An activation was interrupted, if a marker says so and none is running:
 * the process group it left (an init script half way, a gatewayd it
 * started) is ended when it is of this boot, the switch goes back to 0 at
 * once, and the recover job stops the service. */
static int start(struct sysd_companion *c, enum sysd_companion_job what, char *password, char *word, size_t wn,
                 char *msg, size_t mn);

static void recover_after(struct sysd_companion *c, const char *why)
{
    char boot[64];
    char now[64];
    char word[32];
    char msg[SYSD_COMPANION_MESSAGE_MAX];
    long pgid = 0;

    if (c->pid > 0 || !marker_read(c, boot, sizeof(boot), &pgid)) {
        return;
    }
    read_boot_id(&c->paths, now, sizeof(now));
    /* Only within the boot that wrote it: a process group number of an
     * earlier boot names nothing of this one. Never sysd's own group. */
    if (pgid > 1 && now[0] && strcmp(boot, now) == 0 && (pid_t)pgid != getpgrp()) {
        (void)kill(-(pid_t)pgid, SIGKILL);
    }
    if (write_enable(c, "0", NULL, NULL) == 0) {
        marker_clear(c);
    }
    LOG_WARN("companion: an activation was interrupted (%s); the gateway is switched off again", why);
    snprintf(c->recover_why, sizeof(c->recover_why), "%s", why);
    if (start(c, SYSD_COMPANION_RECOVER, NULL, word, sizeof(word), msg, sizeof(msg)) != 0) {
        c->job = SYSD_COMPANION_RECOVER;
        c->failed = true;
        snprintf(c->error, sizeof(c->error), "activation_interrupted");
        snprintf(c->message, sizeof(c->message),
                 "turning the gateway on did not finish (%s); the switch is off, but the service could not be "
                 "stopped from here: restart the unit",
                 why);
    }
}

void sysd_companion_recover(struct sysd_companion *c)
{
    sysd_companion_reap(c);
    recover_after(c, "sysd or the unit restarted while it ran");
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
    /* Recorded before the switch is: from here an interruption is noticed
     * and undone (fail-closed activation, above). */
    if (marker_write(c) != 0) {
        finish(j, "write_failed", "the activation could not be recorded in %s; nothing was changed", c->paths.marker);
    }
    switch (write_enable(c, "1", &before, &had)) {
    case 0:
        break;
    case -2:
        marker_clear(c);
        finish(j, "defaults_unsupported", "%s holds more than settings lines; switch the gateway there by hand",
               c->paths.defaults);
    default:
        marker_clear(c);
        finish(j, "write_failed", "%s could not be written; nothing was changed", c->paths.defaults);
    }
    say(j, "GATEWAYD_ENABLE=1 written");
    {
        const char *const argv[] = { c->paths.init, "start", NULL };
        int rc = c->ops.run(argv, j->log_fd, c->ops.user);

        if (rc == 0 && gateway_answers(c, c->answer_ms)) {
            free(before);
            /* It works: from now on GATEWAYD_ENABLE=1 is the unit's choice. */
            marker_clear(c);
            finish(j, NULL, "done");
        }
        /* Back as it was, the file then the service - but never back to "on":
         * a unit switched on by hand whose gateway would not start is left
         * off, as the message says, rather than starting it at its next boot. */
        {
            char v[8] = "";
            int back;

            if (had && before && !(sysd_radio_value(before, "GATEWAYD_ENABLE", v, sizeof(v)) && strcmp(v, "1") == 0)) {
                back = write_atomic(c->paths.defaults, before, 0644);
            } else {
                back = write_enable(c, "0", NULL, NULL);
            }
            /* The marker stays when the switch could not be put back: then
             * sysd tries again, and S70gatewayd will not start it at a boot. */
            if (back == 0) {
                marker_clear(c);
            }
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
    int w = write_enable(c, "0", NULL, NULL);
    bool written = w == 0;
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
        finish(j, w == -2 ? "defaults_unsupported" : "write_failed",
               "the gateway stopped, but %s could not be written: it would start again at the next boot",
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

    /* Hashed first, so the password itself is gone before the seconds sshd
     * -t and a reload can take; SSH is still made safe before the hash is
     * written anywhere. */
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
    harden_ssh(j);
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
    c->job_limit_s = SYSD_COMPANION_JOB_LIMIT_S;
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
    case SYSD_COMPANION_RECOVER:
        return "recover";
    default:
        return "none";
    }
}

static int start(struct sysd_companion *c, enum sysd_companion_job what, char *password, char *word, size_t wn,
                 char *msg, size_t mn)
{
    int fds[2];
    pid_t pid;
    pid_t parent = getpid();

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
        /* Its own process group, so that what an interrupted activation
         * started (an init script half way, a supervisor, a gatewayd) can be
         * ended as one; and it does not outlive sysd: an activation nobody
         * will see finish is ended, and the next sysd undoes it. */
        (void)setpgid(0, 0);
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) {
            _exit(EXIT_FAILED);
        }
        /* Bounded: a hung init script or sshd reload must not leave reboot,
         * power-off and every later change refused for ever. */
        alarm((unsigned)(c->job_limit_s > 0 ? c->job_limit_s : SYSD_COMPANION_JOB_LIMIT_S));
        j.log_fd = open(c->paths.log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (what == SYSD_COMPANION_ENABLE) {
            job_enable(&j);
        } else if (what == SYSD_COMPANION_DISABLE) {
            job_disable(&j);
        } else if (what == SYSD_COMPANION_RECOVER) {
            job_recover(&j);
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
    bool absent = false;
    char *text;
    enum sysd_ssh_state s;

    text = slurp(c->paths.sshd_config, &absent);
    /* Absent only when there is neither a server nor its configuration: a
     * configuration without a runnable sshd is still judged (and hardened). */
    if (absent && !executable(c->paths.sshd)) {
        return SYSD_SSH_ABSENT;
    }
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
            snprintf(msg, mn, "%s cannot be checked (it includes other files, has lines sshd may read differently, or cannot be read)",
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
        snprintf(msg, mn, "%s cannot be checked (it includes other files, has lines sshd may read differently, or cannot be read)",
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
        } else if (w > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
            snprintf(c->error, sizeof(c->error), "timed_out");
            snprintf(c->message, sizeof(c->message),
                     "the change took longer than %d s and was stopped part way; restart the unit, then look at "
                     "companion.status before trying again",
                     c->job_limit_s);
        } else {
            snprintf(c->error, sizeof(c->error), "failed");
            snprintf(c->message, sizeof(c->message), "the change did not finish (exit %d)", code);
        }
    }
    LOG_ERROR("companion: %s failed: %s: %s. See %s.", job_name(c->job), c->error, c->message, c->paths.log);
    /* An activation that ended without finishing - stopped by its time
     * limit, killed, or failed and unable to put the switch back - leaves its
     * marker: the switch goes back off now (fail-closed activation). A
     * recover job that is itself stopped is not recovered again. */
    if (c->job == SYSD_COMPANION_ENABLE) {
        char why[64];

        if (w > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
            snprintf(why, sizeof(why), "it ran longer than %d s", c->job_limit_s);
        } else if (w > 0 && WIFSIGNALED(status)) {
            snprintf(why, sizeof(why), "it was killed");
        } else {
            snprintf(why, sizeof(why), "it could not put the switch back");
        }
        recover_after(c, why);
    }
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
