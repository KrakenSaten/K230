/*
 * radiod's radio lease: at most one client owns the configured radio session.
 *
 * This is an exclusivity boundary and nothing more. It is not a scheduler,
 * it does not share the radio between protocols, and it does not decide who
 * deserves it - the first client to ask holds it until it releases it or
 * disconnects. What it prevents is the case a long-running protocol daemon
 * cannot defend itself against: a second client reconfiguring the frequency
 * or transmitting on the profile the first one is in the middle of using,
 * silently, with both believing they had the radio.
 *
 * It is opt-in. While nobody holds a lease every client can do everything,
 * which is what every caller written before this existed expects. The
 * boundary appears the moment someone asks for it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RADIOD_LEASE_H
#define RADIOD_LEASE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_LEASE_OWNER_MAX 64

struct radio_lease {
    bool held;
    uint64_t client_id;      /* the connection holding it */
    uint64_t owner_id;       /* this lease's identity, 1 upwards */
    uint64_t next_owner_id;
    uint64_t since_mono_ms;
    char owner[RADIO_LEASE_OWNER_MAX];   /* the label the holder gave */
};

void radio_lease_init(struct radio_lease *l);

bool radio_lease_held(const struct radio_lease *l);
bool radio_lease_is_owner(const struct radio_lease *l, uint64_t client_id);
/* May this client use a lease-gated operation? True when nobody holds the
 * lease, and true for the holder. */
bool radio_lease_permits(const struct radio_lease *l, uint64_t client_id);

/* Take the lease. 0 on success, and 0 again for the client that already
 * holds it - asking twice is what a daemon does after a reconnect it is not
 * sure about, and refusing it would make recovering harder than it is. The
 * label is replaced on a repeat acquire; the owner_id is not, because it is
 * the same lease. -EBUSY when another client holds it. owner may be NULL. */
int radio_lease_acquire(struct radio_lease *l, uint64_t client_id, const char *owner,
                        uint64_t mono_ms, uint64_t *owner_id);

/* Give it up. 0 on success, -EPERM when this client does not hold it -
 * including when nobody does. Releasing a lease somebody else is relying on
 * is exactly the accident the lease exists to prevent, so it is refused
 * rather than quietly ignored. */
int radio_lease_release(struct radio_lease *l, uint64_t client_id);

/* A connection has gone. Returns true if it was holding the lease, which it
 * no longer is. */
bool radio_lease_client_gone(struct radio_lease *l, uint64_t client_id);

const char *radio_lease_owner(const struct radio_lease *l);
uint64_t radio_lease_owner_id(const struct radio_lease *l);
uint64_t radio_lease_client(const struct radio_lease *l);
uint64_t radio_lease_since(const struct radio_lease *l);

#ifdef __cplusplus
}
#endif

#endif
