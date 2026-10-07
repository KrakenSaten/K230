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
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
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

/* The release file (ADR-005 Phase 2). The image writes one file,
 * /etc/doors-release, and makes /etc/pocketos-release a symlink to it, so a
 * reader that only knows the old name reads the same bytes and the two cannot
 * drift apart. New readers ask for the new name first and fall back to the old
 * one, which is all a card flashed before Doors carries - also when a single
 * newer binary has been deployed onto such a card. The format is the one
 * v0.0.7 introduced: line 1 the bare version, then a BUILD_ID=<id> line. */
#define POCKETOS_RELEASE_FILE "/etc/doors-release"
#define POCKETOS_RELEASE_FILE_COMPAT "/etc/pocketos-release"

struct pocketos_release {
    char path[POCKETOS_PATH_MAX]; /* the file that was read, root included */
    char version[256];            /* line 1 without its newline; may be empty */
    char build[256];              /* the first BUILD_ID= value; empty when absent */
};

const char *pocketos_runtime_dir(void);
const char *pocketos_config_dir(void);
const char *pocketos_state_dir(void);
const char *pocketos_log_dir(void);

/* Create path and every missing parent. Returns 0 when the directory exists
 * afterwards, -1 with errno otherwise. An existing path that is not a
 * directory is an error (EEXIST), rather than being reported as success. */
int pocketos_mkdir_p(const char *path, mode_t mode);

/* Read the release file below root ("" for the running system): the new name
 * when it exists, the old one only when the new one does not (ENOENT, which a
 * dangling symlink also gives). A new file that exists but cannot be read is
 * an error, not a reason to report an older file instead. Returns 0 with rel
 * filled, or -1 with errno, ENOENT when neither file is there. */
int pocketos_release_read(const char *root, struct pocketos_release *rel);

#endif
