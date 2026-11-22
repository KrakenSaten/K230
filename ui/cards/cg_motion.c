/*
 * Card games: the motion runner. See cg_motion.h.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cg_motion.h"

#include <string.h>

static int lerp(int a, int b, int p)
{
    return a + (int)(((int64_t)(b - a) * p) / 1024);
}

/* 1 - (1 - u)^3: fast away, settling in. */
static int ease_out(int t)
{
    int64_t left = 1024 - t;

    return (int)(1024 - ((left * left * left) >> 20));
}

int cg_motion_pose_at(const struct cg_flight *f, uint32_t now, struct cg_pose *pose)
{
    int t, e;

    if (!f || !pose) {
        return -1;
    }
    if ((int32_t)(now - f->start) < 0) {
        if (f->hidden_before || f->kind == CG_MOVE_FADE) {
            return 0;
        }
        t = 0;
    } else if (f->dur == 0 || now - f->start >= f->dur) {
        t = 1024;
    } else {
        t = (int)(((uint64_t)(now - f->start) * 1024) / f->dur);
    }
    e = ease_out(t);
    pose->rect = f->to;
    pose->scale_x = 256;
    pose->face = f->face_to;
    pose->opa = LV_OPA_COVER;
    switch ((enum cg_move)f->kind) {
    case CG_MOVE_TRAVEL:
    case CG_MOVE_TURN:
        pose->rect.x1 = lerp(f->from.x1, f->to.x1, e);
        pose->rect.y1 = lerp(f->from.y1, f->to.y1, e);
        pose->rect.x2 = pose->rect.x1 + lv_area_get_width(&f->to) - 1;
        pose->rect.y2 = pose->rect.y1 + lv_area_get_height(&f->to) - 1;
        if (f->kind == CG_MOVE_TURN) {
            /* Away face down, and over in the last half of the way. */
            if (t < 512) {
                pose->face = f->face_from;
            } else if (t < 768) {
                pose->face = f->face_from;
                pose->scale_x = 768 - t;
            } else {
                pose->scale_x = t - 768;
            }
        }
        break;
    case CG_MOVE_FLIP:
        if (t < 512) {
            pose->face = f->face_from;
            pose->scale_x = (512 - t) / 2;
        } else {
            pose->scale_x = (t - 512) / 2;
        }
        break;
    case CG_MOVE_HOP: {
        int lift = lv_area_get_height(&f->to) * 16 / 100;
        int up = (int)(((int64_t)lift * 4 * t * (1024 - t)) >> 20);

        pose->rect.y1 -= up;
        pose->rect.y2 -= up;
        break;
    }
    case CG_MOVE_FADE:
        pose->opa = (lv_opa_t)(e * 255 / 1024);
        break;
    default:
        break;
    }
    return 1;
}

static int done_at(const struct cg_flight *f, uint32_t now)
{
    return (int32_t)(now - f->start) >= 0 && now - f->start >= f->dur;
}

static void frame(lv_timer_t *timer)
{
    struct cg_motion *m = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_get();
    uint32_t px = 0;
    lv_area_t origin;
    int i, kept = 0;

    if (!m || !m->obj) {
        return;
    }
    m->now = now;
    lv_obj_get_coords(m->obj, &origin);
    for (i = 0; i < m->n; i++) {
        struct cg_flight *f = &m->f[i];
        struct cg_pose p;

        if (cg_motion_pose_at(f, now, &p) == 1) {
            lv_area_t b = { origin.x1 + p.rect.x1 - m->margin, origin.y1 + p.rect.y1 - m->margin,
                            origin.x1 + p.rect.x2 + m->margin, origin.y1 + p.rect.y2 + m->margin };
            lv_area_t all = b;

            if (f->drawn.x1 <= f->drawn.x2) {
                all.x1 = LV_MIN(b.x1, f->drawn.x1);
                all.y1 = LV_MIN(b.y1, f->drawn.y1);
                all.x2 = LV_MAX(b.x2, f->drawn.x2);
                all.y2 = LV_MAX(b.y2, f->drawn.y2);
            }
            lv_obj_invalidate_area(m->obj, &all);
            px += lv_area_get_size(&all);
            f->drawn = b;
        }
        if (!done_at(f, now)) {
            m->f[kept++] = *f;
        }
    }
    m->n = kept;
    m->frames++;
    if (px > m->max_px) {
        m->max_px = px;
    }
    if (m->n == 0) {
        lv_timer_delete(m->timer);
        m->timer = NULL;
        /* The still picture takes over everywhere. */
        lv_obj_invalidate(m->obj);
        if (m->done) {
            m->done(m->user);
        }
    }
}

void cg_motion_init(struct cg_motion *m, lv_obj_t *obj, int margin)
{
    if (m) {
        memset(m, 0, sizeof(*m));
        m->obj = obj;
        m->margin = margin;
    }
}

void cg_motion_on_done(struct cg_motion *m, void (*done)(void *user), void *user)
{
    if (m) {
        m->done = done;
        m->user = user;
    }
}

void cg_motion_stop(struct cg_motion *m)
{
    if (!m) {
        return;
    }
    if (m->timer) {
        lv_timer_delete(m->timer);
        m->timer = NULL;
    }
    if (m->n && m->obj) {
        lv_obj_invalidate(m->obj);
    }
    m->n = 0;
}

