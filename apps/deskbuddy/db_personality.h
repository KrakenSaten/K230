/* Camera-free reactions shared by touch and future recognition adapters.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef DB_PERSONALITY_H
#define DB_PERSONALITY_H
#include "db_brain.h"

#define DB_DROWSY_MS 90000
#define DB_PERSONALITY_SLEEP_MS 120000

enum db_reaction { DB_REACT_CALM, DB_REACT_POKE, DB_REACT_CURIOUS, DB_REACT_HAPPY,
    DB_REACT_ANNOYED, DB_REACT_EATING, DB_REACT_DROWSY, DB_REACT_ASLEEP, DB_REACT_WAKE };
enum db_interaction { DB_INTERACT_POKE, DB_INTERACT_PET, DB_INTERACT_SNACK,
    DB_INTERACT_FOLLOW, DB_INTERACT_FEED, DB_INTERACT_CANCEL, DB_INTERACT_REST, DB_INTERACT_WAKE };
struct db_personality {
    enum db_reaction reaction;
    int64_t activity_ms, since_ms, until_ms, last_poke_ms;
    unsigned pokes, variation;
    int x, y; /* gaze, -1000..1000; never persisted */
    bool snack;
};
struct db_pose {
    enum db_expr expr;
    int x, y, lift, mouth; /* lift is per mille of eye box; mouth: 0 none, 1 smile, 2 round */
    bool override;
};
void db_personality_init(struct db_personality *p, int64_t now);
void db_personality_event(struct db_personality *p, enum db_interaction event, int x, int y, int64_t now);
void db_personality_tick(struct db_personality *p, int64_t now);
int64_t db_personality_next_ms(const struct db_personality *p);
void db_personality_pose(const struct db_personality *p, int64_t now, bool reduced_motion, struct db_pose *out);

enum db_gesture_kind { DB_GESTURE_NONE, DB_GESTURE_TAP, DB_GESTURE_STROKE };
struct db_gesture {
    bool active;
    int start_x, start_y, last_x, last_y, path, extent;
    int64_t since_ms;
};
void db_gesture_begin(struct db_gesture *g, int x, int y, int64_t now);
void db_gesture_move(struct db_gesture *g, int x, int y, bool inside);
enum db_gesture_kind db_gesture_end(struct db_gesture *g, int64_t now);
void db_gesture_cancel(struct db_gesture *g);
#endif
