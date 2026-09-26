/*
 * Wave's controller: the loop between the model (wave_view.h), the helper
 * process (wave_session.h) and the files (wave_store.h), without LVGL.
 *
 * The screen calls one function per tap or key and wave_ctl_poll() from its
 * timer, and paints ctl.view afterwards. Everything in between is here:
 *
 *   - a request that needs the running helper stopped first stops it;
 *   - whenever no helper runs, whatever the model says is due is started:
 *     the next send copy, the capture's decode, the resumed listen;
 *   - a send reads the system volume first and does not start while it is
 *     muted;
 *   - the history is saved after every change, and the preset when it is
 *     picked; a capture file is removed once it has been decoded or dropped;
 *   - close() ends whatever runs within its bound and leaves nothing behind.
 *
 * AUDIO OWNERSHIP. At most one helper exists at any time, because the model
 * asks for the next start only once the previous helper has been reaped
 * (wave_session_poll() delivered its EXITED). Every start goes through
 * wave_session.c, so every helper dies with the shell (PR_SET_PDEATHSIG) and
 * a helper killed by a signal has its route and amplifier recovered. close()
 * is the one bounded wait on the UI thread (wave_session_abandon()).
 *
 * Nothing here blocks except close(): poll() is non-blocking reads, a
 * non-blocking reap and at most one fork per call.
 *
 * Tested on the host against tests/fake_pos_wave.sh (tests/wave_ctl_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETWAVE_CTL_H
#define POCKETWAVE_CTL_H

#include "wave_session.h"
#include "wave_view.h"

#include <stdint.h>

/* close(): time for a running helper to close the device cleanly before it
 * is killed. */
#define WAVE_DESTROY_GRACE_MS 300

struct wave_ctl {
    struct wave_view view;
    struct wave_session session;
    /* The system volume (percent, 0 while muted), read before each send. */
    int (*volume)(void);
    unsigned saved_changes;  /* history.changes when it was last saved */
    int store_failed;        /* the last save did not reach the disk */
    int history_skipped;     /* damaged lines dropped when the history was read */
    unsigned starts;         /* helpers started, for tests and logs */
};

/* Load the stored preset and history and remove any stale capture. volume
 * may be NULL (full volume). */
void wave_ctl_open(struct wave_ctl *c, int (*volume)(void));
/* End whatever runs (bounded), keep a send it cut short in the history (as
 * stopped, dated wall_s), remove the capture, save what changed. */
void wave_ctl_close(struct wave_ctl *c, int64_t wall_s);

/* The person's requests. now_ms is monotonic, wall_s wall-clock seconds
 * (0 while the board's clock is not set). send returns 0 when the message
 * was taken, -1 when it was refused (nothing changes). */
int wave_ctl_send(struct wave_ctl *c, const char *text, int64_t now_ms, int64_t wall_s);
void wave_ctl_listen(struct wave_ctl *c, int64_t now_ms, int64_t wall_s);
void wave_ctl_capture(struct wave_ctl *c, int64_t now_ms, int64_t wall_s);
void wave_ctl_stop(struct wave_ctl *c, int64_t now_ms, int64_t wall_s);
/* 1 when the preset changed (and was stored). */
int wave_ctl_set_preset(struct wave_ctl *c, int preset);
/* CLEAR, twice (wave_view_request_clear). 1 when the history was cleared. */
int wave_ctl_clear(struct wave_ctl *c, int64_t now_ms);

/* Deliver the helper's events and start whatever is due. Returns how many
 * events were applied plus how many helpers were started: 0 means nothing
 * happened. */
int wave_ctl_poll(struct wave_ctl *c, int64_t now_ms, int64_t wall_s);

#endif
