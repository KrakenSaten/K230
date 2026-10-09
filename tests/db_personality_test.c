/*
 * DeskBuddy's personality (apps/deskbuddy/db_brain.h PERSONALITY,
 * db_gesture.h, db_face.h): a tap and a stroke told apart, a poke and its
 * escalation to a brief huff and back to calm, petting, a snack followed,
 * eaten or put away, falling asleep by degrees and waking, Guard and Night
 * left as they were, and all of it with no vision at all. Driven through
 * simulated time with a hand-held clock, like tests/db_brain_test.c.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "db_brain.h"
#include "db_face.h"
#include "db_gesture.h"
#include "db_guard.h"
#include "db_prefs.h"
#include "db_vision.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static struct db_guard_log glog;

static void fresh_seed(struct db_brain *b, const struct db_prefs *p, uint32_t seed, int64_t now)
{
    struct db_prefs d;

    if (!p) {
        db_prefs_defaults(&d);
        p = &d;
    }
    db_guard_init(&glog);
    db_brain_init(b, p, &glog, seed, now);
}

static void fresh(struct db_brain *b, const struct db_prefs *p, int64_t now)
{
    fresh_seed(b, p, 1234, now);
}

static void run_until(struct db_brain *b, int64_t until)
{
    int guard = 0;

    while (guard++ < 100000) {
        int64_t next = db_brain_next_ms(b);

        if (next > until) {
            break;
        }
        db_brain_tick(b, next);
    }
    db_brain_tick(b, until);
}

static bool stim(struct db_brain *b, enum db_stim_kind kind, int x, int y, int64_t now)
{
    struct db_stimulus s = { kind, DB_SRC_TOUCH, x, y };

    return db_brain_stimulus(b, &s, now);
}

static struct db_face face_of(const struct db_brain *b)
{
    struct db_face f;

    db_brain_face(b, &f);
    return f;
}

static void see(struct db_brain *b, enum db_vision_kind kind, int64_t now)
{
    struct db_vision_event ev = { .kind = kind, .confidence_pm = DB_CONF_NONE, .mono_ms = now };

    db_brain_vision(b, &ev, now);
}

/* The reaction's time left, played out. */
static int64_t finish(struct db_brain *b, int64_t t)
{
    int n = 0;

    while (b->react != DB_REACT_NONE && n++ < 64) {
        t = b->beat_until_ms;
        db_brain_tick(b, t);
    }
    return t;
}

/* ---- 1. gestures ----------------------------------------------------------------- */

