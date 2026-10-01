/*
 * PocketUI layout audit (DS §46.6): a walk over what is on the screen that
 * reports what a reader would lose - text cut off or shortened, a control
 * that cannot be reached, two things drawn over each other, something with
 * no size at all. It measures the objects LVGL laid out; it never changes
 * one.
 *
 * Made for the text sizes: a layout that was correct at Small can lose text
 * at Large, and a check of every app in both orientations at every size is
 * only practical as a measurement. The shell runs it on request (shell.audit,
 * docs/api/shell.md, and --audit for the simulator), so the same check runs
 * on the device.
 *
 * What counts:
 *
 *   clipped    a label whose text needs more height than its box (wrapping
 *              at the box's width) and is cut, or any meaningful object -
 *              a label with text, a clickable object - that reaches outside
 *              an ancestor which clips it and cannot be scrolled to show it.
 *   truncated  a label in the dots long mode whose text does not fit: it is
 *              shortened with "...", which an app may intend; reported so
 *              that it is a decision rather than an accident.
 *   overlap    two meaningful objects, neither inside the other, whose
 *              visible areas share more than a sliver.
 *   zero       a meaningful object with no visible width or height.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETUI_AUDIT_H
#define POCKETUI_AUDIT_H

#include "lvgl.h"

#include <stdbool.h>

enum pocketui_audit_kind {
    POCKETUI_AUDIT_CLIPPED = 0,
    POCKETUI_AUDIT_TRUNCATED,
    POCKETUI_AUDIT_OVERLAP,
    POCKETUI_AUDIT_ZERO,
    POCKETUI_AUDIT_KIND_COUNT
};

#define POCKETUI_AUDIT_PATH_MAX 96
#define POCKETUI_AUDIT_TEXT_MAX 40

struct pocketui_audit_issue {
    enum pocketui_audit_kind kind;
    char path[POCKETUI_AUDIT_PATH_MAX];  /* child indices from the root, "0.3.1" */
    char text[POCKETUI_AUDIT_TEXT_MAX];  /* the label's text, or "" */
    lv_area_t area;                      /* the object's area on the screen */
    /* clipped: the clipping ancestor's area, or the text's needed box;
     * overlap: the other object's area. */
    lv_area_t other;
    char other_path[POCKETUI_AUDIT_PATH_MAX];
    char other_text[POCKETUI_AUDIT_TEXT_MAX];
};

struct pocketui_audit_stats {
    unsigned objects;  /* visible objects walked */
    unsigned labels;   /* visible labels with text */
    unsigned issues[POCKETUI_AUDIT_KIND_COUNT];
};

typedef void (*pocketui_audit_cb)(const struct pocketui_audit_issue *issue, void *user);

/* Walk root (normally lv_screen_active()) and report each issue to cb.
 * Bounded: at most 1024 meaningful objects take part in the overlap check;
 * the rest are still checked on their own. stats may be NULL. Lay the
 * screen out first (lv_obj_update_layout) - the audit measures, it does not
 * wait. Never from inside an LVGL layout pass. */
void pocketui_audit(lv_obj_t *root, pocketui_audit_cb cb, void *user, struct pocketui_audit_stats *stats);

const char *pocketui_audit_kind_name(enum pocketui_audit_kind kind);

#endif
