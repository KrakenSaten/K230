/*
 * Wave's controller against real helper processes (tests/fake_pos_wave.sh in
 * its "auto" scenario): the audio lifecycle end to end, without LVGL and
 * without a sound card.
 *
 *   - at most one helper at any time, and none left when the app closes;
 *   - listen, then a send: the listen is stopped, the send runs, the listen
 *     resumes - in that order, one process each;
 *   - a preset's copies are separate helpers, and a stop ends the rest;
 *   - capture, then decode of the file, then the file is gone; audio never
 *     lands in the state directory;
 *   - repeated TX/RX cycles leave no process and no descriptor behind;
 *   - errors stop everything and nothing is retried in a loop;
 *   - muted volume sends nothing;
 *   - the preset and the history survive a restart; the history file stays
 *     bounded; a clear removes it;
 *   - closing mid-listen ends the helper within the destroy grace.
 *
 * Usage: wave_ctl_test <fake helper>
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "wave_ctl.h"
#include "wave_store.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static int64_t mono(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static struct wave_ctl ctl;
static char state[64];
static char run[64];
static char logpath[PATH_MAX];
static int volume = 100;

static int get_volume(void)
{
    return volume;
}

/* Poll like the app's 50 ms timer until cond holds or bound_ms passes. */
#define POLL_UNTIL(cond, bound_ms)                                                            \
    ({                                                                                        \
        int64_t end_ = mono() + (bound_ms);                                                   \
        int ok_ = 0;                                                                          \
        while (mono() < end_) {                                                               \
            wave_ctl_poll(&ctl, mono(), 1758900000);                                          \
            if (cond) {                                                                       \
                ok_ = 1;                                                                      \
                break;                                                                        \
            }                                                                                 \
            nap(10);                                                                          \
        }                                                                                     \
        ok_;                                                                                  \
    })

/* Running and past its first word to us: a helper stopped before that may
 * not have written its log line yet. */
static int announced(enum wave_op op);

static int idle_now(void)
{
    return !wave_session_active(&ctl.session) && ctl.view.phase == WAVE_PHASE_IDLE &&
           wave_view_next(&ctl.view) == WAVE_DO_NOTHING;
}

/* The helper runs, as the model says: op, and a live process. */
static int running_op(enum wave_op op)
{
    return ctl.view.op == op && ctl.view.phase == WAVE_PHASE_RUNNING && ctl.session.pid > 0 &&
           kill(ctl.session.pid, 0) == 0;
}

static int announced(enum wave_op op)
{
    if (!running_op(op)) {
        return 0;
    }
    switch (op) {
    case WAVE_OP_SEND: return ctl.view.expected_ms > 0;
    case WAVE_OP_LISTEN:
    case WAVE_OP_CAPTURE: return ctl.view.level >= 0;
    default: return 1;
    }
}

