/*
 * The Recorder's controller. See rec_ctl.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rec_ctl.h"

#include "rec_protocol.h"
#include "rec_view.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void input(struct rec_ctl *c, enum rec_input in, int64_t now_ms);

static void say(struct rec_ctl *c, enum rec_tone tone, const char *fmt, const char *arg)
{
    snprintf(c->message, sizeof(c->message), fmt, arg ? arg : "");
    c->tone = tone;
}

static void refresh(struct rec_ctl *c, int64_t now_ms)
{
    rec_store_list_free(&c->list);
    if (rec_store_list(c->dir, &c->list) != 0) {
        memset(&c->list, 0, sizeof(c->list));
    }
    if (!rec_ctl_selected(c)) {
        c->selected[0] = '\0';
    }
    c->free_bytes = rec_store_free(c->dir);
    c->free_at_ms = now_ms;
    c->changes++;
}

const struct rec_entry *rec_ctl_selected(const struct rec_ctl *c)
{
    int i = rec_ctl_selected_index(c);

    return i >= 0 ? &c->list.e[i] : NULL;
}

int rec_ctl_selected_index(const struct rec_ctl *c)
{
    int i;

    if (!c->selected[0]) {
        return -1;
    }
    for (i = 0; i < c->list.n; i++) {
        if (strcmp(c->list.e[i].name, c->selected) == 0) {
            return i;
        }
    }
    return -1;
}

static void clear_live(struct rec_ctl *c)
{
    c->elapsed_ms = 0;
    c->total_ms = 0;
    c->bytes = 0;
    c->level_peak = 0;
    c->level_rms = 0;
    c->level_at_ms = 0;
    c->peak_hold = 0;
    c->peak_hold_at_ms = 0;
}

/* ---- the actions -------------------------------------------------------- */