static void test_gestures(void)
{
    struct db_gesture g;
    int i;
    int strokes = 0;
    enum db_gesture_kind k;

    db_gesture_init(&g);
    db_gesture_press(&g, 100, 100, 0);
    check("a quick touch in place is a tap", db_gesture_release(&g, 103, 98, 120) == DB_GESTURE_TAP);

    db_gesture_press(&g, 100, 100, 0);
    db_gesture_move(&g, 108, 104, 60);
    check("a tap may wobble a little", db_gesture_release(&g, 110, 104, 150) == DB_GESTURE_TAP);

    db_gesture_press(&g, 100, 100, 0);
    check("a long press that never moves is nothing", db_gesture_release(&g, 101, 100, 900) == DB_GESTURE_NONE);

    db_gesture_press(&g, 100, 100, 0);
    db_gesture_move(&g, 140, 100, 40);
    check("a short drag is not a tap", db_gesture_release(&g, 160, 100, 120) == DB_GESTURE_NONE);

    /* A gentle stroke: left and right across the character, ~0.4 px/ms. */
    db_gesture_press(&g, 100, 300, 0);
    for (i = 1; i <= 60; i++) {
        int x = 100 + (i % 30 < 15 ? (i % 15) * 12 : (15 - i % 15) * 12);

        k = db_gesture_move(&g, x, 300 + i % 3, i * 25);
        strokes += k == DB_GESTURE_STROKE;
        check("nothing but a stroke comes from a stroke", k == DB_GESTURE_NONE || k == DB_GESTURE_STROKE);
    }
    check("a stroke is reported once, while the finger moves", strokes == 1);
    check("and its release adds nothing", db_gesture_release(&g, 120, 300, 1550) == DB_GESTURE_NONE);

    db_gesture_press(&g, 100, 300, 0);
    for (i = 1; i <= 6; i++) {
        k = db_gesture_move(&g, 100 + i * 60, 300, i * 15);
        check("a flick is not a stroke", k == DB_GESTURE_NONE);
    }
    check("not even on release", db_gesture_release(&g, 480, 300, 100) == DB_GESTURE_NONE);

    db_gesture_press(&g, 100, 300, 0);
    db_gesture_move(&g, 160, 300, 150);
    check("a slow stroke not yet long enough waits", g.reported == false);
    check("and counts once let go long enough after", db_gesture_release(&g, 220, 300, 400) == DB_GESTURE_STROKE);

    db_gesture_press(&g, 100, 300, 0);
    for (i = 1; i <= 20; i++) {
        db_gesture_move(&g, 100 + (i & 1) * 40, 300, i * 30);
    }
    db_gesture_cancel(&g);
    check("a lost press reports nothing", db_gesture_release(&g, 100, 300, 700) == DB_GESTURE_NONE &&
                                            db_gesture_move(&g, 200, 300, 800) == DB_GESTURE_NONE);
}

/* ---- 2. a poke, and its variants ------------------------------------------------- */

static void test_poke(void)
{
    struct db_brain b;
    struct db_face f;
    struct db_eye_shape l;
    struct db_eye_shape r;
    int64_t t = 1000;
    int variant_seen[3] = { 0, 0, 0 };
    int64_t lens[2] = { 0, 0 };
    uint32_t seed;

    fresh(&b, NULL, t);
    check("blind and idle to begin with", b.state == DB_ST_IDLE && b.seen == DB_SEEN_UNAVAILABLE);
    check("a poke on the right starts a reaction", stim(&b, DB_STIM_POKE, 900, 100, t));
    f = face_of(&b);
    db_face_eyes(&f, 200, 200, &l, &r);
    check("it looks toward the touch", b.react == DB_REACT_POKE && (l.pupil_dx > 0 || l.pupil_d == 0) &&
                                           f.look && f.look_x > 0);
    check("and starts away from it", f.off_x <= 0);
    t = finish(&b, t);
    check("then it is calm again, in the same state", b.react == DB_REACT_NONE && b.state == DB_ST_IDLE);
    f = face_of(&b);
    check("looking ahead, no mouth", !f.look && f.mouth == DB_MOUTH_NONE);
    check("a poke is counted", b.poked == 1);
    check("and nothing about vision changed", b.seen == DB_SEEN_UNAVAILABLE && b.events == 0);

    /* Restrained variation: different seeds, different films. */
    for (seed = 1; seed < 60; seed++) {
        int64_t tt = 0;
        int64_t len;

        fresh_seed(&b, NULL, seed, 0);
        stim(&b, DB_STIM_POKE, -500, 0, tt);
        variant_seen[0] += b.beats && b.beats[0].expr == DB_EXPR_WIDE && b.beat_count == 2;
        variant_seen[1] += b.beats && b.beats[0].expr == DB_EXPR_CLOSED;
        variant_seen[2] += b.beats && b.beats[b.beat_count - 1].expr == DB_EXPR_WINK;
        len = finish(&b, tt);
        if (seed < 3) {
            lens[seed - 1] = len;
        }
    }
    check("there is more than one way to react to a poke",
          variant_seen[0] > 0 && variant_seen[1] > 0 && variant_seen[2] > 0);
    check("the mischievous one is the rarest", variant_seen[2] < variant_seen[0]);
    check("and the same reaction is not always the same length", lens[0] != lens[1]);
}

