/*
 * PocketUI layout audit. See pocketui_audit.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pocketui_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Below this many pixels an overrun or an overlap is rounding, an outline or
 * a hairline border meeting its neighbour's, not something a reader loses. */
#define AUDIT_SLACK 2
#define AUDIT_MAX_ITEMS 1024
#define AUDIT_MAX_DEPTH 24

struct item {
    lv_obj_t *obj;
    lv_area_t vis;  /* the part not clipped away by any ancestor */
    char path[POCKETUI_AUDIT_PATH_MAX];
};

struct walk {
    pocketui_audit_cb cb;
    void *user;
    struct pocketui_audit_stats st;
    struct item items[AUDIT_MAX_ITEMS];
    unsigned nitems;
};

static const char *const kind_names[POCKETUI_AUDIT_KIND_COUNT] = { "clipped", "truncated", "overlap", "zero" };

const char *pocketui_audit_kind_name(enum pocketui_audit_kind kind)
{
    return (kind >= 0 && kind < POCKETUI_AUDIT_KIND_COUNT) ? kind_names[kind] : "?";
}

/* lv_area_intersect is private in this LVGL (lv_area_private.h), which the
 * panel's sysroot need not carry. */
static bool intersect(lv_area_t *res, const lv_area_t *a, const lv_area_t *b)
{
    res->x1 = LV_MAX(a->x1, b->x1);
    res->y1 = LV_MAX(a->y1, b->y1);
    res->x2 = LV_MIN(a->x2, b->x2);
    res->y2 = LV_MIN(a->y2, b->y2);
    return res->x1 <= res->x2 && res->y1 <= res->y2;
}

static bool is_label(const lv_obj_t *obj)
{
    return lv_obj_check_type(obj, &lv_label_class);
}

/* Text worth reading: anything but spaces. */
static const char *label_text(lv_obj_t *obj)
{
    const char *t;

    if (!is_label(obj)) {
        return NULL;
    }
    t = lv_label_get_text(obj);
    if (!t) {
        return NULL;
    }
    for (const char *p = t; *p; p++) {
        if (*p != ' ' && *p != '\n' && *p != '\t') {
            return t;
        }
    }
    return NULL;
}

/* A control: clickable and listening. LVGL makes every object clickable
 * by default, so the flag alone would count every plain container; one that
 * nobody listens to is a surface, not a control. */
static bool control(lv_obj_t *obj)
{
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_event_count(obj) > 0;
}

static bool meaningful(lv_obj_t *obj)
{
    return label_text(obj) != NULL || control(obj);
}

static bool contains(const lv_area_t *outer, const lv_area_t *inner)
{
    return inner->x1 >= outer->x1 && inner->y1 >= outer->y1 && inner->x2 <= outer->x2 && inner->y2 <= outer->y2;
}

static void copy_text(char *dst, size_t n, const char *src)
{
    size_t k = 0;

    if (!src) {
        dst[0] = '\0';
        return;
    }
    /* One line, printable: the report is read in a log and compared as text. */
    for (; *src && k + 1 < n; src++) {
        dst[k++] = (*src == '\n' || *src == '\t' || *src == '"' || *src == '\\') ? ' ' : *src;
    }
    dst[k] = '\0';
}

static void report(struct walk *w, enum pocketui_audit_kind kind, const struct item *it, const lv_area_t *other,
                   const struct item *it2)
{
    struct pocketui_audit_issue is;

    memset(&is, 0, sizeof(is));
    is.kind = kind;
    snprintf(is.path, sizeof(is.path), "%s", it->path);
    copy_text(is.text, sizeof(is.text), label_text(it->obj));
    lv_obj_get_coords(it->obj, &is.area);
    if (other) {
        is.other = *other;
    }
    if (it2) {
        snprintf(is.other_path, sizeof(is.other_path), "%s", it2->path);
        copy_text(is.other_text, sizeof(is.other_text), label_text(it2->obj));
    }
    w->st.issues[kind]++;
    if (w->cb) {
        w->cb(&is, w->user);
    }
}

