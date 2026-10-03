/*
 * RIFT's message sounds, as the app runs them: the settings and channel
 * mutes, whether anything could be heard, and the pass after every read of
 * the socket that asks the policy (rift_notify.c) whether a sound is due and
 * which, and the seam (rift_sound.c) to make it. Nothing here decides what
 * counts as a new message; the model does (rift_arrivals.c).
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

static void store(struct rift_app *a, const char *what)
{
    a->prefs_saved = rift_store_save(&a->prefs) == 0;
    if (!a->prefs_saved) {
        LOG_WARN("rift: the %s could not be stored at %s", what, rift_store_path());
    }
    rift_app_refresh(a);
}

static int muted_cb(const char *conv_key, void *user)
{
    return rift_prefs_channel_muted(&((struct rift_app *)user)->prefs, conv_key);
}

void rift_app_sound_attach(struct rift_app *a)
{
    if (a) {
        rift_notify_set_channel(&a->notify, a->prefs.ch_sound, muted_cb, a);
    }
}

void rift_app_set_dm_sound(struct rift_app *a, int on)
{
    if (!a) {
        return;
    }
    a->prefs.dm_sound = on ? 1 : 0;
    rift_notify_set_enabled(&a->notify, a->prefs.dm_sound);
    store(a, "DM sound setting");
}

void rift_app_set_channel_sound(struct rift_app *a, int on)
{
    if (!a) {
        return;
    }
    a->prefs.ch_sound = on ? 1 : 0;
    rift_app_sound_attach(a);
    store(a, "channel sound setting");
}

int rift_app_channel_muted(const struct rift_app *a, const char *conv_key)
{
    return a ? rift_prefs_channel_muted(&a->prefs, conv_key) : 0;
}

int rift_app_set_channel_muted(struct rift_app *a, const char *conv_key, int muted)
{
    if (!a || rift_prefs_set_channel_muted(&a->prefs, conv_key, muted) != 0) {
        return -1;
    }
    store(a, "channel mute");
    return 0;
}

/* A message that has just arrived, if there is one worth a sound. After
 * every pass at the socket, so a sound is not a repaint late; the policy
 * (rift_notify.c) makes a burst one sound and a replay none. */
void rift_app_notify_pass(struct rift_app *a, int64_t now)
{
    int want = rift_notify_poll(&a->notify, &a->model, now, rift_app_can_sound(a));

    if (want != RIFT_NOTIFY_NONE) {
        enum rift_sound_kind kind = want == RIFT_NOTIFY_DM ? RIFT_SOUND_DM : RIFT_SOUND_CHANNEL;

        if (rift_sound_play(kind, pocketos_shell_volume_effective()) != 0) {
            LOG_WARN("rift: the %s sound (%s) did not start",
                     kind == RIFT_SOUND_DM ? "direct message" : "channel message",
                     rift_sound_backend_name());
        }
    }
}