static char *read_log(void)
{
    static char buf[8192];
    FILE *f = fopen(logpath, "r");
    size_t n = 0;

    buf[0] = '\0';
    if (f) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static void clear_log(void)
{
    unlink(logpath);
}

/* The subcommands the helper was started with, in order: "listen send listen". */
static void commands(char *out, size_t n)
{
    char *log = read_log();
    char *save = NULL;
    char *line;
    size_t off = 0;

    out[0] = '\0';
    /* A helper writes its line when it starts, which may be a moment after
     * the fork that made it: give the last one time to arrive. */
    nap(150);
    log = read_log();
    for (line = strtok_r(log, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char word[16];

        if (strncmp(line, "text ", 5) == 0 || sscanf(line, "%15s", word) != 1) {
            continue;
        }
        off += (size_t)snprintf(out + off, n - off, "%s%s", off ? " " : "", word);
        if (off >= n) {
            break;
        }
    }
}

/* The status line as the screen would show it now. */
static const char *status(void)
{
    wave_view_refresh(&ctl.view, "", mono());
    return ctl.view.status;
}

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

/* No child of ours is left, not even a zombie. */
static int no_children(void)
{
    return waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD;
}

static int exists(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0;
}

static void capture_file(char *out, size_t n)
{
    snprintf(out, n, "%s/wave/" WAVE_CAPTURE_FILE, run);
}

static void reopen(void)
{
    wave_ctl_close(&ctl, 1758900000);
    wave_ctl_open(&ctl, get_volume);
}

int main(int argc, char **argv)
{
    char helper[PATH_MAX];
    char cmds[512];
    char cap[PATH_MAX];
    int fds_before;

    if (argc < 2 || !realpath(argv[1], helper)) {
        fprintf(stderr, "usage: wave_ctl_test <fake helper>\n");
        return 2;
    }
    snprintf(state, sizeof(state), "/tmp/wave-ctl-state-XXXXXX");
    snprintf(run, sizeof(run), "/tmp/wave-ctl-run-XXXXXX");
    if (!mkdtemp(state) || !mkdtemp(run)) {
        return 2;
    }
    snprintf(logpath, sizeof(logpath), "%s/helper.log", run);
    setenv("POCKETOS_WAVE_HELPER", helper, 1);
    setenv("POCKETOS_STATE_DIR", state, 1);
    setenv("POCKETOS_RUNTIME_DIR", run, 1);
    setenv("WAVE_FAKE", "auto", 1);
    setenv("WAVE_FAKE_LOG", logpath, 1);
    signal(SIGPIPE, SIG_IGN);
    capture_file(cap, sizeof(cap));

    wave_ctl_open(&ctl, get_volume);
    fds_before = count_fds();
    check("opening starts nothing", idle_now() && ctl.starts == 0 && !exists(logpath));
    check("opening stores nothing", ({
              char p[PATH_MAX];
              snprintf(p, sizeof(p), "%s/wave", state);
              !exists(p);
          }));

    /* ---- a plain send ---------------------------------------------------- */
    check("send is taken", wave_ctl_send(&ctl, "HELLO", mono(), 1758900000) == 0);
    check("and started at once", running_op(WAVE_OP_SEND));
    check("empty text is refused", wave_ctl_send(&ctl, "", mono(), 0) == -1);
    check("a second send while one runs is refused", wave_ctl_send(&ctl, "again", mono(), 0) == -1);
    check("the send finishes", POLL_UNTIL(idle_now(), 3000));
    check("with the text on the helper's stdin, fast profile",
          strstr(read_log(), "send --events --protocol audible_fast --volume 10") != NULL &&
              strstr(read_log(), "text 48454c4c4f") != NULL);
    check("it is in the history as sent", ctl.view.last == WAVE_LAST_SENT &&
                                              wave_history_at(&ctl.view.history, 0)->dir == WAVE_DIR_TX);
    check("and on disk", ({
              char p[PATH_MAX];
              snprintf(p, sizeof(p), "%s/wave/" WAVE_HISTORY_FILE, state);
              exists(p);
          }));

    /* ---- listen, send while listening, resume ------------------------------ */
    clear_log();
    setenv("WAVE_FAKE_RX", "444f4f5253", 1);
    wave_ctl_listen(&ctl, mono(), 1758900000);
    check("LISTEN starts a listen helper", running_op(WAVE_OP_LISTEN) && ctl.view.listen_on);
    check("the received message arrives", POLL_UNTIL(wave_history_count(&ctl.view.history) == 2, 3000) &&
                                              strcmp(wave_history_at(&ctl.view.history, 0)->data, "DOORS") == 0);
    {
        pid_t listener = ctl.session.pid;

        check("a send while listening is taken", wave_ctl_send(&ctl, "REPLY", mono(), 0) == 0);
        check("and first stops the listen", ctl.view.phase == WAVE_PHASE_STOPPING &&
                                                ctl.view.op == WAVE_OP_LISTEN);
        check("the send runs once the listener is gone", POLL_UNTIL(running_op(WAVE_OP_SEND), 3000));
        check("the listener really is gone", kill(listener, 0) != 0);
    }
    check("then the listen resumes by itself", POLL_UNTIL(announced(WAVE_OP_LISTEN), 3000));
    commands(cmds, sizeof(cmds));
    check("one listen, one send, one listen - in that order", strcmp(cmds, "listen send listen") == 0);
    check("what the resumed listen hears is filed after the reply",
          POLL_UNTIL(strcmp(wave_history_at(&ctl.view.history, 0)->data, "DOORS") == 0, 2000) &&
              wave_history_at(&ctl.view.history, 0)->dir == WAVE_DIR_RX &&
              strcmp(wave_history_at(&ctl.view.history, 1)->data, "REPLY") == 0 &&
              wave_history_at(&ctl.view.history, 1)->dir == WAVE_DIR_TX);

    /* ---- repeated cycles --------------------------------------------------- */
    {
        int i;
        int ok = 1;

        clear_log();
        for (i = 0; i < 5; i++) {
            ok &= wave_ctl_send(&ctl, "cycle", mono(), 0) == 0;
            ok &= POLL_UNTIL(announced(WAVE_OP_SEND), 3000);
            ok &= POLL_UNTIL(announced(WAVE_OP_LISTEN), 3000);
        }
        commands(cmds, sizeof(cmds));
        check("five send/listen cycles, each a clean hand-over", ok &&
              strcmp(cmds, "send listen send listen send listen send listen send listen") == 0);
    }
    wave_ctl_listen(&ctl, mono(), 0);
    check("LISTEN again turns it off", POLL_UNTIL(idle_now(), 3000) && !ctl.view.listen_on);
    check("no helper, no zombie left", no_children());
    check("no descriptor left", count_fds() == fds_before);
    unsetenv("WAVE_FAKE_RX");

    /* ---- copies ----------------------------------------------------------- */
    clear_log();
    check("ROBUST is picked", wave_ctl_set_preset(&ctl, WAVE_PRESET_ROBUST) == 1);
    wave_ctl_send(&ctl, "TWO", mono(), 0);
    check("two copies play", POLL_UNTIL(idle_now(), 5000));
    commands(cmds, sizeof(cmds));
    check("as two helpers on the slowest speed", strcmp(cmds, "send send") == 0 &&
                                                   strstr(read_log(), "audible_normal") != NULL);
    check("one history entry, two copies", wave_history_at(&ctl.view.history, 0)->count == 2 &&
                                               strcmp(wave_history_at(&ctl.view.history, 0)->data, "TWO") == 0);
    clear_log();
    setenv("WAVE_FAKE_SEND_MS", "800", 1);
    wave_ctl_send(&ctl, "STOPME", mono(), 0);
    POLL_UNTIL(announced(WAVE_OP_SEND), 1000);
    wave_ctl_stop(&ctl, mono(), 0);
    check("STOP ends the send", POLL_UNTIL(idle_now(), 3000));
    commands(cmds, sizeof(cmds));
    check("and the second copy never starts", strcmp(cmds, "send") == 0 &&
                                                  wave_history_at(&ctl.view.history, 0)->result ==
                                                      WAVE_RESULT_STOPPED);
    unsetenv("WAVE_FAKE_SEND_MS");
    wave_ctl_set_preset(&ctl, WAVE_PRESET_STANDARD);

    /* ---- capture then decode ------------------------------------------------ */
    clear_log();
    setenv("WAVE_FAKE_CAPTURE", "4341505455524544", 1); /* CAPTURED */
    wave_ctl_capture(&ctl, mono(), 0);
    check("CAPTURE records, microphone indicator on", running_op(WAVE_OP_CAPTURE) && (status(), ctl.view.mic_on));
    nap(150);
    check("to the runtime directory, for the preset's length",
          strstr(read_log(), "record --events --seconds 10 ") != NULL && strstr(read_log(), run) != NULL);
    check("then decodes it", POLL_UNTIL(idle_now(), 5000));
    commands(cmds, sizeof(cmds));
    check("record, then decode", strcmp(cmds, "record decode") == 0);
    check("the message is in the history, marked captured",
          strcmp(wave_history_at(&ctl.view.history, 0)->data, "CAPTURED") == 0 &&
              wave_history_at(&ctl.view.history, 0)->captured);
    check("and the recording is gone", !exists(cap));

    setenv("WAVE_FAKE_CAPTURE", "noise", 1);
    wave_ctl_capture(&ctl, mono(), 0);
    check("a noisy capture decodes to nothing", POLL_UNTIL(idle_now(), 5000) &&
                                                    ctl.view.last == WAVE_LAST_NOTHING_DECODED &&
                                                    wave_history_at(&ctl.view.history, 0)->result ==
                                                        WAVE_RESULT_UNDECODED);
    check("and that recording is gone too", !exists(cap));

    setenv("WAVE_FAKE_CAPTURE", "4541524c59", 1); /* EARLY */
    wave_ctl_capture(&ctl, mono(), 0);
    POLL_UNTIL(announced(WAVE_OP_CAPTURE), 2000);
    wave_ctl_capture(&ctl, mono(), 0); /* DECODE NOW */
    check("finishing a capture early still decodes it",
          POLL_UNTIL(idle_now(), 5000) && strcmp(wave_history_at(&ctl.view.history, 0)->data, "EARLY") == 0);

    wave_ctl_capture(&ctl, mono(), 0);
    POLL_UNTIL(announced(WAVE_OP_CAPTURE), 2000);
    wave_ctl_stop(&ctl, mono(), 0);
    check("STOP during a capture throws it away", POLL_UNTIL(idle_now(), 3000) && !exists(cap) &&
                                                      strcmp(wave_history_at(&ctl.view.history, 0)->data,
                                                             "EARLY") == 0);
    unsetenv("WAVE_FAKE_CAPTURE");
    check("no audio ever reached the state directory", ({
              char p[PATH_MAX];
              snprintf(p, sizeof(p), "%s/wave/" WAVE_CAPTURE_FILE, state);
              !exists(p);
          }));

    /* ---- failures ------------------------------------------------------------ */
    clear_log();
    setenv("WAVE_FAKE_LISTEN_FAIL", "audio_nodev", 1);
    wave_ctl_listen(&ctl, mono(), 0);
    check("a listen that fails stops", POLL_UNTIL(idle_now(), 3000) && !ctl.view.listen_on &&
                                           strcmp(status(), "No audio device found") == 0);
    nap(200);
    wave_ctl_poll(&ctl, mono(), 0);
    commands(cmds, sizeof(cmds));
    check("and is not retried", strcmp(cmds, "listen") == 0);
    unsetenv("WAVE_FAKE_LISTEN_FAIL");

    clear_log();
    setenv("WAVE_FAKE_SEND_FAIL", "audio_busy", 1);
    wave_ctl_send(&ctl, "busy", mono(), 0);
    check("a send that fails says so", POLL_UNTIL(idle_now(), 3000) &&
                                           strcmp(status(), "Audio is in use by another program") == 0 &&
                                           wave_history_at(&ctl.view.history, 0)->result == WAVE_RESULT_FAILED);
    unsetenv("WAVE_FAKE_SEND_FAIL");

    clear_log();
    volume = 0;
    wave_ctl_send(&ctl, "muted", mono(), 0);
    wave_ctl_poll(&ctl, mono(), 0);
    check("muted: nothing is started", !exists(logpath) && idle_now());
    check("muted: and it says why", strstr(status(), "muted") != NULL);
    volume = 40;
    wave_ctl_send(&ctl, "quiet", mono(), 0);
    POLL_UNTIL(idle_now(), 3000);
    check("the system volume is passed on", strstr(read_log(), "--volume-percent 40") != NULL);
    volume = 100;

    setenv("POCKETOS_WAVE_HELPER", "/nonexistent/pos-wave", 1);
    wave_ctl_listen(&ctl, mono(), 0);
    check("a missing helper says so and stops", POLL_UNTIL(idle_now(), 3000) &&
                                                    strcmp(status(), "Wave helper is not installed") == 0 &&
                                                    !ctl.view.listen_on);
    setenv("POCKETOS_WAVE_HELPER", helper, 1);

    /* ---- restart ------------------------------------------------------------ */
    wave_ctl_set_preset(&ctl, WAVE_PRESET_QUICK);
    {
        int before = wave_history_count(&ctl.view.history);
        uint32_t newest = wave_history_at(&ctl.view.history, 0)->seq;

        reopen();
        check("after a restart the preset is the one picked", ctl.view.preset == WAVE_PRESET_QUICK);
        check("and the history is all there", wave_history_count(&ctl.view.history) == before &&
                                                  wave_history_at(&ctl.view.history, 0)->seq == newest);
        check("the listen toggle is not restored (no microphone at open)",
              !ctl.view.listen_on && idle_now());
    }
    {
        int i;
        char p[PATH_MAX];
        struct stat st;

        for (i = 0; i < WAVE_HISTORY_MAX + 5; i++) {
            wave_history_add_tx(&ctl.view.history, "quick", "fill", 4, WAVE_RESULT_OK, 1, 0);
        }
        wave_ctl_poll(&ctl, mono(), 0);
        snprintf(p, sizeof(p), "%s/wave/" WAVE_HISTORY_FILE, state);
        check("the history file stays bounded", stat(p, &st) == 0 && st.st_size < WAVE_HISTORY_TEXT_MAX);
        reopen();
        check("and holds WAVE_HISTORY_MAX entries", wave_history_count(&ctl.view.history) == WAVE_HISTORY_MAX);
        check("CLEAR once does not clear", wave_ctl_clear(&ctl, mono()) == 0);
        check("CLEAR twice does", wave_ctl_clear(&ctl, mono()) == 1 && !exists(p));
        reopen();
        check("and it stays cleared", wave_history_count(&ctl.view.history) == 0);
    }
    {
        char p[PATH_MAX];
        FILE *f;

        snprintf(p, sizeof(p), "%s/wave", run);
        mkdir(p, 0700);
        f = fopen(cap, "w");
        if (f) {
            fputs("stale", f);
            fclose(f);
        }
        reopen();
        check("a stale capture from a killed run is removed at open", !exists(cap));
    }

    /* ---- closing mid-send ------------------------------------------------------ */
    setenv("WAVE_FAKE_SEND_MS", "2000", 1);
    wave_ctl_send(&ctl, "LEAVING", mono(), 0);
    POLL_UNTIL(announced(WAVE_OP_SEND), 2000);
    {
        pid_t pid = ctl.session.pid;

        reopen();
        check("closing mid-send ends the helper", kill(pid, 0) != 0 && no_children());
        check("and keeps the message in the history, as stopped",
              wave_history_count(&ctl.view.history) >= 1 &&
                  strcmp(wave_history_at(&ctl.view.history, 0)->data, "LEAVING") == 0 &&
                  wave_history_at(&ctl.view.history, 0)->result == WAVE_RESULT_STOPPED);
    }
    unsetenv("WAVE_FAKE_SEND_MS");

    /* ---- closing mid-listen ---------------------------------------------------- */
    wave_ctl_listen(&ctl, mono(), 0);
    POLL_UNTIL(announced(WAVE_OP_LISTEN), 2000);
    {
        pid_t pid = ctl.session.pid;
        int64_t t0 = mono();

        wave_ctl_close(&ctl, 0);
        check("closing ends the listen", kill(pid, 0) != 0);
        check("within the destroy grace", mono() - t0 < WAVE_DESTROY_GRACE_MS + 200);
        check("nothing left running", no_children());
    }
    check("no descriptor left after everything", count_fds() <= fds_before);

    {
        char p[PATH_MAX];

        snprintf(p, sizeof(p), "%s/wave/" WAVE_HISTORY_FILE, state);
        unlink(p);
        snprintf(p, sizeof(p), "%s/wave/" WAVE_PREFS_FILE, state);
        unlink(p);
        snprintf(p, sizeof(p), "%s/wave", state);
        rmdir(p);
        rmdir(state);
        unlink(logpath);
        snprintf(p, sizeof(p), "%s/wave", run);
        rmdir(p);
        rmdir(run);
    }
    printf("wave_ctl_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
