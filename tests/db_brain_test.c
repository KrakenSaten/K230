/*
 * DeskBuddy's state machine and face (apps/deskbuddy/db_brain.h, db_face.h),
 * driven through hours of simulated time with a hand-held clock. Every check
 * is about behaviour - what state it is in, what the face shows, what the
 * guard wrote down, when it next needs a tick - never about how it gets
 * there.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "db_brain.h"
#include "db_face.h"
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

static void see(struct db_brain *b, enum db_vision_kind kind, int16_t conf, int64_t now)
{
    struct db_vision_event ev = { .kind = kind, .confidence_pm = conf, .mono_ms = now };

    db_brain_vision(b, &ev, now);
}

static void fresh(struct db_brain *b, const struct db_prefs *p, int64_t now)
{
    struct db_prefs d;

    if (!p) {
        db_prefs_defaults(&d);
        p = &d;
    }
    db_guard_init(&glog);
    db_brain_init(b, p, &glog, 1234, now);
    db_brain_set_wall(b, 1790000000);
}

/* Tick at every moment the brain asks for, up to until. */
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

static enum db_expr expr_of(const struct db_brain *b)
{
    struct db_face f;

    db_brain_face(b, &f);
    return f.expr;
}

/* ---- 1. companion transitions -------------------------------------------- */

static void test_companion(void)
{
    struct db_brain b;
    struct db_prefs p;
    int64_t t = 1000;

    db_prefs_defaults(&p);
    p.on[DB_PREF_IDLE_ANIMATION] = false; /* the transitions alone */
    fresh(&b, &p, t);
    check("companion starts IDLE and blind", b.state == DB_ST_IDLE && b.seen == DB_SEEN_UNAVAILABLE);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t);
    check("nobody: still IDLE, eyes open", b.state == DB_ST_IDLE && expr_of(&b) == DB_EXPR_OPEN);

    see(&b, DB_VISION_PERSON_DETECTED, 700, t += 100);
    check("a person arriving wakes it", b.state == DB_ST_WAKE && expr_of(&b) == DB_EXPR_WIDE);
    run_until(&b, t += DB_WAKE_MS);
    check("then it tries to recognise them", b.state == DB_ST_RECOGNIZING && expr_of(&b) == DB_EXPR_CURIOUS);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 950, t += 300);
    check("the owner: a happy greeting", b.state == DB_ST_OWNER_GREETING && expr_of(&b) == DB_EXPR_HAPPY);
    run_until(&b, t += DB_GREETING_MS);
    check("and back to IDLE", b.state == DB_ST_IDLE);

    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 1000);
    run_until(&b, t + DB_IDLE_SLEEP_MS - 1);
    check("not asleep a moment before the quiet is long enough", b.state == DB_ST_IDLE);
    run_until(&b, t += DB_IDLE_SLEEP_MS);
    check("asleep after DB_IDLE_SLEEP_MS with nobody there", b.state == DB_ST_SLEEP && expr_of(&b) == DB_EXPR_CLOSED);
    check("asleep and blind to animation: no tick needed", db_brain_next_ms(&b) == DB_NEVER);

    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 5000);
    check("a person wakes it from sleep", b.state == DB_ST_WAKE);
    run_until(&b, t += DB_WAKE_MS + DB_RECOGNIZE_MS);
    check("nobody identified: back to IDLE after RECOGNIZING times out", b.state == DB_ST_IDLE);
    check("someone is there, so it stays awake", db_brain_next_ms(&b) == DB_NEVER);

    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    run_until(&b, t += DB_IDLE_SLEEP_MS);
    check("asleep again", b.state == DB_ST_SLEEP);
    db_brain_poke(&b, t += 10);
    check("a touch wakes it", b.state == DB_ST_WAKE);
    run_until(&b, t += DB_WAKE_MS);
    check("with nobody seen it goes straight to IDLE", b.state == DB_ST_IDLE);
}

