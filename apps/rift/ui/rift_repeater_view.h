/*
 * One repeater's page: who it is and how it was heard, LOGIN / LOGOUT, what
 * it answers to STATUS, NEIGHBOURS and VERSION, and a COMMAND console
 * (rift_repeater_cmd.c). Reached from ACTIVITY's REPEATERS 0-HOP list
 * (rift_scan.c); the ACTIVITY tab stays lit on it, and Back or Esc returns
 * there. Everything shown is what meshcored reported (rift_repeater.h).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_REPEATER_VIEW_H
#define RIFT_REPEATER_VIEW_H

#include "rift_app.h"

#define RIFT_REPV_STATUS_ROWS 12

/* The objects a test presses or reads. */
enum rift_repv_part {
    RIFT_REPV_BACK = 0,
    RIFT_REPV_LOGIN,
    RIFT_REPV_LOGOUT,
    RIFT_REPV_PASSWORD,
    RIFT_REPV_STATUS,
    RIFT_REPV_NEIGHBOURS,
    RIFT_REPV_VERSION,
    RIFT_REPV_COMMAND,
    RIFT_REPV_SEND,
    RIFT_REPV_QUICK_VER,
    RIFT_REPV_QUICK_CLOCK,
    RIFT_REPV_QUICK_NEIGHBORS,
    RIFT_REPV_CONFIRM,
    RIFT_REPV_CANCEL,
    RIFT_REPV_LOGIN_STATE,
    RIFT_REPV_NOTE,
    RIFT_REPV_CMD_NOTE,
    RIFT_REPV_TRANSCRIPT,
    RIFT_REPV_TITLE,
    RIFT_REPV_PART_COUNT,
};

struct rift_repeater_view {
    struct rift_app *app;
    lv_obj_t *root;
    lv_obj_t *split;
    lv_obj_t *col[2];

    lv_obj_t *part[RIFT_REPV_PART_COUNT];

    lv_obj_t *key_value;
    lv_obj_t *heard_value;
    lv_obj_t *signal_value;
    lv_obj_t *contact_value;
    lv_obj_t *clock_line;
    lv_obj_t *login_hint;
    lv_obj_t *login_row;

    lv_obj_t *status_row[RIFT_REPV_STATUS_ROWS];
    lv_obj_t *status_value[RIFT_REPV_STATUS_ROWS];
    lv_obj_t *neigh_caption;
    lv_obj_t *neigh_row[RIFT_REP_NEIGHBOURS_MAX];
    lv_obj_t *neigh_name[RIFT_REP_NEIGHBOURS_MAX];
    lv_obj_t *neigh_snr[RIFT_REP_NEIGHBOURS_MAX];
    lv_obj_t *neigh_age[RIFT_REP_NEIGHBOURS_MAX];
    lv_obj_t *owner_row[3];
    lv_obj_t *owner_value[3];

    /* The console (rift_repeater_cmd.c). */
    lv_obj_t *cmd_panel;
    lv_obj_t *confirm;
    lv_obj_t *confirm_title;
    lv_obj_t *confirm_text;
    int confirming;
    char confirm_cmd[RIFT_REP_CMD_MAX + 1];
    unsigned transcript_drawn;

    /* What was last drawn, so a refresh changes only what changed. */
    int fields_live;
    int cmd_live;
    /* The keyboard's place on the page (rift_repeater_key): the control the
     * outline is on, or NULL before an arrow key was pressed. */
    lv_obj_t *cursor;
};

/* The keys, for rift_app.c's key sink: on ACTIVITY Up/Down walk SCAN 0-HOP
 * and the repeater rows and Enter presses or opens; on a repeater's page
 * they walk its controls, and Enter on a field puts the keys in it. A
 * control the keys are on carries the DS §9 focus outline. Returns 1 when
 * the key was used. */
int rift_repeater_key(struct rift_app *app, uint32_t key);
/* The outline on whatever the keys are on, as one helper for both screens. */
void rift_repeater_cursor_move(lv_obj_t **cursor, lv_obj_t *to);
/* rift_scan.c's half of it. */
int rift_scan_key(struct rift_app *app, uint32_t key);

lv_obj_t *rift_repeater_view_create(struct rift_app *app, lv_obj_t *parent);
void rift_repeater_view_destroy(struct rift_app *app);
void rift_repeater_view_shape(struct rift_app *app);
void rift_repeater_view_refresh(struct rift_app *app);
/* Leaving the page: a command confirmation is cancelled, the fields leave
 * the focus group and the password field is emptied. */
void rift_repeater_view_cancel(struct rift_app *app);
/* Open the page for this repeater (a whole key). */
void rift_repeater_open(struct rift_app *app, const char *key);

lv_obj_t *rift_repeater_view_part(const struct rift_app *app, enum rift_repv_part part);

/* rift_form_field_live, and when the field had the keys they go back to the
 * key sink from the timer. */
void rift_repeater_field_live(struct rift_app *app, lv_obj_t *field, int live);

/* rift_repeater_cmd.c, for the page. */
void rift_repeater_cmd_build(struct rift_repeater_view *v, lv_obj_t *parent);
void rift_repeater_cmd_refresh(struct rift_repeater_view *v, int usable);
void rift_repeater_cmd_cancel(struct rift_repeater_view *v);

#endif
