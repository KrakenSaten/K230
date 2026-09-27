/*
 * The Recorder's state machine (apps/recorder/rec_state.h): every state's
 * answer to every input is one of a small set, and the sequences that go
 * wrong in a recorder - rapid start/stop, a repeated stop, a double tap on
 * RECORD, pause pressed twice before the helper answers, the app closed in
 * any state, a helper that crashes while recording, one that fails to start,
 * RECORD during a playback - end where they should.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rec_state.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static struct rec_machine m;

static enum rec_action in(enum rec_input i)
{
    return rec_machine_input(&m, i);
}

/* A machine in IDLE, past the opening repair. */
static void idle(void)
{
    rec_machine_init(&m);
    in(REC_IN_H_EXITED);
}

static void recording(void)
{
    idle();
    in(REC_IN_RECORD);
    in(REC_IN_H_RECORDING);
}

static void playing(void)
{
    idle();
    in(REC_IN_PLAY);
    in(REC_IN_H_PLAYING);
}

static void test_open(void)
{
    rec_machine_init(&m);
    check("a new machine is checking, the repair being its op", m.state == REC_ST_CHECKING && m.op == REC_OP_CHECK);
    check("nothing starts while it checks", in(REC_IN_RECORD) == REC_ACT_REJECT &&
                                                in(REC_IN_PLAY) == REC_ACT_REJECT &&
                                                in(REC_IN_DELETE) == REC_ACT_REJECT &&
                                                m.state == REC_ST_CHECKING);
    check("stop and pause do nothing", in(REC_IN_STOP) == REC_ACT_NONE && in(REC_IN_PAUSE) == REC_ACT_NONE);
    check("the repair's exit makes it idle and reads the folder",
          in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_IDLE && m.op == REC_OP_NONE);
    rec_machine_init(&m);
    in(REC_IN_H_ERROR);
    check("a repair that failed leaves the app usable, in ERROR",
          in(REC_IN_H_FAILED) == REC_ACT_REFRESH && m.state == REC_ST_ERROR);
    check("from which RECORD works", in(REC_IN_RECORD) == REC_ACT_START_RECORD && m.state == REC_ST_STARTING);
}

static void test_record(void)
{
    idle();
    check("RECORD starts a recording helper", in(REC_IN_RECORD) == REC_ACT_START_RECORD &&
                                                  m.state == REC_ST_STARTING && m.op == REC_OP_RECORD);
    check("the microphone counts as on from the start", rec_machine_mic_on(&m));
    check("a second RECORD while starting does nothing", in(REC_IN_RECORD) == REC_ACT_NONE &&
                                                             m.state == REC_ST_STARTING);
    check("the helper saying it records makes it RECORDING",
          in(REC_IN_H_RECORDING) == REC_ACT_NONE && m.state == REC_ST_RECORDING);
    check("a PLAYING word from a recording helper is ignored",
          in(REC_IN_H_PLAYING) == REC_ACT_NONE && m.state == REC_ST_RECORDING);
    check("PLAY and DELETE are refused while recording",
          in(REC_IN_PLAY) == REC_ACT_REJECT && in(REC_IN_DELETE) == REC_ACT_REJECT);
    check("STOP stops it", in(REC_IN_STOP) == REC_ACT_STOP && m.state == REC_ST_STOPPING);
    check("the microphone still counts as on while it saves", rec_machine_mic_on(&m));
    check("a second STOP does nothing", in(REC_IN_STOP) == REC_ACT_NONE && m.state == REC_ST_STOPPING);
    check("a tenth does nothing either", (in(REC_IN_STOP), in(REC_IN_STOP), in(REC_IN_STOP)) == REC_ACT_NONE);
    check("the exit ends it in IDLE and reads the folder",
          in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_IDLE && !rec_machine_mic_on(&m));
    check("STOP in IDLE does nothing", in(REC_IN_STOP) == REC_ACT_NONE && m.state == REC_ST_IDLE);
    check("a stray exit in IDLE does nothing", in(REC_IN_H_EXITED) == REC_ACT_NONE && m.state == REC_ST_IDLE);
}