/* A label's own fit: the text wrapped at the box's width against the box's
 * height. A label sized to its content always fits; one that rolls its text
 * (the scroll modes) shows all of it in time. */
static void check_label_fit(struct walk *w, const struct item *it)
{
    lv_obj_t *obj = it->obj;
    /* Only a label has a long mode; a control comes here too and must not be
     * read as one (it is a different struct). */
    const char *text = label_text(obj);
    lv_label_long_mode_t mode;
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_area_t c;
    lv_point_t need;
    int32_t cw;
    int32_t ch;

    if (!text || !font) {
        return;
    }
    mode = lv_label_get_long_mode(obj);
    if (mode == LV_LABEL_LONG_MODE_SCROLL || mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR) {
        return;
    }
    lv_obj_get_content_coords(obj, &c);
    cw = lv_area_get_width(&c);
    ch = lv_area_get_height(&c);
    if (cw <= 0 || ch <= 0) {
        return; /* reported as zero */
    }
    /* In the dots mode LVGL writes the "..." into the label's own text once
     * it has laid it out, so what is left to measure already fits: the dots
     * at the end are the sign it did not. */
    if (mode == LV_LABEL_LONG_MODE_DOTS) {
        size_t n = strlen(text);

        if (n >= 3 && strcmp(text + n - 3, "...") == 0) {
            report(w, POCKETUI_AUDIT_TRUNCATED, it, &c, NULL);
            return;
        }
    }
    lv_text_get_size(&need, text, font, lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(obj, LV_PART_MAIN), cw, LV_TEXT_FLAG_NONE);
    if (need.y > ch + AUDIT_SLACK || need.x > cw + AUDIT_SLACK) {
        lv_area_t want = { c.x1, c.y1, c.x1 + need.x - 1, c.y1 + need.y - 1 };

        report(w, mode == LV_LABEL_LONG_MODE_DOTS ? POCKETUI_AUDIT_TRUNCATED : POCKETUI_AUDIT_CLIPPED, it, &want,
               NULL);
    }
}

/* Whether parent scrolls along the axis where child runs out of it: the
 * reader can then bring it into view. */
static bool scrolls_to(lv_obj_t *parent, const lv_area_t *a, const lv_area_t *p)
{
    lv_dir_t dir;
    bool out_x = a->x1 < p->x1 - AUDIT_SLACK || a->x2 > p->x2 + AUDIT_SLACK;
    bool out_y = a->y1 < p->y1 - AUDIT_SLACK || a->y2 > p->y2 + AUDIT_SLACK;

    if (!lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) {
        return false;
    }
    dir = lv_obj_get_scroll_dir(parent);
    if (out_x && !(dir & LV_DIR_HOR)) {
        return false;
    }
    if (out_y && !(dir & LV_DIR_VER)) {
        return false;
    }
    return true;
}

/* Clipping by ancestors, nearest first, as far as the first one that can be
 * scrolled to show it (whose own view is that ancestor's business: it is
 * checked when the walk reaches it, as anything scrollable is meaningful
 * enough to be). */
static void check_clipping(struct walk *w, struct item *it)
{
    lv_area_t a;
    lv_obj_t *p;

    lv_obj_get_coords(it->obj, &a);
    it->vis = a;
    for (p = lv_obj_get_parent(it->obj); p; p = lv_obj_get_parent(p)) {
        lv_area_t pa;
        bool inside;

        if (lv_obj_has_flag(p, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
            continue;
        }
        lv_obj_get_coords(p, &pa);
        inside = a.x1 >= pa.x1 - AUDIT_SLACK && a.y1 >= pa.y1 - AUDIT_SLACK && a.x2 <= pa.x2 + AUDIT_SLACK &&
                 a.y2 <= pa.y2 + AUDIT_SLACK;
        if (!intersect(&it->vis, &it->vis, &pa)) {
            it->vis.x2 = it->vis.x1 - 1; /* nothing left to see */
        }
        if (inside) {
            continue;
        }
        if (scrolls_to(p, &a, &pa)) {
            /* Reachable: the rest is the scroller's own view. */
            return;
        }
        report(w, POCKETUI_AUDIT_CLIPPED, it, &pa, NULL);
        return;
    }
}

static bool is_ancestor(lv_obj_t *anc, lv_obj_t *obj)
{
    for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p)) {
        if (p == anc) {
            return true;
        }
    }
    return false;
}

