/*
 * SESSION, on ACTIVITY: the way to end RIFT (DS §51).
 *
 * RIFT keeps running when it is left with Back or Home (rift_app.h): its
 * meshcored connection, its model and where the reader was all stay, and the
 * status cluster shows RIFT on every other screen. This panel says so and
 * holds the one control that ends it:
 *
 *   CLOSE RIFT   asks first (DS §17.5: what it costs, Cancel first and
 *                accented), then ends the session and goes home. It ends
 *                only what is RIFT's - its connection and what it gathered -
 *                and never meshcored or the radio, which RIFT does not own.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_SESSION_H
#define RIFT_SESSION_H

#include "rift_app.h"

/* The panel, at the end of the column it is given. */
void rift_session_build(struct rift_app *app, lv_obj_t *parent);
void rift_session_refresh(struct rift_app *app);
/* Leaving ACTIVITY or turning the panel is Cancel for the confirmation.
 * Touches no widget: it may run in a layout. */
void rift_session_cancel(struct rift_app *app);
void rift_session_destroy(struct rift_app *app);

/* For the tests: the panel's button, and the confirmation's two. */
lv_obj_t *rift_session_close_button(const struct rift_app *app);
lv_obj_t *rift_session_confirm_button(const struct rift_app *app, int close);
int rift_session_confirming(const struct rift_app *app);

#endif