static void test_rapid(void)
{
    int k;
    int ok = 1;

    idle();
    check("RECORD then STOP before the helper answered: stopping",
          in(REC_IN_RECORD) == REC_ACT_START_RECORD && in(REC_IN_STOP) == REC_ACT_STOP &&
              m.state == REC_ST_STOPPING);
    check("a RECORDING word that arrives now does not restart it",
          in(REC_IN_H_RECORDING) == REC_ACT_NONE && m.state == REC_ST_STOPPING);
    check("RECORD pressed again while it stops is not queued behind itself",
          in(REC_IN_RECORD) == REC_ACT_NONE && m.next == REC_OP_NONE);
    check("the exit leaves it idle", in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_IDLE);

    idle();
    for (k = 0; k < 50; k++) {
        enum rec_action a = in(REC_IN_RECORD);
        enum rec_action b;

        in(REC_IN_H_RECORDING);
        b = in(REC_IN_STOP);
        ok &= a == REC_ACT_START_RECORD && b == REC_ACT_STOP;
        in(REC_IN_STOP);
        ok &= in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_IDLE;
    }
    check("fifty record/stop cycles each start once, stop once and end idle", ok);
}

static void test_pause(void)
{
    recording();
    check("PAUSE sends pause", in(REC_IN_PAUSE) == REC_ACT_SEND_PAUSE && m.cmd_pending);
    check("a second PAUSE before the answer sends nothing", in(REC_IN_PAUSE) == REC_ACT_NONE);
    check("the answer makes it PAUSED", in(REC_IN_H_PAUSED) == REC_ACT_NONE && m.state == REC_ST_PAUSED &&
                                            !m.cmd_pending);
    check("the microphone is not on while paused", !rec_machine_mic_on(&m));
    check("PAUSE again resumes", in(REC_IN_PAUSE) == REC_ACT_SEND_RESUME);
    check("and the answer makes it RECORDING", in(REC_IN_H_RESUMED) == REC_ACT_NONE &&
                                                   m.state == REC_ST_RECORDING);
    in(REC_IN_PAUSE);
    in(REC_IN_H_PAUSED);
    check("STOP while paused saves", in(REC_IN_STOP) == REC_ACT_STOP && m.state == REC_ST_STOPPING);
    check("a late PAUSED word while stopping is ignored", in(REC_IN_H_PAUSED) == REC_ACT_NONE &&
                                                             m.state == REC_ST_STOPPING);
    in(REC_IN_H_EXITED);

    playing();
    check("PAUSE pauses a playback", in(REC_IN_PAUSE) == REC_ACT_SEND_PAUSE &&
                                         in(REC_IN_H_PAUSED) == REC_ACT_NONE && m.state == REC_ST_PLAY_PAUSED);
    check("and resumes it", in(REC_IN_PAUSE) == REC_ACT_SEND_RESUME && in(REC_IN_H_RESUMED) == REC_ACT_NONE &&
                                m.state == REC_ST_PLAYING);
}

static void test_play(void)
{
    idle();
    check("PLAY starts a playback helper", in(REC_IN_PLAY) == REC_ACT_START_PLAY && m.op == REC_OP_PLAY);
    check("a playback never counts as the microphone", !rec_machine_mic_on(&m));
    in(REC_IN_H_PLAYING);
    check("its end ends it", in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_IDLE);

    playing();
    check("RECORD during a playback stops it first", in(REC_IN_RECORD) == REC_ACT_STOP &&
                                                         m.state == REC_ST_STOPPING && m.next == REC_OP_RECORD);
    check("and starts the recording once it has exited - never both at once",
          in(REC_IN_H_EXITED) == REC_ACT_START_RECORD && m.state == REC_ST_STARTING && m.op == REC_OP_RECORD);

    playing();
    check("PLAY during a playback plays the new selection after the old one stopped",
          in(REC_IN_PLAY) == REC_ACT_STOP && in(REC_IN_H_EXITED) == REC_ACT_START_PLAY && m.op == REC_OP_PLAY);

    playing();
    in(REC_IN_RECORD);
    in(REC_IN_H_ERROR);
    check("if the stopped playback reported a failure, nothing is started after it",
          in(REC_IN_H_FAILED) == REC_ACT_REFRESH && m.state == REC_ST_ERROR && m.next == REC_OP_NONE);

    idle();
    in(REC_IN_PLAY);
    check("a playback whose start failed ends in ERROR", in(REC_IN_H_FAILED) == REC_ACT_REFRESH &&
                                                            m.state == REC_ST_ERROR);
    check("DELETE works from ERROR", in(REC_IN_DELETE) == REC_ACT_DELETE && m.state == REC_ST_IDLE);
}

