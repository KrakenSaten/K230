/*
 * Files: the worker (files_job.h).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "files_job.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void *run(void *arg)
{
    struct files_job *j = arg;

    switch (j->op) {
    case FILES_OP_COPY:
        j->result = files_copy(j->pol, j->src, j->dst_dir, j->out_name, sizeof(j->out_name),
                               &j->cancel);
        break;
    case FILES_OP_MOVE:
        j->result = files_move(j->pol, j->src, j->dst_dir, &j->cancel);
        break;
    case FILES_OP_DELETE:
        j->result = files_delete(j->pol, j->src, &j->cancel);
        break;
    default:
        j->result = -EINVAL;
        break;
    }
    atomic_store(&j->done, 1);
    return NULL;
}

int files_job_start(struct files_job *j, enum files_op op, const struct files_policy *pol,
                    const char *src, const char *dst_dir)
{
    int n;
    int r;

    if (j->started) {
        return -EBUSY;
    }
    n = snprintf(j->src, sizeof(j->src), "%s", src);
    if (n < 0 || (size_t)n >= sizeof(j->src)) {
        return -ENAMETOOLONG;
    }
    n = snprintf(j->dst_dir, sizeof(j->dst_dir), "%s", dst_dir ? dst_dir : "");
    if (n < 0 || (size_t)n >= sizeof(j->dst_dir)) {
        return -ENAMETOOLONG;
    }
    j->op = op;
    j->pol = pol;
    j->out_name[0] = '\0';
    j->result = 0;
    atomic_store(&j->done, 0);
    atomic_store(&j->cancel, 0);
    r = pthread_create(&j->thread, NULL, run, j);
    if (r != 0) {
        return -r;
    }
    j->started = true;
    return 0;
}

bool files_job_busy(const struct files_job *j)
{
    return j->started;
}

bool files_job_poll(struct files_job *j, int *result)
{
    if (!j->started || !atomic_load(&j->done)) {
        return false;
    }
    pthread_join(j->thread, NULL);
    j->started = false;
    if (result) {
        *result = j->result;
    }
    return true;
}

void files_job_abandon(struct files_job *j)
{
    if (!j->started) {
        return;
    }
    atomic_store(&j->cancel, 1);
    pthread_join(j->thread, NULL);
    j->started = false;
}
