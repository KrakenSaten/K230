/*
 * System volume. See volume.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int volume_parse_percent(const char *s, int *out)
{
    char *end;
    long v;

    if (!s || !*s || *s == '+' || *s == '-' || *s == ' ') {
        return -1;
    }
    v = strtol(s, &end, 10);
    if (*end || v < VOLUME_MIN_PCT || v > VOLUME_MAX_PCT || v % VOLUME_STEP_PCT != 0) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

int volume_parse_muted(const char *s, int *out)
{
    if (s && strcmp(s, "0") == 0) {
        *out = 0;
        return 0;
    }
    if (s && strcmp(s, "1") == 0) {
        *out = 1;
        return 0;
    }
    return -1;
}

int volume_load(struct volume_state *v, const char *stored_percent, const char *stored_muted)
{
    int bad = 0;

    v->percent = VOLUME_DEFAULT_PCT;
    v->muted = 0;
    if (stored_percent && volume_parse_percent(stored_percent, &v->percent) < 0) {
        v->percent = VOLUME_DEFAULT_PCT;
        bad |= 1;
    }
    if (stored_muted && volume_parse_muted(stored_muted, &v->muted) < 0) {
        v->muted = 0;
        bad |= 2;
    }
    return bad;
}

int volume_round(int percent)
{
    int v = (percent + VOLUME_STEP_PCT / 2) / VOLUME_STEP_PCT * VOLUME_STEP_PCT;

    if (percent < 0) {
        v = 0;
    }
    if (v < VOLUME_MIN_PCT) {
        v = VOLUME_MIN_PCT;
    }
    if (v > VOLUME_MAX_PCT) {
        v = VOLUME_MAX_PCT;
    }
    return v;
}

int volume_effective(const struct volume_state *v)
{
    return v->muted ? 0 : v->percent;
}

int volume_output_present(const char *proc_root)
{
    char path[512];
    char line[160];
    FILE *f;
    int present = 0;

    snprintf(path, sizeof(path), "%s/asound/cards", proc_root ? proc_root : "/proc");
    f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    /* " 0 [K230I2SINNO    ]: ..." per card, or "--- no soundcards ---". */
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;

        while (*p == ' ') {
            p++;
        }
        if (*p >= '0' && *p <= '9') {
            present = 1;
            break;
        }
    }
    fclose(f);
    return present;
}
