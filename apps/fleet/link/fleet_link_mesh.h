/*
 * PocketFleet's link to the mesh: meshcored's app datagrams (mesh.app_send,
 * mesh.app_inbox, the mesh.app event; docs/api/mesh.md) over pocketipc, port
 * FLEET_MESH_PORT. The one file in Fleet that talks to a service.
 *
 * It never touches radiod or the radio (ADR-002): meshcored holds the radio,
 * and everything Fleet sends is a packet handed to it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETFLEET_LINK_MESH_H
#define POCKETFLEET_LINK_MESH_H

#include "fleet_link.h"

#define FLEET_MESH_SERVICE "meshcored"
#define FLEET_MESH_PORT 1

/* A link to meshcored on its usual socket (service NULL), or on another one
 * (tests). Connects lazily, on the first poll. NULL when out of memory. */
struct fleet_link *fleet_link_mesh_open(const char *service);

#endif