/* ---- 2. the owner ------------------------------------------------------------- */

static void test_owner(void)
{
    struct db_brain b;
    struct db_prefs p;
    int64_t t = 0;
    int i;

    fresh(&b, NULL, t);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 10);
    check("an owner event on its own greets", b.state == DB_ST_OWNER_GREETING);
    run_until(&b, t += DB_GREETING_MS);
    for (i = 0; i < 50; i++) {
        see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 200);
    }
    check("fifty more owner events: no second greeting", b.state == DB_ST_IDLE);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += DB_OWNER_AWAY_MS / 2);
    check("back after a short absence: no greeting", b.state == DB_ST_IDLE);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += DB_OWNER_AWAY_MS);
    check("back after DB_OWNER_AWAY_MS: greeted again", b.state == DB_ST_OWNER_GREETING);

    db_prefs_defaults(&p);
    p.on[DB_PREF_GREETING] = false;
    fresh(&b, &p, t = 0);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 10);
    check("greeting off: still a happy face (the text is the screen's)",
          b.state == DB_ST_OWNER_GREETING && expr_of(&b) == DB_EXPR_HAPPY);

    db_prefs_defaults(&p);
    p.on[DB_PREF_RECOGNITION] = false;
    fresh(&b, &p, t = 0);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 10);
    check("recognition off: the owner is only a person", b.state == DB_ST_WAKE && b.seen == DB_SEEN_PERSON);
    run_until(&b, t += DB_WAKE_MS);
    check("and it does not try to recognise them", b.state == DB_ST_IDLE);

    fresh(&b, NULL, t = 0);
    see(&b, DB_VISION_UNKNOWN_PERSON, 800, t += 10);
    check("a stranger: a curious, wary look", b.state == DB_ST_UNKNOWN_REACTION && expr_of(&b) == DB_EXPR_SUSPICIOUS);
    run_until(&b, t += DB_UNKNOWN_REACTION_MS);
    see(&b, DB_VISION_UNKNOWN_PERSON, 800, t += 1000);
    check("the same stranger again soon: not again", b.state == DB_ST_IDLE);
    check("companion mode logs nothing", db_guard_count(&glog) == 0);
}

/* ---- 3. a stranger while the guard is armed ------------------------------------- */