/* ---- 3. rapid pokes: a huff, then calm ------------------------------------------- */

static void test_annoyed(void)
{
    struct db_brain b;
    int64_t t = 0;
    int i;
    int64_t end;

    fresh(&b, NULL, t);
    stim(&b, DB_STIM_POKE, 0, 0, t);
    stim(&b, DB_STIM_POKE, 0, 0, t += 400);
    check("poked again soon: 'hey'", b.react == DB_REACT_HEY);
    run_until(&b, t += 250);
    check("a squint and a flat mouth", face_of(&b).expr == DB_EXPR_SQUINT && face_of(&b).mouth == DB_MOUTH_FLAT);
    stim(&b, DB_STIM_POKE, 0, 0, t += 150);
    stim(&b, DB_STIM_POKE, 0, 0, t += 400);
    check("four in a few seconds: annoyed", b.react == DB_REACT_ANNOYED && face_of(&b).expr == DB_EXPR_ANNOYED);
    check("with a huff from side to side", face_of(&b).off_x != 0);
    for (i = 0; i < 10; i++) {
        stim(&b, DB_STIM_POKE, 0, 0, t += 50);
        run_until(&b, t);
    }
    check("more pokes do not restart the huff", b.react == DB_REACT_ANNOYED && b.beat > 0);
    end = finish(&b, t);
    check("it settles by itself, briefly: under three seconds", end - t < 3000 && b.react == DB_REACT_NONE &&
                                                                     b.state == DB_ST_IDLE);
    t = end + 100;
    for (i = 0; i < 6; i++) {
        stim(&b, DB_STIM_POKE, 0, 0, t += 300);
        check("in the cool-down a poke is only a blink, never a huff", b.react == DB_REACT_POKE);
    }
    t = finish(&b, t);
    run_until(&b, t += DB_ANNOY_COOLDOWN_MS);
    for (i = 0; i < DB_POKES_TO_ANNOY; i++) {
        stim(&b, DB_STIM_POKE, 0, 0, t += 300);
    }
    check("after the cool-down it can be wound up again", b.react == DB_REACT_ANNOYED);

    fresh(&b, NULL, t = 0);
    for (i = 0; i < DB_POKES_TO_ANNOY; i++) {
        stim(&b, DB_STIM_POKE, 0, 0, t += DB_POKE_WINDOW_MS);
    }
    check("pokes spread out never annoy it", b.react != DB_REACT_ANNOYED);

    fresh(&b, NULL, t = 0);
    for (i = 0; i < DB_POKES_TO_ANNOY; i++) {
        stim(&b, DB_STIM_POKE, 0, 0, t += 200);
    }
    stim(&b, DB_STIM_PET, 0, 0, t += 200);
    check("a stroke calms a huff", b.react == DB_REACT_PET);
}

/* ---- 4. petting --------------------------------------------------------------------- */

static void test_pet(void)
{
    struct db_brain b;
    struct db_face f;
    int64_t t = 0;
    int64_t end;

    fresh(&b, NULL, t);
    check("a stroke pleases it", stim(&b, DB_STIM_PET, -200, 300, t) && b.react == DB_REACT_PET);
    f = face_of(&b);
    check("^ ^ and a smile", f.expr == DB_EXPR_HAPPY && f.mouth == DB_MOUTH_SMILE);
    stim(&b, DB_STIM_PET, -200, 300, t += 600);
    check("another stroke keeps it pleased, not starting over", b.react == DB_REACT_PET && b.petted == 2);
    end = finish(&b, t);
    check("then soft and calm", b.react == DB_REACT_NONE && end - t < 4000);
    check("petting never pokes", b.poked == 0);
}

/* ---- 5. the snack ------------------------------------------------------------------- */

