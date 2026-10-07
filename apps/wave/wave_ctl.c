/*
 * Wave's controller. See wave_ctl.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_ctl.h"

#include "wave_store.h"

#include <stdio.h>
#include <string.h>

static void save_history(struct wave_ctl *c)
{
    if (c->view.history.changes == c->saved_changes) {
        return;
    }
    c->store_failed = wave_store_save_history(&c->view.history) != 0;
    /* Saved only once it reached the disk: a failed write stays due and the
     * next save tries again (from the poll, no sooner than
     * WAVE_SAVE_RETRY_MS, so a failing disk is not hit on every tick). */
    if (!c->store_failed) {
        c->saved_changes = c->view.history.changes;
    }
}

void wave_ctl_open(struct wave_ctl *c, int (*volume)(void))
{
    struct wave_prefs prefs;
    int preset = WAVE_PRESET_DEFAULT;

    memset(c, 0, sizeof(*c));
    c->volume = volume;
    if (wave_store_load_prefs(&prefs) == 0 && wave_preset_find(prefs.preset) >= 0) {
        preset = wave_preset_find(prefs.preset);
    }
    wave_view_init(&c->view, preset);
    wave_session_init(&c->session);
    if (wave_store_load_history(&c->view.history, &c->history_skipped) < 0) {
        /* Unusable: start empty, and leave the file alone until something
         * new is worth writing over it. */
        wave_history_init(&c->view.history);
    }
    c->saved_changes = c->view.history.changes;
    /* A capture from a run that was killed between recording and decoding. */
    wave_store_capture_remove();
}

void wave_ctl_close(struct wave_ctl *c, int64_t wall_s)
{
    wave_session_abandon(&c->session, WAVE_DESTROY_GRACE_MS);
    /* A send cut short by leaving is kept, as stopped, so its text is not
     * lost with the screen. */
    if (c->view.send_pending) {
        const struct wave_preset *p = wave_preset_get(c->view.tx_preset);

        wave_history_add_tx(&c->view.history, p ? p->id : "-", c->view.tx_text, c->view.tx_len,
                            WAVE_RESULT_STOPPED, c->view.tx_done, wall_s);
        c->view.send_pending = 0;
    }
    wave_store_capture_remove();
    save_history(c);
}

/* Start what the model says is due, if anything; 1 when a helper started or
 * a start failed (both change the screen). */
static int start_due(struct wave_ctl *c, int64_t now_ms, int64_t wall_s)
{
    struct wave_view *v = &c->view;
    enum wave_action what = wave_view_next(v);
    const struct wave_preset *p = wave_view_preset(v);
    const char *helper = wave_session_helper_path();
    char path[WAVE_HELPER_PATH_MAX];
    char err[160] = "";
    int rc = -1;

    if (what == WAVE_DO_NOTHING || wave_session_active(&c->session) || !p) {
        return 0;
    }
    switch (what) {
    case WAVE_DO_SEND: {
        const struct wave_preset *tp = wave_preset_get(v->tx_preset);
        /* The system volume (Controls): muted sends nothing at all rather
         * than a silent message the other side would wait for. */
        int level = c->volume ? c->volume() : 100;

        if (level <= 0) {
            snprintf(err, sizeof(err), "Sound is muted. Turn it on in Controls to send.");
        } else {
            rc = wave_session_start_send_at(&c->session, helper,
                                            wave_view_profile_name(tp ? tp->profile : p->profile),
                                            WAVE_DEFAULT_VOLUME, level > 100 ? 100 : level,
                                            v->tx_text, v->tx_len, err, sizeof(err));
        }
        break;
    }
    case WAVE_DO_LISTEN:
        rc = wave_session_start_listen(&c->session, helper, p->listen_seconds, err, sizeof(err));
        break;
    case WAVE_DO_CAPTURE:
        wave_store_capture_remove();
        if (wave_store_capture_path(path, sizeof(path)) != 0) {
            snprintf(err, sizeof(err), "No room for a recording");
        } else {
            rc = wave_session_start_record(&c->session, helper, p->capture_seconds, path, err,
                                           sizeof(err));
        }
        break;
    case WAVE_DO_DECODE:
        if (wave_store_capture_path(path, sizeof(path)) != 0) {
            snprintf(err, sizeof(err), "The recording is gone");
        } else {
            rc = wave_session_start_decode(&c->session, helper, path, err, sizeof(err));
        }
        break;
    default:
        return 0;
    }
    if (rc == 0) {
        wave_view_started(v, what, now_ms);
        c->starts++;
    } else {
        wave_view_start_failed(v, what, err, wall_s);
        wave_store_capture_remove();
    }
    return 1;
}