static void test_guard_unknown(void)
{
    struct db_brain b;
    int64_t t = 0;
    int i;

    fresh(&b, NULL, t);
    check("GUARD can be chosen", db_brain_set_mode(&b, DB_MODE_GUARD, t));
    check("it starts disarmed", b.state == DB_ST_GUARD_DISARMED && !b.prefs.guard_armed);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 10);
    run_until(&b, t += DB_GUARD_OWNER_MS);
    check("arming with the owner in front waits for them to leave",
          db_brain_arm(&b, t) && b.state == DB_ST_GUARD_ARMING && b.prefs.guard_armed);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 500);
    check("the owner still there does not disarm it", b.state == DB_ST_GUARD_ARMING);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 500);
    check("once they have gone it is ARMED", b.state == DB_ST_GUARD_ARMED);

    see(&b, DB_VISION_UNKNOWN_PERSON, 820, t += 60000);
    check("a stranger: GUARD_UNKNOWN", b.state == DB_ST_GUARD_UNKNOWN && expr_of(&b) == DB_EXPR_SUSPICIOUS);
    check("one event, unknown, with its confidence, not acknowledged",
          db_guard_count(&glog) == 1 && db_guard_at(&glog, 0)->subject == DB_SUBJECT_UNKNOWN &&
              db_guard_at(&glog, 0)->confidence_pm == 820 && !db_guard_at(&glog, 0)->acknowledged &&
              db_guard_at(&glog, 0)->wall_s == 1790000000);
    for (i = 0; i < 40; i++) {
        see(&b, DB_VISION_UNKNOWN_PERSON, 700, t += 100);
    }
    check("the same visit is one line however many events it makes", db_guard_count(&glog) == 1);
    check("and keeps its best confidence", db_guard_at(&glog, 0)->confidence_pm == 820);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    check("they left: ARMED again, still armed", b.state == DB_ST_GUARD_ARMED && b.prefs.guard_armed);
    see(&b, DB_VISION_UNKNOWN_PERSON, 900, t += DB_VISIT_MERGE_MS / 2);
    check("back within the merge window: the same visit", db_guard_count(&glog) == 1 &&
                                                               db_guard_at(&glog, 0)->confidence_pm == 900);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    see(&b, DB_VISION_UNKNOWN_PERSON, 600, t += DB_VISIT_MERGE_MS + 1);
    check("back later: a new visit", db_guard_count(&glog) == 2);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);

    /* A person the recognizer cannot place in time. */
    see(&b, DB_VISION_PERSON_DETECTED, 500, t += 60000);
    check("a person: GUARD_PERSON_DETECTED, nothing logged yet",
          b.state == DB_ST_GUARD_PERSON_DETECTED && db_guard_count(&glog) == 2);
    run_until(&b, t += DB_GUARD_IDENTIFY_MS);
    check("not identified in time: logged as a person", b.state == DB_ST_GUARD_UNKNOWN &&
                                                             db_guard_count(&glog) == 3 &&
                                                             db_guard_at(&glog, 0)->subject == DB_SUBJECT_PERSON);
    see(&b, DB_VISION_UNKNOWN_PERSON, 880, t += 500);
    check("identified later in the same visit: upgraded, not added",
          db_guard_count(&glog) == 3 && db_guard_at(&glog, 0)->subject == DB_SUBJECT_UNKNOWN &&
              db_guard_at(&glog, 0)->confidence_pm == 880);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);

    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 60000);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 1000);
    check("gone before anyone could say who: still a visit", db_guard_count(&glog) == 4 &&
                                                                  db_guard_at(&glog, 0)->subject == DB_SUBJECT_PERSON &&
                                                                  b.state == DB_ST_GUARD_ARMED);
    check("four visitors to report", db_guard_unacknowledged(&glog) == 4);
}

/* ---- 4. the log stays bounded under the brain ------------------------------------- */

static void test_guard_bounded(void)
{
    struct db_brain b;
    int64_t t = 0;
    int i;

    fresh(&b, NULL, t);
    db_brain_set_mode(&b, DB_MODE_GUARD, t);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t);
    db_brain_arm(&b, t);
    for (i = 0; i < 500; i++) {
        see(&b, DB_VISION_UNKNOWN_PERSON, 700, t += DB_VISIT_MERGE_MS + 10);
        see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 10);
    }
    check("five hundred visits keep only the newest DB_GUARD_CAP", db_guard_count(&glog) == DB_GUARD_CAP);
    check("the newest is the last visit", db_guard_at(&glog, 0)->id == 500);
    check("the oldest kept is the first of the last CAP", db_guard_at(&glog, DB_GUARD_CAP - 1)->id == 500 - DB_GUARD_CAP + 1);
}

/* ---- 5. the owner comes back ------------------------------------------------------- */

