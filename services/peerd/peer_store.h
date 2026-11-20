/*
 * peerd's persistent state (docs/decisions/ADR-017, sections 2, 5, 6, 8):
 * the only part of peerd that touches the filesystem.
 *
 *   identity.v1   the Ed25519 secret key. 0600, written once, temp-flush-
 *                 rename and the directory flushed.
 *   epoch.a, .b   the epoch, twice, each checksummed. The larger valid one
 *                 wins; a run writes its new epoch over the other, in place
 *                 and flushed, so the epoch never depends on a rename.
 *   kept.v1       the kept peers: key, replay floor, last address. Temp-
 *                 flush-rename, directory flushed.
 *   name.v1       the device's name on the network, a label only.
 *
 * Nothing here is ever repaired by regeneration. A state that has an
 * identity but no readable epoch, or an epoch that has moved on (or a kept
 * list) but no identity, is a fault with a name, and peerd stays closed: a
 * new identity would orphan a saved match and every peer's memory of this
 * device, and an epoch started again from 1 would lock it out of them.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PEERD_STORE_H
#define PEERD_STORE_H

#include "peer_crypto.h"

#include <stddef.h>
#include <stdint.h>

#define PS_KEPT_MAX 4
#define PS_NAME_MAX 24
#define PS_EPOCH_INITIAL 1u
/* The epoch is never raised to this: a device that got here stops sending
 * (peer_core) rather than wrap. */
#define PS_EPOCH_LIMIT UINT64_MAX

struct ps_kept {
    uint8_t key[PC_KEY];
    uint64_t epoch;         /* the replay floor: the newest DATA frame accepted, */
    uint64_t mark;          /* compared epoch first; 0, 0 when there is none yet */
    uint32_t addr;          /* the last endpoint, host byte order; 0 when unknown */
    uint16_t port;
};

enum ps_fault {
    PS_FAULT_NONE = 0,
    PS_FAULT_IDENTITY,      /* no usable identity where there should be one */
    PS_FAULT_EPOCH,         /* an identity and no valid epoch, or the epoch at its limit */
    PS_FAULT_IO,            /* the directory or a write failed */
};

struct ps_boot {
    struct pc_identity id;  /* valid when fault is PS_FAULT_NONE; the caller wipes it */
    uint64_t epoch;         /* this run's, already durably written */
    int created;            /* a new identity was made in this call */
    int kept_fault;         /* kept.v1 was there and could not be read: n is 0 */
    struct ps_kept kept[PS_KEPT_MAX];
    int kept_n;
    char name[PS_NAME_MAX + 1];     /* "" when none was stored */
    enum ps_fault fault;
};

/* Read the state under dir (created 0700 when absent), make an identity if
 * and only if there is no state at all, raise the epoch and write it
 * durably. Returns 0 with *out filled, or -1 with out->fault naming why;
 * on -1 nothing was created or changed beyond what the fault describes. */
int ps_boot(const char *dir, struct ps_boot *out);

/* Replace kept.v1 durably. n 0 writes an empty list. 0, or -1. */
int ps_kept_save(const char *dir, const struct ps_kept *recs, int n);
/* Store the name. 0, or -1. */
int ps_name_save(const char *dir, const char *name);

const char *ps_fault_name(enum ps_fault f);

/* Tests: each file's exact size, so a truncated or padded one is refused. */
#define PS_IDENTITY_SIZE (4 + 2 + PC_SECRET + 4)
#define PS_EPOCH_SIZE (4 + 8 + 4)
#define PS_KEPT_RECORD (PC_KEY + 8 + 8 + 4 + 2 + 2)
#define PS_KEPT_HEADER (4 + 2 + 1 + 1)

#endif
