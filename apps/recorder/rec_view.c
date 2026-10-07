/*
 * What the Recorder screen shows. See rec_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rec_view.h"

#include "rec_protocol.h"

#include <stdio.h>
#include <string.h>

#define DOT "\xc2\xb7" /* U+00B7, in the Latin-1 range the product fonts carry */

/* 32767 x 10^(-(k + 0.5)/20) for k = 0..59: a reading at or above entry k
 * is nearer k dB below full scale than k + 1. A table rather than log10: no
 * libm on the LVGL side for one number ten times a second, and the result is
 * the nearest whole dB. */
static const int db_steps[REC_METER_FLOOR_DB] = {
    30934, 27570, 24572, 21900, 19518, 17395, 15504, 13818, 12315, 10976, 9782, 8718,
    7770, 6925, 6172, 5501, 4903, 4370, 3894, 3471, 3093, 2757, 2457, 2190,
    1952, 1740, 1550, 1382, 1232, 1098, 978, 872, 777, 693, 617, 550,
    490, 437, 389, 347, 309, 276, 246, 219, 195, 174, 155, 138,
    123, 110, 98, 87, 78, 69, 62, 55, 49, 44, 39, 35,
};
/* -60 dBFS: anything quieter is silence to the meter. */
#define FLOOR_LEVEL 33

int rec_view_level_db(int level)
{
    int k;

    for (k = 0; k < REC_METER_FLOOR_DB; k++) {
        if (level >= db_steps[k]) {
            return -k;
        }
    }
    return -REC_METER_FLOOR_DB;
}

int rec_view_level_pct(int level)
{
    if (level < FLOOR_LEVEL) {
        return 0;
    }
    return (REC_METER_FLOOR_DB + rec_view_level_db(level)) * 100 / REC_METER_FLOOR_DB;
}

