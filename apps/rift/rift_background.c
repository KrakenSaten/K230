/*
 * RIFT's session, which outlives its screen (rift_app.h, DS §51): the one
 * block, how it starts, how it ends, and how a screen is let go of. Split from
 * rift_app.c, which builds the screens over it, so neither is a monolith
 * (tests/rift_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_app.h"

#include "app.h"
#include "pocketlog/pocketlog.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The one session: NULL while RIFT is not running at all. */
static struct rift_app *session;

struct rift_app *rift_app_session(void)
{
    return session;
}

int rift_app_in_background(void)
{
    return session && !session->frame;
}

struct rift_app *rift_bg_new(void)
{
    struct rift_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    rift_model_init(&a->model);
    rift_ipc_init(&a->ipc, &a->model, RIFT_SERVICE);
    /* The reader's choices, or the defaults when there are none (or none
     * that could be read): opening the app writes nothing. */
    if (rift_store_load(&a->prefs) < 0) {
        LOG_WARN("rift: %s could not be read; using the defaults", rift_store_path());
    }
    a->prefs_saved = 1;
    /* Everything the model holds now is history: nothing has arrived yet. */
    rift_notify_init(&a->notify, &a->model, a->prefs.dm_sound);
    rift_app_sound_attach(a);
    a->section = RIFT_SEC_ACTIVITY;
    return a;
}

void rift_bg_adopt(struct rift_app *a)
{
    session = a;
}

/* Only what is RIFT's: its timer, its one meshcored connection
 * (the subscription given back, the socket closed), its memory and its mark
 * in the status cluster. meshcored, and the radio meshcored owns, carry on
 * exactly as they were. */
void rift_bg_end(void)
{
    struct rift_app *a = session;

    if (!a) {
        return;
    }
    /* The timer first: it reaches the model and the connection, and one more
     * pass after either has been released is a use after free. */
    if (a->pump) {
        lv_timer_delete(a->pump);
        a->pump = NULL;
    }
    /* No sound to stop: the screen's destroy stopped it, and nothing sounds
     * with no screen (rift_app.c pump). */
    rift_ipc_close(&a->ipc);
    session = NULL;
    pocketos_shell_set_background(RIFT_APP_ID, NULL, NULL);
    LOG_INFO("rift: session ended after %u open(s)", a->opens);
    free(a);
}

void rift_app_end(struct rift_app *a)
{
    if (!a || a != session) {
        return;
    }
    a->ending = 1;
    /* Home, as the back slab goes: the shell destroys this screen (in the
     * press that asked, as the back slab's press does), and destroy ends the
     * session rather than keeping it. */
    pocketos_shell_go_home();
}

void rift_bg_forget_screen(struct rift_app *a)
{
    /* Everything before `section` is the screen's (rift_app.h). */
    memset(a, 0, offsetof(struct rift_app, section));
}
