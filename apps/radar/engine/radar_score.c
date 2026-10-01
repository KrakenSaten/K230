/*
 * PocketRadar scoring. See radar_score.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "radar_score.h"

#include <string.h>

void radar_score_init(struct radar_score *score)
{
    if (!score) {
        return;
    }
    memset(score, 0, sizeof(*score));
    score->integrity = RADAR_INTEGRITY_MAX;
}

int32_t radar_score_value(enum radar_class cls, int ttl_left, int ttl_max, int streak)
{
    int32_t base = radar_class_base_points(cls);
    int32_t value;
    int32_t steps;

    if (!radar_class_is_target(cls) || base <= 0) {
        return 0;
    }
    if (ttl_max <= 0 || ttl_left < 0) {
        return 0;
    }
    if (ttl_left > ttl_max) {
        ttl_left = ttl_max;
    }
    /* Response bonus: the whole of it for a track still at full life, none
     * of it on the last tick. Multiply before dividing so the bonus does
     * not vanish into integer truncation for small values. */
    value = base + base * RADAR_SCORE_RESPONSE_PCT * ttl_left / (100 * ttl_max);

    steps = streak;
    if (steps < 0) {
        steps = 0;
    }
    if (steps > RADAR_SCORE_STREAK_CAP) {
        steps = RADAR_SCORE_STREAK_CAP;
    }
    return value * (10 + RADAR_SCORE_STREAK_STEP * steps) / 10;
}

/* Apply a change and hold the total at or above zero. The caller is told
 * what the action was worth, not what survived the floor, so the readout
 * reports the game's rule rather than an arithmetic artefact. */
static void add_points(struct radar_score *score, int32_t delta)
{
    score->points += delta;
    if (score->points < 0) {
        score->points = 0;
    }
}

static void spend_integrity(struct radar_score *score, int cost)
{
    if (score->integrity > cost) {
        score->integrity = (uint16_t)(score->integrity - cost);
    } else {
        score->integrity = 0;
    }
}

int32_t radar_score_hit(struct radar_score *score, enum radar_class cls,
                        int ttl_left, int ttl_max)
{
    int32_t value;

    if (!score || !radar_class_is_target(cls)) {
        return 0;
    }
    value = radar_score_value(cls, ttl_left, ttl_max, score->streak);
    add_points(score, value);
    if (score->streak < 0xFFFFu) {
        score->streak++;
    }
    if (score->streak > score->best_streak) {
        score->best_streak = score->streak;
    }
    if (score->engaged < 0xFFFFu) {
        score->engaged++;
    }
    return value;
}

int32_t radar_score_foul(struct radar_score *score)
{
    int32_t value;

    if (!score) {
        return 0;
    }
    /* Flat, unmultiplied and unmodified by how long the decoy had left: a
     * mistake costs the same whenever it is made. */
    value = radar_class_base_points(RADAR_CLASS_DECOY);
    add_points(score, value);
    score->streak = 0;
    if (score->mistakes < 0xFFFFu) {
        score->mistakes++;
    }
    spend_integrity(score, RADAR_INTEGRITY_FOUL);
    return value;
}

int32_t radar_score_miss(struct radar_score *score)
{
    if (!score) {
        return 0;
    }
    add_points(score, -RADAR_SCORE_MISS_PENALTY);
    score->streak = 0;
    if (score->missed < 0xFFFFu) {
        score->missed++;
    }
    spend_integrity(score, RADAR_INTEGRITY_MISS);
    return -RADAR_SCORE_MISS_PENALTY;
}

int radar_score_spent(const struct radar_score *score)
{
    return score && score->integrity == 0;
}

void radar_record_init(struct radar_record *record)
{
    if (!record) {
        return;
    }
    memset(record, 0, sizeof(*record));
}

int radar_record_note_run(struct radar_record *record, const struct radar_score *score,
                          int level)
{
    int best = 0;

    if (!record || !score) {
        return 0;
    }
    if (score->points > 0 && (uint32_t)score->points > record->best_score) {
        record->best_score = (uint32_t)score->points;
        best = 1;
    }
    if (score->best_streak > record->best_streak) {
        record->best_streak = score->best_streak;
    }
    if (level > 0 && (uint16_t)level > record->best_level) {
        record->best_level = (uint16_t)level;
    }
    record->runs++;
    record->engaged += score->engaged;
    record->mistakes += score->mistakes;
    return best;
}