static void test_snack(void)
{
    struct db_brain b;
    struct db_face f;
    int64_t t = 0;
    int chew_open = 0;
    int chew_shut = 0;
    int n = 0;

    fresh(&b, NULL, t);
    check("nothing to eat, nothing eaten", !stim(&b, DB_STIM_FEED, 0, DB_MOUTH_Y_PM, t) && b.fed == 0);
    check("a snack offered is noticed", stim(&b, DB_STIM_SNACK, 1500, 1500, t) && b.snack);
    f = face_of(&b);
    check("it watches the snack: curious, looking down and right",
          f.expr == DB_EXPR_CURIOUS && f.look && f.look_x > 0 && f.look_y > 0);
    stim(&b, DB_STIM_SNACK, -1500, 1500, t += 100);
    check("and follows it", face_of(&b).look_x < 0);
    check("far from the mouth it is not 'near'", !b.snack_near);
    stim(&b, DB_STIM_SNACK, 60, DB_MOUTH_Y_PM + 50, t += 100);
    f = face_of(&b);
    check("at the mouth: eyes wide, mouth open", b.snack_near && f.expr == DB_EXPR_WIDE && f.mouth == DB_MOUTH_O_BIG);
    check("fed", stim(&b, DB_STIM_FEED, 60, DB_MOUTH_Y_PM, t += 100) && b.fed == 1 && !b.snack);
    while (b.react == DB_REACT_EAT && n++ < 64) {
        f = face_of(&b);
        chew_open += f.mouth == DB_MOUTH_CHEW_OPEN;
        chew_shut += f.mouth == DB_MOUTH_CHEW_SHUT;
        t = b.beat_until_ms;
        db_brain_tick(&b, t);
    }
    check("it chews, three or four times", chew_open >= 3 && chew_open <= 4 && chew_shut == chew_open);
    check("then calm, nothing owed", b.react == DB_REACT_NONE && b.state == DB_ST_IDLE);

    stim(&b, DB_STIM_SNACK, 1500, 1500, t += 1000);
    stim(&b, DB_STIM_SNACK_GONE, 0, 0, t += 100);
    check("a snack put away is gone, and nothing is eaten", !b.snack && b.fed == 1 &&
                                                               !stim(&b, DB_STIM_FEED, 0, 0, t));
    stim(&b, DB_STIM_SNACK, 1500, 1500, t += 100);
    run_until(&b, t += DB_SNACK_IDLE_MS + 10);
    check("a snack nobody touches goes away by itself", !b.snack && b.react == DB_REACT_NONE);
    run_until(&b, t += 6 * 3600 * 1000LL);
    check("never fed again for hours: it only sleeps - no hunger, no fuss", b.state == DB_ST_SLEEP && b.fed == 1);
    {
        int wakes = 0;
        int64_t until = t + 60000;

        while (db_brain_next_ms(&b) <= until && wakes < 1000) {
            db_brain_tick(&b, db_brain_next_ms(&b));
            wakes++;
        }
        check("and asks for nothing: asleep, a tick per slow breath", wakes <= 60000 / DB_SLEEP_BREATH_MS + 1);
    }

    fresh(&b, NULL, t = 0);
    run_until(&b, t += DB_IDLE_SLEEP_MS);
    stim(&b, DB_STIM_SNACK, 1500, 1500, t);
    check("a snack offered to a sleeper wakes it gently first", b.react == DB_REACT_STIR && b.snack);
    t = finish(&b, t);
    check("then it watches the snack", face_of(&b).expr == DB_EXPR_CURIOUS);
}

/* ---- 6. sleepy, asleep, awake ------------------------------------------------------- */

