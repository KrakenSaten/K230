/*
 * PocketTimber scoring. See timber_score.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "timber_score.h"

#include <string.h>

void timber_score_init(struct timber_score *score)
{
    if (!score) {
        return;
    }
    memset(score, 0, sizeof(*score));
}

int32_t timber_score_base(enum timber_class cls)
{
    switch (cls) {
    case TIMBER_CLASS_FREE:
        return TIMBER_SCORE_FREE;
    case TIMBER_CLASS_EASY:
        return TIMBER_SCORE_EASY;
    case TIMBER_CLASS_FIRM:
        return TIMBER_SCORE_FIRM;
    case TIMBER_CLASS_STUCK:
        return TIMBER_SCORE_STUCK;
    default:
        return 0;
    }
}

int32_t timber_score_value(enum timber_class cls, int layers_above, int clean, int tested, int streak)
{
    int64_t v = timber_score_base(cls);

    if (v == 0) {
        return 0;
    }
    if (layers_above < 0) {
        layers_above = 0;
    }
    if (streak < 0) {
        streak = 0;
    }
    if (streak > TIMBER_SCORE_STREAK_CAP) {
        streak = TIMBER_SCORE_STREAK_CAP;
    }
    v = v * (100 + TIMBER_SCORE_DEPTH_PCT * layers_above) / 100;
    if (clean) {
        v = v * TIMBER_SCORE_CLEAN_PCT / 100;
    }
    if (!tested) {
        v = v * TIMBER_SCORE_UNTESTED_PCT / 100;
    }
    v = v * (100 + TIMBER_SCORE_STREAK_STEP * streak) / 100;
    return (int32_t)v;
}

int32_t timber_score_pull(struct timber_score *score, enum timber_class cls, int layers_above,
                          int clean, int tested)
{
    int32_t v;

    if (!score) {
        return 0;
    }
    v = timber_score_value(cls, layers_above, clean, tested, score->streak);
    score->points += v;
    if (score->pulls < 0xFFFFu) {
        score->pulls++;
    }
    if (clean) {
        if (score->clean < 0xFFFFu) {
            score->clean++;
        }
        if (score->streak < 0xFFFFu) {
            score->streak++;
        }
        if (score->streak > score->best_streak) {
            score->best_streak = score->streak;
        }
    } else {
        score->streak = 0;
    }
    return v;
}

int32_t timber_score_layer(struct timber_score *score)
{
    if (!score) {
        return 0;
    }
    score->points += TIMBER_SCORE_LAYER;
    if (score->layers_built < 0xFFFFu) {
        score->layers_built++;
    }
    return TIMBER_SCORE_LAYER;
}

void timber_score_height(struct timber_score *score, int layers)
{
    if (!score || layers < 0 || layers > 0xFF) {
        return;
    }
    if (layers > score->height) {
        score->height = (uint8_t)layers;
    }
}

void timber_record_init(struct timber_record *record)
{
    if (!record) {
        return;
    }
    memset(record, 0, sizeof(*record));
}

int timber_record_note_run(struct timber_record *record, const struct timber_score *score)
{
    int best = 0;

    if (!record || !score) {
        return 0;
    }
    record->runs++;
    record->pulls += score->pulls;
    if ((uint32_t)score->points > record->best_score) {
        record->best_score = (uint32_t)score->points;
        best = 1;
    }
    if (score->height > record->best_height) {
        record->best_height = score->height;
    }
    if (score->best_streak > record->best_streak) {
        record->best_streak = score->best_streak;
    }
    return best;
}
