/*
 * PocketOS filesystem roots.
 *
 * Four directories make up the platform's filesystem contract, each with an
 * environment override that the tests and the init scripts already use:
 *
 *   runtime  $POCKETOS_RUNTIME_DIR  /run/pocketos        sockets, pid files, markers
 *   config   $POCKETOS_CONFIG_DIR   /etc/pocketos        settings
 *   state    $POCKETOS_STATE_DIR    /var/lib/pocketos    app-owned data
 *   log      $POCKETOS_LOG_DIR      /var/lib/pocketos/log  logs and crash reports
 *
 * They were spelled out as literals in pocketipc.h, pocketlog.h, settings.h,
 * three app stores and pos-supervise. Nothing owned them, so the layout could
 * not be moved in one place. This module owns them; the app stores keep their
 * own copies for now and adopt this together with the storage work, so that
 * this release changes no application source.
 *
 * pos-supervise is POSIX sh and cannot link against this. It carries the same
 * two defaults as literals, and tests/supervise_test.sh checks them against
 * this header so the two cannot drift.
 *
 * The returned pointers come from getenv() or from string literals; they stay
 * valid until the environment is changed, which is the same contract
 * pocketipc_runtime_dir() has always had.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_PATHS_H
#define POCKETOS_PATHS_H

#include <sys/types.h>

#define POCKETOS_RUNTIME_DIR_DEFAULT "/run/pocketos"
#define POCKETOS_CONFIG_DIR_DEFAULT "/etc/pocketos"
#define POCKETOS_STATE_DIR_DEFAULT "/var/lib/pocketos"
#define POCKETOS_LOG_DIR_DEFAULT "/var/lib/pocketos/log"

/* Longest path this module will build, including the terminator. */
#define POCKETOS_PATH_MAX 512

const char *pocketos_runtime_dir(void);
const char *pocketos_config_dir(void);
const char *pocketos_state_dir(void);
const char *pocketos_log_dir(void);

/* Create path and every missing parent. Returns 0 when the directory exists
 * afterwards, -1 with errno otherwise. An existing path that is not a
 * directory is an error (EEXIST), rather than being reported as success. */
int pocketos_mkdir_p(const char *path, mode_t mode);

#endif
