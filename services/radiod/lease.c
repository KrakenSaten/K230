/*
 * radiod's radio lease. See lease.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "lease.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void radio_lease_init(struct radio_lease *l)
{
    memset(l, 0, sizeof(*l));
    l->next_owner_id = 1;
}

bool radio_lease_held(const struct radio_lease *l)
{
    return l->held;
}

bool radio_lease_is_owner(const struct radio_lease *l, uint64_t client_id)
{
    return l->held && client_id != 0 && l->client_id == client_id;
}

bool radio_lease_permits(const struct radio_lease *l, uint64_t client_id)
{
    return !l->held || radio_lease_is_owner(l, client_id);
}

int radio_lease_acquire(struct radio_lease *l, uint64_t client_id, const char *owner,
                        uint64_t mono_ms, uint64_t *owner_id)
{
    if (client_id == 0) {
        return -EINVAL;
    }
    if (l->held && l->client_id != client_id) {
        return -EBUSY;
    }
    if (!l->held) {
        l->held = true;
        l->client_id = client_id;
        /* A new identity every time, so a client cannot mistake the lease it
         * has now for the one it had before a reconnection. */
        l->owner_id = l->next_owner_id++;
        l->since_mono_ms = mono_ms;
    }
    snprintf(l->owner, sizeof(l->owner), "%s", (owner && owner[0]) ? owner : "unnamed");
    if (owner_id) {
        *owner_id = l->owner_id;
    }
    return 0;
}

int radio_lease_release(struct radio_lease *l, uint64_t client_id)
{
    if (!radio_lease_is_owner(l, client_id)) {
        return -EPERM;
    }
    l->held = false;
    l->client_id = 0;
    l->owner_id = 0;
    l->since_mono_ms = 0;
    l->owner[0] = '\0';
    return 0;
}

bool radio_lease_client_gone(struct radio_lease *l, uint64_t client_id)
{
    if (!radio_lease_is_owner(l, client_id)) {
        return false;
    }
    return radio_lease_release(l, client_id) == 0;
}

const char *radio_lease_owner(const struct radio_lease *l)
{
    return l->owner;
}

uint64_t radio_lease_owner_id(const struct radio_lease *l)
{
    return l->owner_id;
}

uint64_t radio_lease_client(const struct radio_lease *l)
{
    return l->client_id;
}

uint64_t radio_lease_since(const struct radio_lease *l)
{
    return l->since_mono_ms;
}
