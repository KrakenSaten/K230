/*
 * Managing this node, on ACTIVITY: its channels, its name and its path hash
 * size. The service does each (docs/api/mesh.md); this is where a reader
 * asks, sees what became of it, and is asked first when it cannot be undone.
 *
 *   CHANNELS    every channel the service holds, each with LEAVE - which
 *               asks first (DS §17.5, as FORGET does): the key is forgotten
 *               and nothing on the air gives it back. ADD CHANNEL opens a
 *               form in place: a hashtag topic (key derived from the name), a
 *               private channel (a new random key, shown once to be shared),
 *               or a key somebody shared.
 *   THIS DEVICE RENAME, whichever way the name was set (and said when the
 *               service could not save it); and the path hash size, 1 to 3
 *               bytes, where a move away from 1 asks first, because older
 *               repeaters drop what they cannot read.
 *
 * Nothing here transmits. A channel is a key held by the service, a name
 * reaches peers in this node's next advert (the ADVERT buttons are right
 * above), and a path hash size applies to the next flood.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_MANAGE_H
#define RIFT_MANAGE_H

#include "rift_app.h"

/* The CHANNELS panel, in parent (ACTIVITY's left column). The name and the
 * path hash size are ui/rift_device.c's, in THIS DEVICE. */
void rift_manage_build_channels(struct rift_app *app, lv_obj_t *parent);
void rift_manage_refresh(struct rift_app *app);
/* Leaving ACTIVITY, closing the app or turning the panel is Cancel for any
 * confirmation or form left open, and forgets a key shown for sharing. */
void rift_manage_cancel(struct rift_app *app);
void rift_manage_destroy(struct rift_app *app);

/* For a test: the form's fields and the kind it is set to. */
enum rift_channel_kind {
    RIFT_CHANNEL_HASHTAG = 0,
    RIFT_CHANNEL_PRIVATE,
    RIFT_CHANNEL_KEY,
};
lv_obj_t *rift_manage_name_field(const struct rift_app *app);
lv_obj_t *rift_manage_key_field(const struct rift_app *app);

#endif