static void test_owner_returns(void)
{
    struct db_brain b;
    int64_t t = 0;
    const struct db_guard_event *e;

    fresh(&b, NULL, t);
    db_brain_set_mode(&b, DB_MODE_GUARD, t);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t);
    db_brain_arm(&b, t);
    see(&b, DB_VISION_UNKNOWN_PERSON, 750, t += 30000);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 5000);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 950, t += 600000);
    check("the owner back after a visitor: ALERT_PENDING", b.state == DB_ST_GUARD_ALERT_PENDING);
    check("the guard stood down", !b.prefs.guard_armed && !db_state_armed(b.state));
    e = db_guard_at(&glog, 0);
    check("the return is logged as the owner, already acknowledged",
          db_guard_count(&glog) == 2 && e->subject == DB_SUBJECT_OWNER && e->acknowledged);
    check("one visitor still to report", db_guard_unacknowledged(&glog) == 1);
    run_until(&b, t += 3600000);
    check("the alert waits for the owner, however long", b.state == DB_ST_GUARD_ALERT_PENDING);
    check("acknowledging clears it: GUARD_DISARMED, nothing left to report",
          db_brain_acknowledge(&b, t) && b.state == DB_ST_GUARD_DISARMED && db_guard_unacknowledged(&glog) == 0);
    check("acknowledging twice is refused", !db_brain_acknowledge(&b, t));

    /* Nobody came. */
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    db_brain_arm(&b, t);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 950, t += 600000);
    check("nobody came: a welcome back", b.state == DB_ST_GUARD_OWNER && expr_of(&b) == DB_EXPR_HAPPY);
    run_until(&b, t += DB_GUARD_OWNER_MS);
    check("then GUARD_DISARMED", b.state == DB_ST_GUARD_DISARMED);

    /* The owner comes back while the visitor is still there. */
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    db_brain_arm(&b, t);
    see(&b, DB_VISION_PERSON_DETECTED, 400, t += 1000);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 950, t += 500);
    check("the person was the owner after all: nothing logged for a visitor",
          b.state == DB_ST_GUARD_OWNER && db_guard_unacknowledged(&glog) == 0);

    /* Disarming by hand is the owner coming back too. */
    run_until(&b, t += DB_GUARD_OWNER_MS);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 100);
    db_brain_arm(&b, t);
    see(&b, DB_VISION_UNKNOWN_PERSON, 700, t += 1000);
    check("DISARM by hand with a visitor logged: ALERT_PENDING",
          db_brain_disarm(&b, t += 1000) && b.state == DB_ST_GUARD_ALERT_PENDING && !b.prefs.guard_armed);
    check("disarming what is not armed is refused", !db_brain_disarm(&b, t));
}

/* ---- 6. night ------------------------------------------------------------------------- */

static void test_night(void)
{
    struct db_brain b;
    struct db_prefs p;
    struct db_face f;
    int64_t t = 0;

    db_prefs_defaults(&p);
    p.on[DB_PREF_IDLE_ANIMATION] = false;
    fresh(&b, &p, t);
    check("NIGHT can be chosen", db_brain_set_mode(&b, DB_MODE_NIGHT, t));
    db_brain_face(&b, &f);
    check("the night face: sleepy and dim", b.state == DB_ST_NIGHT_IDLE && f.expr == DB_EXPR_SLEEPY && f.dim);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 1000);
    db_brain_face(&b, &f);
    check("somebody: the eyes open, still dim", b.state == DB_ST_NIGHT_PRESENCE && f.expr == DB_EXPR_OPEN && f.dim);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 1000);
    run_until(&b, t += DB_NIGHT_PRESENCE_MS);
    check("they stay: back to sleepy after a moment", b.state == DB_ST_NIGHT_IDLE);
    run_until(&b, t += DB_NIGHT_SLEEP_MS * 2);
    check("someone still there: the eyes do not close", b.state == DB_ST_NIGHT_IDLE);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t += 1000);
    run_until(&b, t += DB_NIGHT_SLEEP_MS);
    db_brain_face(&b, &f);
    check("quiet: NIGHT_SLEEP, eyes shut", b.state == DB_ST_NIGHT_SLEEP && f.expr == DB_EXPR_CLOSED && f.dim);
    see(&b, DB_VISION_UNKNOWN_PERSON, 800, t += 1000);
    check("anyone at night: a glance, no drama, nothing logged",
          b.state == DB_ST_NIGHT_PRESENCE && db_guard_count(&glog) == 0);

    /* Blind. */
    fresh(&b, &p, t = 0);
    db_brain_set_mode(&b, DB_MODE_NIGHT, t);
    see(&b, DB_VISION_UNAVAILABLE, DB_CONF_NONE, t);
    run_until(&b, t += DB_NIGHT_SLEEP_MS);
    check("without vision it still falls asleep", b.state == DB_ST_NIGHT_SLEEP);
    db_brain_poke(&b, t += 10);
    check("and a touch still wakes it", b.state == DB_ST_NIGHT_PRESENCE);

    p.on[DB_PREF_NIGHT] = false;
    fresh(&b, &p, t = 0);
    check("night off: NIGHT cannot be chosen", !db_brain_set_mode(&b, DB_MODE_NIGHT, t) && b.state == DB_ST_IDLE);
    db_prefs_defaults(&p);
    p.mode = DB_MODE_NIGHT;
    fresh(&b, &p, t);
    check("it starts in the mode it was left in", b.state == DB_ST_NIGHT_IDLE);
    p.on[DB_PREF_NIGHT] = false;
    db_brain_set_prefs(&b, &p, t += 10);
    check("turning night off moves it to an allowed mode", b.state == DB_ST_IDLE && b.prefs.mode == DB_MODE_COMPANION);
}