static void test_sleep(void)
{
    struct db_brain b;
    struct db_prefs p;
    int64_t t = 0;

    fresh(&b, NULL, t);
    run_until(&b, t + DB_DROWSY_MS - 1);
    check("awake until the quiet is long", b.state == DB_ST_IDLE);
    run_until(&b, t += DB_DROWSY_MS);
    check("then drowsy: heavy lids", b.state == DB_ST_DROWSY &&
                                         (face_of(&b).expr == DB_EXPR_SLEEPY || b.react == DB_REACT_YAWN));
    run_until(&b, t += 10);
    t = finish(&b, t);
    check("a drowsy face nods: its timer keeps coming", db_brain_next_ms(&b) - t < DB_DROWSY_GAP_MAX_MS + 1);
    run_until(&b, DB_IDLE_SLEEP_MS);
    check("and asleep when the quiet is longer", b.state == DB_ST_SLEEP && face_of(&b).expr == DB_EXPR_CLOSED);
    t = DB_IDLE_SLEEP_MS;
    check("asleep it still breathes, slowly", db_brain_next_ms(&b) - t <= DB_SLEEP_BREATH_MS);

    check("a poke wakes it", stim(&b, DB_STIM_POKE, 0, 0, t += 10));
    check("gently: it stirs, lids first", b.state == DB_ST_IDLE && b.react == DB_REACT_STIR &&
                                            face_of(&b).expr == DB_EXPR_SLEEPY);
    t = finish(&b, t);
    check("then awake and calm", b.state == DB_ST_IDLE && b.react == DB_REACT_NONE);

    run_until(&b, t += DB_DROWSY_MS);
    stim(&b, DB_STIM_PET, 0, 0, t);
    check("a stroke on a drowsy face: awake and pleased", b.state == DB_ST_IDLE && b.react == DB_REACT_PET);

    t = finish(&b, t);
    check("REST: a yawn", stim(&b, DB_STIM_REST, 0, 0, t) && b.react == DB_REACT_REST &&
                              face_of(&b).mouth == DB_MOUTH_O_BIG);
    t = finish(&b, t);
    check("then asleep", b.state == DB_ST_SLEEP && db_brain_asleep(&b));
    check("REST when asleep does nothing", !stim(&b, DB_STIM_REST, 0, 0, t));
    check("WAKE wakes it", stim(&b, DB_STIM_WAKE, 0, 0, t += 10) && b.react == DB_REACT_STIR);
    t = finish(&b, t);
    check("WAKE when awake does nothing", !stim(&b, DB_STIM_WAKE, 0, 0, t));
    stim(&b, DB_STIM_REST, 0, 0, t += 10);
    stim(&b, DB_STIM_WAKE, 0, 0, t += 300);
    check("WAKE half-way into a rest backs out of it", b.state == DB_ST_IDLE && b.react == DB_REACT_STIR);

    fresh(&b, NULL, t = 0);
    see(&b, DB_VISION_NO_PERSON, t);
    run_until(&b, t += DB_DROWSY_MS + 10);
    see(&b, DB_VISION_PERSON_DETECTED, t += 10);
    check("somebody arriving wakes a drowsy face as it wakes a sleeping one", b.state == DB_ST_WAKE);

    db_prefs_defaults(&p);
    p.on[DB_PREF_IDLE_ANIMATION] = false;
    fresh(&b, &p, t = 0);
    run_until(&b, t += DB_IDLE_SLEEP_MS);
    check("idle animation off: still falls asleep, without a breath", b.state == DB_ST_SLEEP &&
                                                                         db_brain_next_ms(&b) == DB_NEVER);
}

/* ---- 7. Guard and Night are as they were ------------------------------------------------ */

