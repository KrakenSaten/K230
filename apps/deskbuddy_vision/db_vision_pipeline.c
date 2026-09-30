/*
 * DeskBuddy's provider on the Vision pipeline. See db_vision_pipeline.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "db_vision_pipeline.h"

#include "vision_session.h"
#include "vision_settings.h"

#include <stdlib.h>
#include <string.h>

/* ---- the judge ---------------------------------------------------------------- */

void db_judge_init(struct db_judge *j, bool face)
{
    memset(j, 0, sizeof(*j));
    j->face = face;
    j->last_seen_ms = -1;
}

static void fill(const struct db_judge *j, enum db_vision_kind kind, int16_t conf, int64_t now,
                 struct db_vision_event *out)
{
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->face = j->face && kind != DB_VISION_NO_PERSON && kind != DB_VISION_UNAVAILABLE;
    out->confidence_pm = conf;
    out->mono_ms = now;
}

bool db_judge_report(struct db_judge *j, const struct db_judge_report *r, int64_t now_ms,
                     struct db_vision_event *out)
{
    enum db_vision_kind c;
    int16_t conf = DB_CONF_NONE;

    if (r->present <= 0) {
        /* Nobody on this report: "nobody" is a matter of time (tick). */
        j->cand_n = 0;
        return db_judge_tick(j, now_ms, out);
    }
    j->last_seen_ms = now_ms;
    if (r->identity && r->owner) {
        c = DB_VISION_OWNER_RECOGNIZED;
        conf = r->owner_pm;
    } else if (r->identity && r->scored) {
        c = DB_VISION_UNKNOWN_PERSON;
        conf = r->other_pm;
    } else {
        c = DB_VISION_PERSON_DETECTED;
    }
    if (j->said_any && c == j->said) {
        j->cand_n = 0;
        return false;
    }
    if (c == j->cand && j->cand_n > 0) {
        j->cand_n++;
    } else {
        j->cand = c;
        j->cand_n = 1;
    }
    if (j->cand_n < DB_JUDGE_HOLD) {
        return false;
    }
    j->said_any = true;
    j->said = c;
    j->cand_n = 0;
    fill(j, c, conf, now_ms, out);
    return true;
}

bool db_judge_tick(struct db_judge *j, int64_t now_ms, struct db_vision_event *out)
{
    bool nobody_due = j->last_seen_ms < 0 || now_ms - j->last_seen_ms >= DB_JUDGE_ABSENT_MS;

    if (!nobody_due || (j->said_any && j->said == DB_VISION_NO_PERSON)) {
        return false;
    }
    /* Before anybody was ever seen, "nobody" waits the same time from the
     * start, so a slow first frame is not mistaken for an empty room. */
    if (j->last_seen_ms < 0) {
        j->last_seen_ms = now_ms;
        return false;
    }
    j->said_any = true;
    j->said = DB_VISION_NO_PERSON;
    fill(j, DB_VISION_NO_PERSON, DB_CONF_NONE, now_ms, out);
    return true;
}

/* ---- the provider --------------------------------------------------------------- */

#define PIPELINE_POLL_MS 100
#define PIPELINE_GRACE_MS 1000
/* The picture nobody looks at: as small as the helper takes, so its
 * preview costs next to nothing. */
#define PIPELINE_VIEW_W 64
#define PIPELINE_VIEW_H 64

enum pipeline_mode { PIPE_NONE, PIPE_DETECT, PIPE_FACE, PIPE_RECOGNIZE };

struct pipeline {
    struct vision_session s;
    struct db_judge judge;
    enum pipeline_mode mode;
    bool running;
    bool unavailable_said;
    bool have_owner;
    int rotation;
    uint16_t px[PIPELINE_VIEW_W * PIPELINE_VIEW_H];
};

static struct pipeline pipe_state;

static void unavailable(struct pipeline *p, int64_t now, struct db_vision_queue *q)
{
    struct db_vision_event ev;

    if (p->unavailable_said) {
        return;
    }
    p->unavailable_said = true;
    memset(&ev, 0, sizeof(ev));
    ev.kind = DB_VISION_UNAVAILABLE;
    ev.confidence_pm = DB_CONF_NONE;
    ev.mono_ms = now;
    db_vision_queue_push(q, &ev);
    /* After it, whatever is seen again is news. */
    db_judge_init(&p->judge, p->mode >= PIPE_FACE);
}

static int pipeline_start(void *ctx, int64_t now_ms, struct db_vision_queue *q)
{
    struct pipeline *p = &pipe_state;
    const struct db_vision_pipeline_cfg *cfg = ctx;
    struct vision_session_config vc = { 0 };
    char err[96];

    if (p->running) {
        vision_session_abandon(&p->s, PIPELINE_GRACE_MS);
    }
    memset(p, 0, sizeof(*p));
    vision_session_init(&p->s);
    p->rotation = cfg ? cfg->display_rotation : 0;
    db_judge_init(&p->judge, false);
    if (strcmp(vision_session_backend(), "fake") == 0) {
        vc.fake = getenv("POCKETOS_CAMERA_FAKE");
        vc.kpu = getenv("POCKETOS_VISION_KPU_SCRIPT");
    }
    if (vision_session_start(&p->s, &vc, now_ms, err, sizeof(err)) != 0) {
        unavailable(p, now_ms, q);
        return -1;
    }
    p->running = true;
    return 0;
}

