/*
 * DeskBuddy's provider on the Vision pipeline (apps/deskbuddy_vision).
 *
 * The judge: a conclusion said once it holds, and only when it changes;
 * a flicker says nothing; "nobody" needs fresh empty reports over time; the
 * owner over a stranger when both are there; no identity without an owner.
 *
 * The provider, end to end against the real helper on the fake camera and
 * the fake face models (argv[1] is the helper): the owner recognised with
 * the similarity as confidence, somebody else unknown, people found by the
 * object detector when the unit has no face model, "nobody" when they
 * leave, UNAVAILABLE when there is no helper or it dies - once - and
 * stop() safe twice, with no helper left behind; a person told by the
 * class name; the helper started on the shipped detector (R0) rather than
 * the yolov8n.kmodel default no image carries; a turn followed while running.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "db_vision_pipeline.h"
#include "vision_session.h"
#define HELLO_S(x) #x
#define HELLO_X(x) HELLO_S(x)
#define HELLO_V HELLO_X(VISION_PROTO_VERSION)

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks;
static int failed;
static const char *helper;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

/* ---- the judge ------------------------------------------------------------------ */

static struct db_judge_report rep(int present, bool identity, bool owner, bool scored, int owner_pm, int other_pm)
{
    struct db_judge_report r;

    r.present = present;
    r.identity = identity;
    r.owner = owner;
    r.scored = scored;
    r.owner_pm = (int16_t)owner_pm;
    r.other_pm = (int16_t)other_pm;
    return r;
}

