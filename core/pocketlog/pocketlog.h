/*
 * pocketlog: structured line logging and crash reports for PocketOS
 * services and the shell.
 *
 * Line format (one record per line, UTC):
 *   2026-09-04T13:20:01.123Z radiod INFO  message text
 * Output goes to $POCKETOS_LOG_DIR/<name>.log (default
 * /var/lib/pocketos/log), rotated once to <name>.log.1 when it exceeds
 * POCKETLOG_MAX_BYTES, and to stderr unless $POCKETOS_LOG_STDERR is "0".
 * Level filter from $POCKETOS_LOG_LEVEL (debug|info|warn|error), default
 * info.
 *
 * The default directory is under /var/lib on purpose: on the K230 image
 * /var/log is a tmpfs, so logs and crash reports there would not survive
 * a reboot or a power cut. The init scripts set POCKETOS_LOG_STDERR=0 so
 * the same lines are not written a second time into the stdio capture.
 *
 * Crash reports: pocketlog_install_crash_handler() catches SIGSEGV, SIGBUS,
 * SIGILL, SIGFPE and SIGABRT, writes
 * $POCKETOS_LOG_DIR/crash-<name>-<unix time>.txt with a backtrace using only
 * async-signal-safe calls, then re-raises the signal.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETLOG_H
#define POCKETLOG_H

#include <stddef.h>

#define POCKETLOG_DEFAULT_DIR "/var/lib/pocketos/log"
#define POCKETLOG_MAX_BYTES (512u * 1024u)

enum pocketlog_level {
    POCKETLOG_DEBUG = 0,
    POCKETLOG_INFO = 1,
    POCKETLOG_WARN = 2,
    POCKETLOG_ERROR = 3,
};

/* name identifies the process (log file name and crash report prefix). */
void pocketlog_init(const char *name);
void pocketlog_set_level(enum pocketlog_level level);
enum pocketlog_level pocketlog_get_level(void);
const char *pocketlog_dir(void);
void pocketlog_close(void);

void pocketlog_write(enum pocketlog_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) pocketlog_write(POCKETLOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) pocketlog_write(POCKETLOG_INFO, __VA_ARGS__)
#define LOG_WARN(...) pocketlog_write(POCKETLOG_WARN, __VA_ARGS__)
#define LOG_ERROR(...) pocketlog_write(POCKETLOG_ERROR, __VA_ARGS__)

/* Returns 0 on success. Safe to call before pocketlog_init (uses "unknown"). */
int pocketlog_install_crash_handler(void);

#endif