static void test_failures(void)
{
    recording();
    in(REC_IN_H_ERROR);
    check("an error line alone changes nothing yet", m.state == REC_ST_RECORDING && m.failed);
    check("the exit after it is ERROR", in(REC_IN_H_FAILED) == REC_ACT_REFRESH && m.state == REC_ST_ERROR);

    recording();
    in(REC_IN_H_ERROR);
    check("an error line and exit code 0 is still an error",
          in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_ERROR);

    recording();
    check("a recording helper killed by a signal starts the repair",
          in(REC_IN_H_CRASHED) == REC_ACT_START_CHECK && m.state == REC_ST_CHECKING && m.op == REC_OP_CHECK);
    check("which ends in ERROR, so the crash stays said",
          in(REC_IN_H_EXITED) == REC_ACT_REFRESH && m.state == REC_ST_ERROR);

    playing();
    check("a crashed playback needs no repair", in(REC_IN_H_CRASHED) == REC_ACT_REFRESH &&
                                                   m.state == REC_ST_ERROR);

    idle();
    in(REC_IN_RECORD);
    check("a recording whose helper could not start ends in ERROR, microphone off",
          in(REC_IN_H_FAILED) == REC_ACT_REFRESH && m.state == REC_ST_ERROR && !rec_machine_mic_on(&m));
}

static void test_close(void)
{
    static const enum rec_state busy[] = { REC_ST_CHECKING, REC_ST_STARTING, REC_ST_RECORDING,
                                           REC_ST_PAUSED, REC_ST_PLAYING, REC_ST_PLAY_PAUSED,
                                           REC_ST_STOPPING };
    size_t i;
    int ok = 1;

    for (i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        rec_machine_init(&m);
        m.state = busy[i];
        m.op = REC_OP_RECORD;
        ok &= in(REC_IN_CLOSE) == REC_ACT_ABANDON && m.state == REC_ST_IDLE && m.op == REC_OP_NONE;
    }
    check("closing in any busy state abandons the helper and leaves nothing running", ok);
    idle();
    check("closing while idle has nothing to stop", in(REC_IN_CLOSE) == REC_ACT_NONE);
    recording();
    in(REC_IN_CLOSE);
    check("after the close, a late exit from the helper does nothing", in(REC_IN_H_EXITED) == REC_ACT_NONE);
    check("state names are words", strcmp(rec_state_name(REC_ST_PLAY_PAUSED), "play-paused") == 0 &&
                                        strcmp(rec_state_name((enum rec_state)99), "?") == 0);
}

/* Every input in every state gives an action from the allowed set, and the
 * state is always one of the nine. */
static void test_total(void)
{
    enum rec_state s;
    int i;
    int ok = 1;

    for (s = REC_ST_CHECKING; s <= REC_ST_ERROR; s++) {
        for (i = REC_IN_RECORD; i <= REC_IN_H_CRASHED; i++) {
            enum rec_op op;

            for (op = REC_OP_NONE; op <= REC_OP_PLAY; op++) {
                enum rec_action a;

                rec_machine_init(&m);
                m.state = s;
                m.op = op;
                a = in((enum rec_input)i);
                ok &= a >= REC_ACT_NONE && a <= REC_ACT_REFRESH && m.state >= REC_ST_CHECKING &&
                      m.state <= REC_ST_ERROR;
                /* Only a stopping or exiting machine starts something. */
                if ((a == REC_ACT_START_RECORD || a == REC_ACT_START_PLAY) && s != REC_ST_IDLE &&
                    s != REC_ST_ERROR) {
                    ok &= i >= REC_IN_H_EXITED;
                }
            }
        }
    }
    check("every state answers every input with a known action and a known state", ok);
}

int main(void)
{
    test_open();
    test_record();
    test_rapid();
    test_pause();
    test_play();
    test_failures();
    test_close();
    test_total();
    printf("rec_state_test: %d checks, %d failure(s)\n", checks, failed);
    return failed != 0;
}