static void test_judge(void)
{
    struct db_judge j;
    struct db_vision_event ev;
    struct db_judge_report r;
    int said;

    db_judge_init(&j, true);
    r = rep(1, true, false, false, 0, 0);
    check("a face, once: nothing said yet", !db_judge_report(&j, &r, 1000, &ev));
    check("held: somebody", db_judge_report(&j, &r, 1100, &ev) && ev.kind == DB_VISION_PERSON_DETECTED && ev.face &&
                                ev.confidence_pm == DB_CONF_NONE && ev.mono_ms == 1100);
    check("said once, not again", !db_judge_report(&j, &r, 1200, &ev) && !db_judge_report(&j, &r, 1300, &ev));
    r = rep(1, true, true, true, 912, 0);
    db_judge_report(&j, &r, 1400, &ev);
    check("the owner, once held, with the similarity",
          db_judge_report(&j, &r, 1500, &ev) && ev.kind == DB_VISION_OWNER_RECOGNIZED && ev.confidence_pm == 912);
    r = rep(1, true, false, true, 0, 480);
    check("a stranger for one round says nothing", !db_judge_report(&j, &r, 1600, &ev));
    r = rep(1, true, true, true, 905, 0);
    check("and the owner again is no news", !db_judge_report(&j, &r, 1700, &ev));
    r = rep(2, true, true, true, 900, 520);
    check("the owner and a stranger: the owner", !db_judge_report(&j, &r, 1800, &ev));
    r = rep(1, true, false, true, 0, 480);
    db_judge_report(&j, &r, 1900, &ev);
    check("the owner gone, a stranger held: unknown, with the stranger's similarity",
          db_judge_report(&j, &r, 2000, &ev) && ev.kind == DB_VISION_UNKNOWN_PERSON && ev.confidence_pm == 480);
    r = rep(0, true, false, false, 0, 0);
    check("an empty frame is not nobody", !db_judge_report(&j, &r, 2100, &ev) && !db_judge_tick(&j, 2500, &ev));
    r = rep(1, true, false, true, 0, 470);
    check("back within the time: still unknown, nothing said", !db_judge_report(&j, &r, 2600, &ev));
    r = rep(0, true, false, false, 0, 0);
    db_judge_report(&j, &r, 2700, &ev);
    check("nobody for 1.5 s: nobody", !db_judge_report(&j, &r, 4000, &ev) && db_judge_report(&j, &r, 4100, &ev) &&
                                          ev.kind == DB_VISION_NO_PERSON && !ev.face);
    check("said once", !db_judge_report(&j, &r, 4200, &ev));
    r = rep(1, false, false, true, 0, 700);
    db_judge_report(&j, &r, 9100, &ev);
    check("no owner enrolled: a face is somebody, never unknown",
          db_judge_report(&j, &r, 9200, &ev) && ev.kind == DB_VISION_PERSON_DETECTED);
    for (said = 0; said < 1; said++) {
        struct db_vision_event e2;

        db_judge_init(&j, false);
        check("from the start, an empty room is said after the same wait",
              !db_judge_report(&j, &(struct db_judge_report){0}, 100, &e2) &&
                  !db_judge_report(&j, &(struct db_judge_report){0}, 1500, &e2) &&
                  db_judge_report(&j, &(struct db_judge_report){0}, 1600, &e2) &&
                  e2.kind == DB_VISION_NO_PERSON);
        check("every event it makes is one DeskBuddy takes", db_vision_event_valid(&e2) && db_vision_event_valid(&ev));
    }
    db_judge_init(&j, false);
    check("no first result: silence never means nobody", !db_judge_tick(&j, 100, &ev) &&
          !db_judge_tick(&j, 1700, &ev));
    check("no first result: unavailable at the existing Vision deadline",
          db_judge_tick(&j, 100 + VISION_SILENCE_MS, &ev) && ev.kind == DB_VISION_UNAVAILABLE);
    check("unavailable is emitted once", !db_judge_tick(&j, 9000, &ev));
    r = rep(1, false, false, false, 0, 0);
    check("recovery needs two reports", !db_judge_report(&j, &r, 9100, &ev) &&
          db_judge_report(&j, &r, 9200, &ev) && ev.kind == DB_VISION_PERSON_DETECTED);
    check("silence after a person is not departure", !db_judge_tick(&j, 10800, &ev));
    check("stale person becomes unavailable", db_judge_tick(&j, 13200, &ev) &&
          ev.kind == DB_VISION_UNAVAILABLE);
    r = rep(0, false, false, false, 0, 0);
    check("first empty report after outage does not prove sustained absence",
          !db_judge_report(&j, &r, 13300, &ev));
    check("new empty reports establish absence", db_judge_report(&j, &r, 14800, &ev) &&
          ev.kind == DB_VISION_NO_PERSON);
    db_judge_init(&j, false);
    r = rep(1, false, false, false, 0, 0);
    db_judge_report(&j, &r, 100, &ev);
    check("widely separated reports cannot confirm presence",
          !db_judge_report(&j, &r, 5100, &ev));

}

/* ---- the provider --------------------------------------------------------------- */

static struct db_vision_queue queue;
static int kinds[DB_VISION_KIND_COUNT];
static struct db_vision_event last[DB_VISION_KIND_COUNT];

/* Poll as DeskBuddy's timer does until an event of kind arrives. */
static int run_until(enum db_vision_kind kind, int ms)
{
    int64_t end = now_ms() + ms;
    struct db_vision_event ev;

    while (now_ms() < end) {
        int64_t next = db_vision_pipeline_ops.poll(NULL, now_ms(), &queue);

        while (db_vision_queue_pop(&queue, &ev)) {
            kinds[ev.kind]++;
            last[ev.kind] = ev;
            if (ev.kind == kind) {
                return 1;
            }
        }
        if (next < 0) {
            return 0;
        }
        nap(20);
    }
    return 0;
}

/* Helpers of this test still running: processes whose argv is
 * "<helper> session ...", zombies included (a helper nobody reaped). */
