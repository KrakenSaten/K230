/*
 * SYSTEM: what manages this node and this app, in one place, so ACTIVITY is
 * status and traffic only.
 *
 *   DEVICE        who this node is (name, hash, key), RENAME, and the two
 *                 ADVERT buttons that tell the mesh it is here
 *   ADDRESSING    the path hash size of the floods it starts (1, 2 or 3
 *                 bytes), with the confirmation it always had
 *   SOUND         whether a new direct message, and a new channel message,
 *                 makes a sound (rift_notify.h); per-channel mute is on the
 *                 channel's own row under CHANNELS
 *   CHANNELS      join, leave and mute (ui/rift_manage.c)
 *   SESSION       CLOSE RIFT (ui/rift_session.c)
 *
 * Nothing here is new behaviour except the channel sound and the mute: every
 * control is the one ACTIVITY had, moved, with the same rules.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_SYSTEM_H
#define RIFT_SYSTEM_H

#include "rift_app.h"

lv_obj_t *rift_system_create(struct rift_app *app, lv_obj_t *parent);
void rift_system_refresh(struct rift_app *app);
void rift_system_shape(struct rift_app *app);
void rift_system_destroy(struct rift_app *app);
/* Leaving SYSTEM or turning the panel: Cancel for every form and
 * confirmation on it. Touches no widget: it may run in a layout. */
void rift_system_cancel(struct rift_app *app);

#endif