static void test_other_modes(void)
{
    struct db_brain b;
    struct db_prefs p;
    int64_t t = 0;

    fresh(&b, NULL, t);
    stim(&b, DB_STIM_SNACK, 1500, 1500, t);
    db_brain_set_mode(&b, DB_MODE_GUARD, t += 10);
    check("leaving Companion puts a snack away", !b.snack);
    check("in Guard nothing is offered", !db_brain_playful(&b));
    check("a poke in Guard plays nothing", !stim(&b, DB_STIM_POKE, 0, 0, t) && b.react == DB_REACT_NONE);
    check("nor a snack", !stim(&b, DB_STIM_SNACK, 0, 0, t) && !b.snack);
    check("nor food", !stim(&b, DB_STIM_FEED, 0, 0, t) && b.fed == 0);
    check("nor rest", !stim(&b, DB_STIM_REST, 0, 0, t) && b.state == DB_ST_GUARD_DISARMED);
    check("Guard's face has no mouth", face_of(&b).mouth == DB_MOUTH_NONE && face_of(&b).off_y == 0);

    db_brain_set_mode(&b, DB_MODE_NIGHT, t += 10);
    run_until(&b, t += DB_NIGHT_SLEEP_MS);
    check("Night sleeps as before", b.state == DB_ST_NIGHT_SLEEP);
    stim(&b, DB_STIM_POKE, 0, 0, t += 10);
    check("and a touch is its plain touch: presence, no reaction",
          b.state == DB_ST_NIGHT_PRESENCE && b.react == DB_REACT_NONE && face_of(&b).dim);
    stim(&b, DB_STIM_PET, 0, 0, t += 10);
    check("a stroke too", b.react == DB_REACT_NONE && face_of(&b).mouth == DB_MOUTH_NONE);

    db_prefs_defaults(&p);
    p.on[DB_PREF_COMPANION] = false;
    p.on[DB_PREF_GUARD] = false;
    p.on[DB_PREF_NIGHT] = false;
    fresh(&b, &p, t = 0);
    check("Companion switched off: resting, it does not play",
          !db_brain_playful(&b) && !stim(&b, DB_STIM_POKE, 0, 0, t) && !stim(&b, DB_STIM_SNACK, 0, 0, t));
}

/* ---- 8. vision and personality share the face ------------------------------------------ */

static void test_with_vision(void)
{
    struct db_brain b;
    int64_t t = 0;

    fresh(&b, NULL, t);
    stim(&b, DB_STIM_POKE, 300, 0, t);
    see(&b, DB_VISION_OWNER_RECOGNIZED, t += 100);
    check("the owner arriving mid-reaction is greeted: vision wins", b.state == DB_ST_OWNER_GREETING &&
                                                                       b.react == DB_REACT_NONE);
    check("a poke does not cut a greeting short", !stim(&b, DB_STIM_POKE, 0, 0, t += 100) &&
                                                      b.state == DB_ST_OWNER_GREETING);
    run_until(&b, t += DB_GREETING_MS);
    check("and afterwards it plays again", b.state == DB_ST_IDLE && stim(&b, DB_STIM_POKE, 0, 0, t += 10));

    fresh(&b, NULL, t = 0);
    check("GREET is the door a seen wave would use", stim(&b, DB_STIM_GREET, 0, 0, t) &&
                                                         b.react == DB_REACT_GREET &&
                                                         face_of(&b).expr == DB_EXPR_HAPPY);
    {
        struct db_stimulus s = { DB_STIM_GREET, DB_SRC_VISION, 0, 0 };

        t = finish(&b, t);
        check("from any source alike", db_brain_stimulus(&b, &s, t) && b.react == DB_REACT_GREET);
        check("and it never makes up a sighting", b.seen == DB_SEEN_UNAVAILABLE && b.events == 0);
    }
    {
        struct db_stimulus bad = { (enum db_stim_kind)99, DB_SRC_TOUCH, 0, 0 };

        check("a kind out of range is ignored", !db_brain_stimulus(&b, &bad, t) && !db_brain_stimulus(&b, NULL, t));
    }
}

/* ---- 9. reduced motion ---------------------------------------------------------------------- */

