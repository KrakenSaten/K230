/*
 * The MP3 app's controller: the folder on screen (mp3_library.h), the play
 * queue, the track that plays (mp3_player.h) and the system volume, driven
 * by the owner's taps and a 50 ms poll from the screen. No LVGL: the screen
 * (mp3_app.c) reads what is here through mp3_view.h and paints it.
 *
 * Every call returns at once: folders are read on the scanner's thread and
 * each track is a helper process. The only wait on the LVGL thread is
 * mp3_ctl_close() giving a running helper MP3_DESTROY_GRACE_MS to close the
 * device before it is killed.
 *
 * THE QUEUE is the audio files of the folder a track was chosen in, as the
 * list showed them (at most MP3_LIST_MAX). Browsing elsewhere does not
 * change it. NEXT and PREV move in it; at its end playback stops. PREV more
 * than MP3_PREV_RESTART_MS into a track starts the track again.
 *
 * END OF A TRACK: the next one starts. A track that fails to play is said on
 * screen and playback stops - unless it was reached by the previous track's
 * end, in which case it is skipped and the one after it tried, at most once
 * round the queue, so a folder of damaged files cannot loop.
 *
 * VOLUME is the one system volume (app.h: pocketos_shell_volume_*, the same
 * as Controls), changed here in 10 % steps and passed to the helper at once.
 * Nothing starts while the sound is muted, and muting (here or in Controls)
 * pauses what plays.
 *
 * Tested against a scripted fake helper and the real pos-mp3 over the
 * file-backed fake sound card (tests/mp3_ctl_test.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETMP3_CTL_H
#define POCKETMP3_CTL_H

#include "mp3_library.h"
#include "mp3_player.h"

#include <stdint.h>

/* Leaving the app while playing: the helper closes the device within one
 * audio wait (200 ms); this is how long the LVGL thread waits for it before
 * killing it. */
#define MP3_DESTROY_GRACE_MS 1000
#define MP3_PREV_RESTART_MS 3000
#define MP3_VOLUME_STEP 10
#define MP3_VOLUME_MIN 10
#define MP3_VOLUME_MAX 100

enum mp3_tone {
    MP3_TONE_PRIMARY,
    MP3_TONE_MUTED,
    MP3_TONE_OK,
    MP3_TONE_WARN,
    MP3_TONE_ERROR
};

/* The system volume (app.h). Any may be NULL: no volume control, always
 * 100 %, never muted. */
struct mp3_volume_ops {
    int (*get)(void);
    int (*muted)(void);
    int (*available)(void);
    int (*set)(int percent);
    int (*set_muted)(int muted);
};

struct mp3_track {
    char name[MP3_NAME_MAX];
};

struct mp3_ctl {
    struct mp3_player player;
    struct mp3_scanner scanner;
    struct mp3_volume_ops vol;
    struct mp3_place places[MP3_PLACES_MAX];
    int n_places;

    struct mp3_list *list;          /* the folder on screen; NULL until the first read */
    char root[MP3_PATH_MAX];        /* its place's folder ("" for the places) */
    int loading;                    /* a folder is being read */
    int restoring;                  /* ... and it is the remembered one */
    char want_dir[MP3_PATH_MAX];
    char want_root[MP3_PATH_MAX];
    unsigned changes;               /* bumped when the list or the current track changes */

    char q_dir[MP3_PATH_MAX];
    struct mp3_track *queue;        /* MP3_LIST_MAX entries */
    int q_n;
    int q_index;                    /* -1 before a track is chosen */
    int advancing;                  /* the running track followed the last one's end */
    int skipped;                    /* failed tracks skipped in a row */
    unsigned notices_seen;
    int vol_sent;                   /* the volume the helper was last given; 0 muted */

    char message[320];
    enum mp3_tone tone;
};

/* Read the places (or the remembered folder). vol may be NULL. helper NULL
 * means mp3_session_helper_path(). */
void mp3_ctl_open(struct mp3_ctl *c, const struct mp3_volume_ops *vol, const char *helper);
/* Take a finished folder, handle what the helper said, follow the system
 * volume. The number of things that changed (0: nothing to repaint). */
int mp3_ctl_poll(struct mp3_ctl *c, int64_t now_ms);

/* A row of the list: a place or folder opens, a track plays (its folder
 * becomes the queue). */
void mp3_ctl_open_entry(struct mp3_ctl *c, int index, int64_t now_ms);
/* The folder above, or the places from a place's own folder. */
void mp3_ctl_up(struct mp3_ctl *c);

/* PLAY / PAUSE: play the current track (or the folder's first), pause, or
 * resume. */
void mp3_ctl_play_pause(struct mp3_ctl *c, int64_t now_ms);
void mp3_ctl_stop(struct mp3_ctl *c, int64_t now_ms);
void mp3_ctl_next(struct mp3_ctl *c, int64_t now_ms);
void mp3_ctl_prev(struct mp3_ctl *c, int64_t now_ms);
/* To a place in the track, 0..1000 of its length. */
void mp3_ctl_seek_permille(struct mp3_ctl *c, int permille);
/* One step up (+1) or down (-1). Up also unmutes. */
void mp3_ctl_volume_step(struct mp3_ctl *c, int dir, int64_t now_ms);

/* For a destroyed app: the helper is stopped (bounded by
 * MP3_DESTROY_GRACE_MS), the scan abandoned, everything freed. */
void mp3_ctl_close(struct mp3_ctl *c);

/* ---- for the view ---------------------------------------------------------- */

/* Whether a helper runs that the owner has not stopped. */
int mp3_ctl_active(const struct mp3_ctl *c);
/* The current track's file name, or "". */
const char *mp3_ctl_current_name(const struct mp3_ctl *c);
/* Whether the player's facts (title, length, position) are the current
 * track's. */
int mp3_ctl_player_is_current(const struct mp3_ctl *c);
/* Whether row index of the list is the current track. */
int mp3_ctl_row_is_current(const struct mp3_ctl *c, int index);
/* The system volume now: 0 when muted. */
int mp3_ctl_volume_effective(const struct mp3_ctl *c);
int mp3_ctl_volume_level(const struct mp3_ctl *c);
int mp3_ctl_volume_available(const struct mp3_ctl *c);
int mp3_ctl_volume_muted(const struct mp3_ctl *c);
/* The label of the place root is in, or NULL. */
const char *mp3_ctl_place_label(const struct mp3_ctl *c, const char *root);

#endif
