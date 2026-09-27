/*
 * What the Recorder screen shows, as text and flags, from the controller's
 * state: the chip, the timer, the status line, the meter, which buttons say
 * what and which are enabled, and each list row. Pure C, no LVGL, so every
 * word on the screen is tested (tests/rec_view_test.c); rec_app.c only
 * paints this.
 *
 * THE METER is the measured peak (and RMS) of the samples the helper wrote
 * or played in the last 100 ms, drawn on a decibel scale from -60 dBFS
 * (empty) to 0 dBFS (full), with the peak of the last REC_PEAK_HOLD_MS
 * marked. Nothing is animated or invented: with no fresh reading (paused,
 * stopped, the helper stalled) it is empty and says so.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETREC_VIEW_H
#define POCKETREC_VIEW_H

#include "rec_ctl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REC_METER_FLOOR_DB 60
/* A peak at or above this is shown as too loud: -0.5 dBFS. */
#define REC_CLIP_LEVEL 30930

enum rec_chip {
    REC_CHIP_OFF,     /* ready, checking */
    REC_CHIP_LIVE,    /* recording: the microphone is on */
    REC_CHIP_PLAY,    /* playing */
    REC_CHIP_ACTIVE   /* paused, starting, stopping */
};

struct rec_view {
    char chip[16];
    enum rec_chip chip_kind;
    char timer[40];
    char status[512];
    enum rec_tone status_tone;
    bool mic_on;
    int meter_pct;        /* 0..100 */
    int hold_pct;         /* 0..100, the peak mark; 0 none */
    char meter_text[24];  /* "-18 dB", "Too loud", "No signal", "-" */
    enum rec_tone meter_tone;
    char main_label[16];  /* RECORD or STOP */
    bool main_primary;
    bool main_enabled;
    char pause_label[16]; /* PAUSE or RESUME */
    bool pause_enabled;
    char play_label[16];  /* PLAY or STOP */
    bool play_enabled;
    char delete_label[16];
    bool delete_enabled;
    char preset_label[24];
    bool preset_enabled;
    char space[64];       /* what the free space holds at this preset */
    char list_caption[48];
    const char *hint;     /* for the shell header, or NULL */
};

void rec_view_refresh(const struct rec_ctl *c, int64_t now_ms, struct rec_view *v);

/* 0:07, 12:34, 1:02:03. */
void rec_view_duration(char *out, size_t n, int64_t ms);
/* 940 B, 12 KB, 5.8 MB, 1.2 GB (1024-based). */
void rec_view_size(char *out, size_t n, uint64_t bytes);
/* 0..100 on the -60..0 dBFS scale; 0 for silence. */
int rec_view_level_pct(int level);
/* The nearest whole dBFS; -60 or less reads as -60. */
int rec_view_level_db(int level);
/* A helper error line ("<code> <text>") in words for the owner. */
void rec_view_error_text(const char *line, char *out, size_t n);
/* A list row: the name, and "0:42 · 1.3 MB · 16 kHz" or what is wrong. */
void rec_view_row(const struct rec_entry *e, char *title, size_t tn, char *caption, size_t cn);

#endif
