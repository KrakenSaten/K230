/*
 * ACTIVITY's TROPO line. See rift_tropo_view.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_tropo_view.h"

#include "pos_styles.h"
#include "rift_marks.h"
#include "rift_tropo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tropo_view {
    lv_obj_t *root;
    lv_obj_t *sign;
    lv_obj_t *headline;
    lv_obj_t *age;
    lv_obj_t *detail;
    lv_obj_t *note;
    enum pos_style_role role;
    int outlined;
};

static void on_delete(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

static lv_obj_t *words(lv_obj_t *parent, enum pos_style_role role, int wrap)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_remove_style_all(l);
    pos_style_add(l, role, 0);
    lv_label_set_text(l, "");
    if (wrap) {
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    return l;
}

lv_obj_t *rift_tropo_view_create(struct rift_app *app, lv_obj_t *parent)
{
    struct tropo_view *v = calloc(1, sizeof(*v));
    lv_obj_t *col;
    lv_obj_t *top;

    (void)app;
    if (!v) {
        return NULL;
    }
    v->root = lv_obj_create(parent);
    lv_obj_remove_style_all(v->root);
    pos_style_add(v->root, POS_STYLE_SLAB, 0);
    lv_obj_set_width(v->root, LV_PCT(100));
    lv_obj_set_height(v->root, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(v->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(v->root, 12, 0);
    lv_obj_set_style_pad_ver(v->root, 8, 0);
    lv_obj_set_style_pad_column(v->root, 12, 0);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(v->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(v->root, v);
    lv_obj_add_event_cb(v->root, on_delete, LV_EVENT_DELETE, v);

    v->sign = rift_warn_create(v->root);

    col = lv_obj_create(v->root);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_width(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    /* The headline and, at the right of the same line, how long ago. */
    top = lv_obj_create(col);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    v->role = POS_STYLE_TEXT_MUTED;
    v->headline = words(top, v->role, 0);
    lv_obj_set_flex_grow(v->headline, 1);
    lv_obj_set_width(v->headline, 1);
    lv_label_set_long_mode(v->headline, LV_LABEL_LONG_MODE_DOTS);
    v->age = words(top, POS_STYLE_CAPTION, 0);
    v->detail = words(col, POS_STYLE_CAPTION, 1);
    v->note = words(col, POS_STYLE_CAPTION, 1);
    lv_label_set_text(v->note, "Unusual mesh propagation: an indication, not proof of weather "
                               "or distance.");
    lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    return v->root;
}

static struct tropo_view *view_of(lv_obj_t *view)
{
    return view ? lv_obj_get_user_data(view) : NULL;
}

static void set_role(struct tropo_view *v, enum pos_style_role role)
{
    if (role != v->role) {
        lv_obj_remove_style(v->headline, pos_style(v->role), 0);
        pos_style_add(v->headline, role, 0);
        v->role = role;
    }
}

static void set_outline(struct tropo_view *v, int on)
{
    if (on == v->outlined) {
        return;
    }
    /* The red outline of the first hour: the field-error role, which is
     * exactly a status_error border, and nothing else. */
    if (on) {
        pos_style_add(v->root, POS_STYLE_FIELD_ERROR, 0);
    } else {
        lv_obj_remove_style(v->root, pos_style(POS_STYLE_FIELD_ERROR), 0);
    }
    v->outlined = on;
}

/* What the feed can and cannot tell it right now; NULL when it is watching. */
static const char *blind_why(const struct rift_app *app)
{
    const struct rift_rxlog *log = &app->rxlog;

    if (log->supported == 0) {
        return "THIS MESHCORED KEEPS NO RECEIVE LOG: TROPO CANNOT WATCH";
    }
    if (log->lost || app->model.stale || app->model.state == RIFT_SVC_ABSENT) {
        return "NOT LISTENING: MESHCORED IS NOT ANSWERING";
    }
    if (log->supported < 0) {
        return "WAITING FOR MESHCORED";
    }
    return NULL;
}

