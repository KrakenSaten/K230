/*
 * Radio setup for RIFT. See sysd_radio.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_radio.h"

#include "pocketipc/pocketipc.h"
#include "pocketlog/pocketlog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILE_MAX 8192

/* ---- the files ---------------------------------------------------------------- */

/* The whole file, or NULL when it does not exist (*absent) or cannot be
 * read. */
static char *slurp(const char *path, bool *absent)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char *buf;
    size_t n = 0;
    ssize_t r;

    *absent = false;
    if (fd < 0) {
        *absent = errno == ENOENT;
        return NULL;
    }
    buf = malloc(FILE_MAX + 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    while (n < FILE_MAX && (r = read(fd, buf + n, FILE_MAX - n)) != 0) {
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return NULL;
        }
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    return buf;
}

bool sysd_radio_value(const char *text, const char *key, char *out, size_t len)
{
    size_t klen = strlen(key);
    const char *p = text;
    bool found = false;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);

        while (llen > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            llen--;
        }
        if (llen > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = llen - klen - 1;

            while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) {
                vlen--;
            }
            if (vlen >= 2 && (v[0] == '"' || v[0] == '\'') && v[vlen - 1] == v[0]) {
                v++;
                vlen -= 2;
            }
            snprintf(out, len, "%.*s", (int)vlen, v);
            found = true;
        }
        p = eol ? eol + 1 : NULL;
    }
    return found;
}

char *sysd_radio_set(const char *text, const char *key, const char *value)
{
    size_t klen = strlen(key);
    size_t cap = strlen(text) + klen + strlen(value) + 4;
    char *out = malloc(cap);
    const char *p = text;
    size_t n = 0;

    if (!out) {
        return NULL;
    }
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        const char *q = p;
        size_t qlen = llen;

        while (qlen > 0 && (*q == ' ' || *q == '\t')) {
            q++;
            qlen--;
        }
        if (!(qlen > klen && strncmp(q, key, klen) == 0 && q[klen] == '=')) {
            memcpy(out + n, p, llen);
            n += llen;
            out[n++] = '\n';
        }
        p = eol ? eol + 1 : NULL;
    }
    n += (size_t)snprintf(out + n, cap - n, "%s=%s\n", key, value);
    return out;
}

