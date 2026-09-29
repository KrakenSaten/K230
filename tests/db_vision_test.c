/*
 * DeskBuddy's vision boundary (apps/deskbuddy/db_vision.h) and the scripted
 * provider that stands in for the real pipeline (db_vision_mock.h): the
 * bounded queue, what counts as a malformed event or script, and a script
 * played against a clock.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "db_vision.h"
#include "db_vision_mock.h"

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

static void test_events(void)
{
    struct db_vision_event ev = { .kind = DB_VISION_OWNER_RECOGNIZED, .confidence_pm = 1000 };
    enum db_vision_kind k;
    int all = 1;
    int i;

    check("an owner event at full confidence is valid", db_vision_event_valid(&ev));
    ev.confidence_pm = DB_CONF_NONE;
    check("no confidence is valid", db_vision_event_valid(&ev));
    ev.confidence_pm = 1001;
    check("confidence over 1000 is not", !db_vision_event_valid(&ev));
    ev.confidence_pm = -2;
    check("negative confidence is not", !db_vision_event_valid(&ev));
    ev.confidence_pm = 0;
    ev.kind = DB_VISION_KIND_COUNT;
    check("a kind out of range is not", !db_vision_event_valid(&ev) && !db_vision_event_valid(NULL));
    for (i = 0; i < DB_VISION_KIND_COUNT; i++) {
        all &= db_vision_kind_parse(db_vision_kind_name((enum db_vision_kind)i), &k) == 0 && (int)k == i;
    }
    check("every kind's name reads back as that kind", all);
    check("an unknown name is refused", db_vision_kind_parse("owner ", &k) != 0 && db_vision_kind_parse(NULL, &k) != 0);
}

static void test_queue(void)
{
    struct db_vision_queue q;
    struct db_vision_event ev = { .kind = DB_VISION_PERSON_DETECTED, .confidence_pm = DB_CONF_NONE };
    struct db_vision_event out;
    int order = 1;
    int i;

    db_vision_queue_init(&q);
    check("an empty queue pops nothing", !db_vision_queue_pop(&q, &out));
    for (i = 0; i < DB_VISION_QUEUE_CAP + 10; i++) {
        ev.mono_ms = i;
        db_vision_queue_push(&q, &ev);
    }
    check("the queue never holds more than its size", q.count == DB_VISION_QUEUE_CAP);
    check("the ten oldest were dropped, and counted", q.dropped == 10);
    for (i = 10; i < DB_VISION_QUEUE_CAP + 10; i++) {
        order &= db_vision_queue_pop(&q, &out) && out.mono_ms == i;
    }
    check("the newest are kept, oldest first", order && q.count == 0);
    ev.kind = (enum db_vision_kind)-1;
    check("an invalid event is refused and counted", !db_vision_queue_push(&q, &ev) && q.rejected == 1 && q.count == 0);
}

static void test_none(void)
{
    struct db_vision_queue q;
    struct db_vision_event out;

    db_vision_queue_init(&q);
    check("the none provider does not start", db_vision_none_ops.start(NULL, 5, &q) != 0);
    check("but says so: one VISION_UNAVAILABLE", db_vision_queue_pop(&q, &out) && out.kind == DB_VISION_UNAVAILABLE &&
                                                     !db_vision_queue_pop(&q, &out));
    check("and never asks to be polled", db_vision_none_ops.poll(NULL, 10, &q) == -1 && q.count == 0);
    db_vision_none_ops.stop(NULL);
    db_vision_none_ops.stop(NULL);
}

static void test_script(void)
{
    struct db_vision_mock m;
    struct db_vision_queue q;
    struct db_vision_event out;
    int64_t next;
    int i;

    check("a script loads", db_vision_mock_load(&m, "500:person 1500:owner@930,6000:none") == 3);
    check("with its confidence", m.step[1].confidence_pm == 930 && m.step[0].confidence_pm == DB_CONF_NONE);
    check("an empty script is refused", db_vision_mock_load(&m, "  , ") == -1 && m.n == 0);
    check("NULL is refused", db_vision_mock_load(&m, NULL) == -1);
    check("a bad kind is refused", db_vision_mock_load(&m, "10:owner 20:ghost") == -1 && m.n == 0);
    check("a bad time is refused", db_vision_mock_load(&m, "x:owner") == -1 && db_vision_mock_load(&m, ":owner") == -1 &&
                                       db_vision_mock_load(&m, "-5:owner") == -1);
    check("a bad confidence is refused", db_vision_mock_load(&m, "10:owner@1001") == -1 &&
                                             db_vision_mock_load(&m, "10:owner@") == -1 &&
                                             db_vision_mock_load(&m, "10:owner@9x") == -1);
    check("time going backwards is refused", db_vision_mock_load(&m, "100:person 50:none") == -1);
    check("a step after loop is refused", db_vision_mock_load(&m, "100:person loop 200:none") == -1);
    check("an over-long token is refused",
          db_vision_mock_load(&m, "10:ownerrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr") == -1);
    {
        char big[DB_MOCK_MAX_STEPS * 12 + 32];
        size_t n = 0;

        for (i = 0; i <= DB_MOCK_MAX_STEPS; i++) {
            n += (size_t)snprintf(big + n, sizeof(big) - n, "%d:none ", i);
        }
        check("more steps than fit is refused, not truncated", db_vision_mock_load(&m, big) == -1);
    }

    db_vision_queue_init(&q);
    db_vision_mock_load(&m, "500:person 1500:owner@930 6000:none");
    check("it starts", db_vision_mock_ops.start(&m, 1000, &q) == 0);
    next = db_vision_mock_ops.poll(&m, 1000, &q);
    check("nothing before the first step, and it asks for its time", q.count == 0 && next == 1500);
    next = db_vision_mock_ops.poll(&m, 2600, &q);
    check("both due steps at once, in order",
          db_vision_queue_pop(&q, &out) && out.kind == DB_VISION_PERSON_DETECTED && out.mono_ms == 1500 &&
              db_vision_queue_pop(&q, &out) && out.kind == DB_VISION_OWNER_RECOGNIZED && out.face &&
              out.confidence_pm == 930 && next == 7000);
    next = db_vision_mock_ops.poll(&m, 7000, &q);
    check("the last step, then nothing more", db_vision_queue_pop(&q, &out) && out.kind == DB_VISION_NO_PERSON &&
                                                  next == -1);
    db_vision_mock_ops.stop(&m);
    check("stopped, it is silent", db_vision_mock_ops.poll(&m, 100000, &q) == -1 && q.count == 0);

    db_vision_mock_load(&m, "0:person 100:none loop");
    db_vision_mock_ops.start(&m, 0, &q);
    for (i = 0; i <= 5000; i += 50) {
        db_vision_mock_ops.poll(&m, i, &q);
    }
    check("a looping script keeps going", q.dropped > 0 || q.count >= 8);
    db_vision_queue_init(&q);
    db_vision_mock_ops.poll(&m, 1000000000, &q);
    check("a clock that jumps far ahead is bounded to a queue's worth per poll", q.count <= DB_VISION_QUEUE_CAP);

    db_vision_queue_init(&q);
    db_vision_mock_inject(&m, DB_VISION_UNKNOWN_PERSON, 42, &q);
    check("an injected event arrives at once", db_vision_queue_pop(&q, &out) && out.kind == DB_VISION_UNKNOWN_PERSON &&
                                                   out.mono_ms == 42);
}

int main(void)
{
    test_events();
    test_queue();
    test_none();
    test_script();
    printf("db_vision_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
