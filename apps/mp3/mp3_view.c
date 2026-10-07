/*
 * The MP3 screen's view model. See mp3_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mp3_view.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void mp3_view_time(int64_t ms, char *out, size_t n)
{
    int64_t s = ms < 0 ? 0 : ms / 1000;

    if (s >= 3600) {
        snprintf(out, n, "%" PRId64 ":%02d:%02d", s / 3600, (int)(s / 60 % 60), (int)(s % 60));
    } else {
        snprintf(out, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
    }
}

void mp3_view_size(int64_t bytes, char *out, size_t n)
{
    if (bytes < 0) {
        snprintf(out, n, "?");
    } else if (bytes < 1000) {
        snprintf(out, n, "%d B", (int)bytes);
    } else if (bytes < 1000 * 1000) {
        snprintf(out, n, "%d KB", (int)((bytes + 500) / 1000));
    } else {
        int64_t tenths = (bytes + 50000) / 100000;

        snprintf(out, n, "%" PRId64 ".%d MB", tenths / 10, (int)(tenths % 10));
    }
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');

    return s && s[1] ? s + 1 : path;
}

void mp3_view_row(const struct mp3_ctl *c, int index, char *title, size_t tlen, char *caption, size_t clen)
{
    const struct mp3_entry *e;

    title[0] = '\0';
    caption[0] = '\0';
    if (!c->list || index < 0 || index >= c->list->n) {
        return;
    }
    e = &c->list->e[index];
    snprintf(title, tlen, "%s", e->name);
    switch (e->kind) {
    case MP3_ENTRY_PLACE:
        snprintf(caption, clen, "%s", e->place < c->n_places ? c->places[e->place].path : "");
        break;
    case MP3_ENTRY_FOLDER:
        snprintf(caption, clen, "Folder");
        break;
    default: {
        char size[24];

        mp3_view_size(e->bytes, size, sizeof(size));
        snprintf(caption, clen, "%s%s", mp3_ctl_row_is_current(c, index) ? "NOW  " : "",
                 e->bytes == 0 ? "empty file" : size);
        break;
    }
    }
}

static void chip(const struct mp3_ctl *c, struct mp3_view *v)
{
    const struct mp3_player *p = &c->player;

    v->chip_kind = MP3_CHIP_OFF;
    if (!mp3_ctl_active(c)) {
        v->chip = c->q_index >= 0 ? "STOPPED" : "READY";
        return;
    }
    if (p->pending || p->state == MP3_PLAYER_STARTING) {
        v->chip = "OPENING";
    } else if (p->state == MP3_PLAYER_PAUSED) {
        v->chip = "PAUSED";
    } else {
        v->chip = "PLAYING";
        v->chip_kind = MP3_CHIP_ACTIVE;
    }
}

static void deck(const struct mp3_ctl *c, struct mp3_view *v)
{
    const struct mp3_player *p = &c->player;
    const char *name = mp3_ctl_current_name(c);
    int facts = mp3_ctl_player_is_current(c);
    int running = mp3_ctl_active(c);
    int64_t pos = running && facts ? p->pos_ms : 0;
    int64_t total = facts ? p->total_ms : 0;

    v->counter[0] = '\0';
    if (c->q_n > 0 && c->q_index >= 0) {
        snprintf(v->counter, sizeof(v->counter), "%d / %d", c->q_index + 1, c->q_n);
    }
    if (!name[0]) {
        snprintf(v->title, sizeof(v->title), "Nothing playing");
        v->subtitle[0] = '\0';
    } else if (facts && p->title[0]) {
        mp3_copy(v->title, sizeof(v->title), p->title);
        mp3_copy(v->subtitle, sizeof(v->subtitle), p->artist[0] ? p->artist : name);
    } else {
        mp3_copy(v->title, sizeof(v->title), name);
        mp3_copy(v->subtitle, sizeof(v->subtitle), facts && p->artist[0] ? p->artist : base_name(c->q_dir));
    }
    if (total > 0 && pos > total) {
        pos = total;
    }
    mp3_view_time(pos, v->elapsed, sizeof(v->elapsed));
    if (total > 0) {
        mp3_view_time(total, v->total, sizeof(v->total));
        v->progress = (int)(pos * 1000 / total);
    } else {
        snprintf(v->total, sizeof(v->total), "--:--");
        v->progress = 0;
    }
    v->seek_enabled = running && facts && !p->pending && p->seekable && total > 0 &&
                      (p->state == MP3_PLAYER_PLAYING || p->state == MP3_PLAYER_PAUSED);

    v->play_label = running && !p->want_paused ? "PAUSE" : "PLAY";
    v->play_enabled = running || c->q_index >= 0 || (c->list && c->list->tracks > 0);
    v->prev_enabled = c->q_n > 0 && c->q_index >= 0;
    v->next_enabled = c->q_n > 0 && c->q_index >= 0 && c->q_index + 1 < c->q_n;
    v->stop_enabled = running;
}

static void volume(const struct mp3_ctl *c, struct mp3_view *v)
{
    int available = mp3_ctl_volume_available(c);
    int muted = mp3_ctl_volume_muted(c);
    int level = mp3_ctl_volume_level(c);

    if (!available) {
        snprintf(v->volume, sizeof(v->volume), "NO AUDIO");
    } else if (muted) {
        snprintf(v->volume, sizeof(v->volume), "MUTED");
    } else {
        snprintf(v->volume, sizeof(v->volume), "%d %%", level);
    }
    v->vol_down_enabled = available && !muted && level > MP3_VOLUME_MIN;
    v->vol_up_enabled = available && (muted || level < MP3_VOLUME_MAX);
}

static void side(const struct mp3_ctl *c, struct mp3_view *v)
{
    const struct mp3_list *l = c->list;

    v->up_enabled = l && l->dir[0];
    v->note[0] = '\0';
    if (!l || !l->dir[0]) {
        snprintf(v->caption, sizeof(v->caption), "LIBRARY");
    } else {
        const char *label = mp3_ctl_place_label(c, c->root);
        size_t rn = strlen(c->root);
        const char *rest = strlen(l->dir) > rn ? l->dir + rn : "";

        snprintf(v->caption, sizeof(v->caption), "%s%s", label ? label : c->root, rest);
    }
    if (c->loading) {
        snprintf(v->note, sizeof(v->note), "Reading the folder...");
    } else if (l && l->n == 0) {
        snprintf(v->note, sizeof(v->note), "%s",
                 l->dir[0] ? "No audio files or folders here. Copy music into Music with Files or over "
                             "the network."
                           : "No storage to show.");
    } else if (l && l->truncated) {
        snprintf(v->note, sizeof(v->note), "Showing the first %d entries of this folder.", MP3_LIST_MAX);
    }
}

void mp3_view_refresh(const struct mp3_ctl *c, struct mp3_view *v)
{
    chip(c, v);
    deck(c, v);
    volume(c, v);
    side(c, v);
    snprintf(v->status, sizeof(v->status), "%s", c->message);
    v->status_tone = c->tone;
    if (!v->status[0] && c->q_index < 0 && !mp3_ctl_active(c)) {
        snprintf(v->status, sizeof(v->status), "Choose a track in the list.");
        v->status_tone = MP3_TONE_MUTED;
    }
    v->hint = mp3_ctl_active(c) && c->player.state == MP3_PLAYER_PLAYING ? "PLAYING" : NULL;
}
