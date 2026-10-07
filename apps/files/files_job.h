/*
 * Files: the one worker that runs copy, move and delete off the LVGL thread.
 *
 * A copy of a large file or a delete of a large folder takes as long as the
 * storage takes, and on the LVGL thread that would be a frozen screen. So the
 * app hands them to this worker and polls it from a timer; the worker never
 * touches LVGL, only the filesystem (files_fs.h) and the job it was given.
 *
 * One job at a time. The job struct is owned by the app and must outlive
 * the thread: files_job_abandon() is the last thing the app does with it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FILES_JOB_H
#define FILES_JOB_H

#include "files_fs.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

enum files_op {
    FILES_OP_COPY = 0,
    FILES_OP_MOVE,
    FILES_OP_DELETE,
};

struct files_job {
    pthread_t thread;
    bool started;       /* a thread exists that has not been joined */
    atomic_int done;    /* set by the worker as its last act */
    atomic_int cancel;  /* set by the app to stop a copy early */
    enum files_op op;
    const struct files_policy *pol;
    char src[FILES_PATH_MAX];
    char dst_dir[FILES_PATH_MAX]; /* unused by delete */
    char out_name[FILES_NAME_MAX + 1]; /* the name a copy got */
    int result;         /* 0 or a negative error (files_strerror) */
};

/* Start op on src (into dst_dir for copy and move). Returns 0, -EBUSY while
 * another job has not been collected, -ENAMETOOLONG, or the error of
 * pthread_create. The job struct must be zeroed before its first use. */
int files_job_start(struct files_job *j, enum files_op op, const struct files_policy *pol,
                    const char *src, const char *dst_dir);

/* A job has been started and not yet collected. */
bool files_job_busy(const struct files_job *j);

/* Collect a finished job: true, with its result in *result, once the worker
 * is done (the thread is joined here); false while it runs or when there is
 * nothing to collect. Never blocks. */
bool files_job_poll(struct files_job *j, int *result);

/* Ask a running job to stop and wait for it; nothing is collected. A copy
 * stops at its next chunk and removes what it made; a delete stops at its
 * next entry. For the app's destroy(), which cannot leave a thread behind. */
void files_job_abandon(struct files_job *j);

#endif