static int start_record(struct rec_ctl *c, int64_t now_ms, int64_t wall_now, bool wall_valid)
{
    char name[REC_NAME_MAX];
    char path[REC_STORE_PATH_MAX + REC_NAME_MAX + 2];
    char err[160];
    int rate = rec_preset_rate(c->preset);
    int64_t need = (int64_t)REC_RESERVE_BYTES + (int64_t)rate * 2 * REC_START_MARGIN_S;

    (void)now_ms;
    if (c->dir_error) {
        say(c, REC_TONE_ERROR, "Cannot use the Recordings folder: %s", strerror(c->dir_error));
        return -1;
    }
    c->free_bytes = rec_store_free(c->dir);
    if (c->free_bytes >= 0 && c->free_bytes < need) {
        say(c, REC_TONE_ERROR, "Storage is full: free some space to record%s", NULL);
        return -1;
    }
    if (rec_store_next_name(c->dir, wall_now, wall_valid, name, sizeof(name)) != 0) {
        say(c, REC_TONE_ERROR, "No free recording name is left in the folder%s", NULL);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", c->dir, name);
    if (rec_session_start_record(&c->session, c->helper, rate, path, err, sizeof(err)) != 0) {
        say(c, REC_TONE_ERROR, "Cannot start recording: %s", err);
        return -1;
    }
    clear_live(c);
    snprintf(c->current, sizeof(c->current), "%s", name);
    c->rate = rate;
    say(c, REC_TONE_MUTED, "Starting the microphone%s", NULL);
    return 0;
}

static int start_play(struct rec_ctl *c)
{
    const struct rec_entry *e = rec_ctl_selected(c);
    int vol = c->volume ? c->volume() : 100;
    char err[160];

    if (!e) {
        say(c, REC_TONE_WARN, "Select a recording to play%s", NULL);
        return -1;
    }
    if (e->status != REC_ENTRY_OK) {
        say(c, REC_TONE_WARN, "%s", e->status == REC_ENTRY_PART ? "An unfinished recording cannot be played"
                                                                 : "This file cannot be played here");
        return -1;
    }
    if (vol <= 0) {
        say(c, REC_TONE_WARN, "Sound is muted: turn it on in Controls to play%s", NULL);
        return -1;
    }
    snprintf(c->play_path, sizeof(c->play_path), "%s/%s", c->dir, e->name);
    if (rec_session_start_play(&c->session, c->helper, c->play_path, 0, vol > 100 ? 100 : vol, err,
                               sizeof(err)) != 0) {
        say(c, REC_TONE_ERROR, "Cannot play: %s", err);
        return -1;
    }
    clear_live(c);
    snprintf(c->current, sizeof(c->current), "%s", e->name);
    c->total_ms = (int64_t)e->ms;
    c->rate = (int)e->rate;
    c->message[0] = '\0';
    return 0;
}

static void reject(struct rec_ctl *c)
{
    switch (c->m.state) {
    case REC_ST_CHECKING: say(c, REC_TONE_MUTED, "Checking recordings, one moment%s", NULL); break;
    case REC_ST_RECORDING:
    case REC_ST_PAUSED: say(c, REC_TONE_WARN, "Stop the recording first%s", NULL); break;
    default: say(c, REC_TONE_WARN, "Stop first%s", NULL); break;
    }
}

static void perform(struct rec_ctl *c, enum rec_action a, int64_t now_ms)
{
    char err[160];

    switch (a) {
    case REC_ACT_NONE: break;
    case REC_ACT_REJECT: reject(c); break;
    case REC_ACT_START_CHECK:
        if (c->dir_error) {
            /* Nothing to repair in a folder that is not there; the audio
             * route is also restored by every open. */
            input(c, REC_IN_H_EXITED, now_ms);
        } else if (rec_session_start_recover(&c->session, c->helper, c->dir, err, sizeof(err)) != 0) {
            say(c, REC_TONE_ERROR, "Cannot start the recorder helper: %s", err);
            input(c, REC_IN_H_FAILED, now_ms);
        }
        break;
    case REC_ACT_START_RECORD:
        if (start_record(c, now_ms, c->wall_now, c->wall_valid) != 0) {
            input(c, REC_IN_H_FAILED, now_ms);
        }
        break;
    case REC_ACT_START_PLAY:
        if (start_play(c) != 0) {
            input(c, REC_IN_H_FAILED, now_ms);
        }
        break;
    case REC_ACT_SEND_PAUSE:
    case REC_ACT_SEND_RESUME:
        if (rec_session_command(&c->session, a == REC_ACT_SEND_PAUSE ? "pause" : "resume") != 0) {
            c->m.cmd_pending = false;
        }
        break;
    case REC_ACT_STOP:
        rec_session_stop(&c->session, now_ms);
        if (c->m.op == REC_OP_RECORD) {
            say(c, REC_TONE_MUTED, "Saving%s", NULL);
        }
        break;
    case REC_ACT_DELETE: {
        const struct rec_entry *e = rec_ctl_selected(c);
        char name[REC_FILE_NAME_MAX];
        int r;

        if (!e) {
            break;
        }
        snprintf(name, sizeof(name), "%s", e->name);
        r = rec_store_delete(c->dir, name);
        if (r == 0) {
            say(c, REC_TONE_OK, "Deleted %s", name);
            c->selected[0] = '\0';
        } else {
            say(c, REC_TONE_ERROR, "Could not delete: %s", strerror(-r));
        }
        refresh(c, now_ms);
        break;
    }
    case REC_ACT_ABANDON: rec_session_abandon(&c->session, REC_DESTROY_GRACE_MS); break;
    case REC_ACT_REFRESH: refresh(c, now_ms); break;
    }
}

static void input(struct rec_ctl *c, enum rec_input in, int64_t now_ms)
{
    perform(c, rec_machine_input(&c->m, in), now_ms);
}

/* ---- opening ------------------------------------------------------------ */

void rec_ctl_open(struct rec_ctl *c, int (*volume)(void))
{
    int e;

    memset(c, 0, sizeof(*c));
    rec_machine_init(&c->m);
    rec_session_init(&c->session);
    c->helper = rec_session_helper_path();
    c->volume = volume;
    c->free_bytes = -1;
    rec_store_load_preset(&c->preset);
    if (rec_store_dir(c->dir, sizeof(c->dir)) != 0) {
        c->dir_error = ENAMETOOLONG;
    } else if ((e = rec_store_ensure_dir(c->dir)) != 0) {
        c->dir_error = -e;
    }
    refresh(c, 0);
    if (c->dir_error) {
        say(c, REC_TONE_ERROR, "Cannot use the Recordings folder: %s", strerror(c->dir_error));
    }
    /* The first thing, every time: repair what an interrupted recording left
     * and restore an audio route a dead helper left switched. */
    perform(c, REC_ACT_START_CHECK, 0);
}

/* ---- the helper's events ------------------------------------------------ */

static void on_error(struct rec_ctl *c, const char *text)
{
    rec_view_error_text(text, c->message, sizeof(c->message));
    c->tone = REC_TONE_ERROR;
}

static void on_saved(struct rec_ctl *c, const struct rec_event *ev)
{
    char dur[16];
    char line[512];
    const char *lead = c->tone == REC_TONE_WARN && c->message[0] ? c->message : NULL;
    char kept[120] = "";

    if (lead) {
        snprintf(kept, sizeof(kept), "%.100s", lead);
    }
    rec_view_duration(dur, sizeof(dur), ev->a);
    if (ev->c > 0) {
        snprintf(line, sizeof(line), "%s%sSaved %s (%s), with %d short gap%s where the device fell behind",
                 kept, kept[0] ? ". " : "", ev->text, dur, ev->c, ev->c == 1 ? "" : "s");
    } else {
        snprintf(line, sizeof(line), "%s%sSaved %s (%s)", kept, kept[0] ? ". " : "", ev->text, dur);
    }
    snprintf(c->message, sizeof(c->message), "%s", line);
    c->tone = kept[0] || ev->c > 0 ? REC_TONE_WARN : REC_TONE_OK;
    c->elapsed_ms = ev->a;
    c->bytes = ev->b;
    snprintf(c->selected, sizeof(c->selected), "%s", ev->text);
}

static void on_exit(struct rec_ctl *c, int code, int64_t now_ms)
{
    enum rec_op op = c->m.op;
    bool had_error = c->m.failed;

    if (code == 0) {
        input(c, REC_IN_H_EXITED, now_ms);
    } else if (code >= 128) {
        if (!had_error) {
            say(c, REC_TONE_ERROR, "%s", op == REC_OP_RECORD
                                             ? "The recorder stopped unexpectedly; repairing the recording"
                                             : "The recorder stopped unexpectedly");
        }
        input(c, REC_IN_H_CRASHED, now_ms);
    } else {
        if (!had_error) {
            char t[64];

            snprintf(t, sizeof(t), "The recorder helper failed (exit %d)", code);
            say(c, REC_TONE_ERROR, "%s", t);
        }
        input(c, REC_IN_H_FAILED, now_ms);
    }
    if (op == REC_OP_CHECK && c->repaired > 0 && c->tone != REC_TONE_ERROR) {
        char t[80];

        snprintf(t, sizeof(t), "Recovered %u interrupted recording%s", c->repaired,
                 c->repaired == 1 ? "" : "s");
        say(c, REC_TONE_WARN, "%s", t);
    }
    if (!rec_machine_busy(&c->m)) {
        c->level_at_ms = 0;
    }
}

int rec_ctl_poll(struct rec_ctl *c, int64_t now_ms)
{
    struct rec_event ev;
    int n = 0;

    while (rec_session_poll(&c->session, &ev, now_ms)) {
        n++;
        switch (ev.kind) {
        case REC_EV_READY:
        case REC_EV_RECOVERED:
        case REC_EV_STOPPED: break;
        case REC_EV_RECORDING:
            snprintf(c->current, sizeof(c->current), "%s", ev.text);
            c->rate = (int)ev.a;
            c->message[0] = '\0';
            input(c, REC_IN_H_RECORDING, now_ms);
            break;
        case REC_EV_LEVEL:
            c->level_peak = (int)ev.a;
            c->level_rms = (int)ev.b;
            c->level_at_ms = now_ms;
            if (c->level_peak >= c->peak_hold || now_ms - c->peak_hold_at_ms > REC_PEAK_HOLD_MS) {
                c->peak_hold = c->level_peak;
                c->peak_hold_at_ms = now_ms;
            }
            break;
        case REC_EV_PROGRESS:
            c->elapsed_ms = ev.a;
            if (c->m.op == REC_OP_RECORD) {
                c->bytes = ev.b;
            }
            break;
        case REC_EV_PAUSED:
            c->level_at_ms = 0;
            input(c, REC_IN_H_PAUSED, now_ms);
            break;
        case REC_EV_RESUMED:
            if (c->tone == REC_TONE_ERROR) {
                c->message[0] = '\0';
            }
            input(c, REC_IN_H_RESUMED, now_ms);
            break;
        case REC_EV_LIMIT:
            say(c, REC_TONE_WARN, "%s",
                strcmp(ev.text, "space") == 0    ? "Storage is almost full: recording stopped"
                : strcmp(ev.text, "length") == 0 ? "Longest possible recording reached"
                                                 : "Time limit reached");
            break;
        case REC_EV_SAVED: on_saved(c, &ev); break;
        case REC_EV_EMPTY: say(c, REC_TONE_MUTED, "Nothing was recorded%s", NULL); break;
        case REC_EV_KEPT:
            say(c, REC_TONE_WARN, "Could not finish %s; it will be repaired when Recorder opens", ev.text);
            break;
        case REC_EV_PLAYING:
            c->total_ms = ev.a;
            c->rate = (int)ev.b;
            input(c, REC_IN_H_PLAYING, now_ms);
            break;
        case REC_EV_PLAYED:
            c->elapsed_ms = c->total_ms;
            break;
        case REC_EV_REPAIRED:
            c->repaired++;
            break;
        case REC_EV_DAMAGED:
            say(c, REC_TONE_WARN, "%s could not be repaired; it is kept as it is", ev.text);
            break;
        case REC_EV_ERROR:
            /* In the middle of a paused session a failed resume is not the
             * end: the helper stays paused and says why. */
            on_error(c, ev.text);
            if (c->m.state != REC_ST_PAUSED && c->m.state != REC_ST_PLAY_PAUSED) {
                input(c, REC_IN_H_ERROR, now_ms);
            } else {
                c->m.cmd_pending = false;
            }
            break;
        case REC_EV_EXITED: on_exit(c, ev.c, now_ms); break;
        }
    }
    if (c->m.state == REC_ST_RECORDING && now_ms - c->free_at_ms >= REC_SPACE_EVERY_MS) {
        c->free_bytes = rec_store_free(c->dir);
        c->free_at_ms = now_ms;
    }
    if (c->delete_armed_ms && now_ms - c->delete_armed_ms > REC_DELETE_ARM_MS) {
        c->delete_armed_ms = 0;
        n++;
    }
    return n;
}

/* ---- the owner's actions ------------------------------------------------ */

void rec_ctl_record(struct rec_ctl *c, int64_t now_ms, int64_t wall_now, bool wall_valid)
{
    c->delete_armed_ms = 0;
    c->wall_now = wall_now;
    c->wall_valid = wall_valid;
    if (c->m.state == REC_ST_RECORDING || c->m.state == REC_ST_PAUSED ||
        (c->m.state == REC_ST_STARTING && c->m.op == REC_OP_RECORD)) {
        input(c, REC_IN_STOP, now_ms);
        return;
    }
    input(c, REC_IN_RECORD, now_ms);
}

void rec_ctl_stop(struct rec_ctl *c, int64_t now_ms)
{
    c->delete_armed_ms = 0;
    input(c, REC_IN_STOP, now_ms);
}

void rec_ctl_pause(struct rec_ctl *c, int64_t now_ms)
{
    c->delete_armed_ms = 0;
    input(c, REC_IN_PAUSE, now_ms);
}

void rec_ctl_play(struct rec_ctl *c, int64_t now_ms)
{
    c->delete_armed_ms = 0;
    if ((c->m.state == REC_ST_PLAYING || c->m.state == REC_ST_PLAY_PAUSED ||
         (c->m.state == REC_ST_STARTING && c->m.op == REC_OP_PLAY)) &&
        strcmp(c->current, c->selected) == 0) {
        input(c, REC_IN_STOP, now_ms);
        return;
    }
    input(c, REC_IN_PLAY, now_ms);
}

void rec_ctl_select(struct rec_ctl *c, int index)
{
    c->delete_armed_ms = 0;
    if (index >= 0 && index < c->list.n) {
        snprintf(c->selected, sizeof(c->selected), "%s", c->list.e[index].name);
    } else {
        c->selected[0] = '\0';
    }
}

bool rec_ctl_delete_armed(const struct rec_ctl *c, int64_t now_ms)
{
    return c->delete_armed_ms && now_ms - c->delete_armed_ms <= REC_DELETE_ARM_MS;
}

void rec_ctl_delete(struct rec_ctl *c, int64_t now_ms)
{
    const struct rec_entry *e = rec_ctl_selected(c);

    if (!e) {
        say(c, REC_TONE_WARN, "Select a recording first%s", NULL);
        return;
    }
    if (rec_machine_busy(&c->m)) {
        perform(c, rec_machine_input(&c->m, REC_IN_DELETE), now_ms);
        return;
    }
    if (!rec_ctl_delete_armed(c, now_ms)) {
        c->delete_armed_ms = now_ms ? now_ms : 1;
        say(c, REC_TONE_WARN, "Tap DELETE again to delete %s", e->name);
        return;
    }
    c->delete_armed_ms = 0;
    input(c, REC_IN_DELETE, now_ms);
}

void rec_ctl_next_preset(struct rec_ctl *c)
{
    if (rec_machine_busy(&c->m)) {
        return;
    }
    c->preset = c->preset == REC_PRESET_VOICE ? REC_PRESET_STANDARD : REC_PRESET_VOICE;
    if (rec_store_save_preset(c->preset) != 0) {
        say(c, REC_TONE_WARN, "The preset could not be stored%s", NULL);
    }
}

void rec_ctl_close(struct rec_ctl *c)
{
    perform(c, rec_machine_input(&c->m, REC_IN_CLOSE), 0);
    /* Whatever the machine thought, no helper outlives the app. */
    if (rec_session_active(&c->session)) {
        rec_session_abandon(&c->session, REC_DESTROY_GRACE_MS);
    }
    rec_store_list_free(&c->list);
}

/* ---- the meter ---------------------------------------------------------- */

static bool fresh(const struct rec_ctl *c, int64_t now_ms)
{
    return c->level_at_ms && now_ms - c->level_at_ms <= REC_LEVEL_STALE_MS &&
           (c->m.state == REC_ST_RECORDING || c->m.state == REC_ST_PLAYING);
}

int rec_ctl_level_peak(const struct rec_ctl *c, int64_t now_ms)
{
    return fresh(c, now_ms) ? c->level_peak : 0;
}

int rec_ctl_level_rms(const struct rec_ctl *c, int64_t now_ms)
{
    return fresh(c, now_ms) ? c->level_rms : 0;
}

int rec_ctl_level_hold(const struct rec_ctl *c, int64_t now_ms)
{
    return fresh(c, now_ms) && now_ms - c->peak_hold_at_ms <= REC_PEAK_HOLD_MS ? c->peak_hold : 0;
}