static void test_reduced_motion(void)
{
    struct db_brain b;
    int64_t t = 0;
    int moved = 0;
    int chewed = 0;
    int n = 0;
    int i;

    fresh(&b, NULL, t);
    db_brain_set_reduced_motion(&b, true, t);
    check("reduced motion, nobody about: no breath, no blink", db_brain_next_ms(&b) == t + DB_DROWSY_MS);
    for (i = 0; i < DB_POKES_TO_ANNOY; i++) {
        stim(&b, DB_STIM_POKE, 800, 0, t += 200);
        moved += face_of(&b).off_x != 0 || face_of(&b).off_y != 0;
    }
    check("reactions still show, as end states: nothing shakes or leans", moved == 0 &&
                                                                          b.react == DB_REACT_ANNOYED);
    t = finish(&b, t);
    stim(&b, DB_STIM_SNACK, 0, DB_MOUTH_Y_PM, t);
    stim(&b, DB_STIM_FEED, 0, DB_MOUTH_Y_PM, t);
    while (b.react != DB_REACT_NONE && n++ < 64) {
        struct db_face f = face_of(&b);

        chewed += f.mouth == DB_MOUTH_CHEW_OPEN || f.mouth == DB_MOUTH_CHEW_SHUT;
        t = b.beat_until_ms;
        db_brain_tick(&b, t);
    }
    check("and eating is a smile, not a chewing loop", chewed == 0 && b.fed == 1);
}

/* ---- 10. never stuck: thousands of stimuli in any order ---------------------------------- */

static void test_fuzz(void)
{
    struct db_brain b;
    int64_t t = 0;
    uint32_t x = 0xC0FFEEu;
    int ok = 1;
    int calm_again = 1;
    int i;

    fresh(&b, NULL, t);
    for (i = 0; i < 20000; i++) {
        struct db_stimulus s;
        struct db_face f;

        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s.kind = (enum db_stim_kind)(x % (DB_STIM_COUNT + 1));
        s.source = (enum db_stim_source)(x % 3);
        s.x_pm = (int)(x % 9001) - 4500;
        s.y_pm = (int)((x >> 8) % 9001) - 4500;
        t += (int64_t)(x % 700);
        if (x % 97 == 0) {
            db_brain_set_mode(&b, (enum db_mode)((x >> 4) % DB_MODE_COUNT), t);
        }
        if (x % 89 == 0) {
            see(&b, (enum db_vision_kind)((x >> 5) % DB_VISION_KIND_COUNT), t);
        }
        db_brain_stimulus(&b, &s, t);
        run_until(&b, t);
        f = face_of(&b);
        ok &= (int)b.state >= 0 && b.state < DB_STATE_COUNT && f.expr < DB_EXPR_COUNT && f.mouth < DB_MOUTH_COUNT &&
              f.off_x >= -200 && f.off_x <= 200 && f.off_y >= -200 && f.off_y <= 200 && f.look_x >= -1000 &&
              f.look_x <= 1000 && f.look_y >= -1000 && f.look_y <= 1000 &&
              (b.react == DB_REACT_NONE || (b.beats && b.beat < b.beat_count)) &&
              (b.react == DB_REACT_NONE || b.state == DB_ST_IDLE || b.state == DB_ST_DROWSY);
        if (i % 1000 == 999) {
            run_until(&b, t += 30000);
            calm_again &= b.react == DB_REACT_NONE && !b.snack;
        }
    }
    check("twenty thousand stimuli: every face is whole, every reaction in a calm state", ok);
    check("and left alone it is always calm again within 30 s", calm_again);
}

/* ---- 11. the shapes -------------------------------------------------------------------------- */

static int fits(const struct db_eye_shape *e, int w, int h)
{
    return e->w > 0 && e->h > 0 && e->w <= w && e->h + 2 * (e->dy < 0 ? -e->dy : e->dy) <= h && e->lid < e->h &&
           (e->pupil_d == 0 || (e->pupil_d <= e->w && e->pupil_d <= e->h &&
                                2 * (e->pupil_dx < 0 ? -e->pupil_dx : e->pupil_dx) + e->pupil_d <= e->w &&
                                2 * (e->pupil_dy < 0 ? -e->pupil_dy : e->pupil_dy) + e->pupil_d <= e->h));
}

