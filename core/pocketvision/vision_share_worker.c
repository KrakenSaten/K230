/*
 * The shared picture's encoder thread (vision_share_worker.h).
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vision_share_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum job_state {
    JOB_IDLE = 0, /* nothing: a picture may be submitted */
    JOB_QUEUED,   /* copied in, the worker has not begun */
    JOB_WORKING,  /* being encoded; the input and output are the worker's */
    JOB_DONE      /* the result waits to be taken */
};

struct vision_share_worker {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    enum job_state state;
    bool quit;
    /* the picture, copied in */
    uint16_t *px;
    size_t px_cap; /* pixels */
    uint32_t pw;
    uint32_t ph;
    uint32_t div;
    int quality;
    int retry_quality;
    uint32_t tag;
    /* the result */
    int n;
    uint32_t ow;
    uint32_t oh;
    uint32_t ms;
    uint8_t out[VISION_SHARE_MAX_BYTES];
};

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *run(void *arg)
{
    struct vision_share_worker *w = arg;

    pthread_mutex_lock(&w->mu);
    for (;;) {
        int64_t t0;
        uint32_t ow = 0;
        uint32_t oh = 0;
        int n;

        while (w->state != JOB_QUEUED && !w->quit) {
            pthread_cond_wait(&w->cv, &w->mu);
        }
        if (w->quit) {
            break;
        }
        w->state = JOB_WORKING;
        /* While working, px, the parameters and out are this thread's alone:
         * submit() and take() touch them only in other states. */
        pthread_mutex_unlock(&w->mu);
        t0 = now_ms();
        n = vision_share_encode(w->px, w->pw, w->ph, w->pw, w->div, w->quality, w->out, sizeof(w->out), &ow, &oh);
        if (n == -ENOSPC && w->retry_quality > 0 && w->retry_quality < w->quality) {
            n = vision_share_encode(w->px, w->pw, w->ph, w->pw, w->div, w->retry_quality, w->out, sizeof(w->out), &ow,
                                    &oh);
        }
        pthread_mutex_lock(&w->mu);
        w->n = n;
        w->ow = ow;
        w->oh = oh;
        w->ms = (uint32_t)(now_ms() - t0);
        w->state = JOB_DONE;
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

struct vision_share_worker *vision_share_worker_new(void)
{
    struct vision_share_worker *w = calloc(1, sizeof(*w));

    if (!w) {
        return NULL;
    }
    if (pthread_mutex_init(&w->mu, NULL) != 0) {
        free(w);
        return NULL;
    }
    if (pthread_cond_init(&w->cv, NULL) != 0) {
        pthread_mutex_destroy(&w->mu);
        free(w);
        return NULL;
    }
    if (pthread_create(&w->thread, NULL, run, w) != 0) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w);
        return NULL;
    }
    return w;
}

void vision_share_worker_free(struct vision_share_worker *w)
{
    if (!w) {
        return;
    }
    pthread_mutex_lock(&w->mu);
    w->quit = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    free(w->px);
    free(w);
}

int vision_share_worker_submit(struct vision_share_worker *w, const uint16_t *px, uint32_t pw, uint32_t ph,
                               uint32_t stride, uint32_t div, int quality, int retry_quality, uint32_t tag)
{
    size_t need;
    uint32_t y;
    int r = 0;

    if (!w || !px || pw == 0 || ph == 0 || stride < pw || div == 0 || pw > 4096 || ph > 4096) {
        return -EINVAL;
    }
    need = (size_t)pw * ph;
    pthread_mutex_lock(&w->mu);
    if (w->state != JOB_IDLE) {
        r = -EBUSY;
    } else {
        if (w->px_cap < need) {
            uint16_t *p = realloc(w->px, need * sizeof(*p));

            if (p) {
                w->px = p;
                w->px_cap = need;
            } else {
                r = -ENOMEM;
            }
        }
        if (r == 0) {
            for (y = 0; y < ph; y++) {
                memcpy(w->px + (size_t)y * pw, px + (size_t)y * stride, (size_t)pw * sizeof(*px));
            }
            w->pw = pw;
            w->ph = ph;
            w->div = div;
            w->quality = quality;
            w->retry_quality = retry_quality;
            w->tag = tag;
            w->state = JOB_QUEUED;
            pthread_cond_signal(&w->cv);
        }
    }
    pthread_mutex_unlock(&w->mu);
    return r;
}

int vision_share_worker_take(struct vision_share_worker *w, uint8_t *dst, size_t cap, int *n, uint32_t *ow,
                             uint32_t *oh, uint32_t *ms, uint32_t *tag)
{
    int got = 0;

    if (!w || !dst || !n || !ow || !oh || !ms || !tag) {
        return 0;
    }
    pthread_mutex_lock(&w->mu);
    if (w->state == JOB_DONE) {
        *n = w->n;
        if (w->n > 0) {
            if ((size_t)w->n <= cap) {
                memcpy(dst, w->out, (size_t)w->n);
            } else {
                *n = -ENOSPC;
            }
        }
        *ow = w->ow;
        *oh = w->oh;
        *ms = w->ms;
        *tag = w->tag;
        w->state = JOB_IDLE;
        got = 1;
    }
    pthread_mutex_unlock(&w->mu);
    return got;
}

bool vision_share_worker_busy(struct vision_share_worker *w)
{
    bool busy;

    if (!w) {
        return false;
    }
    pthread_mutex_lock(&w->mu);
    busy = w->state != JOB_IDLE;
    pthread_mutex_unlock(&w->mu);
    return busy;
}