void rift_tropo_view_refresh(lv_obj_t *view, struct rift_app *app, int64_t now)
{
    struct tropo_view *v = view_of(view);
    struct rift_tropo_view t;
    const char *blind;
    char detail[200];
    char age[RIFT_AGE_MAX];
    char span[RIFT_AGE_MAX];
    size_t at = 0;

    if (!v || !app) {
        return;
    }
    rift_tropo_read(&app->rxlog.tropo, now, &t);
    blind = blind_why(app);
    rift_label_set(v->headline, rift_tropo_level_word(t.level));
    switch (t.level) {
    case RIFT_TROPO_DETECTED:
        rift_warn_set(v->sign, RIFT_WARN_ERROR);
        set_role(v, POS_STYLE_STATUS_ERROR_TEXT);
        break;
    case RIFT_TROPO_RECENT:
        rift_warn_set(v->sign, RIFT_WARN_WARN);
        set_role(v, POS_STYLE_STATUS_WARN_TEXT);
        break;
    case RIFT_TROPO_LAST:
        rift_warn_set(v->sign, RIFT_WARN_MUTED);
        set_role(v, POS_STYLE_TEXT_SECONDARY);
        break;
    case RIFT_TROPO_NONE:
    default:
        rift_warn_set(v->sign, RIFT_WARN_NEUTRAL);
        set_role(v, POS_STYLE_TEXT_MUTED);
        break;
    }
    set_outline(v, t.level == RIFT_TROPO_DETECTED);

    if (t.level == RIFT_TROPO_NONE) {
        rift_label_set(v->age, "");
        /* What it is watching for, and how far along it is - or why it is
         * not watching at all, which is not the same as a quiet mesh. */
        if (blind) {
            at += (size_t)snprintf(detail + at, sizeof(detail) - at, "%s", blind);
        } else {
            at += (size_t)snprintf(detail + at, sizeof(detail) - at,
                                   "WATCHING FOR %d DEEP FLOODS (%d+ HOPS) IN 15 MIN",
                                   RIFT_TROPO_NEEDED, RIFT_TROPO_HOPS);
            if (t.candidates > 0 && at < sizeof(detail)) {
                at += (size_t)snprintf(detail + at, sizeof(detail) - at, RIFT_SEP "%d SO FAR",
                                       t.candidates);
            }
        }
        if (t.have_base && at < sizeof(detail)) {
            snprintf(detail + at, sizeof(detail) - at, RIFT_SEP "USUAL DEPTH %u HOPS",
                     (unsigned)t.base_hops);
        }
        rift_label_set(v->detail, detail);
        lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* An opening: whether it is still going, its depth and the evidence it
     * rests on. The age at the right is from its latest deep packet - the
     * clock its colour follows. */
    rift_fmt_age(t.age_ms, 1, age, sizeof(age));
    rift_label_set(v->age, age);
    rift_fmt_age(t.span_ms, 1, span, sizeof(span));
    if (t.active) {
        at += (size_t)snprintf(detail + at, sizeof(detail) - at, "ACTIVE");
    } else {
        at += (size_t)snprintf(detail + at, sizeof(detail) - at, "ENDED" RIFT_SEP "LAST DEEP PACKET %s AGO",
                               age);
    }
    if (at < sizeof(detail)) {
        at += (size_t)snprintf(detail + at, sizeof(detail) - at,
                               RIFT_SEP "PEAK %u HOPS" RIFT_SEP "%u DEEP PACKET%s OVER %s",
                               (unsigned)t.peak_hops, t.evidence, t.evidence == 1 ? "" : "S", span);
    }
    if (t.have_base && at < sizeof(detail)) {
        at += (size_t)snprintf(detail + at, sizeof(detail) - at, RIFT_SEP "USUAL DEPTH %u",
                               (unsigned)t.base_hops);
    }
    if (t.openings > 1 && at < sizeof(detail)) {
        at += (size_t)snprintf(detail + at, sizeof(detail) - at, RIFT_SEP "%u SINCE RIFT OPENED",
                               t.openings);
    }
    if (blind && at < sizeof(detail)) {
        snprintf(detail + at, sizeof(detail) - at, RIFT_SEP "%s", blind);
    }
    rift_label_set(v->detail, detail);
    /* The caveat, while it is news; a grey LAST TROPO hours old does not
     * need it repeated. */
    if (t.level == RIFT_TROPO_LAST) {
        lv_obj_add_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(v->note, LV_OBJ_FLAG_HIDDEN);
    }
}

const char *rift_tropo_view_headline(lv_obj_t *view)
{
    struct tropo_view *v = view_of(view);

    return v ? lv_label_get_text(v->headline) : NULL;
}

const char *rift_tropo_view_detail(lv_obj_t *view)
{
    struct tropo_view *v = view_of(view);

    return v ? lv_label_get_text(v->detail) : NULL;
}

lv_obj_t *rift_tropo_view_sign(lv_obj_t *view)
{
    struct tropo_view *v = view_of(view);

    return v ? v->sign : NULL;
}