static void test_shapes(void)
{
    struct db_face f = { .expr = DB_EXPR_OPEN };
    struct db_eye_shape l;
    struct db_eye_shape r;
    struct db_eye_shape m;
    struct db_mouth_shape mo;
    struct db_mouth_shape mo2;
    struct db_mouth_shape mm;
    int all = 1;
    int mouths = 1;
    int s;
    int x;
    int k;

    for (s = 16; s <= 400; s += 7) {
        for (x = 0; x < DB_EXPR_COUNT; x++) {
            for (k = -1000; k <= 1000; k += 500) {
                f.expr = (enum db_expr)x;
                f.look = true;
                f.look_x = k;
                f.look_y = -k;
                db_face_eyes(&f, s, s, &l, &r);
                all &= fits(&l, s, s) && fits(&r, s, s);
            }
        }
        for (x = 0; x < DB_MOUTH_COUNT; x++) {
            f.mouth = (enum db_mouth)x;
            db_face_mouth(&f, s, &mo);
            /* The mouth stays inside the character and clear of the eyes. */
            mouths &= mo.w <= 2 * s && s / 2 + s * DB_MOUTH_Y_PM / 1000 + mo.h / 2 <= s * DB_CHARACTER_H_PM / 1000 &&
                      s * DB_MOUTH_Y_PM / 1000 - mo.h / 2 >= s / 2 &&
                      (x == DB_MOUTH_NONE) == (mo.w == 0);
        }
    }
    check("every expression, looking anywhere, fits its box, 16 to 400 px", all);
    check("every mouth fits below the eyes, inside the character", mouths);

    f.expr = DB_EXPR_WINK;
    f.look = false;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("a wink: one eye shut, one open", r.pupil_d == 0 && r.h * 4 < l.h && l.pupil_d > 0);
    f.expr = DB_EXPR_ANNOYED;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("annoyed: low lids, small pupils", l.lid > l.h / 2 && l.pupil_d < 200 * 200 / 1000);
    f.expr = DB_EXPR_OPEN;
    f.look = true;
    f.look_x = 1000;
    f.look_y = 1000;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("looking down-right moves both pupils there", l.pupil_dx > 0 && r.pupil_dx > 0 && l.pupil_dy > 0);

    f.mouth = DB_MOUTH_SMILE;
    db_face_mouth(&f, 200, &mo);
    check("a smile is a crescent: its cut-out sits higher", mo.cut_w > 0 && mo.cut_y < 0);
    f.mouth = DB_MOUTH_O;
    db_face_mouth(&f, 200, &mo2);
    check("an 'o' has its hole in the middle", mo2.cut_x > 0 && mo2.cut_y > 0 && mo2.cut_x + mo2.cut_w < mo2.w);

    f.expr = DB_EXPR_OPEN;
    f.look = false;
    db_face_eyes(&f, 200, 200, &l, &r);
    f.expr = DB_EXPR_HAPPY;
    db_face_eyes(&f, 200, 200, &r, &m);
    db_eye_shape_mix(&l, &r, 0, &m);
    check("a tween starts where it was", db_eye_shape_equal(&m, &l));
    db_eye_shape_mix(&l, &r, 1000, &m);
    check("and ends where it goes", db_eye_shape_equal(&m, &r));
    db_eye_shape_mix(&l, &r, 500, &m);
    check("half-way is between", m.h < l.h && m.h > r.h && m.arch_d > 0 && m.arch_d < r.arch_d);
    memset(&mm, 0, sizeof(mm));
    db_mouth_shape_mix(&mm, &mo, 500, &mm);
    check("a mouth grows in from nothing", mm.w > 0 && mm.w < mo.w);
}

int main(void)
{
    test_gestures();
    test_poke();
    test_annoyed();
    test_pet();
    test_snack();
    test_sleep();
    test_other_modes();
    test_with_vision();
    test_reduced_motion();
    test_fuzz();
    test_shapes();
    printf("db_personality_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
