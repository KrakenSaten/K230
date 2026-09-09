/*
 * The supervised-service table of system.status (docs/api/system.md).
 *
 * This lives in sysd rather than in core/pocketsys because its source is
 * pos-supervise's state file: a platform detail owned two layers up, which
 * core has no business parsing. core/pocketsys reads /proc, /sys and /etc,
 * sysd adds what the supervisor says, and the two meet in the response
 * object the same way api_version does.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef SYSD_SERVICES_H
#define SYSD_SERVICES_H

#include <cjson/cJSON.h>

/* Add the "services" array to a system.status object: one entry per
 * <name>.state file in the PocketOS runtime directory ($POCKETOS_RUNTIME_DIR,
 * pocketpaths.h), sorted by name. Always adds the key, as an empty array when
 * nothing is supervised. Every entry carries the same six keys, null where the
 * supervisor did not know the answer. */
void sysd_services_add(cJSON *status);

#endif
