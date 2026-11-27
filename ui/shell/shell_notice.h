/*
 * Notices: a short banner at the top of whatever screen is showing, for
 * something that has just happened and belongs to an app that may not be
 * the one on screen - a message that arrived (app.h pocketos_shell_notify).
 *
 * One banner on the top layer, above apps, Controls and the lock: who says
 * it, who or what it is about, and a preview, with a rule down its left edge
 * in an identity accent. It slides in, stays NOTICE_HOLD_MS and slides out
 * again; with reduced motion on (DS §12) it appears and goes without moving.
 * A tap opens what it is about (the notice's own on_open, else its app) and
 * dismisses it - except while the lock is engaged, where it is read and not
 * acted on: the lock keeps a pocket from opening apps. Everywhere else on the
 * screen a touch reaches what is under it.
 *
 * Notices that arrive while one is showing wait their turn, at most
 * NOTICE_QUEUE of them; the oldest waiting one is dropped for a newer one,
 * and a waiting notice shortens the one on screen. Both holds are the
 * notice's own: the shell's default, or what its app asked for (app.h).
 * Nothing is stored: a
 * notice is news once, and the app it came from keeps the thing itself.
 *
 * While nothing has been shown, nothing of it exists: no object, no timer.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_SHELL_NOTICE_H
#define DOORS_SHELL_NOTICE_H

#include "app.h"

#include <stdbool.h>

#define NOTICE_HOLD_MS 5000
/* How long one stays when another is waiting behind it. */
#define NOTICE_HOLD_QUEUED_MS 2200
/* The bounds an app's own hold is kept within (app.h, hold_ms): long enough
 * to be read, short enough that a banner is never furniture. */
#define NOTICE_HOLD_MIN_MS 1000
#define NOTICE_HOLD_MAX_MS 15000
#define NOTICE_QUEUE 4
#define NOTICE_SOURCE_MAX 48
#define NOTICE_TITLE_MAX 64
#define NOTICE_BODY_MAX 200
#define NOTICE_ARG_MAX 96

/* The holds a notice is shown with: what the app asked for (0 for the
 * default), kept within the bounds, and the queued one never longer than the
 * plain one. Pure; shell_notice_post applies it. */
static inline void shell_notice_holds(int asked, int asked_queued, int *hold, int *hold_queued)
{
    int h = asked > 0 ? asked : NOTICE_HOLD_MS;
    int q = asked_queued > 0 ? asked_queued : NOTICE_HOLD_QUEUED_MS;

    h = h < NOTICE_HOLD_MIN_MS ? NOTICE_HOLD_MIN_MS : h > NOTICE_HOLD_MAX_MS ? NOTICE_HOLD_MAX_MS : h;
    q = q < NOTICE_HOLD_MIN_MS ? NOTICE_HOLD_MIN_MS : q > h ? h : q;
    *hold = h;
    *hold_queued = q;
}

/* Show it, or queue it behind the one showing. The strings are copied. */
void shell_notice_post(const struct pocketos_notice *n);
/* Take the one on screen away now, and drop what is waiting. */
void shell_notice_dismiss_all(void);
/* For shell.info and the tests: whether a banner is on screen, how many
 * were ever posted and how many were dropped from a full queue, and the
 * title and body of the newest one shown ("" before the first). */
bool shell_notice_visible(void);
unsigned shell_notice_posted(void);
unsigned shell_notice_dropped(void);
const char *shell_notice_title(void);
const char *shell_notice_body(void);
/* The exit path: delete the timer before LVGL goes. */
void shell_notice_shutdown(void);

#endif
