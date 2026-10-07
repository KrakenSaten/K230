/*
 * One track at a time over one helper. See mp3_player.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_player.h"

#include <stdio.h>
#include <string.h>

void mp3_player_init(struct mp3_player *p, const char *helper)
{
    memset(p, 0, sizeof(*p));
    mp3_session_init(&p->session);
    p->helper = helper ? helper : mp3_session_helper_path();
    p->state = MP3_PLAYER_IDLE;
}

int mp3_player_running(const struct mp3_player *p)
{
    return p->state != MP3_PLAYER_IDLE;
}

static void set_error(struct mp3_player *p, const char *code, const char *text)
{
    mp3_copy(p->err_code, sizeof(p->err_code), code);
    mp3_copy(p->err_text, sizeof(p->err_text), text);
    p->notices++;
}

static int start(struct mp3_player *p, const char *path, int64_t start_ms, int volume)
{
    char err[160];

    snprintf(p->path, sizeof(p->path), "%s", path);
    p->generation++;
    p->pos_ms = start_ms > 0 ? start_ms : 0;
    p->total_ms = 0;
    p->seekable = 0;
    p->title[0] = '\0';
    p->artist[0] = '\0';
    p->codec[0] = '\0';
    p->rate = 0;
    p->channels = 0;
    p->volume = volume;
    p->want_paused = 0;
    p->stop_asked = 0;
    p->saw_played = 0;
    p->err_code[0] = '\0';
    p->err_text[0] = '\0';
    p->end = MP3_END_NONE;
    if (mp3_session_start_play(&p->session, p->helper, path, start_ms, volume, err, sizeof(err)) != 0) {
        set_error(p, MP3_ERR_AUDIO, err);
        p->state = MP3_PLAYER_IDLE;
        p->end = MP3_END_FAILED;
        return -1;
    }
    p->state = MP3_PLAYER_STARTING;
    return 0;
}

int mp3_player_play(struct mp3_player *p, const char *path, int64_t start_ms, int volume, int64_t now_ms)
{
    if (volume < 1) {
        volume = 1;
    } else if (volume > 100) {
        volume = 100;
    }
    if (p->state == MP3_PLAYER_IDLE) {
        p->pending = 0;
        return start(p, path, start_ms, volume);
    }
    p->pending = 1;
    snprintf(p->pending_path, sizeof(p->pending_path), "%s", path);
    p->pending_start_ms = start_ms;
    p->pending_volume = volume;
    if (p->state != MP3_PLAYER_STOPPING) {
        p->stop_asked = 1;
        mp3_session_stop(&p->session, now_ms);
        p->state = MP3_PLAYER_STOPPING;
    }
    return 0;
}

void mp3_player_pause(struct mp3_player *p)
{
    if (p->state == MP3_PLAYER_STARTING || p->state == MP3_PLAYER_PLAYING || p->state == MP3_PLAYER_PAUSED) {
        p->want_paused = 1;
        mp3_session_command(&p->session, "pause");
    }
}

void mp3_player_resume(struct mp3_player *p)
{
    if (p->state == MP3_PLAYER_STARTING || p->state == MP3_PLAYER_PLAYING || p->state == MP3_PLAYER_PAUSED) {
        p->want_paused = 0;
        mp3_session_command(&p->session, "resume");
    }
}

void mp3_player_stop(struct mp3_player *p, int64_t now_ms)
{
    p->pending = 0;
    if (p->state == MP3_PLAYER_IDLE || p->state == MP3_PLAYER_STOPPING) {
        return;
    }
    p->stop_asked = 1;
    mp3_session_stop(&p->session, now_ms);
    p->state = MP3_PLAYER_STOPPING;
}

void mp3_player_seek(struct mp3_player *p, int64_t ms)
{
    if (!p->seekable || (p->state != MP3_PLAYER_PLAYING && p->state != MP3_PLAYER_PAUSED)) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    if (p->total_ms > 0 && ms > p->total_ms) {
        ms = p->total_ms;
    }
    if (mp3_session_seek(&p->session, ms) == 0) {
        p->pos_ms = ms;
    }
}

void mp3_player_set_volume(struct mp3_player *p, int percent)
{
    if (percent < 1 || percent > 100) {
        return;
    }
    if (p->pending) {
        p->pending_volume = percent;
    }
    if (p->state == MP3_PLAYER_IDLE || p->state == MP3_PLAYER_STOPPING) {
        return;
    }
    if (mp3_session_volume(&p->session, percent) == 0) {
        p->volume = percent;
    }
}

static void exited(struct mp3_player *p, int code)
{
    int stop_asked = p->stop_asked;

    p->state = MP3_PLAYER_IDLE;
    if (p->pending) {
        p->pending = 0;
        start(p, p->pending_path, p->pending_start_ms, p->pending_volume);
        return;
    }
    if (p->saw_played && code == MP3_EXIT_OK) {
        p->end = MP3_END_PLAYED;
    } else if (stop_asked || (code == MP3_EXIT_OK && !p->err_code[0])) {
        p->end = MP3_END_STOPPED;
    } else {
        if (!p->err_code[0]) {
            char text[80];

            snprintf(text, sizeof(text), "the player stopped unexpectedly (%d)", code);
            set_error(p, MP3_ERR_AUDIO, text);
        }
        p->end = MP3_END_FAILED;
    }
}

static void handle(struct mp3_player *p, const struct mp3_event *ev)
{
    switch (ev->kind) {
    case MP3_EV_TITLE:
        mp3_copy(p->title, sizeof(p->title), ev->text);
        break;
    case MP3_EV_ARTIST:
        mp3_copy(p->artist, sizeof(p->artist), ev->text);
        break;
    case MP3_EV_PLAYING:
        p->total_ms = ev->a;
        p->seekable = (int)ev->b;
        p->rate = (unsigned)ev->c;
        p->channels = (unsigned)ev->d;
        mp3_copy(p->codec, sizeof(p->codec), ev->text);
        if (p->state == MP3_PLAYER_STARTING) {
            p->state = MP3_PLAYER_PLAYING;
        }
        break;
    case MP3_EV_PROGRESS:
        p->pos_ms = ev->a;
        break;
    case MP3_EV_PAUSED:
        if (p->state != MP3_PLAYER_STOPPING) {
            p->state = MP3_PLAYER_PAUSED;
        }
        break;
    case MP3_EV_RESUMED:
        if (p->state != MP3_PLAYER_STOPPING) {
            p->state = MP3_PLAYER_PLAYING;
        }
        break;
    case MP3_EV_PLAYED:
        p->saw_played = 1;
        break;
    case MP3_EV_ERROR: {
        char code[24];
        const char *sp = strchr(ev->text, ' ');
        size_t n = sp ? (size_t)(sp - ev->text) : strlen(ev->text);

        snprintf(code, sizeof(code), "%.*s", (int)(n < sizeof(code) ? n : sizeof(code) - 1), ev->text);
        set_error(p, code, sp ? sp + 1 : "");
        /* A resume the device refused: still paused, and asked to be. */
        if (p->state == MP3_PLAYER_PAUSED) {
            p->want_paused = 1;
        }
        break;
    }
    case MP3_EV_EXITED:
        exited(p, ev->c);
        break;
    default:
        break;
    }
}

int mp3_player_poll(struct mp3_player *p, int64_t now_ms)
{
    struct mp3_event ev;
    int n = 0;

    if (p->state == MP3_PLAYER_IDLE) {
        return 0;
    }
    while (mp3_session_poll(&p->session, &ev, now_ms)) {
        unsigned gen = p->generation;

        handle(p, &ev);
        n++;
        /* A pending track started: its events come on the next poll. */
        if (p->generation != gen || p->state == MP3_PLAYER_IDLE) {
            break;
        }
    }
    return n;
}

enum mp3_player_end mp3_player_take_end(struct mp3_player *p)
{
    enum mp3_player_end e = p->end;

    p->end = MP3_END_NONE;
    return e;
}

void mp3_player_close(struct mp3_player *p, int grace_ms)
{
    p->pending = 0;
    mp3_session_abandon(&p->session, grace_ms);
    p->state = MP3_PLAYER_IDLE;
}