static int helpers_alive(pid_t *first)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        char path[300];
        char buf[4096];
        size_t got;
        FILE *f;

        if (e->d_name[0] < '1' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[got] = '\0';
        if (strcmp(buf, helper) == 0 && strlen(buf) + 8 < got && strcmp(buf + strlen(buf) + 1, "session") == 0) {
            if (first && n == 0) {
                *first = (pid_t)atoi(e->d_name);
                /* /proc can be mounted from an outer PID namespace on a
                 * cloud host. kill() needs our namespace's child PID. */
                snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
                f = fopen(path, "r");
                while (f && fgets(buf, sizeof(buf), f)) {
                    if (strncmp(buf, "NSpid:", 6) == 0) {
                        char *p = buf + 6;
                        char *end;
                        long id;

                        while ((id = strtol(p, &end, 10)) > 0 && end != p) {
                            *first = (pid_t)id;
                            p = end;
                        }
                        break;
                    }
                }
                if (f) {
                    fclose(f);
                }
            }
            n++;
        }
    }
    closedir(d);
    return n;
}

static void reset_counts(void)
{
    memset(kinds, 0, sizeof(kinds));
    memset(last, 0, sizeof(last));
    db_vision_queue_init(&queue);
}

static void write_owner(const char *path)
{
    /* The fake embedding's person 1 (vision_kpu_fake.c): 0.3 and 1 at 9. */
    FILE *f = fopen(path, "w");
    int i;
    double n = 1.0440306508910550;

    if (!f) {
        return;
    }
    fprintf(f, "doors-vision-owner 1\nmodel face_embed.kmodel 4\ndim 512\nsamples 5\nv");
    for (i = 0; i < 512; i++) {
        fprintf(f, " %.7g", i == 0 ? 0.3 / n : i == 9 ? 1.0 / n : 0.0);
    }
    fprintf(f, "\n");
    fclose(f);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* ---- the classes and the detector ------------------------------------------------- *
 *
 * A person is the class named "person" (COCO row 0 in R0's and UPSTREAM's
 * export), never another row; and the helper is started on the detector
 * the image ships, as Vision would pick it, not on the old yolov8n.kmodel
 * default that no image carries. */

static void test_person_class(void)
{
    check("class 0 is a person (by its name)", db_vision_pipeline_is_person(0));
    check("R0's other classes are not: bicycle, car, motorcycle, bus, truck",
          !db_vision_pipeline_is_person(1) && !db_vision_pipeline_is_person(2) && !db_vision_pipeline_is_person(3) &&
              !db_vision_pipeline_is_person(5) && !db_vision_pipeline_is_person(7));
    check("nor a class the list does not have", !db_vision_pipeline_is_person(80) &&
                                                     !db_vision_pipeline_is_person(65535));
}

static void test_shipped_detector(const char *dir, const struct db_vision_pipeline_cfg *cfg, const char *vdir)
{
    char r0[160];
    char up[160];
    char set[192];

    snprintf(r0, sizeof(r0), "%s/det-r0-traffic6-yolox-tiny-416.kmodel", dir);
    snprintf(up, sizeof(up), "%s/det-upstream-yolox-tiny-416.kmodel", dir);
    snprintf(set, sizeof(set), "%s/settings.v1", vdir);
    setenv("POCKETOS_VISION_MODEL_R0", r0, 1);
    setenv("POCKETOS_VISION_MODEL_UPSTREAM", up, 1);
    check("no detector file: none is named", db_vision_pipeline_detector() == -1);
    write_file(r0, "fake");
    check("R0 on the unit, Vision never opened: R0", db_vision_pipeline_detector() == 0);
    write_file(up, "fake");
    write_file(set, "detector=upstream\n");
    check("both, Vision last confirmed UPSTREAM: UPSTREAM, as Vision would", db_vision_pipeline_detector() == 1);
    unlink(up);
    check("UPSTREAM gone: back on R0", db_vision_pipeline_detector() == 0);
    unlink(set);

    /* The image's case: the helper's own default is not there, R0 is. */
    setenv("POCKETOS_VISION_MODEL", "/nonexistent/vision/yolov8n.kmodel", 1);
    setenv("POCKETOS_VISION_FACE_DET", "/nonexistent/face_det.kmodel", 1);
    reset_counts();
    setenv("POCKETOS_VISION_KPU_SCRIPT", "box=0:900:200:60:120:260,until=40", 1);
    db_vision_pipeline_ops.start((void *)cfg, now_ms(), &queue);
    check("no yolov8n.kmodel, R0 shipped: a person from R0", run_until(DB_VISION_PERSON_DETECTED, 8000) &&
                                                                 kinds[DB_VISION_UNAVAILABLE] == 0 &&
                                                                 !last[DB_VISION_PERSON_DETECTED].face);
    check("and nobody once they leave, not unavailable", run_until(DB_VISION_NO_PERSON, 8000) &&
                                                             kinds[DB_VISION_UNAVAILABLE] == 0);
    db_vision_pipeline_ops.stop(NULL);

    /* Only a car in view (R0's row 2): the desk is empty, and says so. */
    reset_counts();
    setenv("POCKETOS_VISION_KPU_SCRIPT", "box=2:900:200:60:120:260", 1);
    db_vision_pipeline_ops.start((void *)cfg, now_ms(), &queue);
    check("a car is nobody", run_until(DB_VISION_NO_PERSON, 8000) && kinds[DB_VISION_PERSON_DETECTED] == 0 &&
                                 kinds[DB_VISION_UNAVAILABLE] == 0);
    db_vision_pipeline_ops.stop(NULL);

    /* The unit turned while it runs (an external display: no restart): the
     * provider tells the helper the new hold and goes on seeing. */
    {
        struct db_vision_pipeline_cfg turning = *cfg;

        reset_counts();
        setenv("POCKETOS_VISION_KPU_SCRIPT", "box=0:900:200:60:120:260", 1);
        db_vision_pipeline_ops.start(&turning, now_ms(), &queue);
        run_until(DB_VISION_PERSON_DETECTED, 8000);
        turning.display_rotation = turning.display_rotation == 0 ? 90 : 0;
        run_until(DB_VISION_KIND_COUNT, DB_JUDGE_ABSENT_MS + 1000);
        check("turned while running: still somebody, never nobody or unavailable",
              kinds[DB_VISION_PERSON_DETECTED] == 1 && kinds[DB_VISION_NO_PERSON] == 0 &&
                  kinds[DB_VISION_UNAVAILABLE] == 0 && helpers_alive(NULL) == 1);
        db_vision_pipeline_ops.stop(NULL);
    }
    check("no helper left behind", helpers_alive(NULL) == 0);
    unlink(r0);
    unsetenv("POCKETOS_VISION_MODEL_R0");
    unsetenv("POCKETOS_VISION_MODEL_UPSTREAM");
}

static void test_provider(void)
{
    struct db_vision_pipeline_cfg cfg = { 90 };
    char dir[] = "/tmp/db-pipeline-XXXXXX";
    char det[128];
    char emb[128];
    char state[128];
    char vdir[160];
    char owner[192];
    FILE *f;

    if (!mkdtemp(dir)) {
        check("a scratch directory", 0);
        return;
    }
    snprintf(det, sizeof(det), "%s/face_det.kmodel", dir);
    snprintf(emb, sizeof(emb), "%s/face_embed.kmodel", dir);
    snprintf(state, sizeof(state), "%s/state", dir);
    snprintf(vdir, sizeof(vdir), "%s/vision", state);
    snprintf(owner, sizeof(owner), "%s/owner.v1", vdir);
    mkdir(state, 0700);
    mkdir(vdir, 0700);
    write_owner(owner);
    f = fopen(det, "w");
    if (f) {
        fclose(f);
    }
    f = fopen(emb, "w");
    if (f) {
        fputs("fake", f);
        fclose(f);
    }
    setenv("POCKETOS_VISION_HELPER", helper, 1);
    setenv("POCKETOS_CAMERA_BACKEND", "fake", 1);
    setenv("POCKETOS_CAMERA_FAKE", "period=20", 1);
    setenv("POCKETOS_STATE_DIR", state, 1);
    setenv("POCKETOS_VISION_FACE_DET", det, 1);
    setenv("POCKETOS_VISION_FACE_EMBED", emb, 1);

    /* The owner at the desk. */
    reset_counts();
    setenv("POCKETOS_VISION_KPU_SCRIPT", "face=200:100:90:110:1", 1);
    check("start: the helper runs", db_vision_pipeline_ops.start(&cfg, now_ms(), &queue) == 0);
    check("the owner is recognised", run_until(DB_VISION_OWNER_RECOGNIZED, 8000));
    check("with the similarity as confidence, from a face",
          last[DB_VISION_OWNER_RECOGNIZED].confidence_pm >= 990 && last[DB_VISION_OWNER_RECOGNIZED].face);
    check("and nothing claims the camera failed", kinds[DB_VISION_UNAVAILABLE] == 0);
    db_vision_pipeline_ops.stop(NULL);
    db_vision_pipeline_ops.stop(NULL);
    check("stop() twice is safe; no helper left", helpers_alive(NULL) == 0);
    check("a poll after stop() does nothing", db_vision_pipeline_ops.poll(NULL, now_ms(), &queue) == -1);

    /* Somebody else. */
    reset_counts();
    setenv("POCKETOS_VISION_KPU_SCRIPT", "face=200:100:90:110:2", 1);
    db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
    check("somebody else is unknown", run_until(DB_VISION_UNKNOWN_PERSON, 8000) &&
                                          last[DB_VISION_UNKNOWN_PERSON].confidence_pm < 750 &&
                                          kinds[DB_VISION_OWNER_RECOGNIZED] == 0);
    db_vision_pipeline_ops.stop(NULL);

    /* A unit without the face models: people from the detector, and
     * nobody once they leave (the fake detector's boxes end at frame 40). */
    reset_counts();
    unsetenv("POCKETOS_VISION_FACE_DET");
    unsetenv("POCKETOS_VISION_FACE_EMBED");
    setenv("POCKETOS_VISION_FACE_DET", "/nonexistent/face_det.kmodel", 1);
    setenv("POCKETOS_VISION_KPU_SCRIPT", "box=0:900:200:60:120:260,until=40", 1);
    db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
    check("no face model: a person from the detector", run_until(DB_VISION_PERSON_DETECTED, 8000) &&
                                                           !last[DB_VISION_PERSON_DETECTED].face);
    check("and nobody once the person has gone", run_until(DB_VISION_NO_PERSON, 8000));
    db_vision_pipeline_ops.stop(NULL);

    /* A unit without the detector model either (an image that does not ship
     * it): nothing can see a person, so DeskBuddy says it cannot see, once,
     * and leaves the helper - never a desk that looks empty. */
    reset_counts();
    setenv("POCKETOS_VISION_MODEL", "/nonexistent/vision/yolov8n.kmodel", 1);
    db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
    check("no detector and no face model: unavailable", run_until(DB_VISION_UNAVAILABLE, 8000) &&
                                                            kinds[DB_VISION_UNAVAILABLE] == 1 &&
                                                            kinds[DB_VISION_NO_PERSON] == 0 &&
                                                            kinds[DB_VISION_PERSON_DETECTED] == 0);
    check("and the provider stops polling", db_vision_pipeline_ops.poll(NULL, now_ms(), &queue) == -1);
    db_vision_pipeline_ops.stop(NULL);
    check("no helper left behind", helpers_alive(NULL) == 0);

    test_shipped_detector(dir, &cfg, vdir);
    unsetenv("POCKETOS_VISION_MODEL");

    /* Wherever the person sits in the camera's frame, DeskBuddy sees them:
     * at the left edge, in the middle, at the right edge of the 640 x 360
     * sensor, with the camera mounted turned (90, the picture tall) and not
     * (0, wide). A square view cut to fill would leave both sides of the
     * frame out; "nobody" must never be said while somebody is there. */
    {
        static const struct {
            const char *where;
            int x;
        } at[] = { { "at the left edge", 4 }, { "in the middle", 280 }, { "at the right edge", 556 } };
        static const int mounts[] = { 90, 0 };
        size_t m;
        size_t k;

        for (m = 0; m < sizeof(mounts) / sizeof(mounts[0]); m++) {
            for (k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
                char cam[64];
                char kpu[64];
                char name[128];
                int found;

                reset_counts();
                snprintf(cam, sizeof(cam), "period=20,mount=%d", mounts[m]);
                snprintf(kpu, sizeof(kpu), "box=0:900:%d:80:80:200", at[k].x);
                setenv("POCKETOS_CAMERA_FAKE", cam, 1);
                setenv("POCKETOS_VISION_KPU_SCRIPT", kpu, 1);
                db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
                found = run_until(DB_VISION_PERSON_DETECTED, 5000);
                /* Well past DB_JUDGE_ABSENT_MS with the person still there. */
                run_until(DB_VISION_KIND_COUNT, DB_JUDGE_ABSENT_MS + 1000);
                snprintf(name, sizeof(name), "mount %d: a person %s is seen, and nobody is never said", mounts[m],
                         at[k].where);
                check(name, found && kinds[DB_VISION_NO_PERSON] == 0 && kinds[DB_VISION_UNAVAILABLE] == 0);
                db_vision_pipeline_ops.stop(NULL);
            }
        }
        setenv("POCKETOS_CAMERA_FAKE", "period=20", 1);
    }

    /* Camera stops producing frames but helper keeps emitting stall/stats.
     * These keep the session alive; they are not empty detections. */
    reset_counts();
    setenv("POCKETOS_CAMERA_FAKE", "period=20,frames=20", 1);
    setenv("POCKETOS_VISION_KPU_SCRIPT", "box=0:900:200:60:120:260", 1);
    db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
    check("before stall: person", run_until(DB_VISION_PERSON_DETECTED, 8000));
    check("camera stalls: unavailable, never nobody", run_until(DB_VISION_UNAVAILABLE, 6000) &&
          kinds[DB_VISION_NO_PERSON] == 0);
    run_until(DB_VISION_KIND_COUNT, 2000);
    check("continued stalls: unavailable once, never nobody", kinds[DB_VISION_UNAVAILABLE] == 1 &&
          kinds[DB_VISION_NO_PERSON] == 0);
    db_vision_pipeline_ops.stop(NULL);
    db_vision_pipeline_ops.stop(NULL);
    check("stalled helper cleaned up", helpers_alive(NULL) == 0);

    reset_counts();
    setenv("POCKETOS_CAMERA_FAKE", "period=20,delay_at=20:2500", 1);
    db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
    check("before temporary stall: person", run_until(DB_VISION_PERSON_DETECTED, 8000));
    check("temporary stall: unavailable", run_until(DB_VISION_UNAVAILABLE, 6000));
    check("fresh reports recover presence", run_until(DB_VISION_PERSON_DETECTED, 6000) &&
          kinds[DB_VISION_NO_PERSON] == 0);
    db_vision_pipeline_ops.stop(NULL);
    setenv("POCKETOS_CAMERA_FAKE", "period=20", 1);

    /* The helper dies under it. */
    reset_counts();
    setenv("POCKETOS_VISION_KPU_SCRIPT", "box=0:900:200:60:120:260", 1);
    db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
    check("person before helper death", run_until(DB_VISION_PERSON_DETECTED, 8000));
    {
        pid_t pid = 0;

        int alive = helpers_alive(&pid);
        if (alive != 1) {
            fprintf(stderr, "helper count before kill: %d, path %s\n", alive, helper);
        }
        int killed = alive == 1 && pid > 0 ? kill(pid, SIGKILL) : -1;
        if (killed != 0) {
            fprintf(stderr, "kill helper pid %ld: %s\n", (long)pid, strerror(errno));
        }
        check("the helper runs", alive == 1 && pid > 0 && killed == 0);
    }
    check("the helper killed: unavailable", run_until(DB_VISION_UNAVAILABLE, 5000));
    run_until(DB_VISION_KIND_COUNT, 500);
    check("said once", kinds[DB_VISION_UNAVAILABLE] == 1);
    db_vision_pipeline_ops.stop(NULL);

    /* No helper at all. */
    reset_counts();
    setenv("POCKETOS_VISION_HELPER", "/nonexistent/pos-vision", 1);
    {
        int r = db_vision_pipeline_ops.start(&cfg, now_ms(), &queue);
        struct db_vision_event ev;

        if (r == 0) {
            run_until(DB_VISION_UNAVAILABLE, 5000);
        } else if (db_vision_queue_pop(&queue, &ev)) {
            kinds[ev.kind]++;
        }
        check("no helper: unavailable, once", kinds[DB_VISION_UNAVAILABLE] == 1);
    }
    db_vision_pipeline_ops.stop(NULL);
    db_vision_pipeline_ops.stop(NULL);
    check("nothing left running", helpers_alive(NULL) == 0);

    unlink(owner);
    rmdir(vdir);
    rmdir(state);
    unlink(det);
    unlink(emb);
    rmdir(dir);
}

/* A protocol fixture only: batches an old empty result and one new positive
 * snapshot, repeats that snapshot, then supplies independent evidence. */
static int batch_helper(void)
{
    char line[256];
    bool face = getenv("DB_TEST_BATCH_FACE") != NULL;
    const char *hello = face ? "hello " HELLO_V " fake fake\ncaps recognize\nready fake 640 360 1 none 0 0 0\nowner 1 5\n"
                             : "hello " HELLO_V " fake fake\ncaps detect\nready fake 640 360 1 none 0 0 0\n";

    if (write(1, hello, strlen(hello)) != (ssize_t)strlen(hello)) {
        return 1;
    }
    while (fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, "start", 5) == 0) {
            const char *batch = face ? "who 1 0 0\nwho 2 1 1 1:950:1\n"
                                     : "det 1 0\ndet 2 1 1:0:900:10:10:20:20:0:0\n";
            const char *repeat = face ? "who 2 1 1 1:950:1\n" : "det 2 1 1:0:900:10:10:20:20:0:0\n";
            const char *fresh = face ? "who 3 1 1 1:950:1\n" : "det 3 1 1:0:900:10:10:20:20:0:0\n";

            if (write(1, batch, strlen(batch)) != (ssize_t)strlen(batch)) {
                return 1;
            }
            nap(150);
            if (write(1, repeat, strlen(repeat)) != (ssize_t)strlen(repeat)) {
                return 1;
            }
            nap(600);
            if (write(1, fresh, strlen(fresh)) != (ssize_t)strlen(fresh)) {
                return 1;
            }
        } else if (strncmp(line, "quit", 4) == 0) {
            if (write(1, "bye\n", 4) != 4) {
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

static void test_batched_reports(void)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    const char *real_helper = helper;
    int face;

    if (n < 0) {
        check("find protocol fixture", 0);
        return;
    }
    self[n] = '\0';
    helper = self;
    setenv("POCKETOS_VISION_HELPER", self, 1);
    for (face = 0; face < 2; face++) {
        enum db_vision_kind kind = face ? DB_VISION_OWNER_RECOGNIZED : DB_VISION_PERSON_DETECTED;

        if (face) {
            setenv("DB_TEST_BATCH_FACE", "1", 1);
        }
        reset_counts();
        db_vision_pipeline_ops.start(NULL, now_ms(), &queue);
        check("queued events and duplicate seq are not independent evidence", !run_until(kind, 500));
        check("a second distinct snapshot confirms", run_until(kind, 2000));
        db_vision_pipeline_ops.stop(NULL);
        check("protocol fixture reaped", helpers_alive(NULL) == 0);
    }
    unsetenv("DB_TEST_BATCH_FACE");
    helper = real_helper;
    setenv("POCKETOS_VISION_HELPER", helper, 1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "session") == 0) {
        return batch_helper();
    }
    helper = argc > 1 ? argv[1] : "tools/vision/pos-vision";
    if (helper[0] != '/') {
        static char abs_path[4096];

        if (getcwd(abs_path, sizeof(abs_path) - strlen(helper) - 2)) {
            strcat(abs_path, "/");
            strcat(abs_path, helper);
            helper = abs_path;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    test_judge();
    test_person_class();
    test_provider();
    test_batched_reports();
    printf("db_pipeline_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
