/*
 * The MP3 app's playback state: one track at a time, over one pos-mp3
 * helper (mp3_session.h). No LVGL, no files, no playlist - the controller
 * (mp3_ctl.h) decides what plays; this decides how a track starts, pauses,
 * seeks, stops and ends, and what it says about itself.
 *
 *   IDLE      no helper
 *   STARTING  the helper is opening the file and the device
 *   PLAYING   the helper said "playing" (or "resumed")
 *   PAUSED    the helper said "paused": the device is closed
 *   STOPPING  a stop was sent; waiting for the helper to leave
 *
 * ONE HELPER AT A TIME. The audio lock admits one stream, so a new track
 * never starts while the old helper lives: mp3_player_play() while one runs
 * stops it and keeps the new request as the pending one, which starts when
 * the old helper has exited. Asking again before that replaces the pending
 * request, so a burst of NEXT presses starts one helper, not one each.
 *
 * PAUSE is what the owner asked last (want_paused), sent at once; the state
 * follows the helper's word. A resume the device refuses (Wave or Recorder
 * took it meanwhile) leaves the track PAUSED with a notice.
 *
 * HOW IT ENDED is kept once the helper is gone and nothing is pending, and
 * taken by mp3_player_take_end(): PLAYED (the file's end), STOPPED (asked
 * for), or FAILED (err_code and err_text say why).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETMP3_PLAYER_H
#define POCKETMP3_PLAYER_H

#include "mp3_library.h"
#include "mp3_protocol.h"
#include "mp3_session.h"

#include <stdint.h>

enum mp3_player_state {
    MP3_PLAYER_IDLE,
    MP3_PLAYER_STARTING,
    MP3_PLAYER_PLAYING,
    MP3_PLAYER_PAUSED,
    MP3_PLAYER_STOPPING
};

enum mp3_player_end {
    MP3_END_NONE,
    MP3_END_PLAYED,
    MP3_END_STOPPED,
    MP3_END_FAILED
};

struct mp3_player {
    struct mp3_session session;
    const char *helper;
    enum mp3_player_state state;
    int want_paused;
    char path[MP3_PATH_MAX];          /* the running (or last) helper's file */
    unsigned generation;              /* bumped for every helper started */

    /* What the running helper said about its track. */
    int64_t pos_ms;
    int64_t total_ms;                 /* 0 unknown */
    int seekable;
    char title[MP3_META_MAX + 1];
    char artist[MP3_META_MAX + 1];
    char codec[24];
    unsigned rate;
    unsigned channels;
    int volume;                       /* what the helper plays at */

    /* The next track, started when the running helper has gone. */
    int pending;
    char pending_path[MP3_PATH_MAX];
    int64_t pending_start_ms;
    int pending_volume;

    /* The running helper's outcome so far, and the last one's. */
    int stop_asked;
    int saw_played;
    char err_code[24];
    char err_text[200];
    unsigned notices;                 /* bumped for every error event */
    enum mp3_player_end end;
};

void mp3_player_init(struct mp3_player *p, const char *helper);

/* Play path from start_ms at volume (1..100). Starts at once when nothing
 * runs; otherwise stops what runs and starts this after it (see above).
 * 0, or -1 when the helper could not be started (end is FAILED). */
int mp3_player_play(struct mp3_player *p, const char *path, int64_t start_ms, int volume, int64_t now_ms);
void mp3_player_pause(struct mp3_player *p);
void mp3_player_resume(struct mp3_player *p);
/* Stop what runs and forget what is pending. */
void mp3_player_stop(struct mp3_player *p, int64_t now_ms);
/* Only while a seekable track runs; the position moves at once. */
void mp3_player_seek(struct mp3_player *p, int64_t ms);
/* The helper's volume, 1..100, sent at once while one runs. */
void mp3_player_set_volume(struct mp3_player *p, int percent);

/* Handle what the helper said, start a pending track when the old helper has
 * gone. The number of events handled. Never blocks. */
int mp3_player_poll(struct mp3_player *p, int64_t now_ms);

/* How the last track ended, once; MP3_END_NONE otherwise. */
enum mp3_player_end mp3_player_take_end(struct mp3_player *p);

int mp3_player_running(const struct mp3_player *p);
/* For a destroyed app: whatever runs is stopped, bounded by grace_ms. */
void mp3_player_close(struct mp3_player *p, int grace_ms);

#endif
