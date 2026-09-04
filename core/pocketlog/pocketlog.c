/*
 * pocketlog implementation. See pocketlog.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static char log_name[32] = "unknown";
static char log_dir[256] = POCKETLOG_DEFAULT_DIR;
static char log_path[512];
static int log_fd = -1;
static enum pocketlog_level log_level = POCKETLOG_INFO;
static const char *level_names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

static enum pocketlog_level level_from_string(const char *s)
{
    if (!s) {
        return POCKETLOG_INFO;
    }
    if (strcmp(s, "debug") == 0) return POCKETLOG_DEBUG;
    if (strcmp(s, "warn") == 0) return POCKETLOG_WARN;
    if (strcmp(s, "error") == 0) return POCKETLOG_ERROR;
    return POCKETLOG_INFO;
}

static void open_log_file(void)
{
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
    if (mkdir(log_dir, 0755) < 0 && errno != EEXIST) {
        return;
    }
    snprintf(log_path, sizeof(log_path), "%s/%s.log", log_dir, log_name);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

void pocketlog_init(const char *name)
{
    const char *dir = getenv("POCKETOS_LOG_DIR");

    snprintf(log_name, sizeof(log_name), "%s", name ? name : "unknown");
    if (dir && *dir) {
        snprintf(log_dir, sizeof(log_dir), "%s", dir);
    }
    log_level = level_from_string(getenv("POCKETOS_LOG_LEVEL"));
    open_log_file();
}

void pocketlog_set_level(enum pocketlog_level level)
{
    log_level = level;
}

enum pocketlog_level pocketlog_get_level(void)
{
    return log_level;
}

const char *pocketlog_dir(void)
{
    return log_dir;
}

void pocketlog_close(void)
{
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
}

static void rotate_if_needed(void)
{
    struct stat st;

    if (log_fd < 0 || fstat(log_fd, &st) < 0 || st.st_size < (off_t)POCKETLOG_MAX_BYTES) {
        return;
    }
    {
        char old[520];

        snprintf(old, sizeof(old), "%s.1", log_path);
        rename(log_path, old);
    }
    open_log_file();
}

static void timestamp(char *buf, size_t n)
{
    struct timespec ts;
    struct tm tm;

    char ms[8];
    size_t len;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    len = strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(ms, sizeof(ms), ".%03uZ", (unsigned)(ts.tv_nsec / 1000000) % 1000u);
    if (len + strlen(ms) < n) {
        memcpy(buf + len, ms, strlen(ms) + 1);
    }
}

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
{
    char line[1024];
    char ts[40];
    va_list ap;
    int n;
    int m;

    if (level < log_level) {
        return;
    }
    timestamp(ts, sizeof(ts));
    n = snprintf(line, sizeof(line), "%s %s %s ", ts, log_name, level_names[level]);
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) {
        return;
    }
    n += m;
    if ((size_t)n >= sizeof(line) - 1) {
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';
    if (write(STDERR_FILENO, line, (size_t)n) < 0) {
        /* nothing sensible to do */
    }
    if (log_fd >= 0) {
        rotate_if_needed();
        if (write(log_fd, line, (size_t)n) < 0) {
            /* keep going; disk full must not kill the service */
        }
    }
}

/* ---- crash handler (async-signal-safe) ------------------------------- */

static void safe_write(int fd, const char *s)
{
    size_t len = strlen(s);

    while (len > 0) {
        ssize_t w = write(fd, s, len);

        if (w <= 0) {
            return;
        }
        s += w;
        len -= (size_t)w;
    }
}

static void safe_ulong(int fd, unsigned long v)
{
    char buf[24];
    int i = sizeof(buf) - 1;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    safe_write(fd, buf + i);
}

static void crash_handler(int sig)
{
    char path[600];
    void *frames[64];
    int nframes;
    int fd;
    size_t pos = 0;
    const char *p;

    /* build "<dir>/crash-<name>-<time>.txt" without snprintf (not signal-safe) */
    for (p = log_dir; *p && pos < sizeof(path) - 1; p++) path[pos++] = *p;
    for (p = "/crash-"; *p && pos < sizeof(path) - 1; p++) path[pos++] = *p;
    for (p = log_name; *p && pos < sizeof(path) - 1; p++) path[pos++] = *p;
    path[pos++] = '-';
    {
        unsigned long t = (unsigned long)time(NULL);
        char digits[24];
        int i = sizeof(digits) - 1;

        digits[i] = '\0';
        do {
            digits[--i] = (char)('0' + t % 10);
            t /= 10;
        } while (t && i > 0);
        for (p = digits + i; *p && pos < sizeof(path) - 1; p++) path[pos++] = *p;
    }
    for (p = ".txt"; *p && pos < sizeof(path) - 1; p++) path[pos++] = *p;
    path[pos] = '\0';

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    nframes = backtrace(frames, 64);
    if (fd >= 0) {
        safe_write(fd, "PocketOS crash report\nprocess: ");
        safe_write(fd, log_name);
        safe_write(fd, "\npid: ");
        safe_ulong(fd, (unsigned long)getpid());
        safe_write(fd, "\nsignal: ");
        safe_ulong(fd, (unsigned long)sig);
        safe_write(fd, "\nbacktrace:\n");
        backtrace_symbols_fd(frames, nframes, fd);
        close(fd);
    }
    safe_write(STDERR_FILENO, log_name);
    safe_write(STDERR_FILENO, ": fatal signal ");
    safe_ulong(STDERR_FILENO, (unsigned long)sig);
    safe_write(STDERR_FILENO, ", crash report ");
    safe_write(STDERR_FILENO, fd >= 0 ? path : "(not written)");
    safe_write(STDERR_FILENO, "\n");

    signal(sig, SIG_DFL);
    raise(sig);
}

int pocketlog_install_crash_handler(void)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    struct sigaction sa;
    size_t i;
    void *warm[4];

    /* backtrace() may lazily load libgcc; do it now, outside the handler. */
    backtrace(warm, 4);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (sigaction(sigs[i], &sa, NULL) < 0) {
            return -1;
        }
    }
    return 0;
}
