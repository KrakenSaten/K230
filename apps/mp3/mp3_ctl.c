/*
 * The MP3 app's controller. See mp3_ctl.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mp3_ctl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small things ------------------------------------------------------------ */

static void say(struct mp3_ctl *c, enum mp3_tone tone, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void say(struct mp3_ctl *c, enum mp3_tone tone, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(c->message, sizeof(c->message), fmt, ap);
    va_end(ap);
    c->tone = tone;
}

static void quiet(struct mp3_ctl *c)
{
    c->message[0] = '\0';
    c->tone = MP3_TONE_MUTED;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');

    return s && s[1] ? s + 1 : path;
}

int mp3_ctl_volume_available(const struct mp3_ctl *c)
{
    return c->vol.available ? c->vol.available() != 0 : 1;
}

int mp3_ctl_volume_muted(const struct mp3_ctl *c)
{
    return c->vol.muted ? c->vol.muted() != 0 : 0;
}

int mp3_ctl_volume_level(const struct mp3_ctl *c)
{
    int v = c->vol.get ? c->vol.get() : MP3_VOLUME_MAX;

    return v < 1 ? 1 : v > 100 ? 100 : v;
}

int mp3_ctl_volume_effective(const struct mp3_ctl *c)
{
    return mp3_ctl_volume_muted(c) ? 0 : mp3_ctl_volume_level(c);
}

int mp3_ctl_active(const struct mp3_ctl *c)
{
    const struct mp3_player *p = &c->player;

    return p->state != MP3_PLAYER_IDLE && !(p->state == MP3_PLAYER_STOPPING && !p->pending);
}

const char *mp3_ctl_current_name(const struct mp3_ctl *c)
{
    return c->q_index >= 0 && c->q_index < c->q_n ? c->queue[c->q_index].name : "";
}

int mp3_ctl_player_is_current(const struct mp3_ctl *c)
{
    char path[MP3_PATH_MAX];

    if (c->q_index < 0 || c->player.generation == 0 || c->player.pending ||
        mp3_library_join(path, c->q_dir, mp3_ctl_current_name(c)) != 0) {
        return 0;
    }
    return strcmp(path, c->player.path) == 0;
}

int mp3_ctl_row_is_current(const struct mp3_ctl *c, int index)
{
    return c->list && index >= 0 && index < c->list->n && c->q_index >= 0 &&
           c->list->e[index].kind == MP3_ENTRY_TRACK && strcmp(c->list->dir, c->q_dir) == 0 &&
           strcmp(c->list->e[index].name, mp3_ctl_current_name(c)) == 0;
}

const char *mp3_ctl_place_label(const struct mp3_ctl *c, const char *root)
{
    int k;

    for (k = 0; k < c->n_places; k++) {
        if (strcmp(c->places[k].path, root) == 0) {
            return c->places[k].label;
        }
    }
    return NULL;
}

/* ---- folders ------------------------------------------------------------------ */

static void navigate(struct mp3_ctl *c, const char *root, const char *dir, int restoring)
{
    snprintf(c->want_root, sizeof(c->want_root), "%s", root);
    snprintf(c->want_dir, sizeof(c->want_dir), "%s", dir);
    c->restoring = restoring;
    if (mp3_scanner_start(&c->scanner, dir) != 0) {
        c->loading = 0;
        say(c, MP3_TONE_ERROR, "The storage is not responding. Try again later.");
        return;
    }
    c->loading = 1;
}

static void took_list(struct mp3_ctl *c, struct mp3_list *l)
{
    c->loading = 0;
    if (l->err != 0 && l->dir[0]) {
        int restoring = c->restoring;
        char name[MP3_NAME_MAX];

        mp3_copy(name, sizeof(name), base_name(l->dir));
        free(l);
        if (restoring) {
            /* The remembered folder is gone: start from the places, quietly. */
            navigate(c, "", "", 0);
            return;
        }
        say(c, MP3_TONE_WARN, "%s cannot be opened. Was the storage removed?", name);
        if (!c->list) {
            navigate(c, "", "", 0);
        }
        return;
    }
    free(c->list);
    c->list = l;
    snprintf(c->root, sizeof(c->root), "%s", c->want_root);
    c->restoring = 0;
    c->changes++;
    mp3_library_save_last(c->root, l->dir);
}

void mp3_ctl_up(struct mp3_ctl *c)
{
    char parent[MP3_PATH_MAX];

    if (!c->list || !c->list->dir[0]) {
        return;
    }
    if (mp3_library_parent(parent, c->root, c->list->dir)) {
        navigate(c, c->root, parent, 0);
    } else {
        navigate(c, "", "", 0);
    }
}

/* ---- playing ------------------------------------------------------------------- */

static int volume_or_say(struct mp3_ctl *c)
{
    int v = mp3_ctl_volume_effective(c);

    if (v == 0) {
        say(c, MP3_TONE_WARN, "Sound is muted. VOL + turns it on.");
    }
    return v;
}

static void play_index(struct mp3_ctl *c, int i, int advancing, int64_t now_ms)
{
    char path[MP3_PATH_MAX];
    int v;

    if (i < 0 || i >= c->q_n) {
        return;
    }
    c->q_index = i;
    c->changes++;
    v = volume_or_say(c);
    if (v == 0) {
        mp3_player_stop(&c->player, now_ms);
        return;
    }
    if (mp3_library_join(path, c->q_dir, c->queue[i].name) != 0) {
        say(c, MP3_TONE_ERROR, "%s cannot be played: the path is too long.", c->queue[i].name);
        return;
    }
    c->advancing = advancing;
    if (!advancing) {
        c->skipped = 0;
    }
    if (!advancing || !c->message[0]) {
        quiet(c);
    }
    c->vol_sent = v;
    mp3_player_play(&c->player, path, 0, v, now_ms);
    c->notices_seen = c->player.notices;
}

/* Make the list's tracks the queue. The index of name in it, or -1. */
static int queue_from_list(struct mp3_ctl *c, const char *name)
{
    int k;
    int at = -1;

    c->q_n = 0;
    snprintf(c->q_dir, sizeof(c->q_dir), "%s", c->list->dir);
    for (k = 0; k < c->list->n && c->q_n < MP3_LIST_MAX; k++) {
        if (c->list->e[k].kind != MP3_ENTRY_TRACK) {
            continue;
        }
        if (name && strcmp(c->list->e[k].name, name) == 0) {
            at = c->q_n;
        }
        snprintf(c->queue[c->q_n].name, sizeof(c->queue[c->q_n].name), "%s", c->list->e[k].name);
        c->q_n++;
    }
    c->q_index = -1;
    return at;
}

void mp3_ctl_open_entry(struct mp3_ctl *c, int index, int64_t now_ms)
{
    const struct mp3_entry *e;
    char path[MP3_PATH_MAX];

    if (!c->list || index < 0 || index >= c->list->n || !c->queue) {
        return;
    }
    e = &c->list->e[index];
    if (e->kind == MP3_ENTRY_PLACE) {
        if (e->place < c->n_places) {
            navigate(c, c->places[e->place].path, c->places[e->place].path, 0);
        }
        return;
    }
    if (mp3_library_join(path, c->list->dir, e->name) != 0) {
        say(c, MP3_TONE_ERROR, "%s: the path is too long.", e->name);
        return;
    }
    if (e->kind == MP3_ENTRY_FOLDER) {
        navigate(c, c->root, path, 0);
        return;
    }
    play_index(c, queue_from_list(c, e->name), 0, now_ms);
}

void mp3_ctl_play_pause(struct mp3_ctl *c, int64_t now_ms)
{
    struct mp3_player *p = &c->player;

    if (mp3_ctl_active(c)) {
        if (p->want_paused) {
            if (volume_or_say(c) == 0) {
                return;
            }
            quiet(c);
            c->vol_sent = mp3_ctl_volume_effective(c);
            mp3_player_set_volume(p, c->vol_sent);
            mp3_player_resume(p);
        } else {
            mp3_player_pause(p);
        }
        return;
    }
    if (c->q_index >= 0) {
        play_index(c, c->q_index, 0, now_ms);
        return;
    }
    if (c->list && c->list->tracks > 0 && c->queue) {
        queue_from_list(c, NULL);
        play_index(c, 0, 0, now_ms);
        return;
    }
    say(c, MP3_TONE_MUTED, "Choose a track in the list.");
}

void mp3_ctl_stop(struct mp3_ctl *c, int64_t now_ms)
{
    c->advancing = 0;
    mp3_player_stop(&c->player, now_ms);
    quiet(c);
}

static void step(struct mp3_ctl *c, int to, int64_t now_ms)
{
    if (mp3_ctl_active(c)) {
        play_index(c, to, 0, now_ms);
    } else {
        /* Stopped: choose it, play nothing. */
        c->q_index = to;
        c->changes++;
        quiet(c);
    }
}

void mp3_ctl_next(struct mp3_ctl *c, int64_t now_ms)
{
    if (c->q_n == 0) {
        return;
    }
    if (c->q_index + 1 >= c->q_n) {
        say(c, MP3_TONE_MUTED, "This is the last track.");
        return;
    }
    step(c, c->q_index + 1, now_ms);
}

void mp3_ctl_prev(struct mp3_ctl *c, int64_t now_ms)
{
    struct mp3_player *p = &c->player;

    if (c->q_n == 0) {
        return;
    }
    if (mp3_ctl_active(c) && mp3_ctl_player_is_current(c) && p->pos_ms > MP3_PREV_RESTART_MS) {
        if (p->seekable && (p->state == MP3_PLAYER_PLAYING || p->state == MP3_PLAYER_PAUSED)) {
            mp3_player_seek(p, 0);
        } else {
            play_index(c, c->q_index, 0, now_ms);
        }
        return;
    }
    step(c, c->q_index > 0 ? c->q_index - 1 : 0, now_ms);
}

void mp3_ctl_seek_permille(struct mp3_ctl *c, int permille)
{
    struct mp3_player *p = &c->player;

    if (!mp3_ctl_player_is_current(c) || p->total_ms <= 0) {
        return;
    }
    if (permille < 0) {
        permille = 0;
    } else if (permille > 1000) {
        permille = 1000;
    }
    mp3_player_seek(p, p->total_ms * permille / 1000);
}

void mp3_ctl_volume_step(struct mp3_ctl *c, int dir, int64_t now_ms)
{
    int v;

    (void)now_ms;
    if (!mp3_ctl_volume_available(c)) {
        say(c, MP3_TONE_WARN, "There is no audio device.");
        return;
    }
    if (mp3_ctl_volume_muted(c)) {
        if (dir > 0 && c->vol.set_muted) {
            c->vol.set_muted(0);
            quiet(c);
        }
        return;
    }
    v = mp3_ctl_volume_level(c) + dir * MP3_VOLUME_STEP;
    v = v < MP3_VOLUME_MIN ? MP3_VOLUME_MIN : v > MP3_VOLUME_MAX ? MP3_VOLUME_MAX : v;
    /* The control takes multiples of its step only. */
    v = v / MP3_VOLUME_STEP * MP3_VOLUME_STEP;
    if (c->vol.set && v != mp3_ctl_volume_level(c)) {
        c->vol.set(v);
    }
    if (c->player.state != MP3_PLAYER_IDLE && mp3_ctl_volume_effective(c) > 0) {
        c->vol_sent = mp3_ctl_volume_effective(c);
        mp3_player_set_volume(&c->player, c->vol_sent);
    }
}

/* ---- how a track ended ----------------------------------------------------------- */

static int file_fault(const char *code)
{
    return strcmp(code, MP3_ERR_MISSING) == 0 || strcmp(code, MP3_ERR_STORAGE) == 0 ||
           strcmp(code, MP3_ERR_FORMAT) == 0 || strcmp(code, MP3_ERR_DECODE) == 0;
}

static void describe(struct mp3_ctl *c, const char *name, const char *prefix)
{
    const struct mp3_player *p = &c->player;
    char what[320];

    if (strcmp(p->err_code, MP3_ERR_AUDIO_BUSY) == 0) {
        snprintf(what, sizeof(what), "The audio device is in use (Wave or Recorder).");
    } else if (strcmp(p->err_code, MP3_ERR_AUDIO_NODEV) == 0) {
        snprintf(what, sizeof(what), "There is no audio device.");
    } else if (strcmp(p->err_code, MP3_ERR_AUDIO_DISABLED) == 0) {
        snprintf(what, sizeof(what), "Audio playback is not enabled on this device.");
    } else if (strcmp(p->err_code, MP3_ERR_MISSING) == 0) {
        snprintf(what, sizeof(what), "%s%s is not there any more.", prefix, name);
    } else if (strcmp(p->err_code, MP3_ERR_STORAGE) == 0) {
        snprintf(what, sizeof(what), "%s%s could not be read. Was the storage removed?", prefix, name);
    } else if (strcmp(p->err_code, MP3_ERR_FORMAT) == 0) {
        snprintf(what, sizeof(what), "%s%s cannot be played: %s.", prefix, name,
                 p->err_text[0] ? p->err_text : "not a supported audio file");
    } else if (strcmp(p->err_code, MP3_ERR_DECODE) == 0) {
        snprintf(what, sizeof(what), "%s%s stopped playing: the file is damaged.", prefix, name);
    } else {
        snprintf(what, sizeof(what), "Playback failed: %s.", p->err_text[0] ? p->err_text : "unknown error");
    }
    snprintf(c->message, sizeof(c->message), "%s", what);
    c->tone = MP3_TONE_ERROR;
}

static void ended(struct mp3_ctl *c, enum mp3_player_end end, int64_t now_ms)
{
    char name[MP3_NAME_MAX];

    mp3_copy(name, sizeof(name), base_name(c->player.path));
    switch (end) {
    case MP3_END_PLAYED:
        c->skipped = 0;
        if (c->q_index + 1 < c->q_n) {
            play_index(c, c->q_index + 1, 1, now_ms);
        } else {
            c->advancing = 0;
            say(c, MP3_TONE_MUTED, "End of %s.", c->q_dir[0] ? base_name(c->q_dir) : "the queue");
        }
        break;
    case MP3_END_FAILED:
        if (c->advancing && file_fault(c->player.err_code) && c->q_index + 1 < c->q_n &&
            c->skipped + 1 < c->q_n) {
            c->skipped++;
            describe(c, name, "Skipped: ");
            play_index(c, c->q_index + 1, 1, now_ms);
        } else {
            c->advancing = 0;
            describe(c, name, "");
        }
        break;
    case MP3_END_STOPPED:
        c->advancing = 0;
        break;
    default:
        break;
    }
    c->changes++;
}

/* ---- the poll -------------------------------------------------------------------- */

int mp3_ctl_poll(struct mp3_ctl *c, int64_t now_ms)
{
    struct mp3_list *l;
    enum mp3_player_end end;
    int n = 0;

    if (mp3_scanner_poll(&c->scanner, &l)) {
        took_list(c, l);
        n++;
    }
    n += mp3_player_poll(&c->player, now_ms);
    end = mp3_player_take_end(&c->player);
    if (end != MP3_END_NONE) {
        ended(c, end, now_ms);
        n++;
    }
    /* A notice from a helper that carries on: a refused resume. */
    if (c->player.notices != c->notices_seen) {
        c->notices_seen = c->player.notices;
        if (mp3_ctl_active(c)) {
            describe(c, base_name(c->player.path), "");
            n++;
        }
    }
    /* Follow the system volume (Controls may change it while we play). */
    if (mp3_ctl_active(c)) {
        int v = mp3_ctl_volume_effective(c);

        if (v != c->vol_sent) {
            if (v == 0) {
                mp3_player_pause(&c->player);
                say(c, MP3_TONE_WARN, "Sound is muted. VOL + turns it on.");
            } else {
                mp3_player_set_volume(&c->player, v);
            }
            c->vol_sent = v;
            n++;
        }
    }
    return n;
}

/* ---- open and close ------------------------------------------------------------ */

void mp3_ctl_open(struct mp3_ctl *c, const struct mp3_volume_ops *vol, const char *helper)
{
    char root[MP3_PATH_MAX];
    char dir[MP3_PATH_MAX];

    memset(c, 0, sizeof(*c));
    mp3_player_init(&c->player, helper);
    mp3_scanner_init(&c->scanner);
    if (vol) {
        c->vol = *vol;
    }
    c->q_index = -1;
    c->tone = MP3_TONE_MUTED;
    c->queue = calloc(MP3_LIST_MAX, sizeof(*c->queue));
    c->n_places = mp3_library_places(c->places);
    c->vol_sent = -1;
    if (mp3_library_load_last(root, dir) == 0 && mp3_ctl_place_label(c, root)) {
        navigate(c, root, dir, 1);
    } else {
        navigate(c, "", "", 0);
    }
}

void mp3_ctl_close(struct mp3_ctl *c)
{
    mp3_player_close(&c->player, MP3_DESTROY_GRACE_MS);
    mp3_scanner_close(&c->scanner);
    free(c->list);
    c->list = NULL;
    free(c->queue);
    c->queue = NULL;
    c->q_n = 0;
    c->q_index = -1;
}
