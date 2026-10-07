/* How many contacts MeshCore's table holds on this port.
 *
 * Upstream's default is 32 (BaseChatMesh.h:37-39, `#ifndef MAX_CONTACTS`).
 * A dense mesh filled that within minutes, after which every new node's
 * advert was turned away and no message could be sent to it. 256 followed,
 * and then 1000 once RIFT's node list stopped building a row per node
 * (RIFT_MAX_NODES, apps/rift/rift_model.h). Nothing is evicted at either
 * size: the table still fills, later.
 *
 * ---- why this lives in a header -----------------------------------------
 *
 * The same reason as mc_channels.h: the macro decides a CLASS LAYOUT,
 *
 *     ContactInfo contacts[MAX_CONTACTS+MAX_ANON_CONTACTS];  // BaseChatMesh.h:64
 *     int sort_array[MAX_CONTACTS+MAX_ANON_CONTACTS];        // BaseChatMesh.h:66
 *
 * and two makefiles compile against that class - libmeshcore.a and
 * services/meshcored. BaseChatMesh.h includes <Arduino.h> (line 3) before it
 * reaches its default, and <Arduino.h> here is compat/Arduino.h, which
 * includes this file, so no translation unit can see the class with another
 * value. Unlike mc_channels.h there is no `#ifndef`: a -D given to one build
 * would be exactly the silent disagreement this file exists to prevent, so it
 * is an error instead.
 *
 * ---- what a contact costs ------------------------------------------------
 *
 * 184 bytes in the table and 4 in the sort array, so 1000 contacts is about
 * 184 KiB inside the runtime object. services/meshcored keeps one telemetry
 * slot per contact and persists at most this many (mesh_store.h); both
 * follow this value rather than repeating it, and mesh_runtime.cpp
 * static_asserts the one number its API publishes (MCD_MAX_NODES).
 *
 * Nothing about addressing changes: MeshCore still matches an inbound direct
 * packet by a one-byte hash and tries at most MAX_SEARCH_RESULTS (8) contacts
 * that share it (BaseChatMesh.h:12, BaseChatMesh.cpp:203).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef MAX_CONTACTS
#error "MAX_CONTACTS is decided in protocols/meshcore/compat/mc_contacts.h and nowhere else"
#endif
#define MAX_CONTACTS 1000