static void choose_mode(struct pipeline *p, uint32_t caps, int64_t now)
{
    const char *word = "detect";

    if (caps & (1u << VISION_MODE_RECOGNIZE)) {
        p->mode = PIPE_RECOGNIZE;
        word = "recognize";
    } else if (caps & (1u << VISION_MODE_FACE)) {
        p->mode = PIPE_FACE;
        word = "face";
    } else {
        p->mode = PIPE_DETECT;
    }
    db_judge_init(&p->judge, p->mode >= PIPE_FACE);
    vision_session_mode_word(&p->s, word);
    vision_session_view(&p->s, PIPELINE_VIEW_W, PIPELINE_VIEW_H, p->rotation);
    vision_session_stream(&p->s, true, now);
}

static int64_t pipeline_poll(void *ctx, int64_t now_ms, struct db_vision_queue *q)
{
    struct pipeline *p = &pipe_state;
    struct vision_event ev;
    struct db_vision_event out;

    (void)ctx;
    if (!p->running) {
        return -1;
    }
    while (vision_session_poll(&p->s, &ev, now_ms)) {
        struct db_judge_report r;

        memset(&r, 0, sizeof(r));
        switch (ev.kind) {
        case VISION_EV_CAPS:
            choose_mode(p, (uint32_t)ev.value, now_ms);
            break;
        case VISION_EV_FRAME:
            /* Taken so the helper's slot is free again; never shown. */
            vision_session_take_frame(&p->s, p->px, PIPELINE_VIEW_W, PIPELINE_VIEW_H);
            break;
        case VISION_EV_OWNER:
            p->have_owner = ev.value == 1;
            break;
        case VISION_EV_DET:
            if (p->mode == PIPE_DETECT || p->mode == PIPE_FACE) {
                int n = 0;
                int i;
                const struct vision_shown *t = vision_session_tracks(&p->s, &n, NULL);

                for (i = 0; i < n; i++) {
                    /* A face is a person; so is COCO's class 0. */
                    r.present += p->mode == PIPE_FACE || t[i].cls == 0;
                }
                if (db_judge_report(&p->judge, &r, now_ms, &out)) {
                    db_vision_queue_push(q, &out);
                }
            }
            break;
        case VISION_EV_WHO:
            if (p->mode == PIPE_RECOGNIZE) {
                const struct vision_who_report *w = vision_session_who(&p->s);
                int i;

                r.present = w->faces;
                r.identity = p->have_owner;
                for (i = 0; i < w->n; i++) {
                    r.scored = true;
                    if (w->t[i].owner) {
                        r.owner = true;
                        r.owner_pm = (int16_t)(w->t[i].score > r.owner_pm ? w->t[i].score : r.owner_pm);
                    } else if (w->t[i].score > r.other_pm) {
                        r.other_pm = (int16_t)w->t[i].score;
                    }
                }
                if (db_judge_report(&p->judge, &r, now_ms, &out)) {
                    db_vision_queue_push(q, &out);
                }
            }
            break;
        case VISION_EV_RECOGFAIL:
            /* The embedding model cannot run: faces without names. */
            if (p->mode == PIPE_RECOGNIZE) {
                choose_mode(p, 1u << VISION_MODE_FACE, now_ms);
            }
            break;
        case VISION_EV_FACEFAIL:
            /* Nor the face model: people, as the detector sees them. */
            if (p->mode >= PIPE_FACE) {
                choose_mode(p, 1u << VISION_MODE_DETECT, now_ms);
            }
            break;
        case VISION_EV_NODEVICE:
        case VISION_EV_NOMODEL:
        case VISION_EV_ERROR:
        case VISION_EV_LOST:
        case VISION_EV_EXITED:
            /* The camera is taken, a model is missing or broken, or the
             * helper is gone: DeskBuddy cannot see. */
            unavailable(p, now_ms, q);
            if (ev.kind == VISION_EV_EXITED || ev.kind == VISION_EV_LOST || ev.kind == VISION_EV_NODEVICE ||
                ev.kind == VISION_EV_NOMODEL) {
                vision_session_abandon(&p->s, 0);
                p->running = false;
                return -1;
            }
            break;
        default:
            break;
        }
    }
    if (!vision_session_active(&p->s)) {
        unavailable(p, now_ms, q);
        p->running = false;
        return -1;
    }
    if (db_judge_tick(&p->judge, now_ms, &out)) {
        db_vision_queue_push(q, &out);
    }
    return now_ms + PIPELINE_POLL_MS;
}

static void pipeline_stop(void *ctx)
{
    struct pipeline *p = &pipe_state;

    (void)ctx;
    if (p->running) {
        vision_session_abandon(&p->s, PIPELINE_GRACE_MS);
        p->running = false;
    }
}

const struct db_vision_provider_ops db_vision_pipeline_ops = {
    "vision",
    pipeline_start,
    pipeline_poll,
    pipeline_stop,
};
