/*
 * The Recorder's state machine. See rec_state.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_state.h"

#include <string.h>

void rec_machine_init(struct rec_machine *m)
{
    memset(m, 0, sizeof(*m));
    m->state = REC_ST_CHECKING;
    m->op = REC_OP_CHECK;
    m->next = REC_OP_NONE;
}

bool rec_machine_busy(const struct rec_machine *m)
{
    return m->state != REC_ST_IDLE && m->state != REC_ST_ERROR;
}

bool rec_machine_mic_on(const struct rec_machine *m)
{
    return m->op == REC_OP_RECORD &&
           (m->state == REC_ST_STARTING || m->state == REC_ST_RECORDING ||
            m->state == REC_ST_STOPPING);
}

const char *rec_state_name(enum rec_state s)
{
    static const char *const names[] = { "checking", "idle",    "starting",
                                         "recording", "paused", "playing",
                                         "play-paused", "stopping", "error" };

    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static enum rec_action start(struct rec_machine *m, enum rec_op op)
{
    m->state = REC_ST_STARTING;
    m->op = op;
    m->next = REC_OP_NONE;
    m->cmd_pending = false;
    m->failed = false;
    return op == REC_OP_RECORD ? REC_ACT_START_RECORD : REC_ACT_START_PLAY;
}

static enum rec_action stop(struct rec_machine *m, enum rec_op next)
{
    m->state = REC_ST_STOPPING;
    m->next = next;
    m->cmd_pending = false;
    return REC_ACT_STOP;
}

/* The helper is gone: what happens next. */
static enum rec_action exited(struct rec_machine *m, enum rec_input how)
{
    bool bad = m->failed || how != REC_IN_H_EXITED;
    enum rec_op was = m->op;
    enum rec_op next = m->next;

    m->cmd_pending = false;
    m->op = REC_OP_NONE;
    m->next = REC_OP_NONE;
    if (was == REC_OP_RECORD && how == REC_IN_H_CRASHED) {
        /* It could not finalize: its .part is waiting for the repair. */
        m->state = REC_ST_CHECKING;
        m->op = REC_OP_CHECK;
        m->failed = true;
        return REC_ACT_START_CHECK;
    }
    if (next != REC_OP_NONE && !bad) {
        return start(m, next);
    }
    m->state = bad ? REC_ST_ERROR : REC_ST_IDLE;
    return REC_ACT_REFRESH;
}

enum rec_action rec_machine_input(struct rec_machine *m, enum rec_input in)
{
    if (in == REC_IN_CLOSE) {
        enum rec_action a = rec_machine_busy(m) ? REC_ACT_ABANDON : REC_ACT_NONE;

        m->state = REC_ST_IDLE;
        m->op = REC_OP_NONE;
        m->next = REC_OP_NONE;
        m->cmd_pending = false;
        return a;
    }
    if (in == REC_IN_H_EXITED || in == REC_IN_H_FAILED || in == REC_IN_H_CRASHED) {
        return rec_machine_busy(m) ? exited(m, in) : REC_ACT_NONE;
    }
    if (in == REC_IN_H_ERROR) {
        if (rec_machine_busy(m)) {
            m->failed = true;
        }
        return REC_ACT_NONE;
    }

    switch (m->state) {
    case REC_ST_IDLE:
    case REC_ST_ERROR:
        switch (in) {
        case REC_IN_RECORD: return start(m, REC_OP_RECORD);
        case REC_IN_PLAY: return start(m, REC_OP_PLAY);
        case REC_IN_DELETE:
            m->state = REC_ST_IDLE;
            return REC_ACT_DELETE;
        default: return REC_ACT_NONE; /* STOP, PAUSE and stray helper words */
        }

    case REC_ST_CHECKING:
        return in == REC_IN_RECORD || in == REC_IN_PLAY || in == REC_IN_DELETE ? REC_ACT_REJECT
                                                                                : REC_ACT_NONE;

    case REC_ST_STARTING:
        if (in == REC_IN_H_RECORDING && m->op == REC_OP_RECORD) {
            m->state = REC_ST_RECORDING;
            return REC_ACT_NONE;
        }
        if (in == REC_IN_H_PLAYING && m->op == REC_OP_PLAY) {
            m->state = REC_ST_PLAYING;
            return REC_ACT_NONE;
        }
        if (in == REC_IN_STOP) {
            return stop(m, REC_OP_NONE);
        }
        if (in == REC_IN_RECORD && m->op == REC_OP_PLAY) {
            return stop(m, REC_OP_RECORD);
        }
        if (in == REC_IN_PLAY && m->op == REC_OP_PLAY) {
            return stop(m, REC_OP_PLAY);
        }
        return in == REC_IN_PLAY || in == REC_IN_DELETE ? REC_ACT_REJECT : REC_ACT_NONE;

    case REC_ST_RECORDING:
    case REC_ST_PAUSED:
        switch (in) {
        case REC_IN_STOP: return stop(m, REC_OP_NONE);
        case REC_IN_PAUSE:
            if (m->cmd_pending) {
                return REC_ACT_NONE;
            }
            m->cmd_pending = true;
            return m->state == REC_ST_RECORDING ? REC_ACT_SEND_PAUSE : REC_ACT_SEND_RESUME;
        case REC_IN_H_PAUSED:
            m->state = REC_ST_PAUSED;
            m->cmd_pending = false;
            return REC_ACT_NONE;
        case REC_IN_H_RESUMED:
            m->state = REC_ST_RECORDING;
            m->cmd_pending = false;
            return REC_ACT_NONE;
        case REC_IN_PLAY:
        case REC_IN_DELETE: return REC_ACT_REJECT;
        default: return REC_ACT_NONE;
        }

    case REC_ST_PLAYING:
    case REC_ST_PLAY_PAUSED:
        switch (in) {
        case REC_IN_STOP: return stop(m, REC_OP_NONE);
        case REC_IN_RECORD: return stop(m, REC_OP_RECORD);
        case REC_IN_PLAY: return stop(m, REC_OP_PLAY);
        case REC_IN_PAUSE:
            if (m->cmd_pending) {
                return REC_ACT_NONE;
            }
            m->cmd_pending = true;
            return m->state == REC_ST_PLAYING ? REC_ACT_SEND_PAUSE : REC_ACT_SEND_RESUME;
        case REC_IN_H_PAUSED:
            m->state = REC_ST_PLAY_PAUSED;
            m->cmd_pending = false;
            return REC_ACT_NONE;
        case REC_IN_H_RESUMED:
            m->state = REC_ST_PLAYING;
            m->cmd_pending = false;
            return REC_ACT_NONE;
        case REC_IN_DELETE: return REC_ACT_REJECT;
        default: return REC_ACT_NONE;
        }

    case REC_ST_STOPPING:
        /* A later RECORD or PLAY replaces what happens after the exit; the
         * rest waits for it. */
        if (in == REC_IN_RECORD && m->op != REC_OP_RECORD) {
            m->next = REC_OP_RECORD;
        } else if (in == REC_IN_PLAY && m->op == REC_OP_PLAY) {
            m->next = REC_OP_PLAY;
        } else if (in == REC_IN_DELETE || in == REC_IN_PLAY) {
            return REC_ACT_REJECT;
        }
        return REC_ACT_NONE;
    }
    return REC_ACT_NONE;
}