static void visit(struct walk *w, lv_obj_t *obj, char *path, size_t plen, int depth)
{
    uint32_t n;
    uint32_t k;

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || depth > AUDIT_MAX_DEPTH) {
        return;
    }
    w->st.objects++;
    if (meaningful(obj) || (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && depth > 0 &&
                            lv_obj_get_scroll_dir(obj) != LV_DIR_NONE && lv_obj_get_child_count(obj) > 0)) {
        struct item local;
        struct item *it = w->nitems < AUDIT_MAX_ITEMS ? &w->items[w->nitems] : &local;

        memset(it, 0, sizeof(*it));
        it->obj = obj;
        snprintf(it->path, sizeof(it->path), "%s", path);
        if (label_text(obj)) {
            w->st.labels++;
        }
        check_clipping(w, it);
        if (meaningful(obj)) {
            if (lv_obj_get_width(obj) <= 0 || lv_obj_get_height(obj) <= 0) {
                report(w, POCKETUI_AUDIT_ZERO, it, NULL, NULL);
            } else {
                check_label_fit(w, it);
            }
            if (it != &local) {
                w->nitems++;
            }
        }
    }
    n = lv_obj_get_child_count(obj);
    for (k = 0; k < n; k++) {
        size_t used = strlen(path);

        if (used + 6 < plen) {
            snprintf(path + used, plen - used, used ? ".%u" : "%u", (unsigned)k);
        }
        visit(w, lv_obj_get_child(obj, (int32_t)k), path, plen, depth + 1);
        path[used] = '\0';
    }
}

static void check_overlaps(struct walk *w)
{
    unsigned i;
    unsigned j;

    for (i = 0; i < w->nitems; i++) {
        const struct item *a = &w->items[i];

        if (lv_area_get_width(&a->vis) <= 0 || lv_area_get_height(&a->vis) <= 0) {
            continue;
        }
        for (j = i + 1; j < w->nitems; j++) {
            const struct item *b = &w->items[j];
            lv_area_t x;

            if (lv_area_get_width(&b->vis) <= 0 || lv_area_get_height(&b->vis) <= 0) {
                continue;
            }
            if (is_ancestor(a->obj, b->obj) || is_ancestor(b->obj, a->obj)) {
                continue;
            }
            if (!intersect(&x, &a->vis, &b->vis)) {
                continue;
            }
            if (lv_area_get_width(&x) <= AUDIT_SLACK || lv_area_get_height(&x) <= AUDIT_SLACK) {
                continue;
            }
            /* A control lying wholly under or over a smaller thing is the
             * surface it sits on - a board under its labels, a full-screen
             * gesture area under a dialog - not two things in each other's
             * way. Two pieces of text are never that. */
            if ((!label_text(a->obj) && contains(&a->vis, &b->vis)) ||
                (!label_text(b->obj) && contains(&b->vis, &a->vis))) {
                continue;
            }
            report(w, POCKETUI_AUDIT_OVERLAP, a, &b->vis, b);
        }
    }
}

void pocketui_audit(lv_obj_t *root, pocketui_audit_cb cb, void *user, struct pocketui_audit_stats *stats)
{
    /* 1024 items is ~120 kB: not on the LVGL thread's stack, and not kept
     * between audits either - an audit is rare, the memory is the shell's. */
    struct walk *w = calloc(1, sizeof(*w));
    char path[POCKETUI_AUDIT_PATH_MAX] = "";

    if (stats) {
        memset(stats, 0, sizeof(*stats));
    }
    if (!w) {
        return;
    }
    w->cb = cb;
    w->user = user;
    if (root) {
        visit(w, root, path, sizeof(path), 0);
        check_overlaps(w);
    }
    if (stats) {
        *stats = w->st;
    }
    free(w);
}