static void obey(struct wave_ctl *c, enum wave_action what, int64_t now_ms)
{
    if (what == WAVE_DO_STOP && wave_session_active(&c->session)) {
        wave_session_stop(&c->session, now_ms);
        wave_view_stopping(&c->view, now_ms);
    }
}

/* After every request: save what it changed, start what is now due. */
static void settle(struct wave_ctl *c, int64_t now_ms, int64_t wall_s)
{
    start_due(c, now_ms, wall_s);
    save_history(c);
}

int wave_ctl_send(struct wave_ctl *c, const char *text, int64_t now_ms, int64_t wall_s)
{
    int refused = 0;
    enum wave_action what = wave_view_request_send(&c->view, text, &refused);

    if (refused) {
        return -1;
    }
    obey(c, what, now_ms);
    settle(c, now_ms, wall_s);
    return 0;
}

void wave_ctl_listen(struct wave_ctl *c, int64_t now_ms, int64_t wall_s)
{
    obey(c, wave_view_request_listen(&c->view), now_ms);
    settle(c, now_ms, wall_s);
}

void wave_ctl_capture(struct wave_ctl *c, int64_t now_ms, int64_t wall_s)
{
    obey(c, wave_view_request_capture(&c->view), now_ms);
    settle(c, now_ms, wall_s);
}

void wave_ctl_stop(struct wave_ctl *c, int64_t now_ms, int64_t wall_s)
{
    obey(c, wave_view_request_stop(&c->view, wall_s), now_ms);
    settle(c, now_ms, wall_s);
}

int wave_ctl_set_preset(struct wave_ctl *c, int preset)
{
    struct wave_prefs prefs;
    const struct wave_preset *p;

    if (preset == c->view.preset || !wave_view_set_preset(&c->view, preset)) {
        return 0;
    }
    p = wave_view_preset(&c->view);
    memset(&prefs, 0, sizeof(prefs));
    snprintf(prefs.preset, sizeof(prefs.preset), "%s", p->id);
    c->store_failed = wave_store_save_prefs(&prefs) != 0;
    return 1;
}

int wave_ctl_clear(struct wave_ctl *c, int64_t now_ms)
{
    int cleared = wave_view_request_clear(&c->view, now_ms);

    if (cleared) {
        save_history(c);
    }
    return cleared;
}

int wave_ctl_poll(struct wave_ctl *c, int64_t now_ms, int64_t wall_s)
{
    struct wave_event ev;
    int n = 0;
    int was_decoding = c->view.op == WAVE_OP_DECODE || c->view.op == WAVE_OP_CAPTURE;

    while (wave_session_poll(&c->session, &ev, now_ms)) {
        wave_view_apply(&c->view, &ev, now_ms, wall_s);
        n++;
    }
    /* A recording is kept only between its capture and its decode. */
    if (was_decoding && c->view.op == WAVE_OP_NONE && !c->view.decode_pending) {
        wave_store_capture_remove();
    }
    n += start_due(c, now_ms, wall_s);
    if (now_ms >= c->save_retry_ms) {
        save_history(c);
        if (c->view.history.changes != c->saved_changes) {
            c->save_retry_ms = now_ms + WAVE_SAVE_RETRY_MS;
        }
    }
    return n;
}
