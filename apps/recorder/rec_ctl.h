/*
 * The Recorder's controller: the state machine (rec_state.h), the one
 * helper (rec_session.h) and the folder (rec_store.h), driven by the
 * owner's actions and a 50 ms poll from the screen. No LVGL: the screen
 * (rec_app.c) reads what is here through rec_view.h and paints it.
 *
 * Every call returns at once. The only waits are the list being read after
 * a helper exits or a recording is deleted (a page per file, at most
 * REC_LIST_MAX files), and rec_ctl_close(), which gives a running helper
 * REC_DESTROY_GRACE_MS to finalize before it is killed.
 *
 * Tested against the real pos-record over the file-backed fake sound card
 * and against a scripted fake helper (tests/rec_ctl_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_CTL_H
#define POCKETREC_CTL_H

#include "rec_session.h"
#include "rec_state.h"
#include "rec_store.h"

#include <stdbool.h>
#include <stdint.h>

/* Leaving the app while recording: the helper closes the microphone at once
 * and then finalizes (an fsync); this is how long the LVGL thread waits for
 * that before killing it, in which case the .part is repaired next time. */
#define REC_DESTROY_GRACE_MS 1500
/* A level older than this is shown as no level (paused, stalled). */
#define REC_LEVEL_STALE_MS 400
/* The peak mark stays this long before it falls to the current level. */
#define REC_PEAK_HOLD_MS 1500
/* A delete is armed by one tap and done by a second within this long. */
#define REC_DELETE_ARM_MS 5000
/* How often the free space is read while recording. */
#define REC_SPACE_EVERY_MS 2000

enum rec_tone {
    REC_TONE_PRIMARY,
    REC_TONE_MUTED,
    REC_TONE_OK,
    REC_TONE_WARN,
    REC_TONE_ERROR
};

struct rec_ctl {
    struct rec_machine m;
    struct rec_session session;
    const char *helper;
    char dir[REC_STORE_PATH_MAX];
    int dir_error;                 /* 0, or the errno that kept the folder from being made */
    enum rec_preset preset;
    struct rec_list list;
    char selected[REC_FILE_NAME_MAX];   /* "" for none; kept by name across refreshes */
    char current[REC_FILE_NAME_MAX];    /* the file being recorded or played */
    char play_path[REC_STORE_PATH_MAX + REC_FILE_NAME_MAX];
    int rate;                      /* of the current recording or playback */
    int64_t elapsed_ms;            /* recording: audio written; playback: position */
    int64_t total_ms;              /* playback: the file's length */
    int64_t bytes;                 /* recording: the file's size so far */
    int level_peak;                /* 0..32767 */
    int level_rms;
    int64_t level_at_ms;           /* when the last level arrived; 0 none */
    int peak_hold;
    int64_t peak_hold_at_ms;
    int64_t free_bytes;            /* -1 unknown */
    int64_t free_at_ms;
    int64_t delete_armed_ms;       /* 0 when not armed */
    unsigned repaired;             /* recordings repaired since open */
    unsigned changes;              /* bumped whenever the list is read again */
    char message[512];
    enum rec_tone tone;
    int (*volume)(void);           /* the system volume, 0 when muted */
    int64_t wall_now;              /* the wall clock at the last RECORD, for the name; */
    bool wall_valid;               /* also when it starts after a playback has stopped */
};

/* Resolve and create the folder, load the preset, read the list, and start
 * the repair helper. volume may be NULL (always 100). */
void rec_ctl_open(struct rec_ctl *c, int (*volume)(void));
/* Handle whatever the helper said. The number of events handled. */
int rec_ctl_poll(struct rec_ctl *c, int64_t now_ms);

/* The big button: RECORD, or STOP while recording. */
void rec_ctl_record(struct rec_ctl *c, int64_t now_ms, int64_t wall_now, bool wall_valid);
void rec_ctl_stop(struct rec_ctl *c, int64_t now_ms);
void rec_ctl_pause(struct rec_ctl *c, int64_t now_ms);
/* Play the selection, or stop the playback while one runs. */
void rec_ctl_play(struct rec_ctl *c, int64_t now_ms);
void rec_ctl_select(struct rec_ctl *c, int index);
/* First tap arms, second (within REC_DELETE_ARM_MS) deletes. */
void rec_ctl_delete(struct rec_ctl *c, int64_t now_ms);
void rec_ctl_next_preset(struct rec_ctl *c);
/* For a destroyed app: whatever runs is stopped (a recording finalized),
 * bounded by REC_DESTROY_GRACE_MS. */
void rec_ctl_close(struct rec_ctl *c);

/* The selected entry, or NULL. */
const struct rec_entry *rec_ctl_selected(const struct rec_ctl *c);
int rec_ctl_selected_index(const struct rec_ctl *c);
/* The level to show now (0 when stale). */
int rec_ctl_level_peak(const struct rec_ctl *c, int64_t now_ms);
int rec_ctl_level_rms(const struct rec_ctl *c, int64_t now_ms);
int rec_ctl_level_hold(const struct rec_ctl *c, int64_t now_ms);
bool rec_ctl_delete_armed(const struct rec_ctl *c, int64_t now_ms);

#endif
