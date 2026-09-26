/*
 * The Recorder's state machine. Pure: an input (the owner's action, or what
 * the helper said) goes in, one action comes out, and the state is one enum
 * - never a combination of booleans. rec_ctl.c performs the actions; the
 * tests drive this table directly (tests/rec_state_test.c).
 *
 *   CHECKING    the repair helper runs (every open, and after a helper
 *               crashed mid-recording); nothing else may start
 *   IDLE        no helper; recordings can be started, played, deleted
 *   STARTING    a helper was started (op says which), not yet running
 *   RECORDING   capture running, file growing
 *   PAUSED      recording paused: device closed, file kept open
 *   PLAYING     playback running
 *   PLAY_PAUSED playback paused: device closed, position kept
 *   STOPPING    stop sent; waiting for the helper to finalize and exit
 *   ERROR       like IDLE, with the last failure shown until the next action
 *
 * Only one helper exists at a time, so capture and playback exclude each
 * other by construction: RECORD while playing stops the playback first and
 * starts the recording when it has exited (next), and PLAY while recording
 * is refused. STOP in a state with nothing to stop does nothing, however
 * often it is pressed; a second RECORD while starting does nothing; a pause
 * or resume already sent is not sent again until the helper answers.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_STATE_H
#define POCKETREC_STATE_H

#include <stdbool.h>

enum rec_state {
    REC_ST_CHECKING,
    REC_ST_IDLE,
    REC_ST_STARTING,
    REC_ST_RECORDING,
    REC_ST_PAUSED,
    REC_ST_PLAYING,
    REC_ST_PLAY_PAUSED,
    REC_ST_STOPPING,
    REC_ST_ERROR
};

/* What the one helper is for. */
enum rec_op {
    REC_OP_NONE,
    REC_OP_CHECK,
    REC_OP_RECORD,
    REC_OP_PLAY
};

enum rec_input {
    /* the owner */
    REC_IN_RECORD,
    REC_IN_STOP,
    REC_IN_PAUSE,      /* a toggle: pause, or resume when paused */
    REC_IN_PLAY,       /* play the selected recording */
    REC_IN_DELETE,     /* delete the selected recording (after confirmation) */
    REC_IN_CLOSE,      /* the app is being destroyed */
    /* the helper */
    REC_IN_H_RECORDING,
    REC_IN_H_PLAYING,
    REC_IN_H_PAUSED,
    REC_IN_H_RESUMED,
    REC_IN_H_ERROR,    /* an error line; the exit follows */
    REC_IN_H_EXITED,   /* exited with 0 */
    REC_IN_H_FAILED,   /* exited non-zero, or could not be started */
    REC_IN_H_CRASHED   /* killed by a signal */
};

enum rec_action {
    REC_ACT_NONE,
    REC_ACT_REJECT,        /* not now: say why, change nothing */
    REC_ACT_START_CHECK,
    REC_ACT_START_RECORD,
    REC_ACT_START_PLAY,
    REC_ACT_SEND_PAUSE,
    REC_ACT_SEND_RESUME,
    REC_ACT_STOP,
    REC_ACT_DELETE,
    REC_ACT_ABANDON,       /* the app is closing: stop and reap, bounded */
    REC_ACT_REFRESH        /* the helper is gone: read the folder again */
};

struct rec_machine {
    enum rec_state state;
    enum rec_op op;
    enum rec_op next;      /* started once the current helper has exited */
    bool cmd_pending;      /* a pause/resume was sent and not yet answered */
    bool failed;           /* the helper reported an error during this op */
};

/* Starts in CHECKING with the repair to be started (REC_ACT_START_CHECK is
 * what the caller does first). */
void rec_machine_init(struct rec_machine *m);
enum rec_action rec_machine_input(struct rec_machine *m, enum rec_input in);

/* Whether a helper may be alive in this state (anything but IDLE/ERROR). */
bool rec_machine_busy(const struct rec_machine *m);
/* Whether the microphone may be open: STARTING a recording, RECORDING, or
 * STOPPING one. The screen says "Microphone on" exactly then. */
bool rec_machine_mic_on(const struct rec_machine *m);

const char *rec_state_name(enum rec_state s);

#endif
