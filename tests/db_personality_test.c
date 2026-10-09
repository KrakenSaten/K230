/* Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#include "db_personality.h"
#include <stdio.h>

static int checks, failures;
static void check(const char *name, bool ok)
{
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", name); }
}
int main(void)
{
    struct db_personality p;
    struct db_gesture g;
    struct db_pose pose;
    db_gesture_begin(&g, 0, 0, 0);
    db_gesture_move(&g, 5, 3, true);
    check("small finger drift remains a tap", db_gesture_end(&g, 90) == DB_GESTURE_TAP);
    check("one press cannot trigger twice", db_gesture_end(&g, 100) == DB_GESTURE_NONE);
    db_gesture_begin(&g, 0, 0, 0);
    db_gesture_move(&g, 80, 0, true);
    check("a slow deliberate stroke is petting", db_gesture_end(&g, 700) == DB_GESTURE_STROKE);
    db_gesture_begin(&g, 0, 0, 0);
    db_gesture_move(&g, 80, 0, true);
    check("a fast swipe is neither petting nor poking", db_gesture_end(&g, 100) == DB_GESTURE_NONE);
    db_gesture_begin(&g, 0, 0, 0);
    db_gesture_move(&g, 80, 0, true);
    db_gesture_move(&g, 0, 0, true);
    check("scrubbing is not a gentle stroke", db_gesture_end(&g, 700) == DB_GESTURE_NONE);
    db_gesture_begin(&g, 0, 0, 0);
    db_gesture_move(&g, 80, 0, false);
    check("leaving the character cancels", db_gesture_end(&g, 700) == DB_GESTURE_NONE);
    db_gesture_begin(&g, 0, 0, 0);
    check("holding without moving does nothing", db_gesture_end(&g, 900) == DB_GESTURE_NONE);

    db_personality_init(&p, 0);
    db_personality_event(&p, DB_INTERACT_POKE, -600, 300, 100);
    db_personality_pose(&p, 100, false, &pose);
    check("poke is surprised and directed toward touch", pose.expr == DB_EXPR_WIDE && pose.x == -600 && pose.y == 300);
    db_personality_tick(&p, p.until_ms);
    check("surprise connects to curiosity", p.reaction == DB_REACT_CURIOUS);
    db_personality_tick(&p, p.until_ms);
    check("curiosity settles into calm", p.reaction == DB_REACT_CALM);
    db_personality_event(&p, DB_INTERACT_POKE, 0, 0, 2000);
    db_personality_event(&p, DB_INTERACT_POKE, 0, 0, 2100);
    db_personality_event(&p, DB_INTERACT_POKE, 0, 0, 2200);
    check("three rapid pokes briefly annoy", p.reaction == DB_REACT_ANNOYED);
    db_personality_tick(&p, 3100);
    check("annoyance ends without punishment", p.reaction == DB_REACT_CALM);
    db_personality_event(&p, DB_INTERACT_PET, 0, 0, 3200);
    check("petting resets poke escalation", p.reaction == DB_REACT_HAPPY && p.pokes == 0);
    db_personality_event(&p, DB_INTERACT_SNACK, 0, 700, 4000);
    db_personality_event(&p, DB_INTERACT_FOLLOW, 2000, -2000, 4100);
    check("snack tracking clamps gaze", p.snack && p.x == 1000 && p.y == -1000);
    db_personality_event(&p, DB_INTERACT_FEED, 0, 0, 4200);
    check("feeding consumes the snack once", !p.snack && p.reaction == DB_REACT_EATING);
    db_personality_pose(&p, 4200, false, &pose);
    int first = pose.mouth;
    db_personality_pose(&p, 4400, false, &pose);
    check("chewing changes the mouth", pose.mouth != first);
    db_personality_pose(&p, 4400, true, &pose);
    check("reduced motion keeps an eating end pose", pose.lift == 0 && pose.mouth == 1);
    db_personality_event(&p, DB_INTERACT_FEED, 0, 0, 4500);
    check("a second feed cannot restart eating", p.since_ms == 4200);
    db_personality_tick(&p, 5400);
    check("eating leads to a happy response", p.reaction == DB_REACT_HAPPY);
    db_personality_tick(&p, 6400);
    check("and naturally back to calm", p.reaction == DB_REACT_CALM);
    db_personality_init(&p, 0);
    db_personality_tick(&p, DB_DROWSY_MS - 1);
    check("awake before inactivity threshold", p.reaction == DB_REACT_CALM);
    db_personality_tick(&p, DB_DROWSY_MS);
    check("inactivity first lowers eyelids", p.reaction == DB_REACT_DROWSY);
    db_personality_tick(&p, DB_PERSONALITY_SLEEP_MS);
    check("then closes the eyes", p.reaction == DB_REACT_ASLEEP && db_personality_next_ms(&p) == DB_NEVER);
    db_personality_event(&p, DB_INTERACT_POKE, 0, 0, 120100);
    check("touch wakes without escalating annoyance", p.reaction == DB_REACT_WAKE && p.pokes == 0);
    db_personality_event(&p, DB_INTERACT_REST, 0, 0, 121000);
    db_personality_tick(&p, 121900);
    check("explicit rest also settles into sleep", p.reaction == DB_REACT_ASLEEP);
    db_personality_event(&p, DB_INTERACT_SNACK, 0, 0, 122000);
    db_personality_tick(&p, 137000);
    check("an abandoned snack disappears harmlessly", !p.snack && p.reaction == DB_REACT_CALM);
    db_personality_init(&p, 0);
    db_personality_tick(&p, 200000);
    check("a late inactivity tick reaches sleep without loops", p.reaction == DB_REACT_ASLEEP);
    printf("db_personality_test: %d checks, %d failure(s)\n", checks, failures);
    return failures != 0;
}
