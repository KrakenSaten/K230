/*
 * This node's own settings on SYSTEM: its name (the DEVICE panel) and the
 * path hash size of the floods it starts (the ADDRESSING panel). The service does both (docs/api/mesh.md,
 * mesh.set_name and mesh.set_path_hash); neither transmits.
 *
 *   RENAME      a form in place, the current name in it. A name set by the
 *               service's configuration (MESHCORED_NAME) is renamed like any
 *               other; the service keeps the rename over it. Peers learn a
 *               new name from this node's next advert, and the caption says
 *               so. A rename the service took and could not write
 *               ("persisted": false) is shown as NOT SAVED, with a warning
 *               that the old name returns when the service restarts.
 *   PATH HASH   1, 2 or 3 bytes of each relay's key in the paths of this
 *               node's floods. 1 is what every MeshCore node reads; a move to
 *               2 or 3 asks first, because repeaters whose firmware does not
 *               read multi-byte paths drop such floods.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_DEVICE_H
#define RIFT_DEVICE_H

#include "rift_app.h"

/* The name's controls at the end of panel, the path hash's at the end of
 * path_panel (panel too when it is NULL). */
void rift_device_build(struct rift_app *app, lv_obj_t *panel, lv_obj_t *path_panel);
void rift_device_refresh(struct rift_app *app);
/* Leaving SYSTEM or turning the panel is Cancel for the rename form and
 * the path hash confirmation. Touches no widget: it may run in a layout. */
void rift_device_cancel(struct rift_app *app);
void rift_device_destroy(struct rift_app *app);
lv_obj_t *rift_device_rename_field(const struct rift_app *app);

#endif
