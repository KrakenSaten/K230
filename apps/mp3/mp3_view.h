/*
 * What the MP3 screen shows, worked out from the controller: every label's
 * text, every button's state, the progress, the rows. No LVGL, so it is
 * tested on its own (tests/mp3_view_test.c); mp3_app.c only paints it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETMP3_VIEW_H
#define POCKETMP3_VIEW_H

#include "mp3_ctl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mp3_chip {
    MP3_CHIP_OFF,
    MP3_CHIP_ACTIVE
};

struct mp3_view {
    const char *chip;             /* READY, OPENING, PLAYING, PAUSED, STOPPED */
    enum mp3_chip chip_kind;
    char counter[24];             /* "3 / 9", or "" */
    char title[MP3_NAME_MAX];     /* the title tag, else the file name */
    char subtitle[MP3_NAME_MAX];  /* the artist tag, else the file or folder name */
    char elapsed[16];
    char total[16];               /* "--:--" when not known */
    int progress;                 /* 0..1000 */
    bool seek_enabled;

    const char *play_label;       /* PLAY or PAUSE */
    bool play_enabled;
    bool prev_enabled;
    bool next_enabled;
    bool stop_enabled;
    char volume[24];              /* "70 %", "MUTED", "NO AUDIO" */
    bool vol_down_enabled;
    bool vol_up_enabled;

    char status[320];
    enum mp3_tone status_tone;

    char caption[MP3_PATH_MAX];   /* LIBRARY, or the place and folder */
    bool up_enabled;
    char note[160];               /* above the rows: loading, empty, more; "" none */
    const char *hint;             /* the header hint: "PLAYING", or NULL */
};

void mp3_view_refresh(const struct mp3_ctl *c, struct mp3_view *v);

/* One row of the list: its title and caption. */
void mp3_view_row(const struct mp3_ctl *c, int index, char *title, size_t tlen, char *caption, size_t clen);

/* m:ss, or h:mm:ss from an hour. */
void mp3_view_time(int64_t ms, char *out, size_t n);
/* 3.8 MB, 512 KB, 900 B. */
void mp3_view_size(int64_t bytes, char *out, size_t n);

#endif
