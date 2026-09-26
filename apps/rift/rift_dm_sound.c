/*
 * RIFT's DM sound, as the app runs it: the setting, whether anything could
 * be heard, and the pass after every read of the socket that asks the
 * policy (rift_notify.c) whether a sound is due and the seam (rift_sound.c)
 * to make it. Nothing here decides what counts as a new message; the model
 * does (rift_arrivals.c).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_app.h"

#include "app.h"
#include "pocketlog/pocketlog.h"
#include "rift_sound.h"

int rift_app_can_sound(const struct rift_app *a)
{
    (void)a;
    /* The system volume is the shell's (volume.h): 0 while muted, and a
     * muted device starts no sound at all, as Wave does. */
    return rift_sound_available() && pocketos_shell_volume_effective() > 0;
}

void rift_app_set_dm_sound(struct rift_app *a, int on)
{
    if (!a) {
        return;
    }
    a->prefs.dm_sound = on ? 1 : 0;
    rift_notify_set_enabled(&a->notify, a->prefs.dm_sound);
    a->prefs_saved = rift_store_save(&a->prefs) == 0;
    if (!a->prefs_saved) {
        LOG_WARN("rift: the DM sound setting could not be stored at %s", rift_store_path());
    }
    rift_app_refresh(a);
}

/* A direct message that has just arrived, if there is one worth a sound.
 * After every pass at the socket, so a sound is not a repaint late; the
 * policy (rift_notify.c) makes a burst one sound and a replay none. */
void rift_app_notify_pass(struct rift_app *a, int64_t now)
{
    if (rift_notify_poll(&a->notify, &a->model, now, rift_app_can_sound(a))) {
        if (rift_sound_play(pocketos_shell_volume_effective()) != 0) {
            LOG_WARN("rift: the message sound (%s) did not start", rift_sound_backend_name());
        }
    }
}