/* ---- 7. repeated, rapid and malformed events ------------------------------------------- */

static void test_rapid(void)
{
    struct db_brain b;
    struct db_vision_event bad = { .kind = (enum db_vision_kind)99, .confidence_pm = DB_CONF_NONE };
    unsigned events;
    unsigned revs;
    int64_t t = 0;
    int i;

    fresh(&b, NULL, t);
    for (i = 0; i < 10000; i++) {
        see(&b, (enum db_vision_kind)(i % DB_VISION_KIND_COUNT), (int16_t)(i % 1001), t += 1);
        db_brain_tick(&b, t);
    }
    check("ten thousand mixed events at 1 ms: a valid state", b.state < DB_STATE_COUNT &&
                                                               db_state_mode(b.state) == DB_MODE_COMPANION);
    check("companion wrote no log", db_guard_count(&glog) == 0);

    db_brain_set_mode(&b, DB_MODE_GUARD, t);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t);
    db_brain_arm(&b, t);
    revs = b.log_rev;
    for (i = 0; i < 2000; i++) {
        see(&b, i & 1 ? DB_VISION_NO_PERSON : DB_VISION_UNKNOWN_PERSON, 700, t += 3);
    }
    check("a flickering stranger is one visit", db_guard_count(&glog) == 1);
    check("and one write, not two thousand", b.log_rev - revs == 1);

    events = b.events;
    db_brain_vision(&b, &bad, t);
    bad.kind = DB_VISION_PERSON_DETECTED;
    bad.confidence_pm = 5000;
    db_brain_vision(&b, &bad, t);
    db_brain_vision(&b, NULL, t);
    check("malformed events are ignored", b.events == events && db_guard_count(&glog) == 1);

    fresh(&b, NULL, t = 0);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 10);
    for (i = 0; i < 100; i++) {
        see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 5);
    }
    check("a stream of detections does not restart the wake-up", b.state == DB_ST_WAKE && b.since_ms == 10);
    db_brain_tick(&b, t + 100000000);
    check("a clock that jumps hours ahead settles, it does not spin", b.state < DB_STATE_COUNT);
}

/* ---- 8. no vision ------------------------------------------------------------------------ */

