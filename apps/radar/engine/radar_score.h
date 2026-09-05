/*
 * PocketRadar scoring: what an engagement is worth, what a mistake costs,
 * and the lifetime record a run is measured against.
 *
 * The model is deliberately small enough to hold in the head while playing.
 *
 *   points = base * (1 + response bonus) * streak multiplier
 *
 * base comes from the contact's class (radar_types.c). The response bonus
 * is up to RADAR_SCORE_RESPONSE_PCT and falls linearly with the life left
 * in the track, so working a contact the moment it appears is worth half as
 * much again as finishing it on its last tick; that is the whole of "fast
 * response = bonus". The streak multiplier rises by RADAR_SCORE_STREAK_STEP
 * tenths for each consecutive valid engagement and stops at
 * RADAR_SCORE_STREAK_CAP steps, so it tops out at 2.0 and a good run cannot
 * run away from an average one by an order of magnitude.
 *
 * Engaging a decoy costs a flat penalty and the streak; letting a decoy
 * fade costs nothing, because leaving it alone is the correct play. Letting
 * a real target fade costs a small penalty and the streak.
 *
 * Sector integrity is what ends a run. It is not a score: it starts full
 * and only ever falls, a missed target costing more than an engaged decoy
 * because a leaker is the failure the game is about. A player who never
 * misses plays until the difficulty ramp outruns them, which is the
 * intended shape of a two-to-five minute round.
 *
 * The running score is floored at zero. A penalty larger than the score can
 * still be reported in full - the value functions return what the action
 * was worth, not what survived the floor - but it cannot put the player
 * into a hole they can see no way out of.
 *
 * Integer arithmetic throughout, so a replay scores identically.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETRADAR_SCORE_H
#define POCKETRADAR_SCORE_H

#include "radar_types.h"

/* Extra percent of base for engaging the instant a track appears. */
#define RADAR_SCORE_RESPONSE_PCT 50
/* Tenths of a multiplier added per consecutive valid engagement, and the
 * number of steps before it stops: 1.0, 1.2, 1.4, 1.6, 1.8, 2.0. */
#define RADAR_SCORE_STREAK_STEP 2
#define RADAR_SCORE_STREAK_CAP 5
/* Flat cost of letting a valid target fade. */
#define RADAR_SCORE_MISS_PENALTY 25

#define RADAR_INTEGRITY_MAX 100
#define RADAR_INTEGRITY_MISS 20   /* five leakers end a run */
#define RADAR_INTEGRITY_FOUL 10

struct radar_score {
    int32_t points;
    uint16_t streak;        /* consecutive valid engagements */
    uint16_t best_streak;   /* the longest reached in this run */
    uint16_t engaged;       /* valid targets destroyed */
    uint16_t mistakes;      /* decoys engaged */
    uint16_t missed;        /* valid targets that faded */
    uint16_t integrity;     /* RADAR_INTEGRITY_MAX down to 0 */
};

/* What a run leaves behind. Small on purpose: PocketRadar keeps a best
 * score and enough counters to say whether this run was a good one, and
 * nothing that would turn into a progression system. */
struct radar_record {
    uint32_t best_score;
    uint16_t best_streak;
    uint16_t best_level;
    uint32_t runs;
    uint32_t engaged;       /* lifetime valid engagements */
    uint32_t mistakes;      /* lifetime decoys engaged */
};

void radar_score_init(struct radar_score *score);

/* What engaging this contact is worth right now, before it is recorded:
 * the value the UI shows beside an armed shot. streak is the streak
 * already standing. Returns 0 for a class that is not a valid target and
 * for an out-of-range lifetime. */
int32_t radar_score_value(enum radar_class cls, int ttl_left, int ttl_max, int streak);

/* Record a valid engagement and return what it was worth. */
int32_t radar_score_hit(struct radar_score *score, enum radar_class cls,
                        int ttl_left, int ttl_max);
/* Record engaging a decoy and return what it cost (negative). */
int32_t radar_score_foul(struct radar_score *score);
/* Record a valid target that faded and return what it cost (negative). */
int32_t radar_score_miss(struct radar_score *score);
/* 1 once sector integrity is gone, which is what ends a run. */
int radar_score_spent(const struct radar_score *score);

void radar_record_init(struct radar_record *record);
/* Fold a finished run into the record. Returns 1 when it set a new best
 * score, which is the one thing the app announces. */
int radar_record_note_run(struct radar_record *record, const struct radar_score *score,
                          int level);

#endif