void rec_view_duration(char *out, size_t n, int64_t ms)
{
    int64_t s = ms > 0 ? ms / 1000 : 0;

    if (s >= 3600) {
        snprintf(out, n, "%lld:%02d:%02d", (long long)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    } else {
        snprintf(out, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
    }
}

void rec_view_size(char *out, size_t n, uint64_t bytes)
{
    if (bytes < 1024) {
        snprintf(out, n, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ull * 1024) {
        snprintf(out, n, "%llu KB", (unsigned long long)((bytes + 512) / 1024));
    } else if (bytes < 1024ull * 1024 * 1024) {
        unsigned long long t = (bytes * 10 + 512 * 1024) / (1024 * 1024);

        snprintf(out, n, "%llu.%llu MB", t / 10, t % 10);
    } else {
        unsigned long long t = (bytes * 10 + 512ull * 1024 * 1024) / (1024ull * 1024 * 1024);

        snprintf(out, n, "%llu.%llu GB", t / 10, t % 10);
    }
}

void rec_view_error_text(const char *line, char *out, size_t n)
{
    static const struct {
        const char *code;
        const char *words;
    } map[] = {
        { REC_ERR_AUDIO_BUSY, "Audio device in use" },
        { REC_ERR_AUDIO_NODEV, "No audio device found" },
        { REC_ERR_AUDIO_DISABLED, "Audio is not enabled on this device" },
        { REC_ERR_STORAGE_FULL, "Storage is full" },
        { REC_ERR_STORAGE, "Storage problem" },
        { REC_ERR_FORMAT, "This file cannot be played" },
        { REC_ERR_USAGE, "The recorder helper refused the request" },
        { REC_ERR_AUDIO, "Audio device error" },
    };
    const char *sp = line ? strchr(line, ' ') : NULL;
    size_t len = sp ? (size_t)(sp - line) : (line ? strlen(line) : 0);
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strlen(map[i].code) == len && strncmp(line, map[i].code, len) == 0) {
            /* Busy says all there is to say; the rest carry the helper's
             * reason, which never contains a file name. */
            if (strcmp(map[i].code, REC_ERR_AUDIO_BUSY) == 0 || !sp || !sp[1]) {
                snprintf(out, n, "%s", map[i].words);
            } else {
                snprintf(out, n, "%s: %s", map[i].words, sp + 1);
            }
            return;
        }
    }
    snprintf(out, n, "Recorder error: %s", line ? line : "");
}

void rec_view_row(const struct rec_entry *e, char *title, size_t tn, char *caption, size_t cn)
{
    char dur[16];
    char size[16];

    snprintf(title, tn, "%s", e->name);
    rec_view_size(size, sizeof(size), e->bytes);
    switch (e->status) {
    case REC_ENTRY_PART:
        snprintf(caption, cn, "Unfinished " DOT " %s " DOT " repaired when Recorder opens", size);
        return;
    case REC_ENTRY_UNREADABLE:
        snprintf(caption, cn, "Not a readable WAV " DOT " %s", size);
        return;
    case REC_ENTRY_OTHER:
        snprintf(caption, cn, "WAV this app does not play " DOT " %s", size);
        return;
    case REC_ENTRY_OK: break;
    }
    rec_view_duration(dur, sizeof(dur), (int64_t)e->ms);
    snprintf(caption, cn, "%s " DOT " %s " DOT " %u kHz%s%s%s", dur, size, e->rate / 1000u,
             e->channels == 2 ? " stereo" : "", e->recovered ? " " DOT " recovered" : "",
             e->truncated ? " " DOT " shorter than its header" : "");
}

static void set(char *dst, size_t n, const char *s)
{
    snprintf(dst, n, "%s", s);
}

void rec_view_refresh(const struct rec_ctl *c, int64_t now_ms, struct rec_view *v)
{
    const struct rec_machine *m = &c->m;
    const struct rec_entry *sel = rec_ctl_selected(c);
    bool idle = !rec_machine_busy(m);
    bool recording = m->state == REC_ST_RECORDING || m->state == REC_ST_PAUSED;
    bool playing = m->state == REC_ST_PLAYING || m->state == REC_ST_PLAY_PAUSED;
    int peak = rec_ctl_level_peak(c, now_ms);
    char a[16];
    char b[16];

    memset(v, 0, sizeof(*v));
    v->mic_on = rec_machine_mic_on(m);

    /* The chip: words first; the colour only repeats them (DS 2). */
    switch (m->state) {
    case REC_ST_CHECKING: set(v->chip, sizeof(v->chip), "CHECKING"); break;
    case REC_ST_IDLE: set(v->chip, sizeof(v->chip), "READY"); break;
    case REC_ST_ERROR: set(v->chip, sizeof(v->chip), "READY"); break;
    case REC_ST_STARTING: set(v->chip, sizeof(v->chip), "STARTING"); v->chip_kind = REC_CHIP_ACTIVE; break;
    case REC_ST_RECORDING: set(v->chip, sizeof(v->chip), "RECORDING"); v->chip_kind = REC_CHIP_LIVE; break;
    case REC_ST_PAUSED: set(v->chip, sizeof(v->chip), "PAUSED"); v->chip_kind = REC_CHIP_ACTIVE; break;
    case REC_ST_PLAYING: set(v->chip, sizeof(v->chip), "PLAYING"); v->chip_kind = REC_CHIP_PLAY; break;
    case REC_ST_PLAY_PAUSED: set(v->chip, sizeof(v->chip), "PAUSED"); v->chip_kind = REC_CHIP_ACTIVE; break;
    case REC_ST_STOPPING:
        set(v->chip, sizeof(v->chip), m->op == REC_OP_RECORD ? "SAVING" : "STOPPING");
        v->chip_kind = REC_CHIP_ACTIVE;
        break;
    }

    /* The timer: what is recorded, or where the playback is. */
    if (playing || (m->op == REC_OP_PLAY && m->state != REC_ST_IDLE)) {
        rec_view_duration(a, sizeof(a), c->elapsed_ms);
        rec_view_duration(b, sizeof(b), c->total_ms);
        snprintf(v->timer, sizeof(v->timer), "%s / %s", a, b);
    } else {
        rec_view_duration(v->timer, sizeof(v->timer), c->elapsed_ms);
    }

    /* The status line: the last thing that happened, else the state. */
    if (c->message[0]) {
        set(v->status, sizeof(v->status), c->message);
        v->status_tone = c->tone;
    } else if (recording || m->state == REC_ST_STARTING) {
        char size[16];

        rec_view_size(size, sizeof(size), (uint64_t)c->bytes);
        snprintf(v->status, sizeof(v->status), "%s " DOT " %d kHz " DOT " %s", c->current,
                 c->rate / 1000, size);
        v->status_tone = REC_TONE_MUTED;
    } else if (playing) {
        set(v->status, sizeof(v->status), c->current);
        v->status_tone = REC_TONE_MUTED;
    } else {
        set(v->status, sizeof(v->status), "Press RECORD to start. Recordings are kept on this device only.");
        v->status_tone = REC_TONE_MUTED;
    }

    /* The meter, from real readings only. */
    if (m->state == REC_ST_RECORDING || m->state == REC_ST_PLAYING) {
        v->meter_pct = rec_view_level_pct(peak);
        v->hold_pct = rec_view_level_pct(rec_ctl_level_hold(c, now_ms));
        if (!c->level_at_ms || now_ms - c->level_at_ms > REC_LEVEL_STALE_MS) {
            set(v->meter_text, sizeof(v->meter_text), "No reading");
            v->meter_tone = REC_TONE_MUTED;
        } else if (peak >= REC_CLIP_LEVEL) {
            set(v->meter_text, sizeof(v->meter_text), "Too loud");
            v->meter_tone = REC_TONE_WARN;
        } else if (peak < FLOOR_LEVEL) {
            set(v->meter_text, sizeof(v->meter_text), "Silence");
            v->meter_tone = REC_TONE_MUTED;
        } else {
            snprintf(v->meter_text, sizeof(v->meter_text), "%d dB", rec_view_level_db(peak));
            v->meter_tone = REC_TONE_PRIMARY;
        }
    } else {
        set(v->meter_text, sizeof(v->meter_text), "-");
        v->meter_tone = REC_TONE_MUTED;
    }

    /* The big button. */
    if (recording || (m->state == REC_ST_STARTING && m->op == REC_OP_RECORD)) {
        set(v->main_label, sizeof(v->main_label), "STOP");
        v->main_primary = true;
        v->main_enabled = true;
    } else if (m->state == REC_ST_STOPPING) {
        set(v->main_label, sizeof(v->main_label), m->op == REC_OP_RECORD ? "SAVING" : "RECORD");
        v->main_enabled = m->op != REC_OP_RECORD;
        v->main_primary = v->main_enabled;
    } else {
        set(v->main_label, sizeof(v->main_label), "RECORD");
        v->main_primary = true;
        v->main_enabled = m->state != REC_ST_CHECKING && !c->dir_error;
    }

    set(v->pause_label, sizeof(v->pause_label),
        m->state == REC_ST_PAUSED || m->state == REC_ST_PLAY_PAUSED ? "RESUME" : "PAUSE");
    v->pause_enabled = !m->cmd_pending && (recording || playing);

    if (playing || (m->state == REC_ST_STARTING && m->op == REC_OP_PLAY)) {
        bool same = sel && strcmp(sel->name, c->current) == 0;

        set(v->play_label, sizeof(v->play_label), same || !sel ? "STOP" : "PLAY");
        v->play_enabled = true;
    } else {
        set(v->play_label, sizeof(v->play_label), "PLAY");
        v->play_enabled = idle && sel && sel->status == REC_ENTRY_OK;
    }

    v->delete_enabled = idle && sel != NULL;
    set(v->delete_label, sizeof(v->delete_label), rec_ctl_delete_armed(c, now_ms) ? "CONFIRM" : "DELETE");

    set(v->preset_label, sizeof(v->preset_label), rec_preset_label(c->preset));
    v->preset_enabled = idle;

    /* What the free space holds at this preset, above the reserve. */
    if (c->free_bytes >= 0) {
        int64_t usable = c->free_bytes - (int64_t)REC_RESERVE_BYTES;
        int64_t per_min = (int64_t)rec_preset_rate(c->preset) * 2 * 60;
        int64_t min = usable > 0 ? usable / per_min : 0;

        if (min <= 0) {
            set(v->space, sizeof(v->space), "Storage full");
        } else if (min >= 600) {
            snprintf(v->space, sizeof(v->space), "Room for about %lld h", (long long)(min / 60));
        } else {
            snprintf(v->space, sizeof(v->space), "Room for about %lld min", (long long)min);
        }
    }

    if (c->list.total > c->list.n) {
        snprintf(v->list_caption, sizeof(v->list_caption), "RECORDINGS %d OF %d", c->list.n,
                 c->list.total);
    } else {
        snprintf(v->list_caption, sizeof(v->list_caption), "RECORDINGS %d", c->list.n);
    }

    v->hint = v->mic_on ? "MIC ON" : playing ? "PLAYING" : NULL;
}