static void test_unavailable(void)
{
    struct db_brain b;
    int64_t t = 0;

    fresh(&b, NULL, t);
    see(&b, DB_VISION_UNAVAILABLE, DB_CONF_NONE, t);
    check("blind: companion idles", b.state == DB_ST_IDLE && b.seen == DB_SEEN_UNAVAILABLE);
    run_until(&b, t += DB_IDLE_SLEEP_MS);
    check("and naps when nobody touches it", b.state == DB_ST_SLEEP);

    db_brain_set_mode(&b, DB_MODE_GUARD, t);
    check("the guard can be armed blind (there is nobody to wait for)",
          db_brain_arm(&b, t) && b.state == DB_ST_GUARD_ARMED);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 100);
    see(&b, DB_VISION_UNAVAILABLE, DB_CONF_NONE, t += 100);
    check("vision lost mid-visit: the visit is still logged, armed again",
          b.state == DB_ST_GUARD_ARMED && db_guard_count(&glog) == 1);

    fresh(&b, NULL, t = 0);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t += 10);
    see(&b, DB_VISION_UNAVAILABLE, DB_CONF_NONE, t += 10);
    check("vision lost while waking: IDLE, not stuck", b.state == DB_ST_IDLE);
}

/* ---- preferences and modes ---------------------------------------------------------------- */

static void test_prefs(void)
{
    struct db_brain b;
    struct db_prefs p;
    int64_t t = 0;

    fresh(&b, NULL, t);
    check("a fresh brain has nothing to save", b.prefs_rev == 0);
    db_brain_set_mode(&b, DB_MODE_GUARD, t);
    check("a mode change is saved", b.prefs_rev > 0 && b.prefs.mode == DB_MODE_GUARD);
    see(&b, DB_VISION_NO_PERSON, DB_CONF_NONE, t);
    db_brain_arm(&b, t);
    check("arming twice is refused", !db_brain_arm(&b, t));
    p = b.prefs;
    p.on[DB_PREF_GUARD] = false;
    db_brain_set_prefs(&b, &p, t += 10);
    check("turning the guard off disarms it and leaves GUARD",
          !b.prefs.guard_armed && db_state_mode(b.state) == DB_MODE_COMPANION);
    check("arming outside GUARD is refused", !db_brain_arm(&b, t));

    db_prefs_defaults(&p);
    p.mode = DB_MODE_GUARD;
    p.guard_armed = true;
    fresh(&b, &p, t = 0);
    check("reopened armed: ARMED again", b.state == DB_ST_GUARD_ARMED && b.prefs_rev == 0);
    check("leaving GUARD by hand disarms", db_brain_set_mode(&b, DB_MODE_COMPANION, t) && !b.prefs.guard_armed);

    db_prefs_defaults(&p);
    p.on[DB_PREF_COMPANION] = false;
    p.on[DB_PREF_GUARD] = false;
    p.on[DB_PREF_NIGHT] = false;
    fresh(&b, &p, t = 0);
    see(&b, DB_VISION_OWNER_RECOGNIZED, 900, t += 10);
    check("every mode off: a resting face that does not react", b.state == DB_ST_IDLE);
}

/* ---- idle behaviour and ticks -------------------------------------------------------------- */

static void test_idle(void)
{
    struct db_brain b;
    struct db_prefs p;
    int64_t t = 0;
    int blinks = 0;
    int glances = 0;
    int reopened = 1;
    int i;

    fresh(&b, NULL, t);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t);
    run_until(&b, t += DB_WAKE_MS + DB_RECOGNIZE_MS);
    for (i = 0; i < 400; i++) {
        int64_t next = db_brain_next_ms(&b);
        struct db_face f;

        if (next == DB_NEVER || next - t > DB_ACT_GAP_MAX_MS + DB_DOZE_MS) {
            break;
        }
        t = next;
        db_brain_tick(&b, t);
        db_brain_face(&b, &f);
        if (b.act == DB_ACT_BLINK) {
            blinks++;
            reopened &= f.expr == DB_EXPR_CLOSED;
        }
        glances += b.act == DB_ACT_GLANCE_LEFT || b.act == DB_ACT_GLANCE_RIGHT;
    }
    check("idle: it blinks", blinks > 10);
    check("and glances about", glances > 3);
    check("a blink shows closed eyes", reopened);
    check("and always wakes again within a gap", i == 400);

    db_brain_set_reduced_motion(&b, true, t);
    check("reduced motion: no blinks, no ticks", b.act == DB_ACT_NONE && db_brain_next_ms(&b) == DB_NEVER);

    db_prefs_defaults(&p);
    p.on[DB_PREF_IDLE_ANIMATION] = false;
    fresh(&b, &p, t = 0);
    see(&b, DB_VISION_PERSON_DETECTED, DB_CONF_NONE, t);
    run_until(&b, t += DB_WAKE_MS + DB_RECOGNIZE_MS);
    check("idle animation off, someone there: nothing to do until something happens",
          db_brain_next_ms(&b) == DB_NEVER);
}