int cg_motion_busy(const struct cg_motion *m)
{
    return m && m->n > 0;
}

int cg_motion_add(struct cg_motion *m, uint32_t key, enum cg_move kind, const lv_area_t *from, const lv_area_t *to,
                  int face_from, int face_to, uint32_t delay, uint32_t dur)
{
    struct cg_flight *f;

    if (!m || !to || m->n >= CG_MOTION_MAX) {
        return -1;
    }
    if (!m->timer) {
        m->now = lv_tick_get();
    }
    f = &m->f[m->n++];
    memset(f, 0, sizeof(*f));
    f->key = key;
    f->kind = (uint8_t)kind;
    f->face_from = (uint8_t)(face_from != 0);
    f->face_to = (uint8_t)(face_to != 0);
    f->from = from ? *from : *to;
    f->to = *to;
    f->start = m->now + delay;
    f->dur = dur;
    f->drawn.x1 = 1;
    f->drawn.x2 = 0;
    if (m->obj) {
        /* Where it is drawn until the first frame: the next frame repaints
         * that place too, so no copy is left behind there. */
        struct cg_pose p;
        lv_area_t origin;

        if (cg_motion_pose_at(f, m->now, &p) == 1) {
            lv_obj_get_coords(m->obj, &origin);
            f->drawn = (lv_area_t){ origin.x1 + p.rect.x1 - m->margin, origin.y1 + p.rect.y1 - m->margin,
                                    origin.x1 + p.rect.x2 + m->margin, origin.y1 + p.rect.y2 + m->margin };
        }
    }
    if (!m->timer && m->obj) {
        m->timer = lv_timer_create(frame, CG_MOTION_FRAME_MS, m);
    }
    if (m->obj) {
        lv_obj_invalidate(m->obj);
    }
    return 0;
}

static const struct cg_spot *find(const struct cg_spots *s, uint32_t key)
{
    int i;

    for (i = 0; s && i < s->n; i++) {
        if (s->s[i].key == key) {
            return &s->s[i];
        }
    }
    return NULL;
}

static int same(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

int cg_motion_diff(struct cg_motion *m, const struct cg_spots *before, const struct cg_spots *after,
                   const lv_area_t *source, const struct cg_plan *plan)
{
    int need = 0, added = 0, fresh = 0;
    int i;

    if (!m || !after || !plan) {
        return 0;
    }
    for (i = 0; i < after->n; i++) {
        const struct cg_spot *a = &after->s[i];
        const struct cg_spot *b = find(before, a->key);

        need += b ? (!same(&a->rect, &b->rect) || a->face != b->face) : source != NULL;
    }
    if (need == 0 || need > CG_MOTION_MAX - m->n) {
        return 0;
    }
    for (i = 0; i < after->n; i++) {
        const struct cg_spot *a = &after->s[i];
        const struct cg_spot *b = find(before, a->key);
        int rc = 1;

        if (!b && source) {
            lv_area_t from = { source->x1, source->y1, source->x1 + lv_area_get_width(&a->rect) - 1,
                               source->y1 + lv_area_get_height(&a->rect) - 1 };
            uint32_t delay = plan->delay + (uint32_t)fresh++ * plan->stagger;

            rc = cg_motion_add(m, a->key, a->face ? CG_MOVE_TURN : CG_MOVE_TRAVEL, &from, &a->rect, 0, a->face, delay,
                               plan->travel + (a->face ? plan->turn / 2 : 0));
            if (rc == 0) {
                m->f[m->n - 1].hidden_before = 1;
            }
        } else if (b && !same(&a->rect, &b->rect)) {
            rc = cg_motion_add(m, a->key, a->face != b->face ? CG_MOVE_TURN : CG_MOVE_TRAVEL, &b->rect, &a->rect,
                               b->face, a->face, 0, plan->travel + (a->face != b->face ? plan->turn / 2 : 0));
        } else if (b && a->face != b->face) {
            rc = cg_motion_add(m, a->key, CG_MOVE_FLIP, &a->rect, &a->rect, b->face, a->face, 0, plan->turn);
        }
        added += rc == 0;
    }
    return added;
}

int cg_motion_pose(const struct cg_motion *m, uint32_t key, struct cg_pose *pose)
{
    int i;

    for (i = 0; m && i < m->n; i++) {
        if (m->f[i].key == key) {
            struct cg_pose p;
            int rc = cg_motion_pose_at(&m->f[i], m->now, pose ? pose : &p);

            return rc < 0 ? -1 : rc;
        }
    }
    return -1;
}

uint32_t cg_motion_end(const struct cg_motion *m)
{
    uint32_t end = 0;
    int i;

    for (i = 0; m && i < m->n; i++) {
        int32_t left = (int32_t)(m->f[i].start + m->f[i].dur - m->now);

        end = left > (int32_t)end ? (uint32_t)left : end;
    }
    return end;
}

int cg_motion_count(const struct cg_motion *m)
{
    return m ? m->n : 0;
}

uint32_t cg_motion_key(const struct cg_motion *m, int index)
{
    return m && index >= 0 && index < m->n ? m->f[index].key : 0;
}