/* Atomically: a temporary file beside it, fsync, rename. */
static int write_file(const char *path, const char *text, char *err, size_t errlen)
{
    char tmp[512];
    size_t len = strlen(text);
    int fd;

    if (snprintf(tmp, sizeof(tmp), "%s.doors-tmp", path) >= (int)sizeof(tmp)) {
        snprintf(err, errlen, "%s: path too long", path);
        return -1;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, text, len) != (ssize_t)len || fsync(fd) < 0) {
        snprintf(err, errlen, "%s could not be written: %s", path, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        unlink(tmp);
        return -1;
    }
    if (close(fd) < 0 || rename(tmp, path) < 0) {
        snprintf(err, errlen, "%s could not be replaced: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* A file back as it was: its old text, or no file (one that is already gone
 * is back as it was). 0, or -1 with err set. */
static int put_back(const char *path, const char *old, bool absent, char *err, size_t errlen)
{
    if (absent) {
        if (unlink(path) < 0 && errno != ENOENT) {
            snprintf(err, errlen, "%s could not be removed: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (!old) {
        snprintf(err, errlen, "%s: its previous text was not read", path);
        return -1;
    }
    return write_file(path, old, err, errlen);
}

void sysd_radio_read(const struct sysd_radio *r, struct sysd_radio_config *c)
{
    bool absent;
    char *t;
    char v[32];

    memset(c, 0, sizeof(*c));
    snprintf(c->backend, sizeof(c->backend), "mock");
    t = slurp(r->paths.radiod_default, &absent);
    if (t && sysd_radio_value(t, "RADIOD_BACKEND", v, sizeof(v)) && v[0]) {
        snprintf(c->backend, sizeof(c->backend), "%s", v);
    }
    free(t);
    t = slurp(r->paths.meshcored_default, &absent);
    if (t && sysd_radio_value(t, "MESHCORED_ENABLE", v, sizeof(v))) {
        c->meshcored_enabled = strcmp(v, "1") == 0;
    }
    free(t);
}

bool sysd_radio_needed(const struct sysd_radio_config *c)
{
    return strcmp(c->backend, SYSD_RADIO_BACKEND) != 0 || !c->meshcored_enabled;
}

/* ---- the real operations ------------------------------------------------------- */

static int real_run(const char *const argv[], int log_fd, void *user)
{
    int status = 0;
    pid_t pid;

    (void)user;
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);

        dup2(null, STDIN_FILENO);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
        }
        setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin", 1);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static cJSON *real_call(const char *service, const char *method, cJSON *params, int timeout_ms, char *err,
                        size_t errlen, void *user)
{
    int fd = pocketipc_connect_timeout(service, timeout_ms);
    int code = 0;
    cJSON *res;

    (void)user;
    if (fd < 0) {
        snprintf(err, errlen, "%s is not answering (%s)", service, strerror(errno));
        cJSON_Delete(params);
        return NULL;
    }
    res = pocketipc_call_timeout(fd, method, params, timeout_ms, &code, err, errlen);
    close(fd);
    return res;
}

const struct sysd_radio_ops sysd_radio_real_ops = { real_run, real_call, NULL };

/* ---- the job --------------------------------------------------------------------- */

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

static void note(int fd, const char *fmt, const char *a)
{
    char line[400];
    int n;

    if (fd < 0) {
        return;
    }
    n = snprintf(line, sizeof(line), fmt, a ? a : "");
    if (n > 0) {
        ssize_t w = write(fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);

        (void)w;
    }
}

/* service answers method within the bound; with want_backend, radio.info
 * must name it. */
static bool answers(const struct sysd_radio *r, const char *service, const char *method,
                    const char *want_backend, char *err, size_t errlen)
{
    int64_t end = mono_ms() + r->answer_ms;

    for (;;) {
        cJSON *res = r->ops.call(service, method, NULL, 1000, err, errlen, r->ops.user);

        if (res) {
            const cJSON *b = cJSON_GetObjectItemCaseSensitive(res, "backend");
            bool ok = !want_backend || (cJSON_IsString(b) && strcmp(b->valuestring, want_backend) == 0);

            if (!ok) {
                snprintf(err, errlen, "%s answers on the %s backend", service,
                         cJSON_IsString(b) ? b->valuestring : "unknown");
            }
            cJSON_Delete(res);
            if (ok) {
                return true;
            }
        }
        if (mono_ms() >= end) {
            return false;
        }
        nap_ms(250);
    }
}

/* What the job found before it changed anything: each file's text, or that
 * it did not exist. */
struct saved {
    char *radiod;
    bool radiod_absent;
    char *meshcored;
    bool meshcored_absent;
    char *state; /* radiod's radio.conf; unused without paths.radio_state */
    bool state_absent;
};

/* One more thing that did not come back, appended to fail ("; " between). */
static void add_failure(char *fail, size_t n, const char *fmt, ...)
{
    size_t len = strlen(fail);
    va_list ap;

    if (len + 3 >= n) {
        return;
    }
    if (len > 0) {
        len += (size_t)snprintf(fail + len, n - len, "; ");
    }
    va_start(ap, fmt);
    vsnprintf(fail + len, n - len, fmt, ap);
    va_end(ap);
}

/* Everything back as it was, both services restarted on it. True only when
 * every part of that is known to have happened; otherwise false with fail
 * saying what did not, so nobody is told the previous settings are back when
 * they are not.
 *
 * radiod is stopped before its files are put back and started after, not
 * restarted around them: the setup switched the radio on, which radiod
 * stores in radio.conf, and a radio.set_enabled that timed out here may still
 * be finishing in radiod. Stopped, it can store nothing more, so the choice
 * radiod starts with is the owner's from before the setup. A stop that
 * failed leaves radiod running on the setup's choice; S60radiod's start would
 * only answer "already running" then, so it is not asked, and the restore
 * has failed. "Started" is proven the way the setup proves it, by the service
 * answering; meshcored only when its restored file enables it. */
static bool restore(const struct sysd_radio *r, int log_fd, const struct saved *s, char *fail, size_t n)
{
    const char *rstop[] = { r->paths.radiod_init, "stop", NULL };
    const char *rstart[] = { r->paths.radiod_init, "start", NULL };
    const char *mr[] = { r->paths.meshcored_init, "restart", NULL };
    char err[200];
    char v[16];
    bool stopped;
    int rc;

    fail[0] = '\0';
    note(log_fd, "== restoring the previous settings%s\n", NULL);
    rc = r->ops.run(rstop, log_fd, r->ops.user);
    stopped = rc == 0;
    if (!stopped) {
        add_failure(fail, n, "radiod did not stop (S60radiod stop: exit %d)", rc);
    }
    if (put_back(r->paths.radiod_default, s->radiod, s->radiod_absent, err, sizeof(err)) < 0) {
        add_failure(fail, n, "%s", err);
    }
    if (put_back(r->paths.meshcored_default, s->meshcored, s->meshcored_absent, err, sizeof(err)) < 0) {
        add_failure(fail, n, "%s", err);
    }
    if (r->paths.radio_state &&
        put_back(r->paths.radio_state, s->state, s->state_absent, err, sizeof(err)) < 0) {
        add_failure(fail, n, "%s", err);
    }
    if (stopped) {
        rc = r->ops.run(rstart, log_fd, r->ops.user);
        err[0] = '\0';
        if (rc != 0) {
            add_failure(fail, n, "radiod did not start (S60radiod start: exit %d)", rc);
        } else if (!answers(r, "radiod", "radio.info", NULL, err, sizeof(err))) {
            add_failure(fail, n, "radiod did not answer after the restore (%s)", err[0] ? err : "no answer");
        }
    } else {
        note(log_fd, "== radiod was not started again: it did not stop%s\n", NULL);
    }
    rc = r->ops.run(mr, log_fd, r->ops.user);
    err[0] = '\0';
    if (rc != 0) {
        add_failure(fail, n, "meshcored did not restart (S65meshcored restart: exit %d)", rc);
    } else if (s->meshcored && sysd_radio_value(s->meshcored, "MESHCORED_ENABLE", v, sizeof(v)) &&
               strcmp(v, "1") == 0 && !answers(r, "meshcored", "mesh.status", NULL, err, sizeof(err))) {
        add_failure(fail, n, "meshcored did not answer after the restore (%s)", err[0] ? err : "no answer");
    }
    return fail[0] == '\0';
}

static void job(const struct sysd_radio *r, int report_fd)
{
    int log_fd = open(r->paths.log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const char *rr[] = { r->paths.radiod_init, "restart", NULL };
    const char *mr[] = { r->paths.meshcored_init, "restart", NULL };
    struct saved s = { NULL, false, NULL, false, NULL, true };
    char *new_text;
    char err[200] = "";
    char why[SYSD_RADIO_ERROR_MAX];
    char fail[SYSD_RADIO_ERROR_MAX];
    int code = SYSD_RADIO_EXIT_DONE;
    cJSON *params;
    cJSON *res;

    s.radiod = slurp(r->paths.radiod_default, &s.radiod_absent);
    s.meshcored = slurp(r->paths.meshcored_default, &s.meshcored_absent);
    if ((!s.radiod && !s.radiod_absent) || (!s.meshcored && !s.meshcored_absent)) {
        snprintf(why, sizeof(why), "the files in /etc/default could not be read");
        code = SYSD_RADIO_EXIT_WRITE;
        goto out;
    }
    /* Read before anything changes, for the same reason: a choice that could
     * not be read could not be put back. */
    if (r->paths.radio_state) {
        s.state = slurp(r->paths.radio_state, &s.state_absent);
        if (!s.state && !s.state_absent) {
            snprintf(why, sizeof(why), "%s could not be read", r->paths.radio_state);
            code = SYSD_RADIO_EXIT_WRITE;
            goto out;
        }
    }
    /* 1-2: radiod on the SX1262. */
    note(log_fd, "== radiod: RADIOD_BACKEND=%s\n", SYSD_RADIO_BACKEND);
    new_text = sysd_radio_set(s.radiod ? s.radiod : "", "RADIOD_BACKEND", SYSD_RADIO_BACKEND);
    if (!new_text || write_file(r->paths.radiod_default, new_text, err, sizeof(err)) < 0) {
        snprintf(why, sizeof(why), "%s", new_text ? err : "out of memory");
        free(new_text);
        code = SYSD_RADIO_EXIT_WRITE;
        goto out;
    }
    free(new_text);
    r->ops.run(rr, log_fd, r->ops.user);
    if (!answers(r, "radiod", "radio.info", SYSD_RADIO_BACKEND, err, sizeof(err))) {
        snprintf(why, sizeof(why), "radiod did not come back on the SX1262 (%s)", err[0] ? err : "no answer");
        code = SYSD_RADIO_EXIT_RADIOD;
        goto restore;
    }
    /* 3: the radio on - the answer to the antenna question. */
    note(log_fd, "== radiod: radio.set_enabled on%s\n", NULL);
    params = cJSON_CreateObject();
    cJSON_AddBoolToObject(params, "enabled", 1);
    err[0] = '\0';
    res = r->ops.call("radiod", "radio.set_enabled", params, SYSD_RADIO_SWITCH_MS, err, sizeof(err), r->ops.user);
    if (!res) {
        snprintf(why, sizeof(why), "the SX1262 did not switch on (%s)", err[0] ? err : "no answer");
        code = SYSD_RADIO_EXIT_SWITCH;
        goto restore;
    }
    cJSON_Delete(res);
    /* 4-5: meshcored. */
    note(log_fd, "== meshcored: MESHCORED_ENABLE=1%s\n", NULL);
    new_text = sysd_radio_set(s.meshcored ? s.meshcored : "", "MESHCORED_ENABLE", "1");
    if (!new_text || write_file(r->paths.meshcored_default, new_text, err, sizeof(err)) < 0) {
        snprintf(why, sizeof(why), "%s", new_text ? err : "out of memory");
        free(new_text);
        code = SYSD_RADIO_EXIT_WRITE_MESH;
        goto restore;
    }
    free(new_text);
    r->ops.run(mr, log_fd, r->ops.user);
    if (!answers(r, "meshcored", "mesh.status", NULL, err, sizeof(err))) {
        snprintf(why, sizeof(why), "meshcored did not start (%s)", err[0] ? err : "no answer");
        code = SYSD_RADIO_EXIT_MESHCORED;
        goto restore;
    }
    note(log_fd, "== done%s\n", NULL);
    goto out;
restore:
    if (!restore(r, log_fd, &s, fail, sizeof(fail))) {
        size_t len = strlen(why);

        snprintf(why + len, sizeof(why) - len, "; the previous settings could not all be put back: %s", fail);
        code = SYSD_RADIO_EXIT_RESTORE;
    }
out:
    if (code != SYSD_RADIO_EXIT_DONE) {
        ssize_t w;

        note(log_fd, "== failed: %s\n", why);
        w = write(report_fd, why, strlen(why));
        (void)w;
    }
    free(s.radiod);
    free(s.meshcored);
    free(s.state);
    if (log_fd >= 0) {
        close(log_fd);
    }
    _exit(code);
}

/* ---- the state ------------------------------------------------------------------- */

static int report_fd = -1;

void sysd_radio_init(struct sysd_radio *r, const struct sysd_radio_paths *paths,
                     const struct sysd_radio_ops *ops)
{
    memset(r, 0, sizeof(*r));
    r->paths = *paths;
    r->ops = *ops;
    r->answer_ms = SYSD_RADIO_ANSWER_MS;
}

int sysd_radio_start(struct sysd_radio *r, bool antenna_confirmed, char *msg, size_t msg_len)
{
    struct sysd_radio_config c;
    int fds[2];
    pid_t pid;

    sysd_radio_reap(r);
    if (r->pid > 0) {
        snprintf(msg, msg_len, "the radio setup is already running");
        return -2;
    }
    if (!antenna_confirmed) {
        snprintf(msg, msg_len, "the antenna question was not answered (antenna_confirmed)");
        return -1;
    }
    sysd_radio_read(r, &c);
    if (!sysd_radio_needed(&c)) {
        snprintf(msg, msg_len, "the radio is already set up (sx1262, meshcored enabled)");
        return -1;
    }
    if (pipe2(fds, O_CLOEXEC) < 0) {
        snprintf(msg, msg_len, "the radio setup could not start: %s", strerror(errno));
        return -3;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(msg, msg_len, "the radio setup could not start: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -3;
    }
    if (pid == 0) {
        close(fds[0]);
        job(r, fds[1]);
    }
    close(fds[1]);
    if (report_fd >= 0) {
        close(report_fd);
    }
    report_fd = fds[0];
    r->pid = pid;
    r->done = false;
    r->failed = false;
    r->error[0] = '\0';
    LOG_INFO("radio setup: started (radiod on %s, meshcored %s)", c.backend,
             c.meshcored_enabled ? "enabled" : "disabled");
    return 0;
}

void sysd_radio_reap(struct sysd_radio *r)
{
    int status = 0;
    pid_t w;
    int code;
    char why[sizeof(r->error)] = "";

    if (r->pid <= 0) {
        return;
    }
    w = waitpid(r->pid, &status, WNOHANG);
    if (w == 0 || (w < 0 && errno == EINTR)) {
        return;
    }
    r->pid = 0;
    code = w > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (report_fd >= 0) {
        ssize_t n = read(report_fd, why, sizeof(why) - 1);

        why[n > 0 ? n : 0] = '\0';
        close(report_fd);
        report_fd = -1;
    }
    if (code == SYSD_RADIO_EXIT_DONE) {
        r->done = true;
        LOG_INFO("radio setup: done (sx1262, radio on, meshcored enabled)");
        return;
    }
    r->failed = true;
    if (code == SYSD_RADIO_EXIT_WRITE && !why[0]) {
        snprintf(why, sizeof(why), "a file in /etc/default could not be written");
    } else if (!why[0]) {
        snprintf(why, sizeof(why), "the setup did not finish (exit %d)", code);
    }
    /* A failed restore has already said what did not come back. */
    snprintf(r->error, sizeof(r->error), "%s%s", why,
             code == SYSD_RADIO_EXIT_WRITE     ? "; nothing was changed"
             : code == SYSD_RADIO_EXIT_RESTORE ? ""
                                               : "; the previous settings are back");
    LOG_ERROR("radio setup: %s. See %s.", r->error, r->paths.log);
}

bool sysd_radio_busy(const struct sysd_radio *r)
{
    return r->pid > 0;
}

cJSON *sysd_radio_status(struct sysd_radio *r)
{
    struct sysd_radio_config c;
    cJSON *o = cJSON_CreateObject();

    sysd_radio_reap(r);
    sysd_radio_read(r, &c);
    cJSON_AddStringToObject(o, "state", r->pid > 0 ? "running" : r->failed ? "failed" : r->done ? "done" : "idle");
    cJSON_AddBoolToObject(o, "needed", sysd_radio_needed(&c));
    cJSON_AddStringToObject(o, "backend", c.backend);
    cJSON_AddBoolToObject(o, "meshcored_enabled", c.meshcored_enabled);
    if (r->failed) {
        cJSON_AddStringToObject(o, "error", r->error);
    }
    return o;
}