/* ---- the face geometry ---------------------------------------------------------------------- */

static int fits(const struct db_eye_shape *e, int w, int h)
{
    return e->w > 0 && e->h > 0 && e->w <= w && e->h + 2 * (e->dy < 0 ? -e->dy : e->dy) <= h && e->lid < e->h &&
           (e->pupil_d == 0 || (e->pupil_d <= e->w && e->pupil_d <= e->h &&
                                2 * (e->pupil_dx < 0 ? -e->pupil_dx : e->pupil_dx) + e->pupil_d <= e->w));
}

static void test_face(void)
{
    struct db_eye_shape l;
    struct db_eye_shape r;
    struct db_eye_shape open;
    struct db_face f = { DB_EXPR_OPEN, 0, false };
    int all = 1;
    int x;
    int s;

    for (s = 16; s <= 400; s += 7) {
        for (x = 0; x < DB_EXPR_COUNT; x++) {
            int g;

            for (g = -1; g <= 1; g++) {
                f.expr = (enum db_expr)x;
                f.glance = g;
                db_face_eyes(&f, s, s + s / 3, &l, &r);
                all &= fits(&l, s, s + s / 3) && fits(&r, s, s + s / 3);
            }
        }
    }
    check("every expression and glance fits its box, 16 to 400 px", all);

    f.expr = DB_EXPR_OPEN;
    f.glance = 0;
    db_face_eyes(&f, 200, 200, &open, &r);
    check("open eyes are round and have pupils", open.pupil_d > 0 && open.h > open.w / 2);
    f.expr = DB_EXPR_CLOSED;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("a closed eye is a line", l.h * 4 < open.h && l.pupil_d == 0);
    f.expr = DB_EXPR_HAPPY;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("happy is ^ ^: an arch, no pupil", l.arch_d > l.w && r.arch_d > r.w && l.arch_y > 0 &&
                                                 l.arch_y < l.h && l.pupil_d == 0);
    f.expr = DB_EXPR_SUSPICIOUS;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("suspicious: narrowed under a lid", l.lid > 0 && r.lid > 0 && l.h - l.lid < open.h / 2);
    f.expr = DB_EXPR_SLEEPY;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("sleepy: heavier lids than wary", l.lid > 0 && l.h - l.lid < open.h / 3);
    f.expr = DB_EXPR_CURIOUS;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("curious: one eye bigger, looking up", l.h != r.h && l.pupil_dy < 0);
    f.expr = DB_EXPR_OPEN;
    f.glance = -1;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("a glance left moves both pupils left", l.pupil_dx < 0 && r.pupil_dx < 0);
    f.glance = 1;
    db_face_eyes(&f, 200, 200, &l, &r);
    check("a glance right moves both pupils right", l.pupil_dx > 0 && r.pupil_dx > 0);
    check("the same face gives the same shape", db_eye_shape_equal(&l, &l) && !db_eye_shape_equal(&l, &open));
}

int main(void)
{
    test_companion();
    test_owner();
    test_guard_unknown();
    test_guard_bounded();
    test_owner_returns();
    test_night();
    test_rapid();
    test_unavailable();
    test_prefs();
    test_idle();
    test_face();
    printf("db_brain_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
